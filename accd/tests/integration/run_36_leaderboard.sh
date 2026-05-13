#!/bin/sh
# Leaderboard 0x36 byte-decode regression.
#
# accd emits SRV_LEADERBOARD_BCAST (0x36) on every standings change.
# Body layout (write_session_leaderboard_section in handshake.c):
#   u8  0x36
#   u32 session-best lap (LAP_TIME_INVALID sentinel if unset)
#   u8  3 (sector count, hardcoded)
#   3 x u32 session-best sectors
#   u8  cvar8 (race-context flag)
#   u16 entry_count
#   per-car records...
#   u8 tail1 (= cvar8)
#   u8 tail2 (= 0)
# Earlier coverage was differential vs kunos pcap only; this test
# walks the prefix + count + tail in isolation to catch any future
# regression in the outer wrapper without needing a wine capture.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot
PCAP=/tmp/accd_0x36.pcap

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
    --race 911 --grid 1 --name "Bot36" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

# Let the bot run long enough to fire standings deltas (handshake,
# session_start, sector-1 split).  ~15 s suffices.
sleep 15

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
for p in rdpcap("/tmp/accd_0x36.pcap"):
    if TCP not in p or Raw not in p: continue
    if p[TCP].sport != 9302: continue
    streams.setdefault(p[TCP].dport, []).append(
        (p[TCP].seq, bytes(p[Raw].load)))

if not streams:
    print("FAIL: no accd->bot TCP segments captured"); sys.exit(1)

dport, segs = max(streams.items(), key=lambda kv: len(kv[1]))
segs.sort()
stream = b"".join(b for _, b in segs)

# Walk u16-length-prefixed kunos frames
LAP_TIME_INVALID = 0x7FFFFFFF
frames_36 = []
off = 0
while off + 2 <= len(stream):
    n = struct.unpack("<H", stream[off:off+2])[0]
    if off + 2 + n > len(stream): break
    body = stream[off+2:off+2+n]
    if body and body[0] == 0x36:
        frames_36.append(body)
    off += 2 + n

print(f"  0x36 frames observed: {len(frames_36)}")
if not frames_36:
    print("FAIL: no 0x36 broadcast within 15 s"); sys.exit(1)

# Walk the first 0x36's prefix
b = frames_36[-1]   # use latest, most-populated frame
print(f"  latest body len: {len(b)}")
p = 1   # skip msg_id
if p + 4 > len(b): print("FAIL: best_lap u32 truncated"); sys.exit(2)
best_lap = struct.unpack("<I", b[p:p+4])[0]; p += 4
print(f"  session_best_lap_ms: 0x{best_lap:08x}"
      f" ({'sentinel' if best_lap == LAP_TIME_INVALID else best_lap})")

if p + 1 > len(b) or b[p] != 3:
    print(f"FAIL: expected sector_count=3, got {b[p]}"); sys.exit(3)
p += 1
print("  PASS: sector_count == 3")

sectors = []
for i in range(3):
    sectors.append(struct.unpack("<I", b[p:p+4])[0]); p += 4
print(f"  best_sectors_ms: {[hex(x) for x in sectors]}")

if p + 1 > len(b): print("FAIL: cvar8 truncated"); sys.exit(4)
cvar8 = b[p]; p += 1
print(f"  cvar8: {cvar8}")

if p + 2 > len(b): print("FAIL: entry_count truncated"); sys.exit(5)
nc = struct.unpack("<H", b[p:p+2])[0]; p += 2
print(f"  entry_count: {nc}")
if nc != 1:
    print(f"FAIL: expected 1 entry, got {nc}"); sys.exit(6)

# Per-car record is complex; the car_id is the first u16 of the
# record (handshake.c:758 wr_u16(ec->car_id)), so 1001 -> LE 'e9 03'.
if b"\xe9\x03" in b[p:p+8]:
    print("  PASS: car_id 1001 (LE e9 03) at start of first per-car record")
else:
    print(f"FAIL: car_id 1001 not at start of per-car block "
          f"(first 8 B: {b[p:p+8].hex()})")
    sys.exit(7)

# Tail bytes: byte[-1] should be 0, byte[-2] should equal cvar8
if len(b) < 2:
    print("FAIL: body too short for tail bytes"); sys.exit(8)
t1, t2 = b[-2], b[-1]
print(f"  tail bytes: t1=0x{t1:02x}, t2=0x{t2:02x}")
if t2 != 0:
    print(f"FAIL: tail2 = {t2}, expected 0"); sys.exit(9)
if t1 != cvar8:
    print(f"FAIL: tail1 = {t1}, expected cvar8 = {cvar8}"); sys.exit(10)
print("RESULT: PASS (0x36 prefix + count + per-car + tail all decode cleanly)")
PY
