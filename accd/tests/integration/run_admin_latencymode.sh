#!/bin/sh
# /latencymode regression.  Admin sets s->latency_mode (0 or 1);
# valid values broadcast "Latency mode: <n>", invalid values
# broadcast "unknown latency mode <n>" or a usage hint.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotLM --chat-start-tick 60 \
    --chat /admin_admin --chat /latencymode_1 --chat /latencymode_0 \
    --chat /latencymode_5"
TEST_DURATION=14

echo "==> accd + bot, /admin + /latencymode {1,0,5}"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_admin_latencymode.pcap

rm -f accd_admin_latencymode.legacy.pcap
editcap -F pcap accd_admin_latencymode.pcap accd_admin_latencymode.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_latencymode.legacy.pcap', 9302)
chat = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x2b]
def rd_str_a(buf, off):
    if off >= len(buf): return None, off
    n = buf[off]; off += 1
    cps = []
    for _ in range(n):
        if off + 4 > len(buf): return None, off
        cps.append(buf[off] | (buf[off+1]<<8) | (buf[off+2]<<16) | (buf[off+3]<<24))
        off += 4
    return ''.join(chr(c) for c in cps), off

needles = ('Latency mode: 1', 'Latency mode: 0', 'unknown latency mode 5')
seen = set()
for b in chat:
    s, off = rd_str_a(b, 1)
    body, off = rd_str_a(b, off) if s else (None, 0)
    if body is None: continue
    for n in needles:
        if n in body: seen.add(n)
print(f'matched: {sorted(seen)}')
if seen != set(needles):
    print(f'FAIL: missing {set(needles) - seen}')
    sys.exit(1)
print('RESULT: PASS (/latencymode accepts 0/1 and rejects out-of-range)')
"
