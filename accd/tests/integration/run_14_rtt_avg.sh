#!/bin/sh
# 0x14 server-keepalive RTT-averaging regression.
#
# accd sends 0x14 SRV_KEEPALIVE_14 over UDP at ~1 Hz/conn (mirrors
# FUN_140041e80).  Wire (15 B): u8 msg_id + u32 srv_ms +
# u16 conn_rtt + u16 avg_ping + u16 max_ping + 4 trailing const
# bytes.  The conn_rtt field is the EWMA of round-trip times derived
# from inbound 0x16 ACP_PONG_PHYSICS replies (dispatch.c:340-351
# updates pc->avg_rtt_ms with a 7/8 smoothing).
#
# This test spawns 1 bot, captures all UDP egress from accd:9303,
# walks every 0x14 frame, and asserts:
#   - >= 1 frame with conn_rtt == 0  (the very first keepalive
#     before any pong has been seen)
#   - >= 1 later frame with conn_rtt > 0  (RTT seeded by first pong
#     and refreshed by EWMA on every subsequent one)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tmp/bot/bot
PCAP=/tmp/accd_0x14_rtt.pcap

ACCD_PID=""
BOT_PIDS=""
DPID=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
    [ -n "$DPID" ] && sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT

echo "==> capturing UDP egress from accd:9303"
sudo -n rm -f "$PCAP"
sudo -n dumpcap -i lo -w "$PCAP" -f 'udp src port 9303' -q >/dev/null 2>&1 &
DPID=$!
sleep 1

rm -f accd.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "BotRTT" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

# Need at least 4-5 keepalive cycles for the EWMA to register.  1 Hz
# cadence -> ~8 s gives 6-7 0x14 frames + multiple pongs.
echo "==> 12 s for ping/pong loop to settle"
sleep 12

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
sleep 1
sudo -n chown "$(id -u):$(id -g)" "$PCAP" 2>/dev/null || true

python3 - <<'PY'
import sys, struct
from scapy.all import rdpcap, UDP, Raw

frames = []  # list of (srv_ms, conn_rtt, avg_ping, max_ping)
for p in rdpcap("/tmp/accd_0x14_rtt.pcap"):
    if UDP not in p or Raw not in p: continue
    if p[UDP].sport != 9303: continue
    raw = bytes(p[Raw].load)
    if len(raw) != 15 or raw[0] != 0x14:
        continue
    srv_ms = struct.unpack("<I", raw[1:5])[0]
    conn_rtt = struct.unpack("<H", raw[5:7])[0]
    avg_ping = struct.unpack("<H", raw[7:9])[0]
    max_ping = struct.unpack("<H", raw[9:11])[0]
    frames.append((srv_ms, conn_rtt, avg_ping, max_ping))

print(f"  0x14 frames observed: {len(frames)}")
if len(frames) < 4:
    print("FAIL: too few 0x14 frames; bot may not be UDP-associated")
    sys.exit(1)

for i, (ms, r, a, m) in enumerate(frames):
    print(f"  [{i}] srv_ms={ms} conn_rtt={r} avg={a} max={m}")

zeros = [r for _, r, _, _ in frames if r == 0]
nonzeros = [r for _, r, _, _ in frames if r > 0]
print(f"  conn_rtt == 0 frames: {len(zeros)}")
print(f"  conn_rtt >  0 frames: {len(nonzeros)}")
if not zeros:
    print("FAIL: no initial conn_rtt=0 keepalive (the bot pre-empted "
          "the first emit)")
    sys.exit(2)
if not nonzeros:
    print("FAIL: conn_rtt stayed at 0 across all frames; "
          "EWMA never primed (bot not sending 0x16 pong?)")
    sys.exit(3)
# Sanity: the nonzero RTT for localhost should be tiny.
big = [r for r in nonzeros if r > 200]
if big:
    print(f"  WARN: conn_rtt values >200 ms on loopback look fishy: {big}")
print(f"RESULT: PASS (RTT EWMA primed; first-emit=0, later={nonzeros[-1]} ms)")
PY
