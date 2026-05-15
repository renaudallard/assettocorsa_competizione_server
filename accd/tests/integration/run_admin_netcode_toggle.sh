#!/bin/sh
# /mp + /legacy + /regular regression.  All three commands map to
# the same single toggle on s->legacy_netcode (chat.c:1098 — the
# split into separate /legacy /regular was an early mistake fixed
# by mirroring exe behaviour).  Banner alternates between
# "Server now uses legacy netcode" and "Server is now in regular
# mode" with each invocation.
#
# accd's default at boot is legacy_netcode=1 (per state.c:163), so:
#   1st /mp -> 0 -> "regular mode"
#   2nd /mp -> 1 -> "legacy netcode"
#   3rd /mp -> 0 -> "regular mode"
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotNC --chat-start-tick 60 \
    --chat /admin_admin --chat /mp --chat /legacy --chat /regular"
TEST_DURATION=14

echo "==> accd + bot, /admin + /mp /legacy /regular"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_admin_netcode_toggle.pcap

n_mp=$(grep -c 'admin: /mp -> legacy_netcode' accd.log)
echo "  toggle log lines: $n_mp"
if [ "$n_mp" -ne 3 ]; then
    echo "FAIL: expected 3 /mp toggle lines, got $n_mp"
    exit 1
fi

rm -f accd_admin_netcode_toggle.legacy.pcap
editcap -F pcap accd_admin_netcode_toggle.pcap accd_admin_netcode_toggle.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_netcode_toggle.legacy.pcap', 9302)
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

# Default is legacy=1, so three toggles emit regular, legacy, regular.
seq = []
for b in chat:
    s, off = rd_str_a(b, 1)
    body, off = rd_str_a(b, off) if s else (None, 0)
    if body is None: continue
    if 'legacy netcode' in body:
        seq.append('legacy')
    elif 'regular mode' in body:
        seq.append('regular')
print(f'toggle sequence: {seq}')
if seq != ['regular', 'legacy', 'regular']:
    print(f'FAIL: expected [regular, legacy, regular], got {seq}')
    sys.exit(1)
print('RESULT: PASS (/mp /legacy /regular all map to the same toggle)')
"
