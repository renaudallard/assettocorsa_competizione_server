#!/bin/sh
# Garage 0x55 ACP_LOAD_SETUP -> 0x56 SRV_SETUP_DATA_RESPONSE.
# Bot fires 0x55 at tick 100 (3.3 s into session, before any lap
# completes) requesting the lap history for session_type 0 (Practice).
# Both servers should reply with a 0x56 carrying lap_count=0 (no
# laps recorded yet).  Compare the reply bytes.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotGarage --load-setup 100:0"
TEST_DURATION=15

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_garage.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_garage.pcap

echo "==> diff"
rm -f accd_garage.legacy.pcap kunos_garage.legacy.pcap
editcap -F pcap accd_garage.pcap accd_garage.legacy.pcap
editcap -F pcap kunos_garage.pcap kunos_garage.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_garage.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_garage.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x56]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x56]

print(f'accd 0x56 frames: {len(af)}; lengths {[len(b) for b in af]}')
print(f'kunos 0x56 frames: {len(kf)}; lengths {[len(b) for b in kf]}')

if not af or not kf:
    print('FAIL NO_FRAMES')
    sys.exit(1)

a, k = af[0], kf[0]
print(f'accd[0]:  {a.hex()}')
print(f'kunos[0]: {k.hex()}')
if a == k:
    print('RESULT: IDENTICAL (0x56 empty-history reply byte-exact)')
else:
    diffs = sum(1 for j in range(min(len(a),len(k))) if a[j]!=k[j])
    print(f'RESULT: DIFFER ({diffs} byte diff, a={len(a)} k={len(k)})')
    sys.exit(2)
"
