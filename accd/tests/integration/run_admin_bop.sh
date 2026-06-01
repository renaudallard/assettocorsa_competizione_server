#!/bin/sh
# /ballast + /restrictor regression.  Bot elevates with /admin then
# issues "/ballast 911 50" (assign 50 kg) and "/restrictor 911 10"
# (10 % restrictor).  accd's chat_do_bop emits 0x53 SRV_BOP_UPDATE
# with wire body u16 car_id + u16 ballast(signed kg) + f32 restrictor
# (fraction 0.0-0.2) — total 9 B — and broadcasts the "Assigned ..."
# banner.
#
# Pins the wire order + types: ballast BEFORE restrictor, restrictor as
# an IEEE-754 float fraction (NOT restrictor*100 as a u16).  Verified
# against exe FUN_14011d7d0 / FUN_1434f4ba0 and a kunos 0x53 pcap probe
# (car=1001, ballast u16, restrictor f32 0.1 = bytes cd cc cc 3d).  An
# earlier fix (v0.3.39) had the two fields swapped + mis-typed; this
# guards the corrected layout.  Note: the exe clamps ballast to +/-40 kg
# (separate value issue), so a strict byte compare vs kunos differs only
# on the ballast field.
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

echo "==> decode 0x53"
rm -f accd_admin_bop.legacy.pcap
editcap -F pcap accd_admin_bop.pcap accd_admin_bop.legacy.pcap

python3 -c "
import sys, struct
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

# Wire: u8 0x53 + u16 car_id + i16 ballast + f32 restrictor-fraction.
decoded = []
for b in frames:
    car = struct.unpack_from('<H', b, 1)[0]
    ballast = struct.unpack_from('<h', b, 3)[0]
    restrictor = struct.unpack_from('<f', b, 5)[0]
    decoded.append((car, ballast, round(restrictor, 4)))
print(f'(car_id, ballast_kg, restrictor_frac) per frame: {decoded}')

# After /ballast 911 50 -> (1001, 50, 0.0); restrictor not yet set.
# After /restrictor 911 10 -> (1001, 50, 0.1) as a float (NOT 10).
need_ballast = any(c==1001 and bal==50 and r==0.0 for c,bal,r in decoded)
need_rest    = any(c==1001 and bal==50 and abs(r-0.1) < 1e-4 for c,bal,r in decoded)
if not need_ballast:
    print('FAIL: no frame matching /ballast 911 50 -> (1001, ballast=50, restrictor=0.0)')
    sys.exit(3)
if not need_rest:
    print('FAIL: no frame matching /restrictor 911 10 -> (1001, ballast=50, restrictor=0.1f)')
    sys.exit(4)

print('RESULT: PASS (0x53 BoP wire = u16 car_id + u16 ballast + f32 restrictor-fraction)')
"
