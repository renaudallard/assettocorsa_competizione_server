#!/usr/bin/env python3
"""
Compare TCP frames the server sends after the client transmits 0x41.

Approach:
  1. Reassemble the server-to-client TCP byte stream from each pcap.
  2. Walk the stream as ACC-framed messages (u16 length, escape 0xffff).
  3. Index frames by msg id (first body byte).
  4. For each msg id, list the frames seen in each capture in arrival
     order, with arrival timestamp relative to the first SYN.
  5. Side-by-side dump for the msg ids most relevant to the penalty:
     0x36 (leaderboard), 0x2b (chat), 0x41 (relayed report), 0x14 (ping),
     0x28 (state).
  6. Byte-by-byte hex compare for matched 0x36 frames around the 0x41
     send.

This does not strictly need timestamps to align — both bots use the same
trigger logic and sleep cadence, so the post-0x41 frames are emitted in
the same order on both sides.
"""

import argparse
import struct
import sys
from collections import defaultdict


# Minimal pcap reader — we only need the raw IPv4/TCP bytes plus
# timestamps.

PCAP_GLOBAL_HDR = struct.Struct("<IHHiIII")  # 24 B
PCAP_REC_HDR = struct.Struct("<IIII")        # 16 B


def read_pcap(path):
    """Yield (ts_us, data) per packet."""
    with open(path, "rb") as f:
        ghdr = f.read(PCAP_GLOBAL_HDR.size)
        magic, vmaj, vmin, _, _, snaplen, linktype = PCAP_GLOBAL_HDR.unpack(ghdr)
        if magic != 0xa1b2c3d4:
            raise ValueError(f"not a pcap (magic={magic:#x})")
        if linktype not in (0, 1):
            raise ValueError(f"unsupported linktype {linktype}")
        # 0 = NULL/loopback (4-byte family prefix).  1 = ETHERNET.
        while True:
            rh = f.read(PCAP_REC_HDR.size)
            if len(rh) < PCAP_REC_HDR.size:
                return
            tsec, tusec, caplen, _origlen = PCAP_REC_HDR.unpack(rh)
            data = f.read(caplen)
            if len(data) < caplen:
                return
            ts = tsec * 1_000_000 + tusec
            if linktype == 1:
                # Ethernet header = 14 B
                yield ts, data[14:]
            else:
                # NULL/loopback = 4 B family
                yield ts, data[4:]


def parse_ipv4_tcp(pkt):
    """Return (src, sport, dst, dport, seq, payload) or None."""
    if len(pkt) < 20:
        return None
    if (pkt[0] >> 4) != 4:
        return None
    ihl = (pkt[0] & 0x0f) * 4
    if pkt[9] != 6:  # TCP
        return None
    if len(pkt) < ihl + 20:
        return None
    src = ".".join(str(b) for b in pkt[12:16])
    dst = ".".join(str(b) for b in pkt[16:20])
    tcp = pkt[ihl:]
    sport, dport, seq = struct.unpack(">HHI", tcp[:8])
    data_off = (tcp[12] >> 4) * 4
    payload = tcp[data_off:]
    return src, sport, dst, dport, seq, payload


def reassemble_server_tx(pcap_path, server_port):
    """Reassemble TCP bytes flowing FROM server_port (server -> client).
    Returns (start_ts_us, payload_bytes, [(byte_offset, ts_us)])."""
    streams = defaultdict(dict)  # (server_ip, server_port, client_ip, client_port) -> {seq: (payload, ts)}
    first_ts = None
    init_seq = {}
    for ts, pkt in read_pcap(pcap_path):
        info = parse_ipv4_tcp(pkt)
        if info is None:
            continue
        src, sport, dst, dport, seq, payload = info
        if sport != server_port:
            continue
        first_ts = first_ts if first_ts is not None else ts
        key = (src, sport, dst, dport)
        if key not in init_seq and payload:
            init_seq[key] = seq
        if payload:
            streams[key][seq] = (payload, ts)
    # Reassemble per stream by sorted seq, then concat all streams in
    # connection order (we expect only one).
    out_payload = b""
    out_marks = []   # list of (byte_offset, ts_us)
    for key, segs in streams.items():
        base = init_seq[key]
        # Sort by seq (handle wrap by treating all as relative to base)
        prev_seq = None
        running = base
        for seq in sorted(segs.keys(), key=lambda s: (s - base) & 0xffffffff):
            payload, ts = segs[seq]
            if seq != running:
                # Gap — fill with placeholder so we don't misalign.
                gap = (seq - running) & 0xffffffff
                if gap > 0 and gap < 1024:
                    out_payload += b"\x00" * gap
                    running = seq
            out_marks.append((len(out_payload), ts))
            out_payload += payload
            running = (seq + len(payload)) & 0xffffffff
            prev_seq = seq
    return first_ts, out_payload, out_marks


