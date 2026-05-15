#!/bin/sh
# /controllers + /controller probe regression.  Admin issues the
# command; accd's chat.c:967 sends a 1-byte 0x5b SRV_CTRL_INFO_REQUEST
# to every authenticated peer.  This pins the probe emission.
#
# The bot side does not reply (bot.c has no 0x5b handler), so the
# h_ctrl_info -> chat-back path is exercised by other manual /
# real-client tests.  Here we just assert the outbound probe lands
# on every peer.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name BotAdm \
    --chat-start-tick 60 --chat /admin_admin --chat /controllers"
BOT2="--race 912 --grid 2 --name BotPeer"
TEST_DURATION=10

echo "==> accd + 2 bots, admin /controllers"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_admin_controllers.pcap

if ! grep -q 'admin: /controllers' accd.log; then
    echo "FAIL: no /controllers log line"
    exit 1
fi
n=$(grep -oE 'admin: /controllers -> [0-9]+ probes' accd.log | grep -oE '[0-9]+' | head -1)
echo "  probes sent: $n"
if [ "${n:-0}" -lt 2 ]; then
    echo "FAIL: expected >= 2 probes (2 bots), got $n"
    exit 2
fi

rm -f accd_admin_controllers.legacy.pcap
editcap -F pcap accd_admin_controllers.pcap accd_admin_controllers.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_controllers.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x5b]
print(f'0x5b probe frames: {len(frames)} sizes {sorted({len(b) for b in frames})}')
if len(frames) < 2:
    print(f'FAIL: expected >= 2 probes, got {len(frames)}')
    sys.exit(1)
# Each probe is 1 byte body (just the msg id).
if not all(len(b) == 1 for b in frames):
    print(f'FAIL: 0x5b wrong size; expected 1, got {sorted({len(b) for b in frames})}')
    sys.exit(2)
print('RESULT: PASS (0x5b probe fans out to every peer)')
"
