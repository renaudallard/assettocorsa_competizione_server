#!/usr/bin/env python3
"""
Fake ACC client: connects to a running accd, sends a realistic 0x09
handshake, receives the 0x0b welcome, and walks the response the same
way AC2-Win64-Shipping.exe does — logging the cursor position at each
of the 11 log-anchor points the real client emits.

The goal is to catch byte-count regressions in the welcome trailer
without needing the Windows client.  Any section whose internal
advancement does not match the labels the real client reads becomes a
targeted FAIL with the exact offset, making bisection trivial.

Usage:
    python3 fake_client.py                     # spawn accd on a tmp cfg
    python3 fake_client.py --host 1.2.3.4:9232 # connect to a live server

The reader primitives mirror the AC2 client:
  read_u8    → 0x140fa3020
  read_u16   → 0x141092da0
  read_u32   → 0x141092e20
  read_f32   → 0x141092ea0
  read_ksstr → 0x143460680  (u16 len + UTF-8)
  read_fmtA  → 0x1434608c0  (u8 count + count*u32 codepoints)
The section boundaries come from the 11 "ACP_SERVER_RESPONSE.*" log
strings and the EventEntity "post *" anchors in the client binary.
"""

import argparse
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


ACCD_PROTOCOL_VERSION = 0x0100
DEFAULT_TCP_PORT = 19231
DEFAULT_UDP_PORT = 19232
CONNECT_TIMEOUT = 5.0


class Reader:
    """Byte-accurate cursor with read primitives matching the client."""

    def __init__(self, buf):
        self.buf = buf
        self.pos = 0
        self.size = len(buf)

    def _check(self, n, what):
        if self.pos + n > self.size:
            raise ParseError(
                f"{what}: out of range at cursor={self.pos} "
                f"(need {n} B, have {self.size - self.pos})")

    def u8(self):
        self._check(1, "u8")
        v = self.buf[self.pos]
        self.pos += 1
        return v

    def u16(self):
        self._check(2, "u16")
        v = struct.unpack_from("<H", self.buf, self.pos)[0]
        self.pos += 2
        return v

    def i16(self):
        self._check(2, "i16")
        v = struct.unpack_from("<h", self.buf, self.pos)[0]
        self.pos += 2
        return v

    def u32(self):
        self._check(4, "u32")
        v = struct.unpack_from("<I", self.buf, self.pos)[0]
        self.pos += 4
        return v

    def u64(self):
        self._check(8, "u64")
        v = struct.unpack_from("<Q", self.buf, self.pos)[0]
        self.pos += 8
        return v

    def f32(self):
        self._check(4, "f32")
        v = struct.unpack_from("<f", self.buf, self.pos)[0]
        self.pos += 4
        return v

    def ksstr(self):
        """kson_string: u16 length + N bytes UTF-8."""
        n = self.u16()
        self._check(n, f"kson_string({n})")
        s = self.buf[self.pos:self.pos + n].decode("utf-8", errors="replace")
        self.pos += n
        return s

    def fmt_a(self):
        """Format-A wstring: u8 count + N * u32 codepoints."""
        n = self.u8()
        self._check(n * 4, f"fmt_a({n})")
        cps = [struct.unpack_from("<I", self.buf, self.pos + i * 4)[0]
               for i in range(n)]
        self.pos += n * 4
        try:
            return "".join(chr(c) for c in cps)
        except (ValueError, OverflowError):
            return "<non-unicode>"


class ParseError(Exception):
    pass


def wstr_a(s):
    """Format-A wstring: u8 count + N * u32 codepoints."""
    out = bytes([len(s)])
    for ch in s:
        out += struct.pack("<I", ord(ch))
    return out


def ks_str(s):
    """kson_string: u16 length + N * UTF-8 bytes."""
    enc = s.encode("utf-8")
    return struct.pack("<H", len(enc)) + enc


