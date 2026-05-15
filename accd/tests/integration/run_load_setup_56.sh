#!/bin/sh
# 0x55 / 0x56 garage lap-history regression.  The bot sends a 0x55
# ACP_LOAD_SETUP for its own car at session_type 0 (Practice) and
# accd's h_load_setup must reply with a 0x56 SRV_SETUP_DATA_RESPONSE
# whose body shape matches notebook-b §5.6.4a:
#
#     u8  0x56
#     u8  session_index    (echoed from request)
#     u16 car_id           (echoed)
#     i16 lap_count        (count of Lap_records)
#     lap_count × Lap_record
#     trailing single-car leaderboard record (per FUN_1400328f0)
#
# We don't byte-diff against kunos (wine is flaky on the garage
# round-trip), but we pin the structural reply: the first 6 bytes are
# msg-id + echoed fields, and the frame is at least 6 + N bytes for
# any non-empty trailing record.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Drive long enough that the bot completes at least one lap so the
# 0x56 reply carries a Lap_record.  Issue 0x55 at tick 350 (well
# after the first lap-complete around tick 280-320 on the synthetic
# stadium loop at ~30 Hz).
BOT="--race 911 --grid 1 --name BotGarage --load-setup 350:0"
TEST_DURATION=22

echo "==> accd + bot, --load-setup 350:0"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_load_setup_56.pcap

echo "==> diff"
rm -f accd_load_setup_56.legacy.pcap
editcap -F pcap accd_load_setup_56.pcap accd_load_setup_56.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_load_setup_56.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x56]
print(f'accd 0x56 SRV_SETUP_DATA_RESPONSE frames: {len(frames)}')

if not frames:
    print('FAIL: accd never emitted 0x56 in reply to the bot 0x55')
    sys.exit(1)

# Echo check: sess_type byte should be 0 (Practice — what the bot
# sent), and car_id is u16 little-endian at offset 2.
f = frames[0]
sess = f[1]
car_id = f[2] | (f[3] << 8)
lap_count = f[4] | (f[5] << 8)
if lap_count >= 0x8000:
    lap_count -= 0x10000
print(f'frame[0] sess_type={sess} car_id={car_id} lap_count={lap_count} '
      f'total_len={len(f)}')
if sess != 0:
    print(f'FAIL: sess_type echo wrong, got {sess}, expected 0')
    sys.exit(2)
if car_id < 1001:
    print(f'FAIL: car_id echo low {car_id}, expected >= 1001 (ACC_CAR_ID_BASE)')
    sys.exit(3)
if lap_count < 0:
    print(f'FAIL: lap_count negative ({lap_count})')
    sys.exit(4)

# The trailing single-car leaderboard record bumps every frame far
# above the 6-byte header; even a zero-lap reply has >100 B of trailing
# record data appended (write_car_leaderboard_record in handlers.c).
if len(f) < 100:
    print(f'FAIL: 0x56 frame {len(f)} bytes; expected trailing car record')
    sys.exit(5)

print(f'RESULT: PASS (0x56 echoes sess+car, len={len(f)}B, '
      f'lap_count={lap_count} + trailing leaderboard record)')
"
