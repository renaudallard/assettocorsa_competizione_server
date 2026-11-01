#!/usr/bin/env python3
"""
Smoke test: a lap stays with the driver who drove it when that
driver's car slot could be handed to someone else.

The server has 4 connections. A keeper holds the session open. Driver
A laps and leaves, driver B joins and laps, driver C joins and leaves
without a lap, driver E laps and leaves, driver D drives only its
out-lap and leaves, then A comes back in the same car and laps again.
That is more cars than connections, and every car but C has a row, so
the pool must hold departed cars past the connection count. After
/next the results file must give every lap to the car of the driver
who drove it, must not list C, and every lap must belong to a listed
car. Invoked from `make test`. Exits 0 on success, non-zero on any
failure.
"""

import json
import os
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time

from fake_client import build_car_info, build_driver_info, wstr_a
from smoke_handshake import ACCD_PROTOCOL_VERSION, TCP_PORT, fail, write_cfg

KEEPER = "S76561199000000001"
DRIVER_A = "S76561199000000002"
DRIVER_B = "S76561199000000003"
DRIVER_C = "S76561199000000004"
DRIVER_D = "S76561199000000005"
DRIVER_E = "S76561199000000006"
SETTINGS = {"serverName": "smoke", "adminPassword": "admin", "dumpLeaderboards": 1}


def frame(body):
    return struct.pack("<H", len(body)) + body


class Client:
    def __init__(self, proc, steam, race_number):
        self.proc = proc
        self.buf = b""
        self.welcomed = False
        self.sock = socket.create_connection(("127.0.0.1", TCP_PORT), timeout=2)
        body = bytes([0x09])
        body += struct.pack("<H", ACCD_PROTOCOL_VERSION)
        body += wstr_a("")
        body += build_driver_info(steam=steam)
        body += bytes(8)
        body += build_car_info(race_number=race_number)
        self.sock.sendall(frame(body))
        end = time.monotonic() + 3.0
        while not self.welcomed:
            if time.monotonic() >= end:
                fail(f"no welcome for {steam}")
            self.pump(0.05)

    def pump(self, duration):
        end = time.monotonic() + duration
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return
            if self.proc.poll() is not None:
                fail(f"accd exited with status {self.proc.returncode}")
            ready, _, _ = select.select([self.sock], [], [], left)
            if not ready:
                continue
            chunk = self.sock.recv(65536)
            if not chunk:
                fail("server closed a client connection")
            self.buf += chunk
            while len(self.buf) >= 2:
                n = struct.unpack_from("<H", self.buf)[0]
                if len(self.buf) < 2 + n:
                    break
                if n > 0 and self.buf[2] == 0x0B:
                    self.welcomed = True
                self.buf = self.buf[2 + n :]

    def send(self, body):
        self.sock.sendall(frame(body))

    def lap(self, ms, lap_states=0):
        self.send(struct.pack("<BiBiH", 0x20, ms // 3, 0, 100000, 0))
        self.send(struct.pack("<BiBiH", 0x20, ms // 3, 1, 100000, 0))
        self.send(struct.pack("<BiiBHB", 0x21, ms, 100000, 0, lap_states, 0))
        self.pump(0.3)

    def chat(self, text):
        self.send(bytes([0x2A]) + wstr_a(text) + struct.pack("<i", 0))
        self.pump(0.3)

    def close(self):
        self.sock.close()


def find_result(results):
    if not os.path.isdir(results):
        return None
    for name in sorted(os.listdir(results)):
        if not name.endswith(".json"):
            continue
        try:
            with open(os.path.join(results, name)) as f:
                obj = json.load(f)
        except (OSError, ValueError):
            continue
        if obj.get("laps"):
            return obj
    return None


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    accd_bin = os.path.join(os.path.dirname(script_dir), "accd")
    if not os.access(accd_bin, os.X_OK):
        fail(f"accd binary not found at {accd_bin}")

    with tempfile.TemporaryDirectory(prefix="accd-reuse-") as tmp:
        cfg = os.path.join(tmp, "cfg")
        os.mkdir(cfg)
        write_cfg(cfg)
        with open(os.path.join(cfg, "settings.json"), "w") as f:
            json.dump(SETTINGS, f)
        proc = subprocess.Popen(
            [accd_bin, cfg],
            cwd=tmp,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        clients = []
        try:
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                try:
                    with socket.create_connection(("127.0.0.1", TCP_PORT), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                fail(f"accd did not listen on {TCP_PORT} within 3s")

            keeper = Client(proc, KEEPER, 91)
            clients.append(keeper)
            a = Client(proc, DRIVER_A, 92)
            a.lap(88000)
            a.close()
            keeper.pump(0.5)

            b = Client(proc, DRIVER_B, 93)
            clients.append(b)
            b.lap(99000)
            c = Client(proc, DRIVER_C, 94)
            c.close()
            keeper.pump(0.5)

            e = Client(proc, DRIVER_E, 96)
            e.lap(105000)
            e.close()
            keeper.pump(0.5)

            d = Client(proc, DRIVER_D, 95)
            d.lap(111000, lap_states=0x0004)  # IsOutLap
            d.close()
            keeper.pump(0.5)

            a = Client(proc, DRIVER_A, 92)
            clients.append(a)
            a.lap(77000)

            keeper.chat("/admin admin")
            keeper.chat("/next")

            results = os.path.join(tmp, "results")
            obj = None
            end = time.monotonic() + 8.0
            while obj is None and time.monotonic() < end:
                keeper.pump(0.2)
                obj = find_result(results)
            if obj is None:
                fail("no results file with laps")

            owner = {}
            for line in obj["sessionResult"]["leaderBoardLines"]:
                car = line["car"]
                if car["carId"] in owner:
                    fail(f"car {car['carId']} has two result rows")
                owner[car["carId"]] = [d["playerId"] for d in car["drivers"]]
            if any(DRIVER_C in ids for ids in owner.values()):
                fail("driver who left without a lap has a result row")
            for lp in obj["laps"]:
                if lp["carId"] not in owner:
                    fail(f"lap {lp['laptime']} of car {lp['carId']} has no result row")
            laps = {lp["laptime"]: lp["carId"] for lp in obj["laps"]}
            want = {
                88000: DRIVER_A,
                99000: DRIVER_B,
                77000: DRIVER_A,
                111000: DRIVER_D,
                105000: DRIVER_E,
            }
            for ms, steam in want.items():
                if ms not in laps:
                    fail(f"lap {ms} missing from the results")
                got = owner.get(laps[ms])
                if got != [steam]:
                    fail(f"lap {ms} went to car {laps[ms]} of {got}, not {steam}")
            if laps[88000] != laps[77000]:
                fail("returning driver did not get the same car back")
            print("PASS: laps keep their driver across car slot reuse")
        finally:
            for cl in clients:
                cl.close()
            try:
                os.killpg(proc.pid, signal.SIGTERM)
                proc.wait(timeout=2)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


if __name__ == "__main__":
    main()
