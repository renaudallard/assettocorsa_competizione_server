#!/bin/sh
# Admin /dq <raceNumber> regression.
# Bot sends /admin <password> (cfg has adminPassword="admin") then
# /dq 911 to disqualify itself by race number.  Compare both servers'
# resulting 0x36 tail (should be DQ wire) and 0x2b chat broadcast.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# chat_start_tick=60 (2 s) so /admin lands during PRE_SESSION before
# any penalty machinery is exercised.  Second chat 20 ticks later
# (~2.66 s).
# Note: the bot's CarInfo wire layout puts race_number at the wrong
# offset; accd reads car_model (=35) as the race_number.  Until the
# bot is fixed, target the parsed value (35) not the --race input.
BOT="--race 911 --grid 1 --name BotAdmin --chat-start-tick 60 --chat /admin_admin --chat /dq_35"
TEST_DURATION=30

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_admin_dq.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_admin_dq.pcap

echo "==> diff"
rm -f accd_admin_dq.legacy.pcap kunos_admin_dq.legacy.pcap
editcap -F pcap accd_admin_dq.pcap accd_admin_dq.legacy.pcap
editcap -F pcap kunos_admin_dq.pcap kunos_admin_dq.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_dq.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_admin_dq.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]
ac = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x2b]
kc = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x2b]

print(f'accd 0x36: {len(af)} frames; tails {[b[-4:].hex() for b in af]}')
print(f'kunos 0x36: {len(kf)} frames; tails {[b[-4:].hex() for b in kf]}')
print(f'accd 0x2b chat: {len(ac)}; lens {[len(b) for b in ac]}')
print(f'kunos 0x2b chat: {len(kc)}; lens {[len(b) for b in kc]}')

if af and kf:
    a, k = af[-1], kf[-1]
    if a == k:
        print('LAST 0x36: IDENTICAL')
    else:
        diffs = sum(1 for j in range(min(len(a), len(k))) if a[j] != k[j])
        print(f'LAST 0x36: DIFFER ({diffs} bytes, a={len(a)} k={len(k)})')
"
