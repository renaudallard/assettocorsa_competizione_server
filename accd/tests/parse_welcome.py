#!/usr/bin/env python3
"""
Walk a Kunos welcome packet (msg 0x0b) byte-by-byte.

Usage:
    python3 accd/tests/parse_welcome.py --pcap <file.pcapng> [--frame N]

Bring your own pcap.  Needs `tshark` on PATH; the named frame must be a
TCP payload containing a single 0x0b welcome packet from server to
client.  Output is a labelled byte map covering header + ServerName/
TrackName + SpawnDefs + SeasonEntity + EventEntity + session_mgr_state
+ Leaderboard + top-level WeatherData + TrackConditions +
write_track_records + dirt + MTR + RatingSeries + tail.

The parser walks blocks in the order accd's `build_welcome_trailer`
emits.  If a real Kunos capture diverges (e.g., a sub-object reads
more bytes than our static-RE inferred), every later block in the
parser output will be off by the same delta — compare the labelled
field values against expected types (string ASCII, plausible f32
ranges) to find the first divergence.
"""

import argparse
import struct
import subprocess


def get_frame_payload(pcap_path: str, frame_no: int) -> bytes:
    out = subprocess.run(
        ["tshark", "-r", pcap_path, "-Y", f"frame.number == {frame_no}",
         "-T", "fields", "-e", "tcp.payload"],
        capture_output=True, check=True, text=True)
    hex_str = out.stdout.strip().replace("\n", "").replace(":", "")
    return bytes.fromhex(hex_str)


class Reader:
    def __init__(self, buf: bytes):
        self.buf = buf
        self.pos = 0

    def u8(self) -> int:
        v = self.buf[self.pos]
        self.pos += 1
        return v

    def u16(self) -> int:
        v = struct.unpack_from("<H", self.buf, self.pos)[0]
        self.pos += 2
        return v

    def i16(self) -> int:
        v = struct.unpack_from("<h", self.buf, self.pos)[0]
        self.pos += 2
        return v

    def u32(self) -> int:
        v = struct.unpack_from("<I", self.buf, self.pos)[0]
        self.pos += 4
        return v

    def u64(self) -> int:
        v = struct.unpack_from("<Q", self.buf, self.pos)[0]
        self.pos += 8
        return v

    def f32(self) -> float:
        v = struct.unpack_from("<f", self.buf, self.pos)[0]
        self.pos += 4
        return v

    def ksstr(self) -> str:
        n = self.u16()
        s = self.buf[self.pos:self.pos + n].decode("utf-8", errors="replace")
        self.pos += n
        return s

    def fmt_a(self) -> str:
        n = self.u8()
        cps = [struct.unpack_from("<I", self.buf, self.pos + i * 4)[0]
               for i in range(n)]
        self.pos += n * 4
        return "".join(chr(c) for c in cps if c < 0x110000)

    def raw(self, n: int) -> bytes:
        v = self.buf[self.pos:self.pos + n]
        self.pos += n
        return v


# Output formatter: print one labelled line per field with cumulative offset.

