#!/bin/sh
# 0x2f ACP_TYRE_COMPOUND_UPDATE relay.  Bot sends 0x2f at handshake
# (default compound=0 dry).  accd's h_tyre_compound_update stores
# the byte and relays it as 0x2f SRV_TYRE_COMPOUND_RELAY (msg + u16
# car_id + u8 compound = 4 B) to every other peer.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name BotA"
BOT2="--race 912 --grid 2 --name BotB"
TEST_DURATION=8

echo "==> accd + 2 bots"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_tyre_compound_2f.pcap

echo "==> diff"
rm -f accd_tyre_compound_2f.legacy.pcap
editcap -F pcap accd_tyre_compound_2f.pcap accd_tyre_compound_2f.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_tyre_compound_2f.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x2f]
print(f'0x2f frames: {len(frames)} sizes {sorted({len(b) for b in frames})}')
if len(frames) < 1:
    print('FAIL: no 0x2f relay frames observed')
    sys.exit(1)
# Bot1 connects first (no peers, no relay).  Bot2 connects -> server
# relays bot2's compound to bot1 (1 frame).  Bot1's earlier compound
# is not relayed retroactively.  So 2 bots in sequence = 1 relay.

# Body shape: 1 (msg) + 2 (car_id) + 1 (compound) = 4 bytes.
if not all(len(b) == 4 for b in frames):
    print(f'FAIL: 0x2f wrong size; got {sorted({len(b) for b in frames})}')
    sys.exit(2)

# Decode each: car_ids and compounds.
seen = set()
for b in frames:
    car_id = b[1] | (b[2] << 8)
    cmp = b[3]
    seen.add((car_id, cmp))
print(f'observed (car_id, compound) pairs: {sorted(seen)}')

if not any(c >= 1001 for c,_ in seen):
    print(f'FAIL: no car_id >= 1001 (ACC_CAR_ID_BASE)')
    sys.exit(3)

print('RESULT: PASS (0x2f tyre-compound relay carries (car_id, compound))')
"
