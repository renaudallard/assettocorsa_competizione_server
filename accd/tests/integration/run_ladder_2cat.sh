#!/bin/sh
# Cross-category ladder escalation test.
# Bot sends a DT for cat=0 (Cutting, force=1) then a DT for cat=3
# (PitSpeeding, force=0).  Kunos keys the PenaltySheet per-car so the
# second report escalates the existing entry rather than creating a new
# one.  Expected final tail: SG30 + REASON_CUTTING = wire 04, value 0.
#
# The previous per-category ladder would materialise a second DT for
# cat=3 (fresh entry because pen_cat_severity[3]==0) and produce tail
# 07 03 (DT+PitSpeeding wire=07, value=3) — a visible divergence from
# kunos.  This test catches any regression to that behaviour.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotLadder2Cat \
    --report-penalty 0:1:3 \
    --report-penalty 3:1:3"
TEST_DURATION=30

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_ladder_2cat.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_ladder_2cat.pcap

echo "==> diff"
rm -f accd_ladder_2cat.legacy.pcap kunos_ladder_2cat.legacy.pcap
editcap -F pcap accd_ladder_2cat.pcap accd_ladder_2cat.legacy.pcap
editcap -F pcap kunos_ladder_2cat.pcap kunos_ladder_2cat.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_ladder_2cat.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_ladder_2cat.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]

if not af or not kf:
    print('FAIL NO_FRAMES accd=%d kunos=%d' % (len(af), len(kf)))
    sys.exit(1)

a, k = af[-1], kf[-1]
ta, tk = a[-4:].hex(), k[-4:].hex()
print(f'accd_tail={ta} kunos_tail={tk}')
if ta == tk:
    print('RESULT: IDENTICAL (cross-category ladder tail byte-exact)')
else:
    print(f'RESULT: DIFFER tail bytes accd={ta} kunos={tk}')
    sys.exit(2)
"
