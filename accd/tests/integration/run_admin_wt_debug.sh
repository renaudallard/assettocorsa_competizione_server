#!/bin/sh
# /wt + /debug regression.  /wt dumps current weather snapshot in
# the "Standard weather: rain=N clouds=N ..." banner.  /debug
# <conditions|bandwidth|qos> toggles a server-local log-verbosity
# flag and replies with a "* are printed now" banner.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotDbg --chat-start-tick 60 \
    --chat /admin_admin --chat /wt --chat /debug_conditions \
    --chat /debug_bandwidth --chat /debug_qos"
TEST_DURATION=14

echo "==> accd + bot, /admin + /wt + /debug sub-flags"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_admin_wt_debug.pcap

if ! grep -q 'admin: /wt'       accd.log; then echo "FAIL: no /wt log";       exit 1; fi
if ! grep -q 'admin: /debug conditions' accd.log; then echo "FAIL: no /debug conditions log"; exit 2; fi
if ! grep -q 'admin: /debug bandwidth'  accd.log; then echo "FAIL: no /debug bandwidth log";  exit 3; fi
if ! grep -q 'admin: /debug qos'        accd.log; then echo "FAIL: no /debug qos log";        exit 4; fi
echo "  /wt + 3 /debug subs all logged"

rm -f accd_admin_wt_debug.legacy.pcap
editcap -F pcap accd_admin_wt_debug.pcap accd_admin_wt_debug.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_wt_debug.legacy.pcap', 9302)
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

needles = (
    'weather:',                             # /wt banner head
    'conditions are printed now',           # /debug conditions
    'bandwidth stats are printed now',      # /debug bandwidth
    'netcode stats are printed now',        # /debug qos
)
seen = set()
for b in chat:
    s, off = rd_str_a(b, 1)
    body, off = rd_str_a(b, off) if s else (None, 0)
    if body is None: continue
    for n in needles:
        if n in body: seen.add(n)
print(f'banners matched: {sorted(seen)}')
if seen != set(needles):
    print(f'FAIL: missing {set(needles) - seen}')
    sys.exit(1)
print('RESULT: PASS (/wt + /debug conditions/bandwidth/qos all replied)')
"