def build_driver_info(first="Smoke", last="Test", short="SMK",
                     steam="S76561199000000000", category=1, nationality=0):
    """Build DriverInfo matching FUN_14011cea0 (the ACC client's
    DriverInfo::writeToPacket): 5 fmt_a + 41 fixed bytes + 1 fmt_a.
    Field layout of the 41-byte block:
      u8 category  u16 nationality  u8  3*u32  u8  6*u32
    The server's real-path handshake parser reads cat at byte 16
    of this block instead of byte 0 — a long-standing server bug
    that we tolerate (hs_echo preserves the raw bytes for the
    client's reader which uses the correct offsets)."""
    buf = wstr_a(first)
    buf += wstr_a("")                 # aux slot
    buf += wstr_a(last)
    buf += wstr_a("")                 # aux slot
    buf += wstr_a(short)
    # 41-byte numeric block, FUN_14011cea0 layout
    buf += bytes([category])          # +0x00 u8 cat
    buf += struct.pack("<H", nationality)  # +0x01 u16 nat
    buf += bytes([0])                 # +0x03 u8
    buf += struct.pack("<III", 0x1f7, 0x11, 0xf3)  # +0x04 3*u32
    buf += bytes([0])                 # +0x10 u8
    buf += struct.pack("<II", 0, 0)   # +0x11 2*u32
    buf += struct.pack("<IIII", 200, 0x1f8, 0xf3, 0x155)  # +0x19 4*u32
    assert len(buf) - (21 + 1 + 17 + 1 + 13) == 41, \
        "DriverInfo block must be exactly 41 bytes"
    buf += wstr_a(steam)
    return buf


def build_car_info(car_model=35, cup=0, race_number=99):
    """Build CarInfo matching FUN_14011c7c0 (the real ACC client's
    CarInfo::writeToPacket): 45 fixed bytes (3 u32 + 2 u8 + u32 + u8 +
    3 u32 + 4 u8 + 2 u32 + 2 u8), fmt_a (custom skin name), u8, fmt_a
    (team name), u16 nat, 2 × fmt_a (display/competitor), u16 (nat
    again?), 7 u8 trailing."""
    buf = struct.pack("<III", 0, 0, car_model)  # +0x28, +0x2c, +0x30 (car_model)
    buf += bytes([0, 0])              # +0x5e, +0x34
    buf += struct.pack("<I", 0)       # +0x38
    buf += bytes([cup])               # +0x3c
    buf += struct.pack("<III", 0, 0, 0)  # +0x40, +0x44, +0x48
    buf += bytes([0, 0, 0, 0])        # +0x50, +0x4c, +0x4d, +0x4e
    buf += struct.pack("<I", race_number)  # +0x54
    buf += struct.pack("<I", 0)       # +0x58
    buf += bytes([0, 0])              # +0x5c, +0x5d
    buf += wstr_a("")                 # +0xd0 customSkinName
    buf += bytes([0])                 # +0xf3 bannerKey
    buf += wstr_a("Fake Team")        # +0x60 teamName
    buf += struct.pack("<H", 0)       # +0x80 nat
    buf += wstr_a("")                 # +0x88 displayName
    buf += wstr_a("")                 # +0xa8 competitorName
    buf += struct.pack("<H", 0)       # +0xc8
    buf += bytes([0])                 # +0xca
    buf += bytes([car_model])         # +0xf0 carModelType
    buf += bytes([cup])               # +0xf1
    buf += bytes([0])                 # +0xf2
    buf += bytes([0, 0, 0])           # +0xf4, +0xf5, +0xf6 bools
    return buf


def build_handshake():
    """Assemble a real-format ACP_REQUEST_CONNECTION so the server
    routes it through the >200-byte real-path parser and echoes
    proper DriverInfo + CarInfo bytes in the welcome trailer."""
    body = bytes([0x09])
    body += struct.pack("<H", ACCD_PROTOCOL_VERSION)
    body += wstr_a("")                # password (empty)
    body += build_driver_info()
    body += bytes(8)                  # separator between DriverInfo and CarInfo
    body += build_car_info()
    # Pad body past 200 bytes to trigger the real-format parse path
    # on the server if the core payload underruns (doesn't on current
    # builds, but guards against shrinking DriverInfo/CarInfo fields).
    while len(body) <= 200:
        body += bytes([0])
    return struct.pack("<H", len(body)) + body


