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
Parse an ACC `*.ai` racing-line file (e.g. Content/Tracks/<track>/data/
fastlane.ai) and emit a per-waypoint CSV in the same format the bot
consumes: `norm_pos x y z speed` with the cumulative track length in
the header comment.

File format reverse-engineered from the AC2 client decomp:
  - FUN_14346e310 (`AISpline::loadFromStream`) reads 4 bytes (version)
    then dispatches to FUN_14346e630.
  - FUN_14346e630 reads:
        u32 num_points
        u32 reserved_a            (stored at AISpline+0x08)
        u32 reserved_b
        repeat num_points:
            f64 x; f64 y; f64 z   (Vector3D, 24 B)
            8 B  unknown           (discarded by parser)
            4 B  unknown           (discarded by parser)
        u32 num_payloads          (== num_points)
        repeat num_payloads:
            f32 fields[0x10..0x14]  (variable shape — see below)
  - FUN_143453cf0 then reads optional GRID DATA (formation grid).

Payload field layout (per FUN_14346e630 lines 115-140; struct stored
as 0x6C / 108 bytes):
    +0x00   f32 speed         (km/h; the fuel calc tests it against
                               300.0 and uses it as the "max speed"
                               candidate at this spline point)
    +0x04   f32 ?
    +0x08   f32 ?
    +0x0c   f32 ?
    +0x10   f32 ?  (only read if version >= 8, else clones +0x08)
    +0x14   f32 ?  (only read if version >= 8, else clones +0x0c)
    +0x18   f32 ?
    +0x1c   f32 ?
    +0x20   f32 \
    +0x24   f32  >  three floats (probably normal vector) — unused
    +0x28   f32 /
    +0x2c   f32 ?
    +0x30   f32 \
    +0x34   f32  >  computed tangent at load time, not in file
    +0x38   f32 /
    +0x3c   f32 second speed-ish (fuel calc reads ABS() of this)
    +0x40   f32 ?
    +0x44   f32 ?  (constant 1.0 / 0x3F800000 in default-init path)
    +0x48   f32 ?
    +0x4c   f32 ?
    +0x50   f32 ?
    +0x54   u8  ?
    +0x58   f32 ?
    +0x5c   f32 ?
    +0x60   f32 ? -1.0 (sentinel)
    +0x64   f32 ? -1.0 (sentinel)
    +0x68   f32 ? -1.0 (sentinel)

This parser only extracts (position, speed) — the rest is left
unparsed and the file pointer is advanced enough bytes to reach the
GRID DATA section, which is also skipped.

Usage:
    python3 parse_ai.py <path/to/file.ai> [--out CSV] [--debug]
