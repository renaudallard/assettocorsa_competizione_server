#!/usr/bin/env python3
"""LAN discovery (0x48 → 0xc0) probe + decoder.

Sends u8(0xbf) + u8(0x48) + u32(nonce) on UDP to a server and decodes
the 0xc0 reply: str_a server_name + u8 clients + u8 has_password +
u16 tcp_port + u32 nonce_echo + u8 carGroup.
"""
import argparse
import socket
import struct
import sys


def send_probe(host: str, port: int, timeout: float = 2.0) -> bytes:
    nonce = 0xdeadbeef
    payload = bytes([0xbf, 0x48]) + struct.pack("<I", nonce)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(payload, (host, port))
    data, _ = s.recvfrom(2048)
    return data


def validate(reply: bytes) -> int:
    d = decode(reply)
    print(f"  {d}")
    if "error" in d:
        print(f"  FAIL: {d['error']}")
        return 1
    if d["nonce_echo"] != 0xdeadbeef:
        print(f"  FAIL: nonce echo mismatch (got 0x{d['nonce_echo']:08x})")
        return 1
    expected_cg = {0xfa, 0x07, 0x0c, 0x0b, 0xf9, 0x00}
    if d["car_group_byte"] not in expected_cg:
        print(f"  WARN: car_group byte 0x{d['car_group_byte']:02x} not in "
              "{0xfa,0x07,0x0c,0x0b,0xf9,0x00}")
    print("  RESULT: VALID (envelope + nonce echo + carGroup byte ok)")
    return 0


def decode(reply: bytes) -> dict:
    r = memoryview(reply)
    out = {"len": len(reply), "raw": reply.hex()}
    if not r or r[0] != 0xc0:
        out["error"] = f"bad envelope 0x{r[0]:02x}" if r else "empty"
        return out
    # str_a: u8 len_codepoints + N × u32 codepoints
    i = 1
    if i >= len(r):
        out["error"] = "truncated before name length"
        return out
    n = r[i]
    i += 1
    chars = []
    for _ in range(n):
        if i + 4 > len(r):
            out["error"] = "truncated in name"
            return out
        cp = struct.unpack_from("<I", r, i)[0]
        chars.append(chr(cp))
        i += 4
    out["server_name"] = "".join(chars)
    if i + 1 + 1 + 2 + 4 + 1 > len(r):
        out["error"] = "truncated tail"
        return out
    out["clients"] = r[i]; i += 1
    out["has_password"] = r[i]; i += 1
    out["tcp_port"] = struct.unpack_from("<H", r, i)[0]; i += 2
    out["nonce_echo"] = struct.unpack_from("<I", r, i)[0]; i += 4
    out["car_group_byte"] = r[i]; i += 1
    out["trailing_bytes"] = bytes(r[i:]).hex()
    return out


def compare(a: dict, k: dict) -> int:
    structural_fields = (
        "clients", "has_password", "tcp_port",
        "nonce_echo", "car_group_byte"
    )
    mismatch = 0
    print(f"  accd:  {a}")
    print(f"  kunos: {k}")
    if "error" in a:
        print(f"  FAIL accd: {a['error']}")
        return 1
    if "error" in k:
        print(f"  FAIL kunos: {k['error']}")
        return 1
    # Server name differs (accd cfg vs WineTest) — record but don't fail
    print(f"  server names: accd={a['server_name']!r} kunos={k['server_name']!r}")
    if a["nonce_echo"] != 0xdeadbeef or k["nonce_echo"] != 0xdeadbeef:
        print("  FAIL: nonce echo mismatch")
        mismatch += 1
    for f in structural_fields:
        if a[f] != k[f]:
            print(f"  DIFF {f}: accd={a[f]} kunos={k[f]}")
            mismatch += 1
    return mismatch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host")
    ap.add_argument("--port", type=int)
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("--raw", action="store_true",
                    help="print only the reply hex (for sh capture)")
    ap.add_argument("--validate-hex",
                    help="hex string of one reply to structurally validate")
    ap.add_argument("--compare-hex", nargs=2,
                    help="hex strings of two replies to compare")
    args = ap.parse_args()

    if args.validate_hex:
        return validate(bytes.fromhex(args.validate_hex))

    if args.compare_hex:
        a_bytes = bytes.fromhex(args.compare_hex[0])
        k_bytes = bytes.fromhex(args.compare_hex[1])
        mismatch = compare(decode(a_bytes), decode(k_bytes))
        if mismatch == 0:
            print("  RESULT: IDENTICAL (structural fields match)")
            return 0
        print(f"  RESULT: DIFFER ({mismatch} field mismatch)")
        return 2

    if not args.host or not args.port:
        ap.error("--host and --port required (or use --validate-hex/--compare-hex)")

    reply = send_probe(args.host, args.port, args.timeout)
    if args.raw:
        print(reply.hex())
    else:
        for k, v in decode(reply).items():
            print(f"  {k}: {v}")


if __name__ == "__main__":
    sys.exit(main() or 0)
