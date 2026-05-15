#!/bin/sh
# 0x19 ACP_LAP_COMPLETED (SA contact) regression.  Bot sends 0x19
# (reporter, target, ts, quality) via --sa-contact.  accd's
# h_lap_completed (despite the legacy name, it's a safety-rating
# contact report) emits a 0x1b SRV_LAP_BROADCAST relay of body:
# u8 0x1b + u16 reporter + u16 target + i32 ts + u8 quality = 10 B.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Target 912 = the other bot's race number; quality byte 7.
BOT1="--race 911 --grid 1 --name BotS --sa-contact 90:912:7"
BOT2="--race 912 --grid 2 --name BotT"
TEST_DURATION=8

echo "==> accd + 2 bots, bot1 emits 0x19 at tick 90"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_sa_contact_19.pcap

rm -f accd_sa_contact_19.legacy.pcap
editcap -F pcap accd_sa_contact_19.pcap accd_sa_contact_19.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_sa_contact_19.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x1b]
print(f'0x1b frames: {len(frames)} sizes {sorted({len(b) for b in frames})}')
if not frames:
    print('FAIL: no 0x1b relay frames')
    sys.exit(1)

# Expected: 1 + 2 + 2 + 4 + 1 = 10 bytes.
if not all(len(b) == 10 for b in frames):
    print(f'FAIL: 0x1b wrong size {sorted({len(b) for b in frames})}')
    sys.exit(2)

# Decode each relay.
for b in frames[:3]:
    rep = b[1] | (b[2] << 8)
    tgt = b[3] | (b[4] << 8)
    ts  = b[5] | (b[6] << 8) | (b[7] << 16) | (b[8] << 24)
    qual = b[9]
    print(f'  reporter={rep} target={tgt} ts={ts} quality={qual}')

# At least one relay should carry our wire bytes (we sent target_car=912
# as a *race number* not car_id — the server logs what the bot sent).
hits = [b for b in frames if (b[3] | (b[4] << 8)) == 912 and b[9] == 7]
if not hits:
    print('FAIL: no 0x1b relay matches target=912 quality=7')
    sys.exit(3)

print(f'RESULT: PASS ({len(frames)} 0x1b frames, {len(hits)} match target+quality)')
"
