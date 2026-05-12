#!/bin/sh
# Penalty serve via 0x42 ACP_PENALTY_CLEARED.
# Bot fires a 0x41 cat=0 kind=1 value=3 (Cutting DT), drives through
# the pit on lap 2, then emits 0x42 once it exits the pitlane.  Both
# servers should clear the DT from the per-car tail of the final 0x36.
#
# Kunos's FUN_140126b50 removes the queue entry on 0x42; accd's
# penalty_serve_front marks the entry served (same wire effect — both
# zero out the tail bytes).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotServe --report-penalty 0:1:3 \
    --length 800 --pit-on-lap 1 --send-penalty-served --no-mandatory-pit"
TEST_DURATION=90

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_serve_42.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_serve_42.pcap

echo "==> diff"
rm -f accd_serve_42.legacy.pcap kunos_serve_42.legacy.pcap
editcap -F pcap accd_serve_42.pcap accd_serve_42.legacy.pcap
editcap -F pcap kunos_serve_42.pcap kunos_serve_42.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_serve_42.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_serve_42.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]

if not af or not kf:
    print('FAIL NO_FRAMES accd=%d kunos=%d' % (len(af), len(kf)))
    sys.exit(1)

# Last frames per side
a, k = af[-1], kf[-1]
ta = a[-4:-2].hex()
tk = k[-4:-2].hex()
print(f'accd_tail={ta} kunos_tail={tk} accd_frames={len(af)} kunos_frames={len(kf)}')
if ta == '0000' and tk == '0000':
    print('RESULT: SERVED (both tails cleared after 0x42)')
elif ta == '0000':
    print(f'RESULT: accd-only cleared; kunos still shows {tk}')
    sys.exit(2)
elif tk == '0000':
    print(f'RESULT: kunos-only cleared; accd still shows {ta}')
    sys.exit(2)
else:
    print('RESULT: neither cleared')
    sys.exit(2)
"
