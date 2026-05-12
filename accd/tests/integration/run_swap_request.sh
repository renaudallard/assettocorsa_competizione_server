#!/bin/sh
# Driver-swap state request (0x4a) regression (accd-only).
# Bot fires ACP_DRIVER_SWAP_STATE_REQUEST (0x4a) at tick 10 with
# sub_state=2 (initiate), conn_state=3.  Server updates the carEntrys
# swap_state[current_driver] and broadcasts SRV_DRIVER_SWAP_STATE_BCAST
# (0x47) with the resulting per-driver state.
#
# Validates accds 0x47 body shape:
#   u8 0x47 + u16 car_id + u8 driver_count + dcnt × u8 state
#
# Cross-server byte parity isnt asserted: kunoss broadcaster requires
# multi-driver-on-same-car entries (FUN_140020380 iterates conns
# matching car_id), and wines CPU starvation reliably drops the
# 0x4a in transit.  Single-bot single-driver fires accds broadcast;
# kunos may skip.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotSwapReq --swap-request 10:2:3"
TEST_DURATION=10

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_swap_req.pcap

echo "==> diff"
rm -f accd_swap_req.legacy.pcap
editcap -F pcap accd_swap_req.pcap accd_swap_req.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_swap_req.legacy.pcap', 9302)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x47]
print(f'accd 0x47 frames: {len(af)}; lens {[len(b) for b in af]}')
if not af:
    print('FAIL: accd did not broadcast 0x47 in response to 0x4a')
    sys.exit(1)
b = af[-1]
print(f'accd[last]: {b.hex()}')
# Expect: 0x47 + u16 car_id 1001 + u8 dcnt=1 + u8 state=3 = 5 bytes
if len(b) != 5 or b[0] != 0x47:
    print(f'FAIL: unexpected frame layout (len={len(b)} id={b[0]:02x})')
    sys.exit(2)
if b[1] != 0xe9 or b[2] != 0x03:
    print(f'FAIL: car_id != 1001 (got {b[1]:02x}{b[2]:02x})')
    sys.exit(2)
if b[3] != 1:
    print(f'FAIL: dcnt != 1 (got {b[3]})')
    sys.exit(2)
if b[4] != 3:
    print(f'FAIL: state != 3 (got {b[4]})')
    sys.exit(2)
print('RESULT: VALID (0x4a -> 0x47 broadcast applied state=3 for driver[0])')
"