def recv_framed(sock):
    sock.settimeout(CONNECT_TIMEOUT)
    buf = b""
    while len(buf) < 2:
        chunk = sock.recv(8192)
        if not chunk:
            raise ConnectionError("peer closed during length read")
        buf += chunk
    ln = buf[0] | (buf[1] << 8)
    while len(buf) < 2 + ln:
        chunk = sock.recv(8192)
        if not chunk:
            raise ConnectionError("peer closed during body read")
        buf += chunk
    return buf[2:2 + ln]


# ------------------------------------------------------------------
# Welcome trailer parsing (mirrors AC2-Win64-Shipping.exe welcome_parser
# @ 0x14352a150 and its sub-readers).
# ------------------------------------------------------------------

ANCHORS = []


def anchor(r, label):
    ANCHORS.append((r.pos, label))


def parse_car_info(r):
    """CarInfo::readFromPacket — FUN_1401177e0."""
    r.u32()                           # +0x28
    r.u32()                           # +0x2c
    r.u32()                           # +0x30 (car_model u32, logged as "#%d")
    r.u8()                            # +0x5e
    r.u8()                            # +0x34
    r.u32()                           # +0x38
    r.u8()                            # +0x3c
    r.u32()                           # +0x40
    r.u32()                           # +0x44
    r.u32()                           # +0x48
    r.u8()                            # +0x50
    r.u8()                            # +0x4c
    r.u8()                            # +0x4d
    r.u8()                            # +0x4e
    r.u32()                           # +0x54
    r.u32()                           # +0x58
    r.u8()                            # +0x5c
    r.u8()                            # +0x5d
    r.fmt_a()                         # Format-A → +0xd0
    r.u8()                            # +0xf3
    r.fmt_a()                         # Format-A → +0x60
    r.u16()                           # +0x80
    r.fmt_a()                         # Format-A → +0x88
    r.fmt_a()                         # Format-A → +0xa8
    r.u16()                           # +0xc8
    r.u8()                            # +0xca
    r.u8()                            # +0xf0
    r.u8()                            # +0xf1
    r.u8()                            # +0xf2
    r.u8()                            # +0xf4 (bool)
    r.u8()                            # +0xf5 (bool)
    r.u8()                            # +0xf6 (bool)


def parse_driver_info(r):
    """DriverInfo::readFromPacket — FUN_1401180c0."""
    r.fmt_a()                         # first_name
    r.fmt_a()                         # last_name
    r.fmt_a()                         # short_name
    r.fmt_a()                         # slot 3 (unknown)
    r.fmt_a()                         # slot 4 (unknown)
    r.u8()                            # +0xc8 (driver_category)
    r.u16()                           # +0xca (nationality)
    r.u8()                            # +0xcc
    r.u32(); r.u32(); r.u32()         # +0xd0/+0xd4/+0xd8
    r.u8()                            # +0xdc
    r.u32(); r.u32()                  # +0xe0/+0xe4
    r.u32(); r.u32(); r.u32(); r.u32()  # +0xe8/+0xec/+0xf0/+0xf4
    r.fmt_a()                         # steam_id


def parse_spawn_def(r):
    """spawnDef reader (FUN_143528e80)."""
    r.u16()                           # car_id
    r.u8()                            # flag1 (grid+1)
    r.u8()                            # flag2 (grid+1)
    parse_car_info(r)
    dc = r.u8()                       # driver_count
    for _ in range(dc):
        parse_driver_info(r)
    r.u8()                            # active_driver_idx
    r.u64()                           # timestamp
    r.u8()                            # flag (+0x153)
    r.u8()                            # flag (+0x152)
    for _ in range(5):
        r.u8()                        # dirt
    for _ in range(5):
        r.u8()                        # damage
    r.u16()                           # elo
    r.u32()                           # stability


