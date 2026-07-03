#!/bin/sh
# nurburgring_24h rolling-start release with a transforming formationLapType
# (issue #16).
#
# nurburgring_24h is the one shipped track where green_start - 0.05 (0.9433)
# falls just below formation_start (0.9434).  With formationLapType other than
# 3/5 the formation-end zone is transformed to start earlier, producing a
# normal zone that ENDS below the grid spawn (0.9434328).  A car held on the
# grid (the AC2 client locks the car while the server phase reads "formation")
# then sits just past the zone, formation_end never fires, and the race hangs
# on the ROLLING START PROCEDURE screen forever -- the stock accServer.exe
# deadlocks here identically.  The degenerate-track guard in session.c detects
# that the transformed zone no longer covers the grid and falls back to the
# untransformed formation_start, inverting the zone to near-whole-lap so it
# covers the grid; formation_end then fires and the client releases the car.
#
# The bot parks at the grid spawn (--park-pos, v=0) to simulate the locked
# client car.  Asserts formation_end fires; before the fix it never did.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot

ACCD_PID=""
BOT_PID=""
cleanup_on_exit() {
    rc=$?
    [ -n "$BOT_PID"  ] && kill -TERM "$BOT_PID"  2>/dev/null || true
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT

rm -f nur24h.log bot_nur24h.log
$ACCD -c cfg_nur24h >nur24h.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5 6; do
    if ss -tln 2>/dev/null | grep -q ':9342'; then break; fi
    sleep 0.3
done

# Park a bot at the nurburgring_24h pole grid spawn (0.9434328), frozen (v=0),
# reproducing a client-locked car.  Practice(1 min) auto-advances into the race.
echo "==> spawn bot parked at the grid (0.9434328) on nurburgring_24h"
"$BOT" --host 127.0.0.1 --tcp 9342 --race 911 --grid 1 --name "ParkBot" \
    --park-pos 0.9434328 >bot_nur24h.log 2>&1 &
BOT_PID=$!

echo "==> waiting up to 120 s for formation_end (race releases)..."
ok=""
for i in $(seq 1 120); do
    if grep -q 'formation end' nur24h.log 2>/dev/null; then
        ok="yes"
        echo "  formation_end fired after ~${i}s"
        break
    fi
    sleep 1
done

kill -TERM "$BOT_PID"  2>/dev/null || true
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

if [ "$ok" != "yes" ]; then
    echo "FAIL: formation_end never fired -- rolling start deadlocked (issue #16)"
    grep -E 'formation_start raw|Session changed|formation end' nur24h.log | tail -8
    exit 1
fi
echo "  PASS: formation_end fired for a car parked at the grid on nurburgring_24h"
echo "RESULT: PASS (degenerate-track rolling start releases -- issue #16)"
