#!/bin/sh
# Driver-ran-no-stint DQ regression.
#
# Regression test for v0.3.44 fix `b58c81e` (session: drop the early
# bail before the driver-ran-no-stint check).  Pre-fix
# stint_check_violations returned early when both driverStintTime and
# mandatoryPitstopCount were 0, skipping the independent check that
# DQs a multi-driver entry whose registered drivers never drove.  An
# endurance config that omitted both settings silently lost that
# enforcement.
#
# Setup:
#  - One entrylist entry with TWO registered drivers (Primary +
#    Backup), forceEntryList=1, overrideDriverInfo=1.
#  - One short race session (1 minute).  Pre-race 5 s, overtime 5 s.
#  - No driverStintTime, no mandatoryPitstopCount — both default 0.
#  - Bot connects as Primary, drives the synthetic loop, never swaps
#    to Backup.  At race end the server must DQ the car for
#    DriverRanNoStint (REASON code; logged as
#    "Car N driver M never took a stint -> DQ").
#
# Pure accd-side validation (no kunos diff): the test asserts on the
# specific log line emitted by session.c:1409.  Pre-fix the line is
# absent.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot

cleanup_on_exit() {
    rc=$?
    for f in cfg/entrylist.json.bak cfg/event.json.bak; do
        [ -f "$f" ] || continue
        mv "$f" "${f%.bak}"
    done
    rm -f cfg/entrylist.json
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT

mkdir -p log
rm -f cfg/current/*.json log/*.log accd.pcap accd.log bot*.log 2>/dev/null || true

# Defensive: roll back any leftover .bak.
for f in cfg/*.bak; do
    [ -f "$f" ] || continue
    mv "$f" "${f%.bak}"
done

# Stage the multi-driver entrylist and race-only event.json.  Backup
# the existing event.json (entrylist.json doesn't exist by default,
# so no backup needed for it).
[ -f cfg/event.json ] && mv cfg/event.json cfg/event.json.bak
cp cfg_no_stint/local/entrylist.json cfg/entrylist.json
cp cfg_no_stint/local/event.json     cfg/event.json

"$ACCD" -c cfg >accd.log 2>&1 &
ACCD_PID=$!

for i in 1 2 3 4 5 6 7 8 9 10; do
    if ss -tlnp 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.5
done

# Single bot, raceNumber matches Primary's playerID-derived steam.
# Race timeline on misano: 5 s WAITING + ~5 s FORMATION + ~30 s
# PRE_SESSION (formation-lap-complete trigger varies with track
# zone) + 60 s SESSION + ~5 s OVERTIME + 15 s post-race wait ≈ 120 s
# to reach PHASE_COMPLETED.  Sleep 150 s so the bot is still
# connected through stint_check_violations, with a 30 s buffer for
# trigger jitter.
"$BOT" --host 127.0.0.1 --tcp 9302 --race 911 --grid 1 \
    --name BotPrim --no-mandatory-pit \
    >bot1.log 2>&1 &
BOT_PID=$!
sleep 150

kill -TERM $BOT_PID 2>/dev/null || true
wait $BOT_PID 2>/dev/null || true

kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

echo "==> checking accd.log"
phase_log=$(grep -c -- '-> COMPLETED' accd.log || true)
dq_log=$(grep -c 'never took a stint -> DQ' accd.log || true)
echo "phase-end markers: $phase_log"
echo "DQ trigger lines: $dq_log"

if [ "$phase_log" -eq 0 ]; then
    echo 'FAIL: race never reached the post-race phase — increase '\
'sleep window or check session timing.'
    exit 2
fi

if [ "$dq_log" -lt 1 ]; then
    echo 'FAIL: no "never took a stint -> DQ" line emitted — the '\
'pre-fix bail-out is back, or the bot connection / session config is '\
'off (need 2 drivers in entrylist, race phase reached, lap_count > 0).'
    grep -E 'driver|stint|DQ' accd.log | head -10
    exit 3
fi

# The fix is validated by the message firing at all.  The driver
# index the message reports is the first registered driver whose
# stint_ms is 0; in a single-bot scenario both Primary (connected
# but the bot doesn't traverse pit -> Track, so stint_start_tracking
# never fires) and Backup (never connected) show 0, and the check
# finds driver 0 first.  That's still a valid regression signal:
# without the fix no "never took a stint" line is emitted at all.
hit=$(grep -E 'never took a stint' accd.log | head -1)
echo "first hit: $hit"
echo 'RESULT: PASS (driver-ran-no-stint check ran post-race)'
