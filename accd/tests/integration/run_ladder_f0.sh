#!/bin/sh
# Force=0 ladder escalation test.  cat=3 (PitSpeeding) is force=0 per
# the dispatcher (FUN_1400142f0: force = server_flag && cat==0).
# With force=0, the step from DT lands on SG30 (bVar6 = (0+2)*2 = 4),
# then SG30 is terminal (force=0 doesn't escalate to DQ).
#
# Expected: kunos tail = 0a (SG30 wire for cat=3, PitSpeeding).
# accd tail should match after the per-category ladder fix.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotLadderF0 \
    --report-penalty 3:1:3 \
    --report-penalty 3:2:3 \
    --report-penalty 3:3:3 \
    --report-penalty 3:4:3"
TEST_DURATION=30

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_ladder_f0.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_ladder_f0.pcap

echo "==> diff"
rm -f accd_ladder_f0.legacy.pcap kunos_ladder_f0.legacy.pcap
editcap -F pcap accd_ladder_f0.pcap accd_ladder_f0.legacy.pcap
editcap -F pcap kunos_ladder_f0.pcap kunos_ladder_f0.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_ladder_f0.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_ladder_f0.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]

if not af or not kf:
    print('FAIL NO_FRAMES accd=%d kunos=%d' % (len(af), len(kf)))
    sys.exit(1)

a, k = af[-1], kf[-1]
ta, tk = a[-4:].hex(), k[-4:].hex()
print(f'accd_tail={ta} kunos_tail={tk}')
if a == k:
    print('RESULT: IDENTICAL (force=0 ladder DT->SG30 byte-exact)')
else:
    print(f'RESULT: DIFFER ({sum(1 for j in range(min(len(a),len(k))) if a[j]!=k[j])} bytes)')
    sys.exit(2)
"
