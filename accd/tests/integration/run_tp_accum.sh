#!/bin/sh
# TP counter accumulation -> DQ.
# Per FUN_140125f50, TP penalties accumulate `value` seconds on the
# TP sheet; once the sum reaches 256 the car is force-DQ'd.  Send 6
# TP value=50 reports (cat=0 kind=5 value=50) so counter goes
# 50,100,150,200,250,300 — the 6th crosses the threshold.
# Expected: both servers' final 0x36 tail shows the DQ wire (05 for
# Cutting), not the TP wire (0e).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotTPAccum \
    --report-penalty 0:5:50 \
    --report-penalty 0:5:50 \
    --report-penalty 0:5:50 \
    --report-penalty 0:5:50 \
    --report-penalty 0:5:50 \
    --report-penalty 0:5:50"
TEST_DURATION=30

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_tp_accum.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_tp_accum.pcap

echo "==> diff"
rm -f accd_tp_accum.legacy.pcap kunos_tp_accum.legacy.pcap
editcap -F pcap accd_tp_accum.pcap accd_tp_accum.legacy.pcap
editcap -F pcap kunos_tp_accum.pcap kunos_tp_accum.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_tp_accum.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_tp_accum.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]

if not af or not kf:
    print('FAIL NO_FRAMES accd=%d kunos=%d' % (len(af), len(kf)))
    sys.exit(1)

a, k = af[-1], kf[-1]
ta, tk = a[-4:].hex(), k[-4:].hex()
print(f'accd_tail={ta} kunos_tail={tk}')
# Assert on the tail bytes — this test's stated purpose is to
# verify the post-accumulation DQ wire+value byte (last 4 B).
# Full-frame compare would trip on the intentional lap_count
# byte at offset 178 (see memory:reference_lap_scoring_rules.md
# — accd ticks lap_count for the formation crossing to match
# the real ACC client's HUD; kunos's exe doesn't).
if ta == tk:
    print('RESULT: IDENTICAL (TP accumulation DQ tail byte-exact)')
else:
    print(f'RESULT: DIFFER tail bytes accd={ta} kunos={tk}')
    sys.exit(2)
"
