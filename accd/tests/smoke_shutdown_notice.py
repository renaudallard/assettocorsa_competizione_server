#!/usr/bin/env python3
"""Verify graceful shutdown sends client notices before closing sockets."""
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
    with tempfile.TemporaryDirectory(prefix="accd-shutdown-") as tmp:
        write_cfg(tmp)
        proc = subprocess.Popen([executable, tmp], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, start_new_session=True)
        sockets, buffers, notices = [], {}, {}
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
            for i in range(3):
                sock = socket.create_connection(("127.0.0.1", TCP_PORT), timeout=2)
                sockets.append(sock);buffers[sock] = b"";notices[sock] = []
                body = (bytes([9]) + struct.pack("<H", 0x100) + wstr_a("") +
                        build_driver_info(steam="S7656119900000000" + str(i)) +
                        bytes(8) + build_car_info(race_number=90+i))
                sock.sendall(frame(body))
                # Wait for the welcome before signalling shutdown.
                sock.settimeout(3)
                def read_exact(n):
                    data = b""
                    while len(data) < n:
                        chunk = sock.recv(n-len(data))
                        if not chunk: raise RuntimeError("connection closed before welcome")
                        data += chunk
                    return data
                length = struct.unpack("<H", read_exact(2))[0]
                assert read_exact(length)[0] == 0x0b, "driver not accepted"
            os.killpg(proc.pid, signal.SIGTERM)
            active = list(sockets)
            end = time.monotonic() + 4
            while active and time.monotonic() < end:
                ready, _, _ = select.select(active, [], [], end-time.monotonic())
                for sock in ready:
                    chunk = sock.recv(65536)
                    if not chunk:
                        active.remove(sock)
                        continue
                    buffers[sock] += chunk
                    while len(buffers[sock]) >= 2:
                        n = struct.unpack_from("<H", buffers[sock])[0]
                        if len(buffers[sock]) < n + 2: break
                        packet, buffers[sock] = buffers[sock][2:n+2], buffers[sock][n+2:]
                        if packet and packet[0] == 0x2b:
                            reader = Reader(packet);reader.u8();reader.fmt_a()
                            text = reader.fmt_a()
                            notices[sock].append((packet[-1], text))
            assert not active, "clients did not receive EOF"
            for sock in sockets:
                assert any(kind == 4 and "Connection lost" in msg for kind, msg in notices[sock]), "missing shutdown chat"
                assert any(kind == 5 and "Connection lost" in msg for kind, msg in notices[sock]), "missing shutdown state"
            print("PASS: all clients receive shutdown notices before EOF")
        finally:
            for sock in sockets: sock.close()
            try:
                os.killpg(proc.pid, signal.SIGTERM);proc.wait(timeout=2)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try: os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError: pass


if __name__ == "__main__": run()
