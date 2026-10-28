#!/usr/bin/env python3
"""Check optional idle handshake timeout on a disposable server."""
import json
import os
import signal
import socket
import struct
import subprocess
import tempfile
import time

from fake_client import build_handshake, recv_framed, write_cfg, wstr_a


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def run_case(timeout, expect_reaped):
    tcp, udp = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM)
    with tempfile.TemporaryDirectory(prefix='accd-handshake-timeout-') as tmp:
        write_cfg(tmp, tcp, udp)
        path = os.path.join(tmp, 'configuration.json')
        with open(path, encoding='utf-8') as f:
            config = json.load(f)
        if timeout is not None:
            config['unauthHandshakeTimeoutMs'] = timeout
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(config, f)
        log_path = os.path.join(tmp, 'server.log')
        with open(log_path, 'wb') as log:
            proc = subprocess.Popen([os.path.abspath('accd'), tmp], cwd=tmp,
                                    stdout=log, stderr=subprocess.STDOUT,
                                    start_new_session=True)
            sockets = []
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        client = socket.create_connection(('127.0.0.1', tcp), timeout=0.2)
                        break
                    except OSError:
                        if proc.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError('server did not start')
                        time.sleep(0.05)
                sockets.append(client)
                client.sendall(build_handshake())
                welcome, _ = recv_framed(client)
                assert welcome[:1] == b'\x0b'
                for _ in range(3):
                    sockets.append(socket.create_connection(('127.0.0.1', tcp), timeout=2))
                time.sleep(1.6)
                candidate = socket.create_connection(('127.0.0.1', tcp), timeout=2)
                sockets.append(candidate)
                if not expect_reaped:
                    msg, _ = recv_framed(candidate)
                    assert msg[:1] == b'\x0c', 'default should keep 30-second timeout'
                else:
                    body = build_handshake()[2:]
                    original = wstr_a('S76561199000000000')
                    assert body.count(original) == 1
                    body = body.replace(original, wstr_a('LOCAL-TEST-TWO'), 1)
                    candidate.sendall(struct.pack('<H', len(body)) + body)
                    msg, _ = recv_framed(candidate)
                    assert msg[:1] == b'\x0b', 'idle slots should be released'
                    marker = 'TIMEOUT-TEST-AUTHENTICATED'
                    payload = b'\x2a' + wstr_a(marker) + struct.pack('<i', 0)
                    client.sendall(struct.pack('<H', len(payload)) + payload)
                    deadline = time.monotonic() + 2
                    while time.monotonic() < deadline:
                        with open(log_path, encoding='utf-8', errors='replace') as f:
                            if marker in f.read():
                                break
                        time.sleep(0.05)
                    else:
                        raise AssertionError('authenticated connection should stay alive')
            finally:
                for sock in sockets:
                    sock.close()
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    proc.wait(timeout=3)


if __name__ == '__main__':
    run_case(None, False)
    run_case(0, False)
    run_case(1000, True)
    print('handshake timeout: default, invalid setting, idle reaping and authenticated connection OK')
