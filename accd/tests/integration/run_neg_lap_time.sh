#!/bin/sh
# Negative lap-time rejection regression.
#
# A malicious client can forge a 0x21 ACP_SECTOR_SPLIT_SINGLE frame
# with `lap_time = -1000` and, prior to commit 2fd102b, accd's
# h_sector_split_single (handlers.c:293-295) would set the
# attacker's best_lap_ms to that negative value.  session.c's
# cmp_cars then sorts the attacker ahead of every legitimate driver
# in the next 0x36 leaderboard frame because the sort treats only
# `best == 0` as the unset sentinel.  Unauthenticated leaderboard
# manipulation.
#
# Test: drive a single bot with --bad-lap-time pointing the next
# 0x21 emit at a negative lap-time value.  accd should:
#   * log either "negative lap_time" (defensive) OR silently treat
#     the lap as invalid (current implementation flips the invalid
#     flag).
#   * NOT push the negative value through to the leaderboard's 0x36
#     frame: subsequent 0x36 emits must carry best_lap = 0x7fffffff
#     (LAP_TIME_INVALID) or any positive number, never the
#     attacker's negative.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Arm the bad-lap injection BEFORE lap 2's S/F crossing.  The bot
# only fires the bad 0x21 on a lap-complete emit (last_sector==2 -&gt;
# new_sector==0), so the tick threshold must fall between lap 1 end
# (~t=1s, tick ~30) and lap 2 end (~t=33s, tick ~990).  Lap 1 is
# the out-lap (car_field=0x0004) which would set invalid=1 pre-fix
# anyway, so the test must hit lap 2+ to actually probe the fix.
# Tick 100 (~t=3s) lands deep in lap 2.
BOT="--race 911 --grid 1 --name BotEvil --bad-lap-time 100:-1000"
TEST_DURATION=45

echo "==> accd + bot, --bad-lap-time 100:-1000 (arms before lap 2 S/F)"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_neg_lap_time.pcap

rm -f accd_neg_lap_time.legacy.pcap
editcap -F pcap accd_neg_lap_time.pcap accd_neg_lap_time.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_neg_lap_time.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x36]
print(f'0x36 leaderboard frames: {len(frames)}')

# Within each 0x36, walk the leaderboard prefix's first u32 — the
# session best lap.  A negative i32 sign-extends to a 32-bit value
# >= 0x80000000.  LAP_TIME_INVALID is 0x7fffffff which is fine.
neg_seen = False
for i, b in enumerate(frames):
    if len(b) < 5: continue
    best_le = b[1] | (b[2]<<8) | (b[3]<<16) | (b[4]<<24)
    # Reject best_lap values that are negative when read as i32
    # but ARE NOT the kunos invalid sentinel 0x7fffffff.
    if best_le >= 0x80000000 and best_le != 0x7fffffff:
        print(f'frame[{i}] best_lap = 0x{best_le:08x} (i32 = {best_le-0x100000000}) — leaked through')
        neg_seen = True

if neg_seen:
    print('FAIL: leaderboard 0x36 carries the negative attacker time')
    sys.exit(1)
print('RESULT: PASS (negative lap_time did not poison the leaderboard)')
"
