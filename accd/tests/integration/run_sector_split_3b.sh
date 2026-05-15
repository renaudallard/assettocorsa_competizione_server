#!/bin/sh
# 0x3b SRV_SECTOR_SPLIT_RELAY regression.  Bot emits 0x21
# ACP_SECTOR_SPLIT (single) at every S/F crossing.  accd's
# h_sector_split_single transforms to 0x3b SRV_SECTOR_SPLIT_RELAY
# with body: u8 0x3b + u16 car_id + u32 split + u8 flag + u32 lap +
# u16 flags = 14 B.
#
# Two bots; the second one's TCP stream from accd must contain the
# 0x3b relay frames carrying the first bot's lap data.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name BotS"
BOT2="--race 912 --grid 2 --name BotT"
TEST_DURATION=40

echo "==> accd + 2 bots, drive until at least one lap completes"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_sector_split_3b.pcap

echo "==> diff"
rm -f accd_sector_split_3b.legacy.pcap
editcap -F pcap accd_sector_split_3b.pcap accd_sector_split_3b.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_sector_split_3b.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x3b]
print(f'0x3b frames: {len(frames)} sizes {sorted({len(b) for b in frames})}')
if not frames:
    print('FAIL: no 0x3b relay frames')
    sys.exit(1)

# Expected size: 1 + 2 + 4 + 1 + 4 + 2 = 14 bytes.
if not all(len(b) == 14 for b in frames):
    print(f'FAIL: 0x3b wrong size; got {sorted({len(b) for b in frames})}')
    sys.exit(2)

cars = sorted({b[1] | (b[2] << 8) for b in frames})
print(f'observed car_ids in 0x3b relays: {cars}')
if not all(c >= 1001 for c in cars):
    print(f'FAIL: bad car_ids in 0x3b: {cars}')
    sys.exit(3)

print(f'RESULT: PASS ({len(frames)} 0x3b relay frames, 14 B each)')
"
