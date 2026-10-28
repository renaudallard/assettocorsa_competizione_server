#!/usr/bin/env python3
"""Verify departed drivers remain correctly classified in timed sessions."""
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time
from fake_client import build_car_info, build_driver_info, wstr_a
from smoke_handshake import TCP_PORT, write_cfg


def frame(body):
    return struct.pack("<H", len(body)) + body


def run():
    executable = Path(__file__).resolve().parent.parent / "accd"
    with tempfile.TemporaryDirectory(prefix="accd-reuse-") as tmp:
        config = Path(tmp) / "cfg"
        config.mkdir()
        write_cfg(str(config))
        settings = json.loads((config / "settings.json").read_text())
        settings.update(adminPassword="test-secret", dumpLeaderboards=1)
        (config / "settings.json").write_text(json.dumps(settings))
        event = json.loads((config / "event.json").read_text())
        event["sessions"][0]["sessionType"] = os.environ.get("ACC_TEST_SESSION", "P")
        event["sessions"].append(dict(event["sessions"][0], sessionType="Q"))
        (config / "event.json").write_text(json.dumps(event))
        proc = subprocess.Popen([str(executable), str(config)], cwd=tmp,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                start_new_session=True)
        sockets = []
        buffers = {}
        packets = {}
        try:
            deadline = time.monotonic() + 3
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", TCP_PORT), timeout=.2):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("server did not start")
                    time.sleep(.05)

            def pump(duration=.1):
                end = time.monotonic() + duration
                while time.monotonic() < end and sockets:
                    ready, _, _ = select.select(sockets, [], [], end-time.monotonic())
                    for sock in ready:
                        chunk = sock.recv(65536)
                        if not chunk:
                            raise RuntimeError("unexpected disconnect")
                        buffers[sock] += chunk
                        while len(buffers[sock]) >= 2:
                            n = struct.unpack_from("<H", buffers[sock])[0]
                            if len(buffers[sock]) < n + 2:
                                break
                            packet, buffers[sock] = buffers[sock][2:n+2], buffers[sock][n+2:]
                            if packet:
                                packets[sock].append(packet[0])

            def until(condition, timeout=5):
                end = time.monotonic() + timeout
                while not condition() and time.monotonic() < end:
                    pump(.05)
                assert condition(), "timed out waiting for server state"

            def join(index):
                steam = "S7656119900000000" + str(index)
                sock = socket.create_connection(("127.0.0.1", TCP_PORT), timeout=2)
                sockets.append(sock); buffers[sock] = b""; packets[sock] = []
                body = (bytes([9]) + struct.pack("<H", 0x100) + wstr_a("") +
                        build_driver_info(steam=steam) + bytes(8) +
                        build_car_info(race_number=90+index))
                sock.sendall(frame(body))
                until(lambda: 0x0b in packets[sock])
                return sock, steam

            def lap(sock, ms):
                sock.sendall(frame(struct.pack("<BiBiH", 0x20, 22000, 0, 100000, 0)))
                sock.sendall(frame(struct.pack("<BiBiH", 0x20, 38000, 1, 100000, 0)))
                sock.sendall(frame(struct.pack("<BiiBHB", 0x21, ms, 100000, 0, 0, 0)))
                pump(.25)

            slower, slower_id = join(1)
            faster, faster_id = join(2)
            lap(slower, 100000)
            lap(faster, 90000)
            sockets.remove(faster);faster.close();pump(.3)
            lap(slower, 110000)
            for command in ("/admin test-secret", "/next"):
                slower.sendall(frame(bytes([0x2a]) + wstr_a(command) + struct.pack("<i", 0)))
                pump(.3)

            def result():
                for f in (Path(tmp) / "results").glob("*.json"):
                    try:
                        obj = json.loads(f.read_text())
                    except (OSError, ValueError):
                        continue
                    if {90000, 100000, 110000}.issubset({lap["laptime"] for lap in obj.get("laps", [])}):
                        return obj
                return None
            until(lambda: result() is not None, timeout=8)
            order = [line["car"]["drivers"][0]["playerId"]
                     for line in result()["sessionResult"]["leaderBoardLines"] if line["car"]["drivers"]]
            assert order.index(faster_id) < order.index(slower_id), "departed faster driver ranked below slower driver"
            print("PASS: departed faster driver remains ahead in the result")
        finally:
            for sock in sockets:
                sock.close()
            try:
                os.killpg(proc.pid, signal.SIGTERM)
                proc.wait(timeout=2)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


if __name__ == "__main__":
    run()
