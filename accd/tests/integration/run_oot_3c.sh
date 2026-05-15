#!/bin/sh
# 0x3d ACP_OUT_OF_TRACK -> 0x3c SRV_OUT_OF_TRACK_RELAY regression.
# Bot sends 0x3d with force=0 at tick T via --oot-at; accd's
# h_out_of_track latches the cut, increments cuts_this_lap, and
# relays 0x3c body: u8 + u16 car_id + u16 cuts + u32 ts = 9 B.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name BotO --oot-at 90"
BOT2="--race 912 --grid 2 --name BotP"
TEST_DURATION=8

echo "==> accd + 2 bots, bot1 emits 0x3d at tick 90"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_oot_3c.pcap

rm -f accd_oot_3c.legacy.pcap
editcap -F pcap accd_oot_3c.pcap accd_oot_3c.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_oot_3c.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x3c]
print(f'0x3c frames: {len(frames)} sizes {sorted({len(b) for b in frames})}')
if not frames:
    print('FAIL: no 0x3c relay frames')
    sys.exit(1)
if not all(len(b) == 9 for b in frames):
    print(f'FAIL: 0x3c wrong size {sorted({len(b) for b in frames})}')
    sys.exit(2)

for b in frames[:3]:
    car = b[1] | (b[2] << 8)
    cuts = b[3] | (b[4] << 8)
    print(f'  car_id={car} cuts={cuts}')

# bot1 is car_id 1001 (ACC_CAR_ID_BASE + 0).  Expect cuts=1 after the
# single 0x3d emit (the latch debounce gates duplicates).
hits = [b for b in frames if (b[1] | (b[2]<<8)) == 1001 and (b[3]|(b[4]<<8)) >= 1]
if not hits:
    print('FAIL: no 0x3c relay for car_id 1001 cuts>=1')
    sys.exit(3)
print('RESULT: PASS (0x3d -> 0x3c latched, cuts incremented)')
"
