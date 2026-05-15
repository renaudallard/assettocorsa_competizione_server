#!/bin/sh
# /lockprep + /unlockprep regression.  Admin can toggle
# preparation_locked, which gates fresh handshakes during
# PHASE_FORMATION / PHASE_PRE_SESSION (handshake.c:2309).
#
# This test pins the chat banners + log lines.  A full e2e test that
# would also assert the rejection of a mid-prep join is heavier and
# left to run_mid_race_join.sh's pattern.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotPrep --chat-start-tick 60 \
    --chat /admin_admin --chat /lockprep --chat /unlockprep"
TEST_DURATION=14

echo "==> accd + bot, /admin then /lockprep + /unlockprep"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_admin_lockprep.pcap

if ! grep -q 'admin: /lockprep'   accd.log; then echo "FAIL: no /lockprep log";   exit 1; fi
if ! grep -q 'admin: /unlockprep' accd.log; then echo "FAIL: no /unlockprep log"; exit 2; fi
echo "  /lockprep + /unlockprep both logged"

rm -f accd_admin_lockprep.legacy.pcap
editcap -F pcap accd_admin_lockprep.pcap accd_admin_lockprep.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_lockprep.legacy.pcap', 9302)
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

needles = ('Preparation phase is now LOCKED',
           'Preparation phase is now OPEN')
seen = set()
for b in chat:
    s, off = rd_str_a(b, 1)
    body, off = rd_str_a(b, off) if s else (None, 0)
    if body is None: continue
    for n in needles:
        if n in body:
            seen.add(n)
print(f'banner needles matched: {seen}')
if seen != set(needles):
    print(f'FAIL: missing {set(needles) - seen}')
    sys.exit(1)
print('RESULT: PASS (/lockprep + /unlockprep emit the LOCKED + OPEN banners)')
"
