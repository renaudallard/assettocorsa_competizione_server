#!/bin/sh
# 0x3f SRV_GRID_POSITIONS regression.  broadcast_grid() emits one
# frame when a race grid is derived from a preceding qualifying
# session.  Body: u8 0x3f + u8 grid_count + per-car (u16 carId +
# u8 flag_a + u32 grid_pos + u8 flag_b) = 2 + 8*N bytes.
#
# Test: cfg_grid_qr, a 1-min Qualifying followed by a 2-min Race, so
# the frame is actually produced.  A race with no qualifying ahead of
# it never sends one, which is why the old cfg_autodq (Race only)
# config could not exercise this path at all.
#
# Two assertions:
#   1. exactly one 0x3f frame, with a body length matching its count.
#   2. it goes out BEFORE the race session leaves WAITING.  The client
#      lays the grid out during the pre-race countdown and reads each
#      car's start slot and formation column from the record order, so
#      a frame that arrives later is useless to it.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot

ACCD_PID=""
BOT_PID=""
cleanup_on_exit() {
    rc=$?
    [ -n "$BOT_PID"  ] && kill -TERM "$BOT_PID"  2>/dev/null || true
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
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

pkill -KILL -f 'accd -c '       >/dev/null 2>&1 || true
pkill -KILL -f 'tools/bot/bot ' >/dev/null 2>&1 || true
sleep 1

cp cfg/settings.json cfg/settings.json.bak
cp cfg/event.json    cfg/event.json.bak
cp cfg_grid_qr/local/settings.json cfg/settings.json
cp cfg_grid_qr/local/event.json    cfg/event.json

PCAP_TMP=/tmp/penalty_diff_accd.pcap
sudo -n rm -f "$PCAP_TMP" 2>/dev/null || true
rm -f accd.log
sudo -n dumpcap -i lo -w "$PCAP_TMP" -f 'tcp port 9302 or udp port 9303' \
    -q >/dev/null 2>&1 &
TCPDUMP_PID=$!
sleep 1

$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "BotG" >bot1.log 2>&1 &
BOT_PID=$!

echo "==> waiting for the race to reach green (qualy runs first)..."
for i in $(seq 1 180); do
    sleep 1
    if grep -qE 'green flag' accd.log 2>/dev/null; then
        echo "  green fired after ${i}s"; break
    fi
done

sleep 2
kill -TERM "$BOT_PID"  2>/dev/null || true
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
sudo -n kill -TERM "$TCPDUMP_PID" 2>/dev/null || true
sleep 1
mv "$PCAP_TMP" accd_grid_3f.pcap 2>/dev/null || sudo -n cp "$PCAP_TMP" accd_grid_3f.pcap
sudo -n chown $(id -u):$(id -g) accd_grid_3f.pcap 2>/dev/null || true

rm -f accd_grid_3f.legacy.pcap
editcap -F pcap accd_grid_3f.pcap accd_grid_3f.legacy.pcap

python3 -c "
import re
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_grid_3f.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x3f]
print(f'0x3f frames: {len(frames)} sizes {sorted({len(b) for b in frames})}')
if not frames:
    print('FAIL: no 0x3f SRV_GRID_POSITIONS frame for a race behind a qualy')
    sys.exit(1)
if len(frames) != 1:
    print(f'FAIL: expected exactly one 0x3f frame, got {len(frames)}')
    sys.exit(2)

f = frames[0]
n = f[1]
print(f'grid_count={n}, frame_len={len(f)}, expected={2 + 8*n}')
if len(f) != 2 + 8 * n:
    print('FAIL: 0x3f size mismatch')
    sys.exit(3)

# The frame has to reach the client before it places the cars, which it
# does when the race session leaves WAITING.
log = open('accd.log', errors='replace').read().splitlines()
grid_at = next((i for i,l in enumerate(log) if 'Sending grid positions' in l), None)
race_si = next((re.search(r'session (\\d+): waiting for drivers \\(RACE\\)', l).group(1)
                for l in log if 'waiting for drivers (RACE)' in l), None)
if grid_at is None or race_si is None:
    print('FAIL: log has no grid broadcast or no race session start')
    sys.exit(4)
start_at = next((i for i,l in enumerate(log)
                 if f'session {race_si}: WAITING -> FORMATION' in l), None)
if start_at is None:
    print('FAIL: race session never left WAITING')
    sys.exit(5)
print(f'grid broadcast at log line {grid_at + 1}, '
      f'race WAITING -> FORMATION at line {start_at + 1}')
if grid_at > start_at:
    print('FAIL: 0x3f sent after the client had already laid out the grid')
    sys.exit(6)

print('RESULT: PASS (one 0x3f, correct size, sent before the race grid forms)')
"
