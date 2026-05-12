#!/bin/sh
# Driver-swap state (0x47) broadcast regression (accd-only).
# Bot fires ACP_UPDATE_DRIVER_SWAP_STATE (0x47) at tick 10 with one
# driver slot in state=2 (CONNECTED).  Server should broadcast
# SRV_DRIVER_SWAP_STATE_BCAST (same 0x47).
#
# Wire body: u8 0x47 + u16 car_id + u8 driver_count + dcnt × u8 state
#
# Cross-server byte parity is not asserted here: kunos's exe only
# rebroadcasts under multi-driver entrylist scenarios (FUN_140020380
# iterates conns matching the car_id; without entrylist there's
# nothing to broadcast to but the originator).  Wine's CPU starvation
# also tends to drop the 0x47 in transit.  Validate accd's own emit
# structurally instead.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotSwap --swap-state 10:2"
TEST_DURATION=10

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_swap.pcap

echo "==> diff"
rm -f accd_swap.legacy.pcap
editcap -F pcap accd_swap.pcap accd_swap.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_swap.legacy.pcap', 9302)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x47]

print(f'accd 0x47 frames: {len(af)}; lens {[len(b) for b in af]}')
if not af:
    print('FAIL: accd did not broadcast 0x47')
    sys.exit(1)
b = af[-1]
print(f'accd[last]: {b.hex()}')
# Expect msg_id + u16 car_id + u8 dcnt=1 + u8 state=2 = 5 bytes
if len(b) != 5:
    print(f'FAIL: expected 5-byte broadcast, got {len(b)}')
    sys.exit(2)
if b[0] != 0x47:
    print(f'FAIL: msg id wrong: {b[0]:02x}')
    sys.exit(2)
if b[1] != 0xe9 or b[2] != 0x03:
    print(f'FAIL: car_id != 1001 (got {b[1]:02x}{b[2]:02x})')
    sys.exit(2)
if b[3] != 1:
    print(f'FAIL: dcnt != 1 (got {b[3]})')
    sys.exit(2)
if b[4] != 2:
    print(f'FAIL: state != 2 (got {b[4]})')
    sys.exit(2)
print('RESULT: VALID (5-B 0x47 + car=1001 + dcnt=1 + state=2)')
"
