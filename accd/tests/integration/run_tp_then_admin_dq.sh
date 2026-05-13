#!/bin/sh
# TP-accumulation auto-DQ followed by admin /dq on the same category.
#
# Regression test for v0.3.44 fix `ccdb6ad` (penalty: mark category
# as terminal when TP accumulation auto-escalates to DQ).  When six
# value=50 TP reports cross the 256 s threshold for cat=0 the server
# materialises a RACE_CONTROL DQ; without the fix, pen_cat_severity
# for cat=0 was left blank, so a subsequent /dq on the same category
# bypassed the dedup guard at penalty.c:228 and queued a SECOND DQ.
# Result on the wire: the leaderboard's 0x36 tail / chat broadcast
# count would diverge from kunos.
#
# With the fix accd matches kunos byte-for-byte: one DQ on the queue,
# one chat broadcast, one final 0x36 tail.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# chat-start-tick=240 (8 s) puts the /admin + /dq AFTER the bot's
# --report-penalty burst at tick 200, so the auto-DQ has already
# materialised by the time the admin command arrives.
BOT="--race 911 --grid 1 --name BotTPDedup \
    --report-penalty 0:5:50 \
    --report-penalty 0:5:50 \
    --report-penalty 0:5:50 \
    --report-penalty 0:5:50 \
    --report-penalty 0:5:50 \
    --report-penalty 0:5:50 \
    --chat-start-tick 240 \
    --chat /admin_admin \
    --chat /dq_911"
TEST_DURATION=30

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_tp_then_admin_dq.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_tp_then_admin_dq.pcap

echo "==> diff"
rm -f accd_tp_then_admin_dq.legacy.pcap kunos_tp_then_admin_dq.legacy.pcap
editcap -F pcap accd_tp_then_admin_dq.pcap accd_tp_then_admin_dq.legacy.pcap
editcap -F pcap kunos_tp_then_admin_dq.pcap kunos_tp_then_admin_dq.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_tp_then_admin_dq.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_tp_then_admin_dq.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]
ac = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x2b]
kc = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x2b]

print(f'accd  0x36={len(af)}  0x2b={len(ac)}')
print(f'kunos 0x36={len(kf)}  0x2b={len(kc)}')

# Both servers should emit exactly the same final-state leaderboard
# and the same number of chat broadcasts; a duplicate DQ on accd's
# side would show up as one extra 0x2b and/or a different 0x36 tail.
if not af or not kf:
    print('FAIL: missing 0x36 frames')
    sys.exit(1)
if len(ac) != len(kc):
    print(f'FAIL: chat count differs (accd={len(ac)} kunos={len(kc)}) — '
          'second DQ likely materialised')
    sys.exit(2)
a, k = af[-1], kf[-1]
if a == k:
    print('RESULT: IDENTICAL (TP-then-admin-DQ dedup byte-exact)')
else:
    diffs = sum(1 for j in range(min(len(a), len(k))) if a[j] != k[j])
    print(f'RESULT: DIFFER ({diffs} bytes, a={len(a)} k={len(k)})')
    sys.exit(3)
"
