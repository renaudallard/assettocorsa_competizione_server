#!/usr/bin/env python3
"""
Reject-code regression test: send handshakes that should fail, verify
the 0x0c reply carries the right reject code (byte 1 of the body).

Catches the 2026-04-17 bug where accd wrote u32(7) after 0x0c for
every rejection regardless of reason, making a bad-password reply
look like a wrong-version reply to the ACC client.

Exe codes (from FUN_14002db30 call sites):
  4 = kicked, 5 = banned, 6 = wrong password,
  7 = wrong version (low byte), 8 = wrong version (high byte),
  9 = server full, 10 = CP rating, 11 = bad car, 12 = bad session.
"""

import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

TCP_PORT = 19233
UDP_PORT = 19234


def wstr_a(s):
    out = bytes([len(s)])
    for ch in s:
        out += struct.pack("<I", ord(ch))
    return out


def build_handshake(version=0x0100, password=""):
    body = bytes([0x09])
    body += struct.pack("<H", version)
    body += wstr_a(password)
    body += wstr_a("Tester")
    body += wstr_a("Smoke")
    body += wstr_a("SMK")
    body += bytes([0])
    body += struct.pack("<H", 0)
    body += wstr_a("S76561199000000000")
    body += struct.pack("<i", 99)
    body += bytes([35, 0])
    body += wstr_a("Smoke Team")
    return struct.pack("<H", len(body)) + body


def recv_framed(sock, timeout=3.0):
    sock.settimeout(timeout)
    buf = b""
    while len(buf) < 2:
        d = sock.recv(8192)
        if not d:
            raise ConnectionError("peer closed")
        buf += d
    ln = buf[0] | (buf[1] << 8)
    while len(buf) < 2 + ln:
        d = sock.recv(8192)
        if not d:
            raise ConnectionError("peer closed")
        buf += d
    return buf[2:2 + ln]


def write_cfg(tmp, password):
    with open(os.path.join(tmp, "configuration.json"), "w") as f:
        f.write('{"tcpPort": %d, "udpPort": %d, "maxConnections": 4, '
                '"lanDiscovery": 0}\n' % (TCP_PORT, UDP_PORT))
    with open(os.path.join(tmp, "settings.json"), "w") as f:
        f.write('{"serverName": "smoke", "password": "%s", '
                '"adminPassword": "", "spectatorPassword": ""}\n'
                % password)
    with open(os.path.join(tmp, "event.json"), "w") as f:
        f.write('{"track": "misano", "ambientTemp": 22, '
                '"cloudLevel": 0.1, "rain": 0.0, "weatherRandomness": 0, '
                '"sessions": [{"sessionType": "P", "hourOfDay": 12, '
                '"dayOfWeekend": 1, "timeMultiplier": 1, '
                '"sessionDurationMinutes": 10}]}\n')
    with open(os.path.join(tmp, "ratings.json"), "w") as f:
        f.write("[]\n")


def send_and_read(port, handshake):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    sock.connect(("127.0.0.1", port))
    sock.sendall(handshake)
    body = recv_framed(sock)
    sock.close()
    return body


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    accd_bin = os.path.join(os.path.dirname(script_dir), "accd")
    if not os.access(accd_bin, os.X_OK):
        fail(f"accd binary not found at {accd_bin}")

    server_password = "correct"
    with tempfile.TemporaryDirectory(prefix="accd-reject-") as tmp:
        write_cfg(tmp, server_password)
        proc = subprocess.Popen(
            [accd_bin, tmp],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            preexec_fn=os.setsid)
        try:
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                try:
                    with socket.create_connection(
                            ("127.0.0.1", TCP_PORT), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                fail(f"accd did not listen on {TCP_PORT}")

            # 1. Wrong version: body should start with 0x0c + code 7
            body = send_and_read(TCP_PORT,
                build_handshake(version=0x0099, password=""))
            if body[0] != 0x0c or body[1] != 7:
                fail(f"wrong-version: expected 0c 07, got "
                     f"{body[0]:02x} {body[1]:02x}")
            # detail_a = received version, detail_b = protocol version
            rx_ver = struct.unpack_from("<I", body, 6)[0]
            proto = struct.unpack_from("<I", body, 10)[0]
            if rx_ver != 0x99 or proto != 0x100:
                fail(f"wrong-version details: a={rx_ver:#x} b={proto:#x}")

            # 2. Wrong password: 0x0c + code 6
            body = send_and_read(TCP_PORT,
                build_handshake(password="not-correct"))
            if body[0] != 0x0c or body[1] != 6:
                fail(f"wrong-password: expected 0c 06, got "
                     f"{body[0]:02x} {body[1]:02x}")

            # 3. Correct password: should get 0x0b (accept), not 0x0c
            body = send_and_read(TCP_PORT,
                build_handshake(password=server_password))
            if body[0] != 0x0b:
                fail(f"correct-password: expected 0b, got "
                     f"{body[0]:02x} (body[1]={body[1]:02x})")

            print("PASS: reject codes wrong-version=7, "
                  "wrong-password=6, correct=accept")
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