def parse_season_entity(r):
    """SeasonEntity::readFromPacket — 104 bytes of HUD/Assist/Graphics/
    Realism/Gameplay/Online/RaceDirector rules + 5 × u16 counts."""
    # HudRules: 7 u8
    for _ in range(7):
        r.u8()
    # AssistRules: 2 u8 + 2 f32 + 6 u8
    r.u8(); r.u8()
    r.f32(); r.f32()
    for _ in range(6):
        r.u8()
    # GraphicsRules: 6 u8
    for _ in range(6):
        r.u8()
    # RealismRules: 2 u8 + f32 + u8 + 2 f32 + 9 u8
    r.u8(); r.u8()
    r.f32()
    r.u8()
    r.f32(); r.f32()
    for _ in range(9):
        r.u8()
    # GameplayRules: 5 u8 + u32
    for _ in range(5):
        r.u8()
    r.u32()
    # OnlineRules: 4 u8 + u16 + u8 + u8 + 6 u8
    for _ in range(4):
        r.u8()
    r.u16()
    r.u8(); r.u8()
    for _ in range(6):
        r.u8()
    # RaceDirectorRules: 5 u8 + 3 u32 + u8
    for _ in range(5):
        r.u8()
    r.u32(); r.u32(); r.u32()
    r.u8()
    # Five u16 vector counts (4 empty + 1 event count).  The last one
    # tells us how many EventEntity records follow.
    for _ in range(4):
        r.u16()
    return r.u16()                    # event_count


def parse_event_entity(r):
    """EventEntity: trackName + CircuitInfo + GraphicsInfo + RaceRules +
    WeatherRules.  No CarSet sub-block in the current welcome layout."""
    r.fmt_a()                         # trackName (Format-A wstring)
    anchor(r, "EventEntity post trackName")
    # CircuitInfo: 3 u8 + 4 f32 = 19 bytes
    r.u8(); r.u8(); r.u8()
    r.f32(); r.f32(); r.f32(); r.f32()
    anchor(r, "EventEntity post circuit")
    # GraphicsInfo: 6 u8 + u16 + u8 = 9 bytes
    for _ in range(6):
        r.u8()
    r.u16()
    r.u8()
    anchor(r, "EventEntity post graphics")
    # (no CarSet — v0.2.46 layout)
    # RaceRules: 16 bytes
    for _ in range(16):
        r.u8()
    anchor(r, "EventEntity post race")
    # WeatherRules header: 4 u8 + 7 f32 = 32 bytes
    for _ in range(4):
        r.u8()
    for _ in range(7):
        r.f32()
    # WeatherRules forecast: 15 f32 = 60 bytes
    for _ in range(15):
        r.f32()
    anchor(r, "EventEntity post weatherData")


def parse_session_mgr_state(r):
    """session_mgr_state — FUN_140033890.
    u8 session_index + 7 × (u8 valid + optional f32) + 23-byte tail."""
    r.u8()                            # session_index
    for _ in range(7):
        valid = r.u8()
        if valid:
            r.f32()
    # Tail (FUN_140034f60): u8 + u8 + u8 + f32 + u16 + u32 + u32 + u8 + u8 + f32
    r.u8(); r.u8(); r.u8()
    r.f32()
    r.u16()
    r.u32(); r.u32()
    r.u8(); r.u8()
    r.f32()


def parse_leaderboard_section(r):
    """FUN_140034a40: u32 + u8 cnt + cnt*u32 + u8 cvar8 + u16 entry_count +
    per-entry record + 2 u8 tail."""
    r.u32()                           # 0x7fffffff
    n_ints = r.u8()
    for _ in range(n_ints):
        r.u32()
    cvar8 = r.u8()
    entries = r.u16()
    for _ in range(entries):
        parse_leaderboard_record(r, cvar8)
    r.u8(); r.u8()                    # tail


