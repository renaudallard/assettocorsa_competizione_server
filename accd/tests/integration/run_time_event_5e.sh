#!/bin/sh
# 0x5e UDP ACP_TIME_EVENT regression.  Bot sends one 0x5e UDP frame
# via --time-event T (source=target=self, latency=42 ms, chat=1).
# accd's dispatch.c:548 logs "0x5e latency report: <s> -> <t> = ..."
# and, when chat=1 and src!=NULL, relays a 0x2b "Latency error: N ms"
# message to the target conn.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotL --time-event 90"
TEST_DURATION=8

echo "==> accd + bot, 0x5e at tick 90"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_time_event_5e.pcap

if ! grep -q '0x5e latency report' accd.log; then
    echo "FAIL: accd.log has no 0x5e latency report line"
    tail -10 accd.log >&2
    exit 1
fi
echo "  '0x5e latency report' logged"
grep '0x5e latency report' accd.log | head -2 | sed 's/^/    /'

rm -f accd_time_event_5e.legacy.pcap
editcap -F pcap accd_time_event_5e.pcap accd_time_event_5e.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_time_event_5e.legacy.pcap', 9302)
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

found_latency = False
for b in chat:
    s, off = rd_str_a(b, 1)
    body, off = rd_str_a(b, off) if s else (None, 0)
    if body is None: continue
    if 'Latency error' in body:
        found_latency = True
        print(f'  found: {body!r}')
        break

if not found_latency:
    print('FAIL: no 0x2b with \"Latency error\" body')
    sys.exit(2)

print('RESULT: PASS (0x5e logged + Latency error 0x2b emitted)')
"
