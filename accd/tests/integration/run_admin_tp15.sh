#!/bin/sh
# Admin /tp15 <raceNumber> — server adds a 15-second time penalty to
# a car.  Verify both servers' 0x36 tail shows the TP wire (14) and
# value=15.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotAdminTP --chat-start-tick 60 --chat /admin_admin --chat /tp15_911"
TEST_DURATION=30

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_admin_tp15.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_admin_tp15.pcap

echo "==> diff"
rm -f accd_admin_tp15.legacy.pcap kunos_admin_tp15.legacy.pcap
editcap -F pcap accd_admin_tp15.pcap accd_admin_tp15.legacy.pcap
editcap -F pcap kunos_admin_tp15.pcap kunos_admin_tp15.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_tp15.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_admin_tp15.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]

print(f'accd 0x36: {len(af)} tails {[b[-4:].hex() for b in af]}')
print(f'kunos 0x36: {len(kf)} tails {[b[-4:].hex() for b in kf]}')
if af and kf:
    a, k = af[-1], kf[-1]
    if a == k:
        print('LAST 0x36: IDENTICAL')
    else:
        d = sum(1 for j in range(min(len(a), len(k))) if a[j] != k[j])
        print(f'LAST 0x36: DIFFER ({d} bytes, a={len(a)} k={len(k)})')
"