def parse_leaderboard_record(r, cvar8):
    """FUN_14352ae00 per-car reader."""
    r.u16()                           # car_id
    r.u16()                           # race_number
    r.u8()                            # cup_category
    r.u8()                            # current_driver_index
    r.u16()                           # 0
    if r.u8():                        # penalty flag
        r.u16()                       # penalty code
        r.f32()                       # laps remaining
    if cvar8:
        r.u8()                        # formation_lap_done
    n_pen = r.u8()
    for _ in range(n_pen):
        r.u32()                       # penalty entry
    dc = r.u8()
    for _ in range(dc):
        r.fmt_a()                     # steam_id
        r.fmt_a()                     # short_name
        r.fmt_a()                     # first_name
        r.fmt_a()                     # last_name
        r.u8()                        # driver_category
        r.u16()                       # nationality
    # Six per-car fields after driver list
    r.u16()
    r.u32()                           # best_lap_ms
    r.u32()                           # last_sys_data
    r.u16()                           # lap_count
    r.u32()                           # race_time_ms
    r.u8()                            # last_elo
    # Sectors: u8 wide_flag + u8 l1_n + l1_n*(u32/u16) + u8 l2_n + l2_n*(u32/u16)
    wide = r.u8()
    l1 = r.u8()
    for _ in range(l1):
        r.u32() if wide else r.u16()
    l2 = r.u8()
    for _ in range(l2):
        r.u32() if wide else r.u16()
    r.u8(); r.u8()                    # tail


def parse_weather_data(r):
    """WeatherData (FUN_14011e660) — 12 u32/f32 + i16 sine_n + N*f32 +
    i16 cos_n + N*f32."""
    for _ in range(12):
        r.u32()
    ns = r.i16()
    for _ in range(ns):
        r.f32()
    nc = r.i16()
    for _ in range(nc):
        r.f32()


def parse_track_conditions_update(r):
    """FUN_14352cb30: 7 × f32 + vtable call (WeatherData-like object, we
    approximate with our server's 17-f32 block) + 1 f32.  Our server
    emits write_trailer_additional_state as 17 × f32 straight."""
    for _ in range(17):
        r.f32()


def parse_track_records(r):
    """u8 count + N × 23-byte entry (3 u8 + f32 + u16 + 2 u32 + 2 u8 + f32)."""
    n = r.u8()
    for _ in range(n):
        r.u8(); r.u8(); r.u8()
        r.f32()
        r.u16()
        r.u32(); r.u32()
        r.u8(); r.u8()
        r.f32()


def parse_mtr(r):
    """MTR (FUN_14011da70): 3 kson_str + u8 + u8 + u32 + u8 cnt + N*u32 +
    u8 + u8 + f32.  Note: 0x1434f5339 reads f32 at +0xb8, not u32."""
    r.ksstr(); r.ksstr(); r.ksstr()
    r.u8()
    r.u8()
    r.u32()
    n = r.u8()
    for _ in range(n):
        r.u32()
    r.u8()
    r.u8()
    r.f32()


def parse_ccr(r):
    """CCR (FUN_14011d9a0): 2 kson_str + u32 count + N × 21-byte RatingLine
    (3 kson_str + u8 + f32 + 3 u16 + u32)."""
    r.ksstr(); r.ksstr()
    n = r.u32()
    for _ in range(n):
        r.ksstr(); r.ksstr(); r.ksstr()
        r.u8()
        r.f32()
        r.u16(); r.u16(); r.u16()
        r.u32()


def parse_welcome(body):
    """Walk the 0x0b body exactly like the AC2 welcome_parser does.
    Raises ParseError on any structural mismatch."""
    ANCHORS.clear()
    r = Reader(body)

    # Header (before welcome_parser proper).
    msg_id = r.u8()
    if msg_id != 0x0b:
        raise ParseError(f"msg_id = 0x{msg_id:02x}, expected 0x0b")
    udp_port = r.u16()
    pad = r.u8()
    if pad != 0x12:
        raise ParseError(f"header pad = 0x{pad:02x}, expected 0x12")
    conn_id = r.u16()
    if conn_id == 0xFFFF:
        raise ParseError("server rejected handshake (conn_id=0xFFFF)")

    # welcome_parser @ 0x14352a150
    car_index = r.u32()
    server_name = r.ksstr()
    track = r.ksstr()
    anchor(r, "pre carSpawnInfo")

    n_spawns = r.u8()
    for _ in range(n_spawns):
        parse_spawn_def(r)
    anchor(r, "post carSpawnInfo")

    n_events = parse_season_entity(r)
    anchor(r, "SeasonEntity (5-vec counts done)")
    for _ in range(n_events):
        parse_event_entity(r)
    anchor(r, "post seasonEntity")

    parse_session_mgr_state(r)
    anchor(r, "post session")

    parse_leaderboard_section(r)
    anchor(r, "post leaderboard")

    parse_weather_data(r)
    anchor(r, "post weatherData")

    parse_track_conditions_update(r)
    anchor(r, "post readTrackConditionsUpdate")

    parse_track_records(r)
    r.u8(); r.u8()                    # 2 tyre bytes (dirt freq + delta threshold)
    anchor(r, "post deltaThreshold (= track_records + 2 u8)")

    parse_mtr(r)
    anchor(r, "post trackRecords (= MTR)")

    parse_ccr(r)
    anchor(r, "post cppResults (= CCR)")

    r.u8(); r.u8(); r.u8()
    anchor(r, "post formationlap (final 3 u8)")

    return {
        "total": r.size,
        "consumed": r.pos,
        "leftover": r.size - r.pos,
        "car_index": car_index,
        "server_name": server_name,
        "track": track,
        "udp_port": udp_port,
        "conn_id": conn_id,
    }


