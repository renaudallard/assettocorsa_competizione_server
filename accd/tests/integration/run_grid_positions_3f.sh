#!/bin/sh
# 0x3f SRV_GRID_POSITIONS regression.  Emitted by tick.c:579
# broadcast_grid() once at the start of the RACE phase.  Body:
# u8 0x3f + u8 grid_count + per-car (u16 carId + u8 flag_a + u32
# grid_pos + u8 flag_b) = 2 + 8*N bytes.
#
# Test: cfg_autodq (5-min Race) so green fires fast; assert one
# 0x3f frame appears.
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
cp cfg_autodq/local/settings.json cfg/settings.json
cp cfg_autodq/local/event.json    cfg/event.json

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

echo "==> waiting for green flag..."
for i in $(seq 1 90); do
    sleep 1
    if grep -qE 'green_fired|GREEN flag|green flag|PRE_SESSION -> SESSION' accd.log 2>/dev/null; then
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
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_grid_3f.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x3f]
print(f'0x3f frames: {len(frames)} sizes {sorted({len(b) for b in frames})}')
if not frames:
    print('FAIL: no 0x3f SRV_GRID_POSITIONS frame after green')
    sys.exit(1)

f = frames[0]
n = f[1]
print(f'grid_count={n}, frame_len={len(f)}, expected={2 + 8*n}')
if len(f) != 2 + 8 * n:
    print(f'FAIL: 0x3f size mismatch')
    sys.exit(2)

print('RESULT: PASS (0x3f emitted at race start with per-car records)')
"
