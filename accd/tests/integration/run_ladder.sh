#!/bin/sh
# Same-category ladder escalation regression.
# Bot sends DT/SG10/SG20/SG30 sequentially for cat=0 (Cutting).
# Kunos's FUN_140125f50 runs a per-CATEGORY ladder that escalates
# repeated cat=0 reports through DT -> SG10 -> SG20 -> SG30 -> DQ;
# accd runs a per-EXE_KIND ladder where each kind has its own sheet,
# so successive different-kind reports DON'T escalate.
#
# Expected (per matrix_results.tsv FAIL Scenario B):
#   kunos tail = 05 03 (DQ wire for cutting, value=3)
#   accd  tail = wire of the LAST kind sent (07 03 for SG20, etc.)
#
# This test ENCODES the known divergence so a future fix can flip
# RESULT from DIFFER to IDENTICAL automatically.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotLadder \
    --report-penalty 0:1:3 \
    --report-penalty 0:2:3 \
    --report-penalty 0:3:3 \
    --report-penalty 0:4:3"
TEST_DURATION=30

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_ladder.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_ladder.pcap

echo "==> diff"
rm -f accd_ladder.legacy.pcap kunos_ladder.legacy.pcap
editcap -F pcap accd_ladder.pcap accd_ladder.legacy.pcap
editcap -F pcap kunos_ladder.pcap kunos_ladder.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_ladder.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_ladder.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]

if not af or not kf:
    print('FAIL NO_FRAMES accd=%d kunos=%d' % (len(af), len(kf)))
    sys.exit(1)

a, k = af[-1], kf[-1]
ta, tk = a[-4:].hex(), k[-4:].hex()
print(f'accd_tail={ta} kunos_tail={tk}')
if a == k:
    print('RESULT: IDENTICAL (ladder escalation now matches kunos)')
else:
    print('RESULT: DIFFER (expected: known per-exe_kind vs per-category divergence)')
    sys.exit(2)
"
