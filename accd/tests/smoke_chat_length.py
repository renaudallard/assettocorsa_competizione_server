#!/usr/bin/env python3
"""Verify that the server relays short, long and Unicode chat intact."""
import os
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time

from fake_client import Reader, build_car_info, build_driver_info, wstr_a
from smoke_handshake import TCP_PORT, write_cfg


def frame(body):
    return struct.pack("<H", len(body)) + body


def run():
    executable = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "accd")
    with tempfile.TemporaryDirectory(prefix="accd-chat-") as tmp:
        write_cfg(tmp)
        proc = subprocess.Popen([executable, tmp], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, start_new_session=True)
        sockets = []
        buffers = {}
        messages = []
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

            def collect(duration):
                end = time.monotonic() + duration
                while time.monotonic() < end:
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
                            packet = buffers[sock][2:n+2]
                            buffers[sock] = buffers[sock][n+2:]
                            if packet and packet[0] == 0x2b:
                                reader = Reader(packet)
                                reader.u8()
                                reader.fmt_a()
                                messages.append((sock, reader.fmt_a()))

            for i in range(2):
                sock = socket.create_connection(("127.0.0.1", TCP_PORT), timeout=2)
                sockets.append(sock)
                buffers[sock] = b""
                payload = (bytes([9]) + struct.pack("<H", 0x100) + wstr_a("") +
                           build_driver_info(steam="S7656119900000000" + str(i)) +
                           bytes(8) + build_car_info(race_number=90+i))
                sock.sendall(frame(payload))
                collect(.2)

            samples = ["Hello", "A message longer than thirty-five characters must remain intact.",
                       "Unicode characters remain intact: äöü ÄÖÜ ß and other characters."]
            for text in samples:
                messages.clear()
                sockets[0].sendall(frame(bytes([0x2a]) + wstr_a(text) + struct.pack("<i", 0)))
                collect(.3)
                received = {sock for sock, body in messages if body == text}
                assert received == set(sockets), "chat was truncated or not delivered to both clients"
            print("PASS: short, long and Unicode chat delivered intact to both clients")
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