def write_cfg(tmp, tcp, udp):
    with open(os.path.join(tmp, "configuration.json"), "w") as f:
        f.write(f'{{"tcpPort": {tcp}, "udpPort": {udp}, '
                f'"maxConnections": 4, "lanDiscovery": 0}}\n')
    with open(os.path.join(tmp, "settings.json"), "w") as f:
        f.write('{"serverName": "fake", "password": "", '
                '"adminPassword": "", "spectatorPassword": ""}\n')
    with open(os.path.join(tmp, "event.json"), "w") as f:
        f.write('{"track": "misano", "ambientTemp": 22, '
                '"cloudLevel": 0.1, "rain": 0.0, "weatherRandomness": 0, '
                '"sessions": [{"sessionType": "P", "hourOfDay": 12, '
                '"dayOfWeekend": 1, "timeMultiplier": 1, '
                '"sessionDurationMinutes": 10}]}\n')
    with open(os.path.join(tmp, "ratings.json"), "w") as f:
        f.write("[]\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", help="host:port of a live accd")
    ap.add_argument("--accd", help="path to accd binary (spawns a test "
                    "instance if --host is not given)")
    args = ap.parse_args()

    proc = None
    if args.host:
        host, _, port = args.host.partition(":")
        port = int(port or DEFAULT_TCP_PORT)
    else:
        accd = args.accd or os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "accd")
        if not os.access(accd, os.X_OK):
            print(f"accd not found: {accd}", file=sys.stderr)
            return 1
        tmp = tempfile.mkdtemp(prefix="fake-client-")
        write_cfg(tmp, DEFAULT_TCP_PORT, DEFAULT_UDP_PORT)
        proc = subprocess.Popen(
            [accd, tmp], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        host, port = "127.0.0.1", DEFAULT_TCP_PORT
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            try:
                with socket.create_connection((host, port), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            if proc:
                os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            print(f"accd didn't come up on {port}", file=sys.stderr)
            return 1

    try:
        sock = socket.socket()
        sock.connect((host, port))
        sock.sendall(build_handshake())
        body = recv_framed(sock)
        sock.close()

        print(f"Received 0x0b welcome: {len(body)} B")
        try:
            info = parse_welcome(body)
        except ParseError as e:
            print(f"PARSE FAILED at cursor={[a[0] for a in ANCHORS][-1] if ANCHORS else 0}")
            print(f"  error: {e}")
            print("Anchors reached:")
            for pos, label in ANCHORS:
                print(f"  {pos:5d}  {label}")
            return 2

        print(f"car_id = 0x{info['car_index']:04x}  "
              f"server = {info['server_name']!r}  track = {info['track']!r}")
        print("Section cursor positions:")
        for pos, label in ANCHORS:
            print(f"  {pos:5d}  {label}")
        print(f"Consumed {info['consumed']} of {info['total']} B "
              f"(leftover {info['leftover']})")
        if info["leftover"] != 0:
            print("WARN: unparsed trailing bytes — section layout "
                  "diverged from exe or our server emits extra bytes")
            return 3
        print("PASS: welcome parses cleanly to EOF")
        return 0
    finally:
        if proc is not None:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
                proc.wait(timeout=2.0)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass


if __name__ == "__main__":
    sys.exit(main())