def label(off, size, name, value=""):
    print(f"  +{off:04x} ({size:>3} B) {name:<40} {value}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pcap", required=True,
        help="path to the pcap/pcapng containing the welcome packet")
    ap.add_argument("--frame", type=int, required=True,
        help="frame number of the 0x0b welcome packet")
    args = ap.parse_args()

    payload = get_frame_payload(args.pcap, args.frame)
    print(f"Frame {args.frame}: {len(payload)} B TCP payload\n")

    # u16 length prefix (TCP framing)
    length = struct.unpack_from("<H", payload, 0)[0]
    print(f"TCP frame length prefix: 0x{length:04x} = {length}")
    body = payload[2:2 + length]
    print(f"Body: {len(body)} B\n")

    r = Reader(body)
    base = lambda: r.pos

    print("=== Header ===")
    o = base(); v = r.u8();  label(o, 1, "msg_id",            f"0x{v:02x}")
    o = base(); v = r.u16(); label(o, 2, "udp_port",          v)
    o = base(); v = r.u8();  label(o, 1, "magic",             f"0x{v:02x}")
    o = base(); v = r.u16(); label(o, 2, "conn_id",           v)
    o = base(); v = r.u32(); label(o, 4, "car_id",            v)

    trailer_start = r.pos
    print(f"\n=== Trailer (starts at body offset {trailer_start}) ===")

    o = base(); v = r.ksstr(); label(o, len(v) + 2, "ServerName ksstr",   repr(v))
    o = base(); v = r.ksstr(); label(o, len(v) + 2, "TrackName ksstr",    repr(v))
    o = base(); spawn_count = r.u8(); label(o, 1, "spawnDefCount",        spawn_count)

    for s in range(spawn_count):
        print(f"\n--- SpawnDef[{s}] ---")
        sd_start = r.pos
        o = base(); v = r.u16(); label(o, 2, "  car_id",              v)
        o = base(); v = r.u8();  label(o, 1, "  flag1",               v)
        o = base(); v = r.u8();  label(o, 1, "  flag2",               v)

        # CarInfo: 45 fixed + variable-length strings + 7 trailing u8
        ci_start = r.pos
        # 3 u32
        for k in range(3):
            o = base(); v = r.u32(); label(o, 4, f"  CarInfo.u32[{k}]", v)
        # 2 u8
        for k in range(2):
            o = base(); v = r.u8();  label(o, 1, f"  CarInfo.u8[{k}]",  v)
        # u32
        o = base(); v = r.u32(); label(o, 4, "  CarInfo.u32[3]",       v)
        # u8
        o = base(); v = r.u8();  label(o, 1, "  CarInfo.cup_category", v)
        # 3 u32
        for k in range(3):
            o = base(); v = r.u32(); label(o, 4, f"  CarInfo.u32[{k+4}]", v)
        # 4 u8
        for k in range(4):
            o = base(); v = r.u8();  label(o, 1, f"  CarInfo.u8[{k+2}]", v)
        # u32 race_number
        o = base(); v = r.u32(); label(o, 4, "  CarInfo.race_number", v)
        # u32
        o = base(); v = r.u32(); label(o, 4, "  CarInfo.u32[8]",       v)
        # 2 u8
        for k in range(2):
            o = base(); v = r.u8();  label(o, 1, f"  CarInfo.u8[{k+6}]", v)
        # fmt_a customSkinName
        o = base(); v = r.fmt_a(); label(o, 1 + len(v) * 4, "  CarInfo.customSkinName", repr(v))
        # u8 bannerKey
        o = base(); v = r.u8();    label(o, 1, "  CarInfo.bannerKey",    v)
        # fmt_a teamName
        o = base(); v = r.fmt_a(); label(o, 1 + len(v) * 4, "  CarInfo.teamName", repr(v))
        # u16 nat
        o = base(); v = r.u16();   label(o, 2, "  CarInfo.nat",          v)
        # fmt_a displayName
        o = base(); v = r.fmt_a(); label(o, 1 + len(v) * 4, "  CarInfo.displayName", repr(v))
        # fmt_a competitorName
        o = base(); v = r.fmt_a(); label(o, 1 + len(v) * 4, "  CarInfo.competitorName", repr(v))
        # u16
        o = base(); v = r.u16();   label(o, 2, "  CarInfo.u16[?]",       v)
        # u8
        o = base(); v = r.u8();    label(o, 1, "  CarInfo.u8[?]",        v)
        # u8 carModel
        o = base(); v = r.u8();    label(o, 1, "  CarInfo.carModel",     v)
        # u8 cup
        o = base(); v = r.u8();    label(o, 1, "  CarInfo.cup",          v)
        # u8 + 3 u8
        for k in range(4):
            o = base(); v = r.u8(); label(o, 1, f"  CarInfo.tail[{k}]",  v)
        ci_len = r.pos - ci_start
        print(f"  [CarInfo total: {ci_len} B]")

        # DriverInfo array
        o = base(); dc = r.u8(); label(o, 1, f"  driver_count",          dc)
        for d in range(dc):
            di_start = r.pos
            print(f"  -- DriverInfo[{d}] --")
            # 5 fmt_a strings
            for k in range(5):
                o = base(); v = r.fmt_a(); label(o, 1 + len(v) * 4, f"    drv.fmt_a[{k}]", repr(v))
            # 41-byte numeric block: u8 cat, u16 nat, u8, 3 u32, u8, 2 u32, 4 u32 = 1+2+1+12+1+8+16 = 41
            o = base(); v = r.u8();  label(o, 1, "    drv.cat",          v)
            o = base(); v = r.u16(); label(o, 2, "    drv.nat",          v)
            o = base(); v = r.u8();  label(o, 1, "    drv.u8[0]",        v)
            for k in range(3):
                o = base(); v = r.u32(); label(o, 4, f"    drv.u32[{k}]", v)
            o = base(); v = r.u8();  label(o, 1, "    drv.u8[1]",        v)
            for k in range(2):
                o = base(); v = r.u32(); label(o, 4, f"    drv.u32[{k+3}]", v)
            for k in range(4):
                o = base(); v = r.u32(); label(o, 4, f"    drv.u32[{k+5}]", v)
            # 1 fmt_a steam_id
            o = base(); v = r.fmt_a(); label(o, 1 + len(v) * 4, "    drv.steam_id", repr(v))
            di_len = r.pos - di_start
            print(f"    [DriverInfo total: {di_len} B]")

        # Per-car trailing block: u8 active_drv, u64 ts, u8, u8, 5 tire, 5 dmg, u16 elo, u32 stab
        o = base(); v = r.u8();  label(o, 1, "  active_driver_idx",    v)
        o = base(); v = r.u64(); label(o, 8, "  timestamp",            v)
        o = base(); v = r.u8();  label(o, 1, "  flag_a",               v)
        o = base(); v = r.u8();  label(o, 1, "  flag_b",               v)
        o = base(); v = r.raw(5); label(o, 5, "  tire[5]",             v.hex())
        o = base(); v = r.raw(5); label(o, 5, "  damage[5]",           v.hex())
        o = base(); v = r.u16(); label(o, 2, "  elo",                  v)
        o = base(); v = r.u32(); label(o, 4, "  stability",            v)
        sd_len = r.pos - sd_start
        print(f"  [SpawnDef[{s}] total: {sd_len} B]")

    # SeasonEntity body — NOT a vtable dispatch from here; the welcome reader
    # calls vtable[0x28] of param_3+0x58 which IS the SeasonEntity reader,
    # which in turn dispatches the 7 sub-readers and then reads 4 rule
    # vectors + EventEntity vector.

    print(f"\n=== SeasonEntity (starts at body offset {r.pos}) ===")
    se_start = r.pos
    # 7 sub-objects
    print("--- HudRules (7 u8 = 7 B) ---")
    for k in range(7):
        o = base(); v = r.u8(); label(o, 1, f"hud[{k}]", v)
    print("--- AssistRules (8 u8 + 2 f32 = 16 B) ---")
    for k in range(2):
        o = base(); v = r.u8(); label(o, 1, f"asst.u8[{k}]", v)
    for k in range(2):
        o = base(); v = r.f32(); label(o, 4, f"asst.f32[{k}]", v)
    for k in range(6):
        o = base(); v = r.u8(); label(o, 1, f"asst.u8[{k+2}]", v)
    print("--- GraphicsRules (6 u8 = 6 B) ---")
    for k in range(6):
        o = base(); v = r.u8(); label(o, 1, f"gfx[{k}]", v)
    print("--- RealismRules (12 u8 + 3 f32 = 24 B) ---")
    for k in range(2):
        o = base(); v = r.u8(); label(o, 1, f"real.u8[{k}]", v)
    o = base(); v = r.f32(); label(o, 4, "real.f32[0]", v)
    o = base(); v = r.u8(); label(o, 1, "real.u8[2]", v)
    for k in range(2):
        o = base(); v = r.f32(); label(o, 4, f"real.f32[{k+1}]", v)
    for k in range(9):
        o = base(); v = r.u8(); label(o, 1, f"real.u8[{k+3}]", v)
    print("--- GameplayRules (5 u8 + u32 = 9 B) ---")
    for k in range(5):
        o = base(); v = r.u8(); label(o, 1, f"play.u8[{k}]", v)
    o = base(); v = r.u32(); label(o, 4, "play.u32", v)
    print("--- OnlineRules (4 u8 + u16 + 8 u8 = 14 B) ---")
    for k in range(4):
        o = base(); v = r.u8(); label(o, 1, f"online.u8[{k}]", v)
    o = base(); v = r.u16(); label(o, 2, "online.u16", v)
    for k in range(8):
        o = base(); v = r.u8(); label(o, 1, f"online.u8[{k+4}]", v)
    print("--- RaceDirectorRules (5 u8 + 3 u32 + u8 = 18 B) ---")
    for k in range(5):
        o = base(); v = r.u8(); label(o, 1, f"rd.u8[{k}]", v)
    for k in range(3):
        o = base(); v = r.u32(); label(o, 4, f"rd.u32[{k}]", v)
    o = base(); v = r.u8(); label(o, 1, "rd.u8[5]", v)
    print("--- 4 list counts + 1 EventEntity count (5 u16 = 10 B) ---")
    for nm in ("realism_extra", "gameplay_extra", "online_extra",
               "race_director_extra", "event_count"):
        o = base(); v = r.u16(); label(o, 2, f"count.{nm}", v)
    se_len = r.pos - se_start
    print(f"[SeasonEntity total: {se_len} B]")

    # EventEntity body
    o = base(); v = r.fmt_a(); label(o, 1 + len(v) * 4, "EventEntity.trackName fmt_a", repr(v))

    print("\n--- EventEntity sub-objects ---")
    print("CircuitInfo (19 B = 3 u8 + 4 f32):")
    for k in range(3):
        o = base(); v = r.u8(); label(o, 1, f"  ci.u8[{k}]", v)
    for k in range(4):
        o = base(); v = r.f32(); label(o, 4, f"  ci.f32[{k}]", v)

    print("GraphicsInfo (9 B = 6 u8 + u16 + u8):")
    for k in range(6):
        o = base(); v = r.u8(); label(o, 1, f"  gi.u8[{k}]", v)
    o = base(); v = r.u16(); label(o, 2, "  gi.u16", v)
    o = base(); v = r.u8(); label(o, 1, "  gi.u8[6]", v)

    print("CarSet (2 B = 2 u8):")
    o = base(); v = r.u8(); label(o, 1, "  cs.u8[0]", v)
    o = base(); v = r.u8(); label(o, 1, "  cs.u8[1]", v)

    print("RaceRules (18 B = 12 fields + 2 literal-1 + tyreSetCount):")
    o = base(); v = r.u8();  label(o, 1, "  rr.qualifyStandingType", v)
    o = base(); v = r.u8();  label(o, 1, "  rr.superpoleMaxCar",     f"0x{v:02x}")
    o = base(); v = r.u16(); label(o, 2, "  rr.pitWindowLengthSec",  f"0x{v:04x}")
    o = base(); v = r.u16(); label(o, 2, "  rr.driverStintTimeSec",  f"0x{v:04x}")
    o = base(); v = r.u8();  label(o, 1, "  rr.isRefuellingAllowed", v)
    o = base(); v = r.u8();  label(o, 1, "  rr.isRefuellingTimeFixed", v)
    o = base(); v = r.u8();  label(o, 1, "  rr.maxDriversCount",     v)
    o = base(); v = r.u8();  label(o, 1, "  rr.mandatoryPitstopCount", v)
    o = base(); v = r.u16(); label(o, 2, "  rr.maxTotalDrivingTime", f"0x{v:04x}")
    o = base(); v = r.u8();  label(o, 1, "  rr.isMandatoryPitRefuel", v)
    o = base(); v = r.u8();  label(o, 1, "  rr.isMandatoryPitTyre", v)
    o = base(); v = r.u8();  label(o, 1, "  rr.isMandatoryPitSwap", v)
    o = base(); v = r.u8();  label(o, 1, "  rr.literal_1[0]",        f"0x{v:02x}")
    o = base(); v = r.u8();  label(o, 1, "  rr.literal_1[1]",        f"0x{v:02x}")
    o = base(); v = r.u8();  label(o, 1, "  rr.tyreSetCount",        v)

    print("WeatherStatus (24 B = 6 f32 in wire order ambient/windDir/road/windSpeed/rain/cloud):")
    for nm in ("ambient", "windDir", "road", "windSpeed", "rain", "cloud"):
        o = base(); v = r.f32(); label(o, 4, f"  ws.{nm}", v)

    print("WeatherData (52 B = 12 u32 + u16 + u16):")
    for k in range(12):
        o = base(); v_u = struct.unpack_from("<I", r.buf, r.pos)[0]
        v_f = struct.unpack_from("<f", r.buf, r.pos)[0]
        r.pos += 4
        label(o, 4, f"  wd.field[{k}]", f"u32=0x{v_u:08x} f32={v_f:g}")
    o = base(); v = r.u16(); label(o, 2, "  wd.count1", v)
    o = base(); v = r.u16(); label(o, 2, "  wd.count2", v)

    print(f"\n=== session_mgr_state (starts at body offset {r.pos}) ===")
    sm_start = r.pos
    o = base(); v = r.u8(); label(o, 1, "session_index", v)
    for k in range(7):
        o = base(); valid = r.u8(); label(o, 1, f"phase[{k}].valid", valid)
        if valid:
            o = base(); v = r.f32(); label(o, 4, f"phase[{k}].ts_f32", v)
    print("session_mgr_state tail (FUN_14352c640): 1 u8 + 1 u8 + 1 u8 + u32/f32 + u16 + 2 u32 + 1 u8 + 1 u8 + u32/f32")
    o = base(); v = r.u8();  label(o, 1, "tail.hour_of_day",  v)
    o = base(); v = r.u8();  label(o, 1, "tail.u8[0]",        v)
    o = base(); v = r.u8();  label(o, 1, "tail.time_mult",    v)
    o = base(); v = r.f32(); label(o, 4, "tail.grip",         v)
    o = base(); v = r.u16(); label(o, 2, "tail.sched_field",  v)
    o = base(); v = r.u32(); label(o, 4, "tail.duration_s",   v)
    o = base(); v = r.u32(); label(o, 4, "tail.overtime_s",   v)
    o = base(); v = r.u8();  label(o, 1, "tail.u8[1]",        v)
    o = base(); v = r.u8();  label(o, 1, "tail.u8[2]",        v)
    o = base(); v = r.f32(); label(o, 4, "tail.f32_const",    v)
    sm_len = r.pos - sm_start
    print(f"[session_mgr_state total: {sm_len} B]")

    print(f"\n=== Leaderboard (starts at body offset {r.pos}) ===")
    lb_start = r.pos
    o = base(); v = r.u32(); label(o, 4, "lb.sess_best_lap_ms", v)
    o = base(); n = r.u8();  label(o, 1, "lb.sect_count", n)
    for k in range(n):
        o = base(); v = r.u32(); label(o, 4, f"lb.sect[{k}]", v)
    o = base(); v = r.u8();  label(o, 1, "lb.cvar8", v)
    o = base(); nc = r.u16(); label(o, 2, "lb.entry_count", nc)
    for c in range(nc):
        print(f"  -- LeaderboardEntry[{c}] --")
        e_start = r.pos
        o = base(); v = r.u16(); label(o, 2, f"    entry.car_id", v)
        o = base(); v = r.u16(); label(o, 2, f"    entry.race_num", v)
        o = base(); v = r.u8();  label(o, 1, f"    entry.car_model", v)
        o = base(); v = r.u8();  label(o, 1, f"    entry.cup", v)
        o = base(); v = r.u16(); label(o, 2, f"    entry.u16_zero", v)
        o = base(); has_pen = r.u8(); label(o, 1, f"    entry.active_pen_flag", has_pen)
        if has_pen:
            o = base(); v = r.u16(); label(o, 2, f"    entry.pen_kind", v)
            o = base(); v = r.f32(); label(o, 4, f"    entry.pen_remaining", v)
        if v:
            pass
        # cvar8-gated u8: missingMandatoryPitstop / formation_mid_passed
        o = base(); v = r.u8(); label(o, 1, f"    entry.cvar8_byte (+0x204)", v)
        o = base(); pq_count = r.u8(); label(o, 1, f"    entry.pq_count", pq_count)
        for p in range(pq_count):
            o = base(); v = r.u32(); label(o, 4, f"    entry.pq[{p}]", v)
        o = base(); ndrv = r.u8(); label(o, 1, f"    entry.driver_count", ndrv)
        for d in range(ndrv):
            print(f"    --- entry.driver[{d}] ---")
            o = base(); v = r.ksstr(); label(o, len(v) + 2, "      steam_id", repr(v))
            o = base(); v = r.ksstr(); label(o, len(v) + 2, "      short_name", repr(v))
            o = base(); v = r.ksstr(); label(o, len(v) + 2, "      first_name", repr(v))
            o = base(); v = r.ksstr(); label(o, len(v) + 2, "      last_name", repr(v))
            o = base(); v = r.u8();    label(o, 1, "      driver_category", v)
            o = base(); v = r.u16();   label(o, 2, "      nationality", v)
        # 6 fields after driver list: u16, u32, u32, u16, u32, u8
        o = base(); v = r.u16(); label(o, 2, "    entry.tail.u16_zero",     v)
        o = base(); v = r.u32(); label(o, 4, "    entry.tail.best_lap_ms",  v)
        o = base(); v = r.u32(); label(o, 4, "    entry.tail.last_lap_ms",  v)
        o = base(); v = r.u16(); label(o, 2, "    entry.tail.lap_count",    v)
        o = base(); v = r.u32(); label(o, 4, "    entry.tail.race_time_ms", v)
        o = base(); v = r.u8();  label(o, 1, "    entry.tail.elo",          v)
        # wide_flag, l1_n, l1 sectors, l2_n, l2 history
        o = base(); wf = r.u8(); label(o, 1, "    entry.wide_flag", wf)
        o = base(); l1 = r.u8(); label(o, 1, "    entry.l1_n", l1)
        for k in range(l1):
            if wf:
                o = base(); v = r.u32(); label(o, 4, f"      l1[{k}]", v)
            else:
                o = base(); v = r.u16(); label(o, 2, f"      l1[{k}]", v)
        o = base(); l2 = r.u8(); label(o, 1, "    entry.l2_n", l2)
        for k in range(l2):
            if wf:
                o = base(); v = r.u32(); label(o, 4, f"      l2[{k}]", v)
            else:
                o = base(); v = r.u16(); label(o, 2, f"      l2[{k}]", v)
        # 2 trailing u8
        o = base(); v = r.u8(); label(o, 1, "    entry.tail.u8[0]", v)
        o = base(); v = r.u8(); label(o, 1, "    entry.tail.u8[1]", v)
        e_len = r.pos - e_start
        print(f"    [LeaderboardEntry total: {e_len} B]")
    # 2 trailing u8 (assist_rules tail)
    o = base(); v = r.u8(); label(o, 1, "lb.assist_tail.u8[0]", v)
    o = base(); v = r.u8(); label(o, 1, "lb.assist_tail.u8[1]", v)
    lb_len = r.pos - lb_start
    print(f"[Leaderboard total: {lb_len} B]")

    print(f"\n=== Top-level WeatherData (starts at body offset {r.pos}) ===")
    twd_start = r.pos
    for k in range(12):
        o = base(); v_u = struct.unpack_from("<I", r.buf, r.pos)[0]
        v_f = struct.unpack_from("<f", r.buf, r.pos)[0]
        r.pos += 4
        label(o, 4, f"twd.field[{k}]", f"u32=0x{v_u:08x} f32={v_f:g}")
    o = base(); v = r.u16(); label(o, 2, "twd.count1", v)
    o = base(); v = r.u16(); label(o, 2, "twd.count2", v)
    twd_len = r.pos - twd_start
    print(f"[Top-level WeatherData total: {twd_len} B]")

    print(f"\n=== TrackConditions (starts at body offset {r.pos}) ===")
    tc_start = r.pos
    print("7 leading f32 (TrackConditions outer):")
    for k in range(7):
        o = base(); v = r.f32(); label(o, 4, f"tc.f32[{k}]", v)
    print("WeatherStatus sub-object (6 f32 in wire order ambient/windDir/road/windSpeed/rain/cloud):")
    for nm in ("ambient", "windDir", "road", "windSpeed", "rain", "cloud"):
        o = base(); v = r.f32(); label(o, 4, f"tc.ws.{nm}", v)
    print("Trailing f32:")
    o = base(); v = r.f32(); label(o, 4, "tc.weekend_time_s", v)
    tc_len = r.pos - tc_start
    print(f"[TrackConditions total: {tc_len} B]")

    print(f"\n=== write_track_records (starts at body offset {r.pos}) ===")
    wt_start = r.pos
    o = base(); n = r.u8(); label(o, 1, "session_count", n)
    for k in range(n):
        rec_start = r.pos
        print(f"  -- session[{k}] (23 B) --")
        o = base(); v = r.u8();  label(o, 1, f"    hour_of_day", v)
        o = base(); v = r.u8();  label(o, 1, f"    pad_zero",    v)
        o = base(); v = r.u8();  label(o, 1, f"    session_type", v)
        o = base(); v = r.f32(); label(o, 4, f"    f32_const",   v)
        o = base(); v = r.u16(); label(o, 2, f"    sched_field", v)
        o = base(); v = r.u32(); label(o, 4, f"    duration_s",  v)
        o = base(); v = r.u32(); label(o, 4, f"    overtime_s",  v)
        o = base(); v = r.u8();  label(o, 1, f"    pad_zero[1]", v)
        o = base(); v = r.u8();  label(o, 1, f"    session_type[1]", v)
        o = base(); v = r.f32(); label(o, 4, f"    f32_const[1]", v)
    wt_len = r.pos - wt_start
    print(f"[write_track_records total: {wt_len} B]")

    print(f"\n=== Dirt fields (starts at body offset {r.pos}) ===")
    o = base(); v = r.u8(); label(o, 1, "dirt.update_freq",  v)
    o = base(); v = r.u8(); label(o, 1, "dirt.delta_thresh", v)

    print(f"\n=== MTR / MultiplayerTrackRecord (19 B starts at body offset {r.pos}) ===")
    mtr_start = r.pos
    o = base(); v = r.u32(); label(o, 4, "mtr.u32[0]",     f"0x{v:08x}")
    o = base(); v = r.u8();  label(o, 1, "mtr.u8[0]",      f"0x{v:02x}")
    o = base(); v = r.u8();  label(o, 1, "mtr.u8[1]",      f"0x{v:02x}")
    o = base(); v = r.u8();  label(o, 1, "mtr.u8[2]",      f"0x{v:02x}")
    o = base(); v = r.u8();  label(o, 1, "mtr.u8[3]",      f"0x{v:02x}")
    o = base(); v = r.u32(); label(o, 4, "mtr.u32[1]",     f"0x{v:08x}")
    o = base(); v = r.u32(); label(o, 4, "mtr.u32[2]",     f"0x{v:08x}")
    o = base(); v = r.u8();  label(o, 1, "mtr.u8[4]",      f"0x{v:02x}")
    o = base(); v = r.u8();  label(o, 1, "mtr.u8[5]",      f"0x{v:02x}")
    o = base(); v = r.u8();  label(o, 1, "mtr.u8[6]",      f"0x{v:02x}")
    mtr_len = r.pos - mtr_start
    print(f"[MTR total: {mtr_len} B]")

    print(f"\n=== RatingSeries (37 B starts at body offset {r.pos}) ===")
    rs_start = r.pos
    o = base(); v = r.ksstr(); label(o, len(v) + 2, "rs.series_name", repr(v))
    o = base(); v = r.ksstr(); label(o, len(v) + 2, "rs.empty_str",   repr(v))
    o = base(); cnt = r.u32(); label(o, 4, "rs.count", cnt)
    for k in range(cnt):
        rl_start = r.pos
        print(f"  -- RatingLine[{k}] (21 B for empty) --")
        o = base(); v = r.ksstr(); label(o, len(v) + 2, "    rl.str[0]", repr(v))
        o = base(); v = r.ksstr(); label(o, len(v) + 2, "    rl.str[1]", repr(v))
        o = base(); v = r.ksstr(); label(o, len(v) + 2, "    rl.str[2]", repr(v))
        o = base(); v = r.u8();    label(o, 1, "    rl.u8",    v)
        o = base(); v = r.u32();   label(o, 4, "    rl.u32[0]", v)
        o = base(); v = r.u16();   label(o, 2, "    rl.u16[0]", v)
        o = base(); v = r.u16();   label(o, 2, "    rl.u16[1]", v)
        o = base(); v = r.u16();   label(o, 2, "    rl.u16[2]", v)
        o = base(); v = r.u32();   label(o, 4, "    rl.u32[1]", v)
    rs_len = r.pos - rs_start
    print(f"[RatingSeries total: {rs_len} B]")

    print(f"\n=== Tail (3 B at body offset {r.pos}) ===")
    o = base(); v = r.u8(); label(o, 1, "tail.formation_lap_type", v)
    o = base(); v = r.u8(); label(o, 1, "tail.zero[0]",            v)
    o = base(); v = r.u8(); label(o, 1, "tail.zero[1]",            v)

    leftover = len(body) - r.pos
    print(f"\nBody bytes consumed: {r.pos}/{len(body)} (leftover: {leftover})")


if __name__ == "__main__":
    main()
