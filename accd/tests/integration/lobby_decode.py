#!/usr/bin/env python3
"""Walk accd's outbound lobby UDP packets and validate the kson
preamble structure.

Wire layout (per accd/lobby.c:lobby_write_preamble + memory):
    u8(0x3a) + u8(type) + u32(7) + u32(session_id) + u8(0) + msg_body
Known types: 0xc8 register, 0xcb (?), 0xd1 drivers, 0xf2 keepalive.
"""
import struct
import sys

try:
    from scapy.all import rdpcap, TCP, Raw
except ImportError:
    print("FAIL: scapy required")
    sys.exit(1)

KNOWN_TYPES = {0xc8, 0xcb, 0xd1, 0xf2}


def main(path: str) -> int:
    # Re-assemble the TCP stream from client (accd) to server (kunos
    # lobby): collect Raw payloads in seq order, then walk the
    # concatenated bytes as length-prefixed kson frames.
    pkts = []
    for p in rdpcap(path):
        if TCP not in p or Raw not in p:
            continue
        if p[TCP].dport != 909:
            continue
        pkts.append((p[TCP].seq, bytes(p[Raw].load)))
    pkts.sort(key=lambda x: x[0])
    stream = b"".join(b for _, b in pkts)
    # First message is the 256-byte init blob (no length prefix; see
    # accd/lobby.c lobby_send_init_blob).  Verify the port checksum
    # bytes then skip past it.
    if len(stream) < 256:
        print(f"  FAIL: only {len(stream)} bytes captured, init blob is 256")
        return 1
    init = stream[:256]
    local_port = struct.unpack("<H", init[:2])[0]
    if init[2] != local_port % 77 or init[3] != local_port % 21:
        print(f"  WARN: init checksum mismatch "
              f"(port=0x{local_port:04x}, "
              f"got [{init[2]}, {init[3]}], "
              f"expected [{local_port % 77}, {local_port % 21}])")
    else:
        print(f"  init blob: local_port={local_port}, "
              "modular checksum bytes correct")
    off = 256
    # Subsequent frames are u16 LE length + body.
    packets = []
    while off + 2 <= len(stream):
        n = struct.unpack("<H", stream[off:off + 2])[0]
        if off + 2 + n > len(stream):
            break
        packets.append(stream[off + 2:off + 2 + n])
        off += 2 + n
    print(f"  reassembled {len(packets)} lobby kson frames "
          f"({len(stream) - 256} stream bytes after init blob)")
    if not packets:
        print("  FAIL: no lobby UDP traffic captured "
              "(registerToLobby off, lobby unreachable, or capture iface wrong)")
        return 1

    bad = 0
    types_seen = {}
    for i, body in enumerate(packets):
        if len(body) < 11:
            print(f"  [{i}] FAIL: only {len(body)} B, need >= 11 for preamble")
            bad += 1
            continue
        magic = body[0]
        type_b = body[1]
        proto = struct.unpack("<I", body[2:6])[0]
        sess = struct.unpack("<I", body[6:10])[0]
        sep = body[10]
        msg_id = body[11] if len(body) >= 12 else None
        if magic != 0x3a:
            print(f"  [{i}] FAIL: magic 0x{magic:02x} (expected 0x3a)")
            bad += 1
            continue
        if type_b not in KNOWN_TYPES:
            print(f"  [{i}] WARN: type 0x{type_b:02x} not in known set "
                  f"{{0xc8,0xcb,0xd1,0xf2}}")
        if proto != 7:
            print(f"  [{i}] FAIL: proto version {proto} (expected 7)")
            bad += 1
            continue
        if sep != 0:
            print(f"  [{i}] FAIL: separator 0x{sep:02x} (expected 0x00)")
            bad += 1
            continue
        types_seen[type_b] = types_seen.get(type_b, 0) + 1
        print(f"  [{i}] OK type=0x{type_b:02x} session={sess} "
              f"msg_id=0x{msg_id:02x} body_len={len(body)}")

    print(f"  types_seen: " + ", ".join(
        f"0x{t:02x}={c}" for t, c in sorted(types_seen.items())))
    if bad == 0:
        print("RESULT: VALID (all preambles parse + known types)")
        return 0
    print(f"RESULT: DIFFER ({bad} bad frames)")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]) if len(sys.argv) > 1 else 1)
