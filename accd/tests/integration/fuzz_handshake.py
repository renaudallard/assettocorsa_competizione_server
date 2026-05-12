#!/usr/bin/env python3
"""Malformed-handshake fuzzer for accd.

Opens N TCP connections to accd, sends a random-length random-byte
TCP frame (with a valid length prefix so the framer doesnt drop us
immediately), and closes.  The fuzzer doesnt verify replies -- the
goal is purely to exercise the handshake parser on inputs the bot
would never produce and watch accd not crash.
"""
import argparse
import os
import random
import socket
import struct
import sys


def random_frame(rng):
    """Build one randomized TCP frame.

    Bias the lengths so we cover:
      - tiny bodies that fail every read_u8 inside the parser
      - short-but-plausible (~50-200 B) bodies that get past msg_id
        but trip on the variable-length strings
      - oversized (>2 KB) bodies that probe TCP-frame reassembly
    """
    bucket = rng.random()
    if bucket < 0.30:
        n = rng.randint(0, 8)
    elif bucket < 0.70:
        n = rng.randint(8, 256)
    elif bucket < 0.90:
        n = rng.randint(256, 2048)
    else:
        n = rng.randint(2048, 16384)
    body = bytes(rng.getrandbits(8) for _ in range(n))
    if n < 0xFFFF:
        hdr = struct.pack("<H", n)
    else:
        hdr = b"\xff\xff" + struct.pack("<I", n)
    # 50 % of the time set the first body byte to 0x09 so the parser
    # routes us into the handshake handler proper.
    if body and rng.random() < 0.5:
        body = b"\x09" + body[1:]
    return hdr + body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9302)
    ap.add_argument("--iters", type=int, default=200)
    ap.add_argument("--seed", type=int, default=None)
    args = ap.parse_args()

    seed = args.seed if args.seed is not None else int.from_bytes(
        os.urandom(4), "little")
    print(f"  seed={seed:#x} iters={args.iters}")
    rng = random.Random(seed)

    sent_bytes = 0
    for i in range(args.iters):
        frame = random_frame(rng)
        sent_bytes += len(frame)
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.5)
            s.connect((args.host, args.port))
            try:
                s.sendall(frame)
            except (BrokenPipeError, ConnectionResetError):
                pass
            try:
                s.recv(4096)  # consume any reject; ignore timeouts
            except (socket.timeout, ConnectionResetError):
                pass
            s.close()
        except (ConnectionRefusedError, OSError) as e:
            print(f"  iter {i}: connect failed ({e}) -- accd may be down")
            return 1
    print(f"  done: {args.iters} frames, {sent_bytes} bytes total")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
