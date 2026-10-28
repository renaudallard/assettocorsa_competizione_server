#!/usr/bin/env python3
"""Exercise optional first-UDP-port pinning with local synthetic clients."""
import json
import os
import signal
import socket
import struct
import subprocess
import tempfile
import time

from fake_client import (ACCD_PROTOCOL_VERSION, build_car_info, build_driver_info,
                         recv_all_frames, recv_framed, write_cfg, wstr_a)


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def join(port, name, steam):
    body = b'\x09' + struct.pack('<H', ACCD_PROTOCOL_VERSION) + wstr_a('')
    body += build_driver_info(first=name, last='Test', short=name[:3], steam=steam)
    body += bytes(8) + build_car_info()
    sock = socket.create_connection(('127.0.0.1', port), timeout=2)
    sock.sendall(struct.pack('<H', len(body)) + body)
    welcome, trailing = recv_framed(sock)
    if welcome[:1] != b'\x0b':
        raise RuntimeError('handshake rejected')
    conn_id = struct.unpack_from('<H', welcome, 4)[0]
    car_id = struct.unpack_from('<I', welcome, 6)[0]
    recv_all_frames(sock, trailing, idle_ms=120)
    return sock, conn_id, car_id


def recv_packet(sock, marker, timeout=2):
    deadline = time.monotonic() + timeout
    sock.settimeout(0.1)
    while time.monotonic() < deadline:
        try:
            data, _ = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if marker(data):
            return True
    return False


def run_case(strict):
    tcp, udp = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM)
    with tempfile.TemporaryDirectory(prefix='accd-udp-pin-') as tmp:
        write_cfg(tmp, tcp, udp)
        settings_path = os.path.join(tmp, 'settings.json')
        with open(settings_path, encoding='utf-8') as f:
            settings = json.load(f)
        if strict:
            settings['strictUdpPeer'] = 1
        with open(settings_path, 'w', encoding='utf-8') as f:
            json.dump(settings, f)
        with open(os.path.join(tmp, 'server.log'), 'wb') as log:
            proc = subprocess.Popen([os.path.abspath('accd'), tmp], cwd=tmp,
                                    stdout=log, stderr=subprocess.STDOUT,
                                    start_new_session=True)
            sockets = []
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        with socket.create_connection(('127.0.0.1', tcp), timeout=0.2):
                            break
                    except OSError:
                        if proc.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError('server did not start')
                        time.sleep(0.05)
                victim, victim_id, victim_car = join(tcp, 'Vctm', 'LOCAL-TEST-ONE')
                target, target_id, target_car = join(tcp, 'Trgt', 'LOCAL-TEST-TWO')
                sockets.extend([victim, target])
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as owner, \
                     socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other, \
                     socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as observer:
                    for sock in (owner, other, observer):
                        sock.bind(('127.0.0.1', 0))
                    dest = ('127.0.0.1', udp)
                    if strict:
                        # Neither a truncated keepalive nor a wrong-car update
                        # may claim the initial endpoint before the owner.
                        other.sendto(b'\x13', dest)
                        wrong_car = bytearray(68)
                        struct.pack_into('<BHHBI', wrong_car, 0, 0x1e,
                                         victim_id, target_car, 1, 1000)
                        other.sendto(wrong_car, dest)
                    owner.sendto(struct.pack('<BH', 0x13, victim_id), dest)
                    observer.sendto(struct.pack('<BH', 0x13, target_id), dest)
                    assert recv_packet(owner, lambda p: p[:1] == b'\x14')
                    other.sendto(struct.pack('<BH', 0x13, victim_id), dest)
                    redirected = recv_packet(other, lambda p: p[:1] == b'\x14', 1.5)
                    if not strict:
                        return redirected
                    assert not redirected

                    request = struct.pack('<BHH', 0x22, target_car, victim_id)
                    recv_all_frames(victim, b'', idle_ms=100)
                    other.sendto(request, dest)
                    frames, _ = recv_all_frames(victim, b'', idle_ms=400)
                    assert not any(msg == 0x23 for msg, _ in frames)
                    owner.sendto(request, dest)
                    frames, _ = recv_all_frames(victim, b'', idle_ms=400)
                    assert any(msg == 0x23 for msg, _ in frames)

                    event = struct.pack('<BHHdB', 0x5e, victim_id, target_id, 1234.0, 1)
                    recv_all_frames(target, b'', idle_ms=100)
                    other.sendto(event, dest)
                    frames, _ = recv_all_frames(target, b'', idle_ms=400)
                    assert not any(msg == 0x2b for msg, _ in frames)
                    owner.sendto(event, dest)
                    frames, _ = recv_all_frames(target, b'', idle_ms=400)
                    assert any(msg == 0x2b for msg, _ in frames)

                    packet = bytearray(68)
                    struct.pack_into('<BHHBI', packet, 0, 0x1e, victim_id, victim_car, 1, 1000)
                    struct.pack_into('<f', packet, 10, 123.0)
                    other.sendto(packet, dest)
                    match = lambda p: p[:1] in (b'\x1e', b'\x39') and struct.pack('<f', 123.0) in p
                    assert not recv_packet(observer, match, 0.8)
                    owner.sendto(packet, dest)
                    assert recv_packet(observer, match, 2)
                    return True
            finally:
                for sock in sockets:
                    sock.close()
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    proc.wait(timeout=3)


if __name__ == '__main__':
    assert run_case(False), 'default should retain legacy rebinding behavior'
    assert run_case(True), 'strict peer pinning checks failed'
    print('UDP peer pinning: default and protected paths OK')