"""

import argparse
import os
import struct
import sys


def read_exact(f, n, what):
    data = f.read(n)
    if len(data) != n:
        raise ValueError(f"short read for {what}: got {len(data)} of {n}")
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--out", default=None,
                    help="output CSV path (default: stdout)")
    ap.add_argument("--debug", action="store_true",
                    help="print field hexdumps and offsets to stderr")
    args = ap.parse_args()

    with open(args.path, "rb") as f:
        version, = struct.unpack("<I", read_exact(f, 4, "version"))
        num_points, = struct.unpack("<I", read_exact(f, 4, "num_points"))
        reserved_a, = struct.unpack("<I", read_exact(f, 4, "reserved_a"))
        reserved_b, = struct.unpack("<I", read_exact(f, 4, "reserved_b"))

        if args.debug:
            print(f"# version={version} num_points={num_points} "
                  f"reserved=({reserved_a},{reserved_b})", file=sys.stderr)
        if num_points == 0 or num_points > 50000:
            raise ValueError(f"implausible num_points={num_points}")

        positions = []
        for i in range(num_points):
            x, y, z = struct.unpack("<ddd", read_exact(f, 24, "xyz"))
            read_exact(f, 8, "spline-tail-8")
            read_exact(f, 4, "spline-tail-4")
            positions.append((x, y, z))

        num_payloads, = struct.unpack(
            "<I", read_exact(f, 4, "num_payloads"))
        if num_payloads != num_points:
            print(f"warning: num_payloads={num_payloads} != "
                  f"num_points={num_points}", file=sys.stderr)

        speeds = []
        # Per-payload byte sequence reproduced from the decomp's
        # basic_istream::read() call order.  Total per entry:
        #   v >= 8: 4*9 + 4*2 + 12*2 + 4*2 = 76 bytes
        #   v <  8: 4*9 + 12*2 + 4*2 = 68 bytes
        for i in range(min(num_payloads, num_points)):
            field_00, = struct.unpack("<f", read_exact(f, 4, "f0"))
            read_exact(f, 4, "f4")
            read_exact(f, 4, "f8")
            read_exact(f, 4, "fc")
            read_exact(f, 4, "f10a")
            read_exact(f, 4, "f10b")
            read_exact(f, 4, "f10c")
            if version >= 8:
                read_exact(f, 4, "f18a")
                read_exact(f, 4, "f18b")
            read_exact(f, 4, "f100")
            read_exact(f, 4, "ffc")
            read_exact(f, 12, "f8_block")
            read_exact(f, 4, "fe0")
            read_exact(f, 12, "tangent_pre")
            read_exact(f, 4, "res20")
            read_exact(f, 4, "d8_hi")
            speeds.append(field_00)

        # Compute cumulative chord distance for norm_pos derivation.
        total = 0.0
        cum = [0.0]
        for i in range(1, len(positions)):
            x0, y0, z0 = positions[i - 1]
            x1, y1, z1 = positions[i]
            d = ((x1 - x0) ** 2 + (y1 - y0) ** 2 + (z1 - z0) ** 2) ** 0.5
            total += d
            cum.append(total)
        # Close the loop.
        x0, y0, z0 = positions[-1]
        x1, y1, z1 = positions[0]
        total += ((x1 - x0) ** 2 + (y1 - y0) ** 2 + (z1 - z0) ** 2) ** 0.5

        # Decide whether to emit the speed column.  Kunos's
        # fastlane.ai leaves payload+0x00 at zero — the AI computes
        # speeds dynamically from curvature and physics — so we drop
        # the column rather than emit "0 m/s" rows that would stall
        # the bot.  ideal_line.ai (when present) does carry real
        # speeds at the same offset.
        nonzero_speeds = sum(1 for s in speeds if abs(s) > 1e-6)
        emit_speed = nonzero_speeds > num_points // 4
        if not emit_speed and nonzero_speeds:
            print(f"warning: only {nonzero_speeds}/{num_points} non-zero "
                  f"speeds — dropping speed column", file=sys.stderr)

        out = sys.stdout if args.out is None else open(args.out, "w")
        if emit_speed:
            out.write(f"# norm_pos x y z speed_m_s    "
                      f"(track_length_m={total:.1f}, "
                      f"version={version}, points={num_points})\n")
        else:
            out.write(f"# norm_pos x y z    "
                      f"(track_length_m={total:.1f}, "
                      f"version={version}, points={num_points}, "
                      f"speeds=dynamic-AI)\n")
        for i, (x, y, z) in enumerate(positions):
            u = cum[i] / total if total > 0 else float(i) / num_points
            if emit_speed:
                sp_ms = (speeds[i] if i < len(speeds) else 0.0) / 3.6
                out.write(f"{u:.5f} {x:.2f} {y:.2f} {z:.2f} {sp_ms:.2f}\n")
            else:
                out.write(f"{u:.5f} {x:.2f} {y:.2f} {z:.2f}\n")
        if out is not sys.stdout:
            out.close()
            print(f"wrote {os.path.basename(args.out)}: {num_points} wp, "
                  f"{total:.0f} m total, version={version}, "
                  f"{'with-speed' if emit_speed else 'no-speed'}",
                  file=sys.stderr)


if __name__ == "__main__":
    main()
