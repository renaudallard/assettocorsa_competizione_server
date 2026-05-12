#!/bin/sh
# Driver stint reset (0x4f) relay regression.
# Bot1 emits ACP_DRIVER_STINT_RESET (0x4f) with force=0 at tick 200;
# server relays SRV_DRIVER_STINT_RELAY to peer bot2 with body
# `u8 0x4f + u16 car_id + u8 force` (force=0 path, 4-byte body).
#
# Kunos's dispatcher case 0x4f reads u8 force + u64 ts and emits a
# SwapState relay; accd's h_driver_stint_reset mirrors the same wire.
# Compare the first 0x4f frame sent to bot2 between the two servers.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name BotStint --stint-reset 200:0"
BOT2="--race 922 --grid 2 --name BotPeer"
TEST_DURATION=30

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT1\" \"$BOT2\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_stint.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_stint.pcap

echo "==> diff"
rm -f accd_stint.legacy.pcap kunos_stint.legacy.pcap
editcap -F pcap accd_stint.pcap accd_stint.legacy.pcap
editcap -F pcap kunos_stint.pcap kunos_stint.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_stint.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_stint.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x4f]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x4f]

print(f'accd 0x4f frames: {len(af)}; lengths {[len(b) for b in af]}')
print(f'kunos 0x4f frames: {len(kf)}; lengths {[len(b) for b in kf]}')

if not af or not kf:
    print('FAIL NO_FRAMES')
    sys.exit(1)

a, k = af[0], kf[0]
print(f'accd[0]:  {a.hex()}')
print(f'kunos[0]: {k.hex()}')
if a == k:
    print('RESULT: IDENTICAL (0x4f relay byte-exact)')
else:
    print(f'RESULT: DIFFER ({sum(1 for j in range(min(len(a),len(k))) if a[j]!=k[j])} byte diff)')
    sys.exit(2)
"
