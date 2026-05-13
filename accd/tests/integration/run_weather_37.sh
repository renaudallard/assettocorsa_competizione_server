#!/bin/sh
# Weather 0x37 cadence + body-shape regression.
#
# accd emits 0x37 every 5 s to every connected client (handlers /
# tick).  Wire body is 1 + 17*4 = 69 bytes (cmd + 17 f32 fields:
# ambient/track temp, cloud level, rain level, etc).  This test
# captures TCP egress from accd to a connected bot for ~17 s,
# pulls every 0x37 frame, and asserts:
#   - >= 3 frames in the window (5 s cadence)
#   - mean gap is 4..6 s
#   - every body length is exactly 69 bytes
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tmp/bot/bot
PCAP=/tmp/accd_weather37.pcap

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

echo "==> capturing TCP egress from accd"
sudo -n rm -f "$PCAP"
sudo -n dumpcap -i lo -w "$PCAP" -f 'tcp port 9302' -q >/dev/null 2>&1 &
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
    --race 911 --grid 1 --name "BotWx" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

echo "==> driving for ~17 s (expecting ~3 weather frames)"
sleep 17

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
sleep 1
sudo -n chown "$(id -u):$(id -g)" "$PCAP" 2>/dev/null || true

python3 - <<'PY'
import sys, struct
from scapy.all import rdpcap, TCP, Raw

streams = {}  # dport -> [(seq, payload, ts)]
for p in rdpcap("/tmp/accd_weather37.pcap"):
    if TCP not in p or Raw not in p:
        continue
    if p[TCP].sport != 9302:
        continue
    streams.setdefault(p[TCP].dport, []).append(
        (p[TCP].seq, bytes(p[Raw].load), float(p.time)))

if not streams:
    print("FAIL: no accd->bot TCP segments captured"); sys.exit(1)

# Walk biggest stream
dport, segs = max(streams.items(), key=lambda kv: len(kv[1]))
segs.sort()
stream = b"".join(b for _, b, _ in segs)

# Map TCP seq positions to wall-clock for cadence calc: build a
# (stream_offset -> timestamp) index per segment.
seq_to_ts = []
off = 0
for seq, payload, ts in segs:
    seq_to_ts.append((off, ts))
    off += len(payload)

def ts_at(off):
    last = None
    for so, t in seq_to_ts:
        if so <= off:
            last = t
        else:
            break
    return last

# Walk u16-length-prefixed kunos frames
off = 0
weather = []
while off + 2 <= len(stream):
    n = struct.unpack("<H", stream[off:off+2])[0]
    if off + 2 + n > len(stream):
        break
    body = stream[off+2:off+2+n]
    if body and body[0] == 0x37:
        weather.append((ts_at(off), body))
    off += 2 + n

print(f"  0x37 frames observed: {len(weather)}")
for i, (ts, body) in enumerate(weather):
    print(f"  [{i}] ts={ts:.2f} body_len={len(body)}")

rc = 0
if len(weather) < 3:
    print(f"FAIL: only {len(weather)} 0x37 frames in 17 s; expected >= 3")
    rc = 1
bad = [b for _, b in weather if len(b) != 69]
if bad:
    print(f"FAIL: {len(bad)} 0x37 frame(s) with body_len != 69")
    rc = 2
if rc == 0 and len(weather) >= 2:
    gaps = [weather[i+1][0] - weather[i][0]
            for i in range(len(weather) - 1)]
    print(f"  gaps: " + ", ".join(f"{g:.2f}s" for g in gaps))
    bad_gaps = [g for g in gaps if not (3.5 <= g <= 6.5)]
    if bad_gaps:
        print(f"FAIL: cadence gap(s) outside [3.5, 6.5] s: {bad_gaps}")
        rc = 3
if rc == 0:
    print("RESULT: PASS (0x37 fires every ~5 s at 69 B body)")
sys.exit(rc)
PY
