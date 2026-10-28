#!/usr/bin/env python3
"""Check the optional flying-lap time floor on a disposable server."""
import glob
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


def frame(sock, payload):
    sock.sendall(struct.pack('<H', len(payload)) + payload)


def run_case(floor, lap_time, flags):
    tcp = free_port(socket.SOCK_STREAM)
    udp = free_port(socket.SOCK_DGRAM)
    with tempfile.TemporaryDirectory(prefix='accd-lap-floor-') as tmp:
        write_cfg(tmp, tcp, udp)
        path = os.path.join(tmp, 'settings.json')
        with open(path, encoding='utf-8') as f:
            settings = json.load(f)
        settings['adminPassword'] = 'test-only'
        settings['dumpLeaderboards'] = 1
        if floor is not None:
            settings['minFlyingLapTimeMs'] = floor
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(settings, f)
        with open(os.path.join(tmp, 'server.log'), 'wb') as log:
            proc = subprocess.Popen([os.path.abspath('accd'), tmp], cwd=tmp,
                                    stdout=log, stderr=subprocess.STDOUT,
                                    start_new_session=True)
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
                with client:
                    client.sendall(build_handshake())
                    welcome, _ = recv_framed(client)
                    if welcome[:1] != b'\x0b':
                        raise RuntimeError('handshake rejected')
                    time.sleep(3.3)
                    frame(client, struct.pack('<BiiBHB', 0x21, lap_time, 2000, 0, flags, 0))
                    for command in ('/admin test-only', '/next'):
                        frame(client, b'\x2a' + wstr_a(command) + struct.pack('<i', 0))
                        time.sleep(0.1)
                    deadline = time.monotonic() + 6
                    while time.monotonic() < deadline:
                        paths = glob.glob(os.path.join(tmp, 'results', '*.json'))
                        if paths:
                            with open(paths[0], encoding='utf-8') as f:
                                return json.load(f).get('laps', [])
                        time.sleep(0.05)
                    raise RuntimeError('session result not written')
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    proc.wait(timeout=3)


def main():
    default = run_case(None, 1, 0)
    rejected = run_case(30000, 1, 0)
    accepted = run_case(30000, 30000, 0)
    in_lap = run_case(30000, 1, 0x08)
    assert len(default) == 1 and default[0]['isValidForBest']
    assert rejected == []
    assert len(accepted) == 1 and accepted[0]['isValidForBest']
    assert len(in_lap) == 1 and not in_lap[0]['isValidForBest']
    print('optional floor: default, rejection, boundary and in-lap OK')


if __name__ == '__main__':
    main()
