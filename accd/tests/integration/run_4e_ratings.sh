#!/bin/sh
# Ratings 0x4e periodic-broadcast regression.
#
# accd emits SRV_RATING_SUMMARY (0x4e) when the ratings ledger is
# dirty AND CADENCE_RATINGS_MS (81 s) has elapsed since the last
# emit.  Anchored to server start so the first emit fires ~81 s
# after boot.  Body layout (handshake.c:build_rating_summary):
#   u8  0x4e
#   u8  car_count
#   per-car: u16 car_id, u8 0, u16 SA, u16 TR, i16 -1, i16 -1,
#            wr_str_a steam_id  (u8 cp_count + N x u32 codepoints)
#
# Slow test (~85 s).  Skip via SKIP=1 in CI smoke runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tmp/bot/bot
PCAP=/tmp/accd_0x4e.pcap

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

echo "==> capturing TCP egress on lo:9302"
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
    --race 911 --grid 1 --name "BotRat" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

# Bot completes a lap (~30 s); CADENCE_RATINGS_MS=81000 ms is anchored
# to server start, so the first periodic 0x4e emit fires near t=81 s.
echo "==> waiting 85 s for first periodic 0x4e emit"
sleep 85

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
sleep 1
sudo -n chown "$(id -u):$(id -g)" "$PCAP" 2>/dev/null || true

python3 - <<'PY'
import sys, struct
from scapy.all import rdpcap, TCP, Raw

streams = {}
for p in rdpcap("/tmp/accd_0x4e.pcap"):
    if TCP not in p or Raw not in p: continue
    if p[TCP].sport != 9302: continue
    streams.setdefault(p[TCP].dport, []).append(
        (p[TCP].seq, bytes(p[Raw].load)))

if not streams:
    print("FAIL: no accd->bot TCP segments captured"); sys.exit(1)

dport, segs = max(streams.items(), key=lambda kv: len(kv[1]))
segs.sort()
stream = b"".join(b for _, b in segs)

frames_4e = []
off = 0
while off + 2 <= len(stream):
    n = struct.unpack("<H", stream[off:off+2])[0]
    if off + 2 + n > len(stream): break
    body = stream[off+2:off+2+n]
    if body and body[0] == 0x4e:
        frames_4e.append(body)
    off += 2 + n

# Filter the welcome-trailer 0x4e (inside the 0x0b frame) by looking
# at the standalone broadcast frames only - they have a u8 0x4e header
# at body[0].  All captured 0x4e here ARE standalone since we slice
# the kunos frame envelope.
print(f"  standalone 0x4e frames observed: {len(frames_4e)}")
if not frames_4e:
    print("FAIL: no periodic 0x4e in 85 s"); sys.exit(1)

# Decode the LAST 0x4e (newest)
b = frames_4e[-1]
print(f"  body len: {len(b)}  hex (first 40 B): {b[:40].hex()}")
p = 1
nc = b[p]; p += 1
print(f"  car_count: {nc}")
if nc != 1:
    print(f"FAIL: expected car_count = 1, got {nc}"); sys.exit(2)

# Per-car: u16 car_id, u8 0, u16 SA, u16 TR, i16 -1, i16 -1, str_a sid
car_id = struct.unpack("<H", b[p:p+2])[0]; p += 2
pad = b[p]; p += 1
sa = struct.unpack("<H", b[p:p+2])[0]; p += 2
tr = struct.unpack("<H", b[p:p+2])[0]; p += 2
ma = struct.unpack("<h", b[p:p+2])[0]; p += 2
mb = struct.unpack("<h", b[p:p+2])[0]; p += 2
print(f"  car_id={car_id} pad={pad} SA={sa} TR={tr} marker_a={ma} marker_b={mb}")

rc = 0
if car_id != 1001: print(f"FAIL: car_id {car_id} != 1001"); rc = 3
if pad != 0:       print(f"FAIL: pad byte {pad} != 0"); rc = 4
if ma != -1:       print(f"FAIL: marker_a {ma} != -1"); rc = 5
if mb != -1:       print(f"FAIL: marker_b {mb} != -1"); rc = 6
if rc: sys.exit(rc)

# wr_str_a: u8 cp_count + cp_count * u32 codepoints
cp_count = b[p]; p += 1
cps = []
for _ in range(cp_count):
    cps.append(struct.unpack("<I", b[p:p+4])[0]); p += 4
sid = "".join(chr(c) for c in cps)
print(f"  steam_id ({cp_count} codepoints): {sid!r}")
if "S76561199000000911" not in sid:
    print(f"FAIL: steam_id mismatch; expected '...911', got {sid!r}")
    sys.exit(7)
if p != len(b):
    print(f"FAIL: {len(b) - p} trailing bytes"); sys.exit(8)
print("RESULT: PASS (0x4e periodic emit fired with correct body shape)")
PY
