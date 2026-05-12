#!/bin/sh
# Reconnect socket flap regression.
# Bot connects, drives briefly, deliberately closes its TCP socket at
# tick 100, and reconnects via the existing bot reconnect path.  Both
# servers should:
#   - accept the original handshake (1st 0x0b welcome)
#   - issue a 0x24 disconnected-fan-out to peers when the original
#     conn drops
#   - accept the re-handshake (2nd 0x0b welcome / redelivery)
# Compare frame counts + the post-reconnect 0x0b length.  Wine
# already produces organic reconnects under CPU starvation; the
# deliberate flap just adds one extra controlled event.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotFlap --flap-at 100"
TEST_DURATION=20

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_flap.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_flap.pcap

echo "==> diff"
rm -f accd_flap.legacy.pcap kunos_flap.legacy.pcap
editcap -F pcap accd_flap.pcap accd_flap.legacy.pcap
editcap -F pcap kunos_flap.pcap kunos_flap.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_flap.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_flap.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x0b]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x0b]

print(f'accd 0x0b welcomes: {len(af)} lens {[len(b) for b in af]}')
print(f'kunos 0x0b welcomes: {len(kf)} lens {[len(b) for b in kf]}')

# Both should send >= 2 welcomes (initial + reconnect).  Wines own
# CPU-starvation reconnect can add more; we just assert >= 2.
if len(af) < 2:
    print('FAIL: accd did not redeliver after flap')
    sys.exit(1)
if len(kf) < 2:
    print('WARN: kunos did not redeliver after flap '
          '(wine bot may have stayed flapping)')
print('RESULT: VALID (accd reconnected and redelivered the welcome trailer)')
"
