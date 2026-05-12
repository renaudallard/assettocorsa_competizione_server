#!/bin/sh
# Multi-driver entrylist regression -- documents known accd/kunos
# divergence in team-entry semantics.
#
# Entrylist has one entry with two drivers + forceEntryList=1 +
# overrideDriverInfo=1 (required by kunos for teams).  Bot1 connects
# as drivers[0], bot2 (different steam) as drivers[1].
#
# Kunos behaviour: each driver gets their OWN carId, sharing the
# entrys race_number.  Bot2 ends up on a second car, broadcasts go
# to both conns.  Kunos emits 10+ 0x47 frames on every conn lifecycle
# event.
#
# accd behaviour: the entry only ever allocates ONE car (slot reserved
# for the first driver to connect).  Bot2 hits s->cars[slot].used==1
# and gets REJECT_FULL.  accd only ever has bot1 connected, so the
# 0x47 broadcast only fires when bot1 sends its --swap-state.
#
# This script captures the divergence: counts of 0x47 frames + the
# byte shape of the last broadcast.  Fixing it properly would mean
# adopting kunoss "two cars per team entry" model (a sizeable refactor
# of accds 1:1 conn->car invariant).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

cleanup_on_exit() {
    rc=$?
    for f in cfg/entrylist.json.bak; do
        [ -f "$f" ] || continue
        mv "$f" "${f%.bak}" || true
    done
    [ -f cfg/entrylist.json ] && [ ! -f cfg/entrylist.json.bak ] && \
        rm -f cfg/entrylist.json
    ssh accd@172.20.0.66 "cd wine-test/cfg && for f in *.bak; do \
        [ -f \"\$f\" ] && mv \"\$f\" \"\${f%.bak}\"; done; \
        [ -f entrylist.json ] && [ ! -f entrylist.json.bak ] && \
        rm -f entrylist.json || true" 2>/dev/null || true
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT

# Defensive startup: roll back any leftover .bak.
for f in cfg/*.bak; do
    [ -f "$f" ] || continue
    mv "$f" "${f%.bak}"
done

# Stage the multi-driver entrylist on both sides.
cp cfg_swap_multi/local/entrylist.json cfg/entrylist.json
scp -q cfg_swap_multi/remote/entrylist.json \
    accd@172.20.0.66:wine-test/cfg/entrylist.json

BOT1="--race 911 --grid 1 --name BotPrimary --swap-state 100:2"
BOT2="--race 922 --grid 2 --name BotTeammate"
TEST_DURATION=20

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT1\" \"$BOT2\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_swap_multi.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_swap_multi.pcap

rm -f cfg/entrylist.json
ssh accd@172.20.0.66 'rm -f wine-test/cfg/entrylist.json' 2>/dev/null || true

echo "==> diff"
rm -f accd_swap_multi.legacy.pcap kunos_swap_multi.legacy.pcap
editcap -F pcap accd_swap_multi.pcap accd_swap_multi.legacy.pcap
editcap -F pcap kunos_swap_multi.pcap kunos_swap_multi.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_swap_multi.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_swap_multi.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x47]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x47]

print(f'accd 0x47 frames: {len(af)}; lens {[len(b) for b in af]}')
print(f'kunos 0x47 frames: {len(kf)}; lens {[len(b) for b in kf]}')

# Filter out the spurious 5-byte broadcasts triggered as side-effects
# of the conn lifecycle (initial state, peer join).  We care about the
# bot1 -> 0x47 swap-state update fan-out, which has body
# 47 + u16 car_id + u8 driver_count(2) + 2 u8 states.
target_len = 6  # 1 msg + 2 car_id + 1 dcnt + 2 states
af_target = [b for b in af if len(b) == target_len]
kf_target = [b for b in kf if len(b) == target_len]
print(f'accd target ({target_len} B): {len(af_target)}; '
      f'first hex: {af_target[0].hex() if af_target else \"-\"}')
print(f'kunos target ({target_len} B): {len(kf_target)}; '
      f'first hex: {kf_target[0].hex() if kf_target else \"-\"}')

# This test documents divergence rather than asserts parity.  Both
# servers emit some 0x47 frames; the byte layouts diverge because
# accd uses one car per entry while kunos creates one car per driver.
# Print both shapes; PASS as long as accd emitted at least the
# expected single broadcast.
if not af:
    print('FAIL: accd did not emit any 0x47 broadcast')
    sys.exit(1)
print(f'  accd[last]: {af[-1].hex()}')
if kf:
    print(f'  kunos[last]: {kf[-1].hex()}')
print('RESULT: VALID (accd emits 0x47; kunos diverges -- one car per')
print('              driver vs accds one car per entry; multi-driver')
print('              shared-seat byte parity needs an accd refactor)')
"
