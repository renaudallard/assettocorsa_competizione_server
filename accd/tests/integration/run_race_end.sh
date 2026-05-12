#!/bin/sh
# Race-end DT/SG -> TP conversion regression.
# Uses a single 1-min Race session.  Bot sends a DT (cat=0:1:3) early
# in the race and never serves it.  After PHASE_COMPLETED fires
# (~ts[3] + overtime 10 s + aftercare ~10 s), the server runs
# penalty_convert_race_end (mirror of exe FUN_140127440), which
# rewrites the unserved DT to a TP30.  Compare the last 0x36 emit
# (per-car tail + pq list) between accd and kunos.
#
# duration breakdown:
#   pre = 5 s, race = 60 s, overtime = 10 s, aftercare = 10 s
#   total ~= 85 s -> TEST_DURATION 150 leaves headroom
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Trap-based cleanup so an interrupted run (Ctrl-C, bot timeout)
# never leaves the cfg overlays in place.  Restores from .bak on
# both accd cfg/ and wine ~/wine-test/cfg/ for every file that was
# swapped during this script.
cleanup_on_exit() {
    rc=$?
    for f in cfg/event.json.bak cfg/configuration.json.bak; do
        [ -f "$f" ] || continue
        echo "==> trap cleanup: restoring $(basename "${f%.bak}")" >&2
        mv "$f" "${f%.bak}" || true
    done
    ssh accd@172.20.0.66 "cd wine-test/cfg && for f in *.bak; do \
        [ -f \"\$f\" ] && mv \"\$f\" \"\${f%.bak}\"; done" 2>/dev/null || true
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT

# Defensive startup recovery: if a previous crashed run left
# cfg/*.bak files, the live cfg holds the test overlay -- roll any
# .bak back so we start from the canonical original.
for f in cfg/*.bak; do
    [ -f "$f" ] || continue
    echo "==> startup recovery: $(basename "${f%.bak}") <- $(basename "$f")"
    mv "$f" "${f%.bak}"
done
ssh accd@172.20.0.66 "cd wine-test/cfg && for f in *.bak; do \
    [ -f \"\$f\" ] && echo \"==> wine startup recovery: \${f%.bak} <- \$f\" >&2 && \
    mv \"\$f\" \"\${f%.bak}\"; done" >&2 || true

# Cfg = 1 min P + 1 min R, pre 5 s, overtime 10 s.  Time map:
#   server_t  0  = boot, bot connects ~0.1 s later
#   server_t ~3  = FORMATION -> PHASE_SESSION (P active)
#   server_t ~63 = P PHASE_OVERTIME
#   server_t ~73 = P PHASE_COMPLETED, then session_advance to R
#   server_t ~78 = R FORMATION -> PHASE_SESSION (R active)
#   server_t ~138 = R PHASE_OVERTIME
#   server_t ~148 = R PHASE_COMPLETED -> race-end conversion fires
#   server_t ~158 = session_advance wrap (resets queue)
# Bot tick 0 ~= server_t 0.1, 30 Hz, so tick 3000 = 100 s = mid-R
# PHASE_SESSION.  Penalty lands during R, survives to race-end.
# Observed accd timeline (one run, may shift +/- 5 s for kunos):
#   t=  0 s: boot
#   t=  3 s: P FORMATION -> SESSION
#   t= 63 s: P OVERTIME -> COMPLETED
#   t= 83 s: P ADVANCE -> R waiting
#   t= 88 s: R FORMATION -> PRE_SESSION
#   t=133 s: R PRE_SESSION -> SESSION  (green flag fires here)
#   t=193 s: R SESSION -> OVERTIME -> COMPLETED (race-end conversion)
#   t=213 s: R ADVANCE
# Bot tick 4500 (= 150 s @ 30 Hz) lands mid R-SESSION (133..193).
BOT="--race 911 --grid 1 --name BotRaceEnd --report-penalty 0:1:3 --penalty-start-tick 4500"
TEST_DURATION=240

echo "==> swap kunos cfg to single-race"
ssh accd@172.20.0.66 "cd ~/wine-test/cfg && \
    cp event.json event.json.bak 2>/dev/null && \
    cp configuration.json configuration.json.bak 2>/dev/null"
scp -q cfg_race_end/event.json accd@172.20.0.66:wine-test/cfg/event.json

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_race_end.pcap

echo "==> restore kunos cfg"
ssh accd@172.20.0.66 "cd ~/wine-test/cfg && \
    mv event.json.bak event.json 2>/dev/null"

echo "==> accd run (with cfg_race_end)"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap

# Run accd locally with the race-end cfg by temporarily swapping cfg.
cp cfg/event.json cfg/event.json.bak
cp cfg_race_end/event.json cfg/event.json
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1 || true
mv cfg/event.json.bak cfg/event.json
mv accd.pcap accd_race_end.pcap

echo "==> diff"
rm -f accd_race_end.legacy.pcap kunos_race_end.legacy.pcap
editcap -F pcap accd_race_end.pcap accd_race_end.legacy.pcap
editcap -F pcap kunos_race_end.pcap kunos_race_end.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_race_end.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_race_end.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]
ar = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x3e]
kr = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x3e]

print(f'accd 0x36: {len(af)} frames; lengths: {[len(b) for b in af]}')
print(f'kunos 0x36: {len(kf)} frames; lengths: {[len(b) for b in kf]}')
print(f'accd 0x3e (session results): {len(ar)} frames; lengths: {[len(b) for b in ar]}')
print(f'kunos 0x3e: {len(kr)} frames; lengths: {[len(b) for b in kr]}')

if af and kf:
    a, k = af[-1], kf[-1]
    print(f'accd last 0x36 tail: {a[-4:].hex()}')
    print(f'kunos last 0x36 tail: {k[-4:].hex()}')
    print('LAST 0x36:', 'IDENTICAL' if a == k else f'DIFFER ({sum(1 for i in range(min(len(a),len(k))) if a[i]!=k[i])} bytes)')

if ar and kr:
    a, k = ar[-1], kr[-1]
    print('LAST 0x3e:', 'IDENTICAL' if a == k else f'DIFFER (a={len(a)} k={len(k)})')
"
