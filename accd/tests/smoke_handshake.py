#!/usr/bin/env python3
"""
Smoke test: send a synthetic ACP_REQUEST_CONNECTION to accd and verify the
welcome trailer comes back intact.

The test catches two classes of regression:

 1. Handshake outright rejected (0x0b with conn_id=0xFFFF, or a 0x0c
    response).  Password or version mismatch, bans, slot exhaustion.
 2. Trailer layout drift.  If a write_* block inside the 0x0b body
    changes size, every subsequent field shifts and the ACC client
    parser aborts with "failed with value 13".  The shape checks below
    (length range + final 3-byte sentinel 03 00 00 + "Standard" tail
    marker at the expected offset from EOF) flag this before it ships.

Invoked from `make test` after the build.  Exits 0 on success, non-zero
on any failure.  Prints a one-line PASS / FAIL summary.
"""

import os
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


ACCD_PROTOCOL_VERSION = 0x0100
TCP_PORT = 19231   # non-default so we don't collide with a running server
UDP_PORT = 19232
CONNECT_TIMEOUT = 5.0


def wstr_a(s):
    """Encode a string as Format-A: u8 codepoint count + u32 codepoints."""
    out = bytes([len(s)])
    for ch in s:
        out += struct.pack("<I", ord(ch))
    return out


def build_handshake():
    """
    Build a minimal-but-valid ACP_REQUEST_CONNECTION body using the
    simple (<200 byte) format that handshake_handle recognizes:
      u8 0x09 + u16 ver + str_a password +
      str_a first + str_a last + str_a short +
      u8 cat + u16 nat + str_a steam_id +
      i32 race_number + u8 car_model + u8 cup + str_a team
    """
    body = bytes([0x09])
    body += struct.pack("<H", ACCD_PROTOCOL_VERSION)
    body += wstr_a("")                      # password (empty)
    body += wstr_a("Smoke")                 # first
    body += wstr_a("Test")                  # last
    body += wstr_a("SMK")                   # short (3 chars, exe expects 3)
    body += bytes([0])                      # category (Bronze)
    body += struct.pack("<H", 0)            # nationality
    body += wstr_a("S76561199000000000")    # steam_id (18 chars, synthetic)
    body += struct.pack("<i", 99)           # race_number
    body += bytes([35])                     # car_model (same as captured)
    body += bytes([0])                      # cup_category
    body += wstr_a("Smoke Team")            # team
    return struct.pack("<H", len(body)) + body


def recv_framed(sock, timeout=CONNECT_TIMEOUT):
    sock.settimeout(timeout)
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


def write_cfg(tmp):
    with open(os.path.join(tmp, "configuration.json"), "w") as f:
        f.write('{"tcpPort": %d, "udpPort": %d, "maxConnections": 4, '
                '"lanDiscovery": 0}\n' % (TCP_PORT, UDP_PORT))
    with open(os.path.join(tmp, "settings.json"), "w") as f:
        f.write('{"serverName": "smoke", "password": "", '
                '"adminPassword": "", "spectatorPassword": ""}\n')
    with open(os.path.join(tmp, "event.json"), "w") as f:
        f.write('{"track": "misano", "ambientTemp": 22, '
                '"cloudLevel": 0.1, "rain": 0.0, "weatherRandomness": 0, '
                '"sessions": [{"sessionType": "P", "hourOfDay": 12, '
                '"dayOfWeekend": 1, "timeMultiplier": 1, '
                '"sessionDurationMinutes": 10}]}\n')
    with open(os.path.join(tmp, "ratings.json"), "w") as f:
        f.write("[]\n")


def fail(msg, code=1):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(code)


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    accd_bin = os.path.join(os.path.dirname(script_dir), "accd")
    if not os.access(accd_bin, os.X_OK):
        fail(f"accd binary not found at {accd_bin}")

    with tempfile.TemporaryDirectory(prefix="accd-smoke-") as tmp:
        write_cfg(tmp)
        proc = subprocess.Popen(
            [accd_bin, tmp],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            preexec_fn=os.setsid)
        try:
            # Wait for the listen socket to come up.
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                try:
                    with socket.create_connection(
                            ("127.0.0.1", TCP_PORT), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                fail(f"accd did not listen on {TCP_PORT} within 3s")

            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(CONNECT_TIMEOUT)
            sock.connect(("127.0.0.1", TCP_PORT))
            sock.sendall(build_handshake())
            body = recv_framed(sock)
            sock.close()

            # 1. Right opcode?
            if not body:
                fail("empty 0x0b body")
            if body[0] == 0x0c:
                fail("server sent reject (0x0c) instead of accept (0x0b)")
            if body[0] != 0x0b:
                fail(f"unexpected msg id {body[0]:#04x} (expected 0x0b)")

            # 2. conn_id must not be the rejection sentinel.
            if len(body) < 6:
                fail(f"0x0b body too short ({len(body)} bytes)")
            conn_id = struct.unpack_from("<H", body, 4)[0]
            if conn_id == 0xFFFF:
                fail("server set conn_id=0xFFFF (rejection)")

            # 3. Size is deterministic: the handshake body above is
            #    fixed, the cfg below is fixed, so the trailer size is
            #    fixed too.  Any drift means a write_* block inside
            #    build_welcome_trailer changed size — possibly
            #    legitimately, but the reviewer should notice.  If you
            #    deliberately change a block size, update this constant
            #    together with a commit explaining why.
            EXPECTED_LEN = 919
            if len(body) != EXPECTED_LEN:
                fail(f"welcome trailer is {len(body)} B, expected "
                     f"{EXPECTED_LEN} B — a write_* block changed "
                     f"size.  Verify against the pcap before shipping.")

            # 4. Tail sentinels — must end with u8(3) u8(0) u8(0) preceded
            #    by the 37-byte RatingSeries block whose first payload
            #    byte is "Standard" string length.
            if body[-3:] != b"\x03\x00\x00":
                fail(f"missing final 3-byte sentinel; got "
                     f"{body[-3:].hex()}")
            if b"Standard" not in body[-60:]:
                fail("RatingSeries 'Standard' marker missing near tail")

            print(f"PASS: welcome trailer {len(body)} B, "
                  f"conn_id={conn_id}, tail OK")
            return 0
        finally:
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
