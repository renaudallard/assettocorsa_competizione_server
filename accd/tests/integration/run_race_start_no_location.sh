#!/bin/sh
# Race green flag must fire even when the car never reports carLocation=Track
# — issue #16 regression.
#
# The green-flag leader-pick gate (tick.c) requires race.on_track, which is
# set only by the 0x32 CAR_LOCATION_UPDATE handler.  A car spawned already on
# the grid never emits a Track transition, so on_track would stay 0, no leader
# is ever picked, session_advance_race_triggers is never called, and the race
# stays frozen on the ROLLING START PROCEDURE HUD.  session_start now seeds
# on_track=1 for grid cars so the green flag fires.
#
# The bot is run with BOT_NO_LOCATION=1 so it never sends a 0x32 — reproducing
# a client that gives no Track report.  Before the fix this hangs forever;
# after the fix the green flag fires within a lap.
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

rm -f race_no_loc.log bot_no_loc.log
$ACCD -c cfg_race_no_loc >race_no_loc.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5 6; do
    if ss -tln 2>/dev/null | grep -q ':9332'; then break; fi
    sleep 0.3
done

# BOT_NO_LOCATION=1 -> the bot never emits a 0x32 CAR_LOCATION_UPDATE, so
# accd only sees on_track via the session_start seed (the fix under test).
echo "==> spawn bot with BOT_NO_LOCATION=1 (never reports carLocation=Track)"
BOT_NO_LOCATION=1 "$BOT" --host 127.0.0.1 --tcp 9332 \
    --race 911 --grid 1 --name "BotNoLoc" >bot_no_loc.log 2>&1 &
BOT_PID=$!

echo "==> waiting up to 60 s for the green flag..."
green=""
for i in $(seq 1 60); do
    if grep -q 'green flag' race_no_loc.log 2>/dev/null; then
        green="yes"
        echo "  green fired after ~${i}s"
        break
    fi
    sleep 1
done

kill -TERM "$BOT_PID"  2>/dev/null || true
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

if [ "$green" != "yes" ]; then
    echo "FAIL: green flag never fired — race stuck at rolling start (issue #16)"
    grep -E 'session 0:|FORMATION|PRE_SESSION|formation end|green flag' \
        race_no_loc.log | tail -12
    exit 1
fi
echo "  PASS: green flag fired without any carLocation=Track report"
echo "RESULT: PASS (race starts with no 0x32 location — issue #16)"
