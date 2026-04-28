#!/usr/bin/env python3
#
# Copyright (c) 2025-2026 Renaud Allard
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
"""
Extract a racing-line CSV from a pcap of an ACC race.

Reads UDP 0x1e ACP_CAR_UPDATE packets, takes vec_a (position) and
scalar_44 (norm_pos), groups by car_id, and writes one CSV per car
with one waypoint per ~0.005 norm_pos step.

Output rows: norm_pos x y z   (whitespace-separated, sorted by norm_pos)

Usage:
    python3 extract_racing_line.py <pcap> <out_dir> [--car ID] [--step S]

A typical full-race pcap produces a ~200-waypoint racing line per
car in ~10 seconds.  Useful for harvesting a track's geometry from
a single recorded session when you don't have access to the source
.ai files.
"""

import argparse
import os
import struct
import sys

from scapy.all import rdpcap, UDP, Raw


def parse_0x1e(payload):
    if len(payload) != 68 or payload[0] != 0x1e:
        return None
    # Layout (matches accd handlers.c h_udp_car_update):
    #   u8  msg
    #   u16 source_conn_id
    #   u16 target_car_id
    #   u8  seq
    #   u32 client_ts_ms                            (10 B header)
    #   3 * Vector3 f32 (vec_a, vec_b, vec_c)       (36 B)
    #   4 u8 input_a + 11 B scalars + 4 u8 + 3 B    (22 B)
    # vec_a at +10 (position), vec_c at +34 (velocity).
    target_car = struct.unpack_from("<H", payload, 3)[0]
    vec_a = struct.unpack_from("<fff", payload, 10)
    vec_c = struct.unpack_from("<fff", payload, 34)
    scalar_44 = struct.unpack_from("<f", payload, 57)[0]
    return target_car, vec_a, vec_c, scalar_44


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("out_dir")
    ap.add_argument("--car", type=int, default=None,
                    help="only emit this target_car_id")
    ap.add_argument("--step", type=float, default=0.005,
                    help="norm_pos sampling step (default 0.005 = 200 wp)")
    args = ap.parse_args()

    if not os.path.isdir(args.out_dir):
        os.makedirs(args.out_dir)

    import math
    samples = {}        # car_id -> {bucket: (u, x, y, z, speed)}
    p = rdpcap(args.pcap)
    print(f"loaded {len(p)} packets", file=sys.stderr)

    for pkt in p:
        if UDP not in pkt or Raw not in pkt:
            continue
        out = parse_0x1e(bytes(pkt[Raw]))
        if out is None:
            continue
        car, (x, y, z), (vx, vy, vz), u = out
        if args.car is not None and car != args.car:
            continue
        if not (0.0 <= u <= 1.0):
            continue
        speed = math.sqrt(vx * vx + vy * vy + vz * vz)
        bucket = int(u / args.step)
        # Keep the highest-speed sample per bucket — racing-line
        # passes through each bucket once per lap, but a stalled /
        # spinning sample in the same bucket would otherwise hold the
        # slot.  Highest speed = the actual racing pass.
        prev = samples.setdefault(car, {}).get(bucket)
        if prev is None or speed > prev[4]:
            samples[car][bucket] = (u, x, y, z, speed)

    for car, by_bucket in samples.items():
        # Compute total lap length from cumulative chord distance —
        # the bot uses this to convert recorded speed into d(norm_pos).
        ordered = [by_bucket[b] for b in sorted(by_bucket)]
        length = 0.0
        for i in range(1, len(ordered)):
            ux, x0, y0, z0, _ = ordered[i - 1]
            ux2, x1, y1, z1, _ = ordered[i]
            length += math.sqrt(
                (x1 - x0) ** 2 + (y1 - y0) ** 2 + (z1 - z0) ** 2)
        # Close the loop back to the first waypoint.
        if len(ordered) > 1:
            x0, y0, z0 = ordered[-1][1:4]
            x1, y1, z1 = ordered[0][1:4]
            length += math.sqrt(
                (x1 - x0) ** 2 + (y1 - y0) ** 2 + (z1 - z0) ** 2)

        path = os.path.join(args.out_dir, f"car_{car:04d}.csv")
        with open(path, "w") as f:
            f.write(f"# norm_pos x y z speed_m_s    "
                    f"(track_length_m={length:.1f})\n")
            for u, x, y, z, sp in ordered:
                f.write(
                    f"{u:.5f} {x:.2f} {y:.2f} {z:.2f} {sp:.2f}\n")
        print(f"wrote {path} ({len(by_bucket)} wp, "
              f"{length:.0f} m total)", file=sys.stderr)


if __name__ == "__main__":
    main()
