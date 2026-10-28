#!/usr/bin/env python3
"""Check foreign reporter rejection and valid own-car contact relay."""
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

from fake_client import (ACCD_PROTOCOL_VERSION, build_car_info, build_driver_info,
                         recv_all_frames, recv_framed, write_cfg, wstr_a)

def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def join(port, first, last, steam):
    body = b"\x09" + struct.pack("<H", ACCD_PROTOCOL_VERSION)
    body += wstr_a("")
    body += build_driver_info(first=first, last=last, short=first[:3], steam=steam)
    body += bytes(8) + build_car_info()
    frame = struct.pack("<H", len(body)) + body
    sock = socket.create_connection(("127.0.0.1", port), timeout=2)
    sock.sendall(frame)
    welcome, trailing = recv_framed(sock)
    if welcome[:1] != b"\x0b":
        raise RuntimeError("synthetic handshake rejected")
    conn_id = struct.unpack_from("<H", welcome, 4)[0]
    car_id = struct.unpack_from("<I", welcome, 6)[0]
    recv_all_frames(sock, trailing, idle_ms=120)
    return sock, conn_id, car_id



tcp, udp = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM)
with tempfile.TemporaryDirectory(prefix='accd-contact-test-') as tmp:
    write_cfg(tmp, tcp, udp)
    with open(os.path.join(tmp, 'server.log'), 'wb') as log:
        proc = subprocess.Popen([os.path.abspath("accd"), tmp], cwd=tmp, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True)
        sockets = []
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(('127.0.0.1', tcp), timeout=0.2):
                        break
                except OSError:
                    if proc.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('server failed to listen')
                    time.sleep(0.05)
            victim, _, victim_car = join(tcp, 'Smoke', 'Vctm', 'LOCAL-TEST-ONE')
            sender, _, sender_car = join(tcp, 'Smoke', 'Trgt', 'LOCAL-TEST-TWO')
            sockets.extend([victim, sender])
            def relay(reporter, target):
                body = struct.pack('<BHHiB', 0x19, reporter, target, 1234, 5)
                sender.sendall(struct.pack('<H', len(body)) + body)
                frames, _ = recv_all_frames(victim, b'', idle_ms=600)
                return any(t == 0x1b and len(b) >= 5 and
                           struct.unpack_from('<HH', b, 1) == (reporter, target)
                           for t, b in frames)
            foreign = relay(victim_car, sender_car)
            own = relay(sender_car, victim_car)
            print('foreign_reporter_relayed=' + str(foreign))
            print('own_reporter_relayed=' + str(own))
            sys.exit(0 if not foreign and own else 1)
        finally:
            for sock in sockets:
                sock.close()
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                proc.wait(timeout=3)
