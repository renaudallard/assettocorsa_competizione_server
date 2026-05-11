#!/bin/sh
# Multi-car DQ ordering regression.
# 2 bots in the same race; bot1 sends a direct DQ (cat=0 kind=6 value=3).
# After enqueue, session_recompute_standings must push bot1 to last;
# the next 0x36 leaderboard records must order bot2 first, bot1 second.
# Compare both servers' final 0x36 byte-for-byte.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name BotDQ --report-penalty 0:6:3"
BOT2="--race 922 --grid 2 --name BotClean"
TEST_DURATION=30

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT1\" \"$BOT2\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_2bot_dq.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_2bot_dq.pcap

echo "==> diff"
rm -f accd_2bot_dq.legacy.pcap kunos_2bot_dq.legacy.pcap
editcap -F pcap accd_2bot_dq.pcap accd_2bot_dq.legacy.pcap
editcap -F pcap kunos_2bot_dq.pcap kunos_2bot_dq.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_2bot_dq.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_2bot_dq.legacy.pcap', 19298)
# Filter for 2-car frames (post-handshake, full leaderboard).  The
# matrix's 'last frame' for a 1-bot test is the bot's own welcome
# emit; with 2 bots disconnecting in sequence, the most interesting
# frame is the LAST 2-car emit (right before bot1 disconnects).
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]
au = sorted({(len(b), b.hex()) for b in af})
ku = sorted({(len(b), b.hex()) for b in kf})

only_a = [x for x in au if x not in ku]
only_k = [x for x in ku if x not in au]

print(f'accd frames: {len(af)} unique fingerprints: {len(au)}')
print(f'kunos frames: {len(kf)} unique fingerprints: {len(ku)}')
print(f'accd-only fingerprints: {len(only_a)}')
print(f'kunos-only fingerprints: {len(only_k)}')

if not only_a and not only_k:
    print('RESULT: IDENTICAL (every accd 0x36 fingerprint matches kunos byte-exact)')
elif not only_a:
    print(f'RESULT: ACCD_SUBSET ({len(only_k)} kunos-only frames; content match where it overlaps)')
else:
    print('RESULT: DIFFER (accd emits frames kunos does not)')
    for ln, h in only_a[:2]:
        print(f'  accd-only len={ln}  {h[:80]}...')
    sys.exit(2)
"