def walk_acc_frames(buf):
    """Yield (start_off, length, body_bytes) for each ACC-framed message."""
    i = 0
    while i + 2 <= len(buf):
        ln = buf[i] | (buf[i+1] << 8)
        i += 2
        if ln == 0xffff:
            if i + 4 > len(buf):
                break
            ln = buf[i] | (buf[i+1] << 8) | (buf[i+2] << 16) | (buf[i+3] << 24)
            i += 4
        if ln == 0 or i + ln > len(buf):
            break
        body = buf[i:i+ln]
        yield i - 2, ln, body
        i += ln


def offset_to_ts(off, marks):
    """Linear search for last mark <= off."""
    last = 0
    for m_off, m_ts in marks:
        if m_off <= off:
            last = m_ts
        else:
            break
    return last


def index_frames(payload, marks):
    """Return list of (rel_ts_us, frame_offset, msg_id, body) post-handshake.
    The handshake response (0x0b) is the first big frame; we drop it from
    the diff because it carries timestamps + weather randomness.
    """
    base_ts = marks[0][1] if marks else 0
    out = []
    seen_welcome = False
    for off, ln, body in walk_acc_frames(payload):
        if not body:
            continue
        msg_id = body[0]
        ts = offset_to_ts(off, marks) - base_ts
        if msg_id == 0x0b and not seen_welcome:
            seen_welcome = True
            out.append((ts, off, msg_id, body, True))  # mark welcome
            continue
        out.append((ts, off, msg_id, body, False))
    return out


