#!/bin/sh
# Zombie-slot reconnect regression.  Drive long enough to log at
# least one 0x21 lap-complete; then deliberately flap the TCP socket
# (--flap-at).  accd's three-part reconnect cascade (quick-reconnect,
# zombie-slot reclaim, unsafeRejoin) reclaims the same CarEntry slot
# rather than spawning a fresh one.  After the reconnect, the welcome
# trailer should still mention the bot, and accd.log should show
# the "Recognized reconnect" line emitted by handshake.c:2255.
#
# A regression that broke reclaim would log a fresh-slot allocation
# instead and the post-flap welcome would carry default state.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Synthetic stadium loop laps run ~25 s for the kinematic bot.  Flap
# at tick 600 (about 20 s in at 30 Hz) so the bot has driven through
# the formation transition and at least started its first race lap.
BOT="--race 911 --grid 1 --name BotZombie --flap-at 600"
TEST_DURATION=40

echo "==> accd + bot, deliberate flap at tick 600"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_zombie_reconnect.pcap

echo "==> check"
rm -f accd_zombie_reconnect.legacy.pcap
editcap -F pcap accd_zombie_reconnect.pcap accd_zombie_reconnect.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, sa_out, _ = reassemble_server_tx('accd_zombie_reconnect.legacy.pcap', 9302)
welcomes = [b for o,l,b in walk_acc_frames(sa_out) if b and b[0]==0x0b]
print(f'accd 0x0b welcomes: {len(welcomes)} sizes {[len(b) for b in welcomes]}')

if len(welcomes) < 2:
    print('FAIL: accd did not redeliver welcome after flap')
    sys.exit(1)

# Reclaim invariant: post-flap welcomes should be comparable in size
# to the pre-flap welcome.  A welcome that lost the bot's state would
# be smaller (fewer LeaderboardEntry blocks).  We assert the
# post-flap welcome is at least 80 % of the pre-flap welcome size.
pre = len(welcomes[0])
post = max(len(w) for w in welcomes[1:])
print(f'pre-flap welcome:  {pre} B')
print(f'post-flap welcome: {post} B (max of {len(welcomes)-1} post-flap)')
if post < pre * 0.8:
    print(f'FAIL: post-flap welcome shrank from {pre} to {post}, '
          'likely a fresh-slot regression')
    sys.exit(2)

print('RESULT: PASS welcome-size structural check')
"

# Reclaim signal in accd.log: the 'Recognized reconnect' or '(quick)
# reconnect' line is emitted by the cascade.  If neither appears, the
# reconnect went through the fresh-slot path.
if grep -qE 'Recognized reconnect|\(quick\) reconnect' accd.log 2>/dev/null; then
    echo "RESULT: PASS (reconnect cascade fired -- log has 'Recognized reconnect')"
else
    echo "FAIL: accd.log shows no reconnect cascade signal" >&2
    grep -E 'reconnect|reclaim|Recognized' accd.log 2>/dev/null | head -10 >&2 || true
    exit 3
fi
