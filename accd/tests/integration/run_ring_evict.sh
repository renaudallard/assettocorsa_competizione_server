#!/bin/sh
# Ring-buffer eviction regression test.
# Bot sends 12 distinct penalties; ACC_MAX_PENALTIES=8 so the queue
# evicts the oldest 4.  Last 0x36 tail bytes must reflect the NEWEST
# penalty in both servers (kunos keeps single-entry overwrite, accd
# keeps a sliding window of 8; both expose the newest at the tail).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# 12 penalties spread across distinct (cat, kind) combos so each one
# has a different wire code; the last one (penalty 12) is cat=11
# kind=1 value=14 = PitSpeeding DT, wire code 0x07 per FUN_1400f03b0.
RP=""
for combo in \
    0:1:3 1:5:4 2:5:5 3:1:6 4:5:7 5:5:8 \
    6:5:9 7:5:10 8:1:11 9:5:12 10:5:13 11:1:14
do
    RP="$RP --report-penalty $combo"
done

BOT1="--race 911 --grid 1 --name BotRing $RP"
BOT2="--race 922 --grid 2 --name BotPeer"
TEST_DURATION=30

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT1\" \"$BOT2\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_ring.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_ring.pcap

echo "==> diff"
rm -f accd_ring.legacy.pcap kunos_ring.legacy.pcap
editcap -F pcap accd_ring.pcap accd_ring.legacy.pcap
editcap -F pcap kunos_ring.pcap kunos_ring.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_ring.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_ring.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]

if not af or not kf:
    print('FAIL NO_FRAMES accd=%d kunos=%d' % (len(af), len(kf)))
    sys.exit(1)

a, k = af[-1], kf[-1]
ta, tk = a[-4:].hex(), k[-4:].hex()
print(f'accd_tail={ta} kunos_tail={tk} accd_frames={len(af)} kunos_frames={len(kf)}')
if a == k:
    print('RESULT: IDENTICAL (last frame byte-exact)')
else:
    diffs = sum(1 for j in range(min(len(a), len(k))) if a[j] != k[j])
    print(f'RESULT: DIFFER ({diffs} byte differences in last frame)')
    sys.exit(2)
"