def hexdump(b, prefix="    "):
    lines = []
    for i in range(0, len(b), 16):
        chunk = b[i:i+16]
        h = " ".join(f"{x:02x}" for x in chunk)
        ascii_ = "".join(chr(x) if 0x20 <= x < 0x7f else "." for x in chunk)
        lines.append(f"{prefix}{i:04x}  {h:<48}  {ascii_}")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--accd", default="accd.pcap")
    ap.add_argument("--accd-port", type=int, default=9302)
    ap.add_argument("--kunos", default="kunos.pcap")
    ap.add_argument("--kunos-port", type=int, default=19298)
    args = ap.parse_args()

    print(f"=== Reassembling {args.accd} (server port {args.accd_port}) ===")
    a_first, a_buf, a_marks = reassemble_server_tx(args.accd, args.accd_port)
    print(f"  payload bytes: {len(a_buf)}")
    a_idx = index_frames(a_buf, a_marks)
    print(f"  frames: {len(a_idx)}")

    print(f"=== Reassembling {args.kunos} (server port {args.kunos_port}) ===")
    k_first, k_buf, k_marks = reassemble_server_tx(args.kunos, args.kunos_port)
    print(f"  payload bytes: {len(k_buf)}")
    k_idx = index_frames(k_buf, k_marks)
    print(f"  frames: {len(k_idx)}")

    # Bucket by msg id
    a_by = defaultdict(list)
    k_by = defaultdict(list)
    for ts, off, mid, body, is_welcome in a_idx:
        a_by[mid].append((ts, body, is_welcome))
    for ts, off, mid, body, is_welcome in k_idx:
        k_by[mid].append((ts, body, is_welcome))

    all_ids = sorted(set(a_by.keys()) | set(k_by.keys()))
    print(f"\n=== Frame counts per msg id ===")
    print(f"{'id':>4}  {'accd':>6}  {'kunos':>6}  {'name':>20}")
    name_map = {
        0x0b: "ACP_SERVER_RESP",
        0x14: "ACP_PING",
        0x28: "ACP_STATE",
        0x2b: "ACP_CHAT",
        0x36: "ACP_LEADERBOARD",
        0x37: "ACP_WEATHER",
        0x3a: "ACP_CAR_LOC",
        0x3b: "ACP_SECTOR_RELAY",
        0x3c: "ACP_GRID_RELAY",
        0x3f: "ACP_DRIVERINFO",
        0x40: "ACP_WEATHER_RESET",
        0x41: "ACP_PENALTY_RELAY",
        0x42: "ACP_PEN_PENDING",
        0x44: "ACP_DAMAGE_RELAY",
        0x46: "ACP_DIRT_RELAY",
        0x4e: "ACP_SESSION_PHASE",
        0x53: "ACP_PIT_LANE_TIME",
        0x55: "ACP_GARAGE",
        0x56: "ACP_GARAGE_HISTORY",
        0x5b: "ACP_RACE_RESULT",
    }
    for mid in all_ids:
        a = len(a_by.get(mid, []))
        k = len(k_by.get(mid, []))
        flag = "" if a == k else " <- DIFF"
        print(f"{mid:#04x}  {a:6d}  {k:6d}  {name_map.get(mid, ''):>20}{flag}")

    # Penalty-relevant: 0x36 (leaderboard), 0x2b (chat), 0x41 (relay)
    print(f"\n=== 0x41 ACP_REPORT_PENALTY relays ===")
    for label, frames in (("accd", a_by.get(0x41, [])), ("kunos", k_by.get(0x41, []))):
        print(f"  {label}: {len(frames)} frames")
        for ts, body, _ in frames:
            print(f"    ts={ts/1e6:7.3f}s  len={len(body):3d}  {body.hex()}")

    print(f"\n=== 0x2b ACP_CHAT ===")
    for label, frames in (("accd", a_by.get(0x2b, [])), ("kunos", k_by.get(0x2b, []))):
        print(f"  {label}: {len(frames)} frames")
        for ts, body, _ in frames:
            print(f"    ts={ts/1e6:7.3f}s  len={len(body):3d}  {body.hex()}")

    # 0x36 byte-comparison over time.
    print(f"\n=== 0x36 ACP_LEADERBOARD (per-emit length) ===")
    a_lb = a_by.get(0x36, [])
    k_lb = k_by.get(0x36, [])
    print(f"  accd  : {len(a_lb)} frames; lengths: {[len(b) for _, b, _ in a_lb]}")
    print(f"  kunos : {len(k_lb)} frames; lengths: {[len(b) for _, b, _ in k_lb]}")

    # Side-by-side hex diff of last 0x36 emit (most likely to carry penalty)
    if a_lb and k_lb:
        a_last = a_lb[-1][1]
        k_last = k_lb[-1][1]
        print(f"\n=== Last 0x36 hex (penalty likely here) ===")
        print(f"--- accd  ({len(a_last)} B) ---")
        print(hexdump(a_last))
        print(f"--- kunos ({len(k_last)} B) ---")
        print(hexdump(k_last))

    # First 0x36 (clean baseline)
    if a_lb and k_lb:
        a_first_f = a_lb[0][1]
        k_first_f = k_lb[0][1]
        print(f"\n=== First 0x36 hex (no penalty yet) ===")
        if a_first_f == k_first_f:
            print(f"  accd and kunos identical ({len(a_first_f)} B)")
        else:
            # Mark first byte that differs
            diff_at = next((i for i in range(min(len(a_first_f), len(k_first_f)))
                            if a_first_f[i] != k_first_f[i]), None)
            if diff_at is None:
                diff_at = min(len(a_first_f), len(k_first_f))
            print(f"  first diff at offset 0x{diff_at:x}")
            print(f"--- accd  ({len(a_first_f)} B) ---")
            print(hexdump(a_first_f))
            print(f"--- kunos ({len(k_first_f)} B) ---")
            print(hexdump(k_first_f))


if __name__ == "__main__":
    main()
