#!/usr/bin/env python3
"""Check bounded /report burst persistence in a disposable ACCD instance."""

import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
from fake_client import Reader, build_handshake, recv_all_frames, recv_framed, write_cfg, wstr_a  # noqa: E402


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    tcp_port = free_port(socket.SOCK_STREAM)
    udp_port = free_port(socket.SOCK_DGRAM)
    with tempfile.TemporaryDirectory(prefix="accd-report-audit-") as tmp:
        write_cfg(tmp, tcp_port, udp_port)
        with open(os.path.join(tmp, "server.log"), "wb") as log:
            proc = subprocess.Popen(
                [os.path.abspath("accd"), tmp], cwd=tmp, stdout=log,
                stderr=subprocess.STDOUT, start_new_session=True,
            )
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        sock = socket.create_connection(("127.0.0.1", tcp_port), timeout=0.2)
                        break
                    except OSError:
                        if proc.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError("disposable server failed to listen")
                        time.sleep(0.05)
                with sock:
                    sock.sendall(build_handshake())
                    welcome, trailing = recv_framed(sock)
                    if welcome[:1] != b"\x0b":
                        raise RuntimeError("synthetic handshake rejected")
                    recv_all_frames(sock, trailing, idle_ms=100)
                    # The stock fake handshake's value at the race-number
                    # wire offset is 35 in this upstream checkout.
                    report = b"\x2a" + wstr_a("/report 35") + struct.pack("<i", 0)
                    frame = struct.pack("<H", len(report)) + report
                    for _ in range(10):
                        sock.sendall(frame)
                    frames, _ = recv_all_frames(sock, b"", idle_ms=700)
                    ack_count = sum(msg_id == 0x2B for msg_id, _ in frames)
                    ack_text = []
                    for msg_id, body in frames:
                        if msg_id == 0x2B:
                            reader = Reader(body)
                            reader.u8()
                            reader.fmt_a()
                            ack_text.append(reader.fmt_a())
                    report_path = os.path.join(tmp, "reports.txt")
                    if os.path.exists(report_path):
                        with open(report_path, encoding="utf-8") as f:
                            persisted = len(f.readlines())
                    else:
                        persisted = 0
                    print("burst_messages=10")
                    print("persisted_report_rows=" + str(persisted))
                    print("chat_acks_received=" + str(ack_count))
                    print("ack_messages=" + str(sorted(set(ack_text))))
                    time.sleep(2.1)
                    sock.sendall(frame)
                    recv_all_frames(sock, b"", idle_ms=500)
                    with open(report_path, encoding="utf-8") as f:
                        later = len(f.readlines())
                    print("rows_after_cooldown=" + str(later))
                    if persisted == 0:
                        log.flush()
                        with open(os.path.join(tmp, "server.log"), "r", errors="replace") as f:
                            for line in f:
                                if "report" in line.lower() or "CHAT" in line:
                                    print("server_diagnostic=" + line.strip()[:200])
                    return 0 if persisted == 1 and later == 2 and "Please wait before reporting again" in ack_text else 1
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
