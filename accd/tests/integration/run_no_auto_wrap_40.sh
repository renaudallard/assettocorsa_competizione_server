#!/bin/sh
# Auto-wrap-must-not-emit-0x40 regression.
#
# Kunos's 81-min replay saw 8 x 0x3e session-results and 0 x 0x40
# on the natural session-loop wrap (accd/session.c:1207 comment).
# 0x40 SRV_RACE_WEEKEND_RESET is reserved for the admin /resetWeekend
# command (covered by run_admin_reset.sh).  This test runs a single
# 1-min Quali session, lets accd run through OVERTIME -> COMPLETED ->
# wrap back to Quali, and asserts:
#   - >= 1 0x3e session-results frame is emitted
#   - exactly 0 x 0x40 frames are emitted
#
# Slow test (~90 s).  Skip via SKIP=1 in CI smoke runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tmp/bot/bot
PCAP=/tmp/accd_no_auto_wrap.pcap

ACCD_PID=""
BOT_PIDS=""
DPID=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
    [ -n "$DPID" ] && sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
    for f in cfg/settings.json.bak cfg/event.json.bak; do
        [ -f "$f" ] || continue
        mv "$f" "${f%.bak}" || true
    done
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT
for f in cfg/*.bak; do
    [ -f "$f" ] || continue
    mv "$f" "${f%.bak}"
done

cp cfg/settings.json cfg/settings.json.bak
cp cfg/event.json    cfg/event.json.bak
cp cfg_phase_collapse/local/settings.json cfg/settings.json
cp cfg_phase_collapse/local/event.json    cfg/event.json

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
    --race 911 --grid 1 --name "BotWrap" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

echo "==> waiting through full Q -> OT -> COMPLETED -> wrap cycle (~85 s)"
sleep 85

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
sleep 1
sudo -n chown "$(id -u):$(id -g)" "$PCAP" 2>/dev/null || true

# Verify accd actually traversed the wrap internally
if ! grep -qE "session [0-9]+: .* -> COMPLETED" accd.log; then
    echo "FAIL: accd never entered PHASE_COMPLETED, test isn't exercising wrap"
    exit 1
fi
echo "  OK: accd entered COMPLETED at least once"

python3 - <<'PY'
import sys, struct
from scapy.all import rdpcap, TCP, Raw

streams = {}
for p in rdpcap("/tmp/accd_no_auto_wrap.pcap"):
    if TCP not in p or Raw not in p:
        continue
    if p[TCP].sport != 9302:
        continue
    streams.setdefault(p[TCP].dport, []).append(
        (p[TCP].seq, bytes(p[Raw].load)))

if not streams:
    print("FAIL: no TCP egress captured"); sys.exit(1)

# Pull all per-conn streams and tally frame types
counts = {0x3e: 0, 0x40: 0, 0x3a: 0}  # session-results / weekend / sector
for dport, segs in streams.items():
    segs.sort()
    stream = b"".join(b for _, b in segs)
    off = 0
    while off + 2 <= len(stream):
        n = struct.unpack("<H", stream[off:off+2])[0]
        if off + 2 + n > len(stream):
            break
        body = stream[off+2:off+2+n]
        if body:
            cmd = body[0]
            counts[cmd] = counts.get(cmd, 0) + 1
        off += 2 + n

print(f"  0x3e session-results frames: {counts.get(0x3e, 0)}")
print(f"  0x40 weekend-reset frames:   {counts.get(0x40, 0)}")

rc = 0
if counts.get(0x40, 0) != 0:
    print("FAIL: accd emitted 0x40 on natural session wrap"
          " (expected 0, kunos's 81-min replay never emitted it)")
    rc = 2
if counts.get(0x3e, 0) < 1:
    print(f"FAIL: expected >= 1 0x3e session-results frame, got "
          f"{counts.get(0x3e, 0)}")
    rc = 3
if rc == 0:
    print("RESULT: PASS (no 0x40 on auto-wrap, >= 1 0x3e emitted)")
sys.exit(rc)
PY
