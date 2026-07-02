#!/bin/sh
# Admin /restart on a pre-green RACE session — issue #19 regression.
#
# "During qualifying /restart sends to the next session and during a race it
# does nothing."  The old /restart body collapsed only ts[4..6] and set
# advance_at_ms=0 — a partial copy of the pre-issue-#17 /next path.  A qualy is
# already past ts[2]/ts[3] so that reached PHASE_ADVANCE and advanced; a
# pre-green race holds ts[2]/ts[3] at UINT64_MAX so it never reached ADVANCE
# and /restart was a silent no-op.  The exe's /restart (FUN_140021680:320-337)
# restarts the CURRENT session in place — it does NOT advance — so
# session_restart_current now re-arms the same session index.
#
# This reuses the single-Race weekend (cfg_next_race): the bot connects, the
# race enters its pre-green window, and an admin /restart issued there must
# re-arm the race (a second session_start) WITHOUT advancing/wrapping the
# weekend (which is what /next does).  run_admin_restart.sh separately pins
# the 0x2b banner wire; this pins the session transition.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot

ACCD_PID=""
BOT_PIDS=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT

rm -f restart_race.log bot_restart_race.log
$ACCD -c cfg_next_race >restart_race.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5 6; do
    if ss -tln 2>/dev/null | grep -q ':9312'; then break; fi
    sleep 0.3
done

# Bot elevates with /admin admin then issues /restart while the race is still
# in its pre-green window.  chat-start-tick 200 (~6.7s at 30 Hz) lands after
# the 5s pre-race wait, so the race is in PRE_SESSION/SESSION with ts[2]/ts[3]
# still UINT64_MAX — the exact window the old /restart no-op'd on.
echo "==> spawn bot with /admin admin + /restart during pre-green race"
"$BOT" --host 127.0.0.1 --tcp 9312 \
    --race 911 --grid 1 --name "BotAdmin" \
    --chat-start-tick 200 \
    --chat /admin_admin \
    --chat /restart >bot_restart_race.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

sleep 12

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

if ! grep -q 'admin: /restart' restart_race.log; then
    echo "FAIL: accd did not process '/restart' admin command"
    grep -iE 'admin|chat' restart_race.log | head -10
    exit 1
fi
echo "  PASS: '/restart' command logged"

# session_restart_current re-arms the current session, so a second
# session_start runs (a second green-trigger roll).  Before the fix the
# pre-green race saw no reset at all.
rolls=$(grep -c 'green trigger rolled' restart_race.log || true)
if [ "$rolls" -lt 2 ]; then
    echo "FAIL: session was not re-armed by /restart (green rolls=$rolls, want >=2)"
    grep -E 'session_start:|admin: /restart' restart_race.log | tail -10
    exit 2
fi
echo "  PASS: session re-armed (green trigger rolled ${rolls}x)"

# /restart must NOT advance/wrap the weekend — that is /next's job.  For a
# single-Race weekend /next logs the weekend wrap; /restart must not.
if grep -qiE 'weekend complete, resetting to session 0|Resetting race weekend' restart_race.log; then
    echo "FAIL: /restart advanced/wrapped the weekend (should reset in place)"
    grep -iE 'weekend|Resetting|admin: /restart' restart_race.log | tail -10
    exit 3
fi
echo "  PASS: weekend not advanced (session reset in place, same index)"
echo "RESULT: PASS (/restart resets the current pre-green race — issue #19)"
