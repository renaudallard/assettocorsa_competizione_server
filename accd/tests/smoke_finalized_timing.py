"""Qualifying aftercare must not allow late timing to diverge from the grid.

Spawns a private local server with synthetic clients and temporary data.
Run: python3 tests/smoke_finalized_timing.py
"""
import json
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fake_client as client

channels = []
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
last_keepalive = 0


class Channel:
    def __init__(self, index):
        self.sock = socket.create_connection(('127.0.0.1', 19232), timeout=3)
        self.buf = b''
        self.events = []
        self.info = None
        channels.append(self)
        body = bytes([9]) + struct.pack('<H', 0x100) + client.wstr_a('')
        body += client.build_driver_info(first='Synthetic', last=str(index),
            steam='S' + str(76561199010000000 + index))
        body += bytes(8) + client.build_car_info(race_number=100+index)
        self.send(body)

    def send(self, body):
        self.sock.sendall(struct.pack('<H', len(body)) + body)


def pump(seconds=.05):
    global last_keepalive
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        now = time.monotonic()
        if now - last_keepalive > 1:
            for c in channels:
                if c.info:
                    udp.sendto(struct.pack('<BH', 0x13, c.info['conn_id']), ('127.0.0.1', 19231))
            last_keepalive = now
        ready, _, _ = select.select([c.sock for c in channels], [], [], min(.1, max(0, end-now)))
        for c in channels:
            if c.sock not in ready:
                continue
            chunk = c.sock.recv(65536)
            if not chunk:
                raise AssertionError('Synthetic driver disconnected')
            c.buf += chunk
            while len(c.buf) >= 2:
                n = struct.unpack_from('<H', c.buf)[0]
                off = 2
                if n == 65535:
                    if len(c.buf) < 6:
                        break
                    n = struct.unpack_from('<I', c.buf, 2)[0]
                    off = 6
                if len(c.buf) < n+off:
                    break
                packet, c.buf = c.buf[off:off+n], c.buf[off+n:]
                c.events.append((time.monotonic(), packet))
                if packet[0] == 0x0b:
                    c.info = client.parse_welcome(packet)


def until(check, seconds=5):
    end = time.monotonic()+seconds
    while time.monotonic() < end:
        if check():
            return
        pump(.1)
    assert check(), 'Timed out waiting for isolated protocol event'


def leaderboard(packet):
    r = client.Reader(packet[1:])
    r.u32()
    for _ in range(r.u8()):
        r.u32()
    cvar = r.u8()
    rows = []
    for _ in range(r.u16()):
        start = r.pos
        car_id, number = r.u16(), r.u16()
        r.pos = start
        client.parse_leaderboard_record(r, cvar)
        rows.append(dict(carId=car_id, number=number))
    return rows


def submit(c, ms):
    for sector, duration in enumerate((30000, 30000)):
        c.send(struct.pack('<BiBiH', 0x20, duration, sector, 100000, 0))
    c.send(struct.pack('<BiiBHB', 0x21, ms, 100000, 0, 0, 0))


with tempfile.TemporaryDirectory(prefix='accd-observer-probe-') as temp:
    root = Path(temp)
    cfg = root/'cfg'
    cfg.mkdir()
    (root/'results').mkdir()
    config = {
        'configuration.json': dict(tcpPort=19232, udpPort=19231, maxConnections=4,
            lanDiscovery=0, registerToLobby=0),
        'settings.json': dict(serverName='Isolated observation probe', password='',
            adminPassword='', spectatorPassword='', maxCarSlots=4, dumpLeaderboards=1),
        'event.json': dict(track='indianapolis', preRaceWaitingTimeSeconds=30,
            sessionOverTimeSeconds=4, postQualySeconds=8, postRaceSeconds=5,
            ambientTemp=22, cloudLevel=.1, rain=0, weatherRandomness=0,
            sessions=[dict(sessionType=t, hourOfDay=14, dayOfWeekend=2,
                           timeMultiplier=1, sessionDurationMinutes=1) for t in ('Q','R')]),
        'ratings.json': []}
    for name, data in config.items():
        (cfg/name).write_text(json.dumps(data))
    binary = root/'accd'
    shutil.copyfile(Path(__file__).resolve().parents[1] / 'accd', binary)
    binary.chmod(0o755)
    with (root / 'server.log').open('w') as log:
        proc = subprocess.Popen([str(binary), '-c', str(cfg)], cwd=root, stdout=log, stderr=log)
        try:
            end = time.monotonic()+5
            while True:
                startup = (root / 'server.log').read_text(errors='replace')
                if proc.poll() is not None:
                    raise AssertionError('Private test server exited during startup:\n' + startup)
                # Do not connect to a different process if these test ports
                # are already occupied. Only our own log proves successful bind.
                if 'listening: tcp/19232 udp/19231' in startup:
                    break
                if time.monotonic()>end:
                    raise AssertionError('Private test server did not start:\n' + startup)
                time.sleep(.05)
            started = time.monotonic()
            a, b = Channel(1), Channel(2)
            until(lambda: a.info and b.info)
            pump(4)
            submit(a,100000)
            submit(b,101000)
            pump(.5)
            before = next(leaderboard(pkt) for _, pkt in reversed(a.events) if pkt[0]==0x36)
            print('Initial qualifying order:', before, flush=True)
            until(lambda:any(pkt[0]==0x3e for _,pkt in a.events), seconds=70)
            finalized = time.monotonic()
            print('Qualifying results broadcast after %.2fs' % (finalized-started), flush=True)
            a.events.clear()
            submit(b,90000)
            pump(.5)
            late = next((leaderboard(pkt) for _,pkt in reversed(a.events) if pkt[0]==0x36), before)
            print('Live order after late lap:', late, flush=True)
            until(lambda:any(pkt[0]==0x3f for _,pkt in a.events), seconds=15)
            packet = next(pkt for _,pkt in reversed(a.events) if pkt[0]==0x3f)
            grid=[]
            for i in range(packet[1]):
                car_id, di, best, flag = struct.unpack_from('<HBIB', packet, 2+i*8)
                grid.append(dict(carId=car_id, bestLap=best))
            result = dict(
                initialOrder=before, lateLiveOrder=late, grid=grid,
                gridDelayAfterResultsSeconds=round(time.monotonic()-finalized, 2),
                orderMismatch=[r['carId'] for r in late] != [r['carId'] for r in grid],
                scope='Synthetic TCP clients; real ACC client rendering not tested.')
            assert late == before, 'Late lap changed the finalized live leaderboard'
            assert not result['orderMismatch'], 'Race grid differs from final live order'
            assert [r['bestLap'] for r in grid] == [100000, 101000], 'Grid times changed'
            print('PASS: finalized qualifying order and subsequent grid agree', flush=True)
            print(json.dumps(result, indent=2), flush=True)
        finally:
            for c in channels:
                c.sock.close()
            udp.close()
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
