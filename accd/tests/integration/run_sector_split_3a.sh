#!/bin/sh
# 0x20 -> 0x3a sector-splits relay regression (PR #7, 0.3.76).
#
# Bot 1 drives real laps; the bot emits 0x20 per sector boundary
# (sectors 0 and 1) per tools/bot/bot.c.  Bot 2 just sits in pit.
# The server must broadcast a 0x3a SRV_SECTOR_SPLITS_RELAY to bot
# 2 carrying the sender's car_id, split_count=1, the new sector's
# split_time, a session-relative i32 timestamp, and the wire
# car_field.  Sender (bot 1) must NOT receive its own 0x3a echo
# (kunos broadcast pattern uses except-sender for 0x3a).
#
# Also pins the clock normalisation: the i32 timestamp in the
# 0x3a body should be the wire clock_ms shifted by the per-conn
# session_clock_offset_ms.  We don't know offset exactly from the
# test side, but we assert it's BOUNDED by a sane delta from
# session_now (no raw boot-clock leakage into the field).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name Driver --laps 3"
BOT2="--race 912 --grid 2 --name Sitter --laps 0"
TEST_DURATION=45

echo "==> accd + 2 bots, bot1 drives"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_sector_split_3a.pcap

echo "==> diff"
rm -f accd_sector_split_3a.legacy.pcap
editcap -F pcap accd_sector_split_3a.pcap accd_sector_split_3a.legacy.pcap

python3 -c "
import struct, sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_sector_split_3a.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b and b[0] == 0x3a]
print(f'accd 0x3a sector-splits frames: {len(frames)}')

if len(frames) == 0:
    print('FAIL: no 0x3a frames emitted')
    sys.exit(1)

# Body: u8 0x3a + u16 car_id + u8 split_count + N*u32 splits + i32 ts + u16 car_field
ok = 0
for b in frames:
    if len(b) < 1 + 2 + 1 + 4 + 4 + 2:
        print(f'  FAIL: 0x3a frame too short: {len(b)} bytes')
        sys.exit(2)
    car_id    = struct.unpack_from('<H', b, 1)[0]
    sc        = b[3]
    if sc < 1 or 1 + 2 + 1 + sc*4 + 4 + 2 != len(b):
        print(f'  FAIL: 0x3a split_count={sc} but body len={len(b)}')
        sys.exit(3)
    splits    = list(struct.unpack_from(f'<{sc}I', b, 4))
    ts_i32    = struct.unpack_from('<i', b, 4 + sc*4)[0]
    car_field = struct.unpack_from('<H', b, 4 + sc*4 + 4)[0]
    # split_time must be positive and < 5 min for a small bot loop
    for s in splits:
        if s <= 0 or s > 300_000:
            print(f'  FAIL: implausible split_time={s}ms')
            sys.exit(4)
    # ts must be bounded: server has been running < 60s when the bot
    # started, plus ~45s test duration.  Raw boot-clock leakage would
    # produce values in the millions (uptime since boot in ms).
    if ts_i32 <= 0 or ts_i32 > 600_000:
        print(f'  FAIL: ts={ts_i32}ms looks raw (not session-relative)')
        sys.exit(5)
    ok += 1
    if ok == 1:
        print(f'  sample frame: car_id={car_id} split_count={sc} '
              f'splits={splits} ts={ts_i32}ms car_field=0x{car_field:04x}')

print(f'RESULT: PASS ({ok} 0x3a frames, all wire-correct + ts normalised)')
"
