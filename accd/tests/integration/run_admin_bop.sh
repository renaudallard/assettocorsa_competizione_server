#!/bin/sh
# /ballast + /restrictor regression.  Bot elevates with /admin then
# issues "/ballast 911 50" (assign 50 kg) and "/restrictor 911 10"
# (10 % restrictor).  accd's chat_do_bop emits 0x53 SRV_BOP_UPDATE
# with wire body u16 car_id + u16 restrictor_pct + u32 ballast_kg
# (total 9 B), broadcasts the "Assigned ... to car #..." banner.
#
# Pins the (restrictor_pct, ballast_kg) wire order + types — a real
# regression we fixed in v0.3.39 where the two fields were flipped
# and typed wrong (chat.c:173 comment).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotBOP --chat-start-tick 60 \
    --chat /admin_admin --chat /ballast_911_50 --chat /restrictor_911_10"
TEST_DURATION=14

echo "==> accd + bot, /admin then /ballast + /restrictor"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_admin_bop.pcap

echo "==> diff"
rm -f accd_admin_bop.legacy.pcap
editcap -F pcap accd_admin_bop.pcap accd_admin_bop.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_bop.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x53]
print(f'0x53 SRV_BOP_UPDATE frames: {len(frames)} sizes {sorted({len(b) for b in frames})}')
if len(frames) < 2:
    print(f'FAIL: expected >= 2 0x53 frames (one per /ballast, /restrictor); got {len(frames)}')
    sys.exit(1)
if not all(len(b) == 9 for b in frames):
    print(f'FAIL: 0x53 wrong size; got {sorted({len(b) for b in frames})}')
    sys.exit(2)

decoded = []
for b in frames:
    car = b[1] | (b[2] << 8)
    rest = b[3] | (b[4] << 8)
    bal = b[5] | (b[6] << 8) | (b[7] << 16) | (b[8] << 24)
    decoded.append((car, rest, bal))
print(f'(car_id, restrictor_pct, ballast_kg) per frame: {decoded}')

# After /ballast 911 50 -> (1001, 0, 50)
# After /restrictor 911 10 -> (1001, 10, 50)
need_ballast = any(c==1001 and r==0  and b==50 for c,r,b in decoded)
need_rest    = any(c==1001 and r==10 and b==50 for c,r,b in decoded)
if not need_ballast:
    print('FAIL: no frame matching /ballast 911 50 -> (1001, 0, 50)')
    sys.exit(3)
if not need_rest:
    print('FAIL: no frame matching /restrictor 911 10 -> (1001, 10, 50)')
    sys.exit(4)

print('RESULT: PASS (0x53 BoP wire carries u16 restrictor_pct + u32 ballast_kg)')
"
