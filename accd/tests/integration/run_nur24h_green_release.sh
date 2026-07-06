#!/bin/sh
# nurburgring_24h green-flag release when the leader drives off the grid
# (issue #16, the second half of the rolling-start release).
#
# The park test (run_nur24h_rolling_release.sh) proves the degenerate-track
# guard fires formation_end for a car frozen at the grid spawn (0.9434328),
# advancing the client from the pre-session phase to the grid-lights phase.
# But the AC2 client only hands control back at the GREEN flag: it keeps the
# car in the rolling-start autopilot, driving it forward, until the server
# stamps the green descriptor (ts[3]), which fires when the leader reaches the
# per-track green zone.  On nurburgring_24h the grid (0.9434) sits well below
# the green zone ([0.9933, 1.0000]), so green only fires once the car has
# driven up to it.
#
# This test drives a bot forward from the grid spawn (--drive-from, formation
# speed) and asserts the full sequence fires: formation_end THEN green flag.
# The park test alone would pass even if green never fired, leaving the car
# locked one phase later; this closes that gap.
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

rm -f nur24h_green.log bot_nur24h_green.log
$ACCD -c cfg_nur24h >nur24h_green.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5 6; do
    if ss -tln 2>/dev/null | grep -q ':9342'; then break; fi
    sleep 0.3
done

# Drive a bot from the nurburgring_24h pole grid spawn (0.9434328) forward at
# formation speed.  --length 200 keeps the norm_pos sweep quick.  Practice
# (1 min) auto-advances into the race.
echo "==> spawn bot driving from the grid (0.9434328) on nurburgring_24h"
"$BOT" --host 127.0.0.1 --tcp 9342 --race 911 --grid 1 --name "DriveBot" \
    --drive-from 0.9434328 --length 200 >bot_nur24h_green.log 2>&1 &
BOT_PID=$!

echo "==> waiting up to 120 s for formation_end then green flag..."
formation=""
green=""
for i in $(seq 1 120); do
    if [ -z "$formation" ] && grep -q 'formation end' nur24h_green.log 2>/dev/null; then
        formation="yes"
        echo "  formation_end fired after ~${i}s"
    fi
    if grep -q 'green flag' nur24h_green.log 2>/dev/null; then
        green="yes"
        echo "  green flag fired after ~${i}s"
        break
    fi
    sleep 1
done

kill -TERM "$BOT_PID"  2>/dev/null || true
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

if [ "$formation" != "yes" ]; then
    echo "FAIL: formation_end never fired -- rolling start deadlocked (issue #16)"
    grep -E 'formation_start raw|Session changed|formation end|green flag' nur24h_green.log | tail -8
    exit 1
fi
if [ "$green" != "yes" ]; then
    echo "FAIL: green flag never fired -- car stuck at the grid-lights phase (issue #16)"
    grep -E 'formation_start raw|Session changed|formation end|green flag' nur24h_green.log | tail -8
    exit 1
fi
echo "  PASS: formation_end and green flag both fired driving off the grid on nurburgring_24h"
echo "RESULT: PASS (degenerate-track green release fires -- issue #16)"
