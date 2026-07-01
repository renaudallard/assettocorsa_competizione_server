#!/bin/sh
# Admin /next on a pre-green RACE session — issue #17 regression.
#
# A single-Race weekend.  The bot connects, so the race enters its
# pre-race waiting window (PHASE_FORMATION) and then PRE_SESSION, holding
# ts[2]/ts[3] at UINT64_MAX until the green flag.  An admin /next issued
# in that window must still advance: before the fix, /next collapsed only
# ts[4..6], so compute_phase stayed stuck below PHASE_ADVANCE and /next
# was a silent no-op on the last (race) session.  session_advance_now()
# now collapses ts[1..6], so the phase jumps straight to ADVANCE and the
# weekend wraps back to session 0.
#
# Asserts: the '/next' is processed AND the schedule reaches ADVANCE AND
# the weekend wraps (session_advance actually fired).
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

rm -f next_race.log bot_next_race.log
$ACCD -c cfg_next_race >next_race.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5 6; do
    if ss -tln 2>/dev/null | grep -q ':9312'; then break; fi
    sleep 0.3
done

# Bot elevates with /admin admin then issues /next while the race is still
# in its pre-green window (chat starts ~2s in, pre-race wait is 5s).
echo "==> spawn bot with /admin admin + /next during pre-green race"
"$BOT" --host 127.0.0.1 --tcp 9312 \
    --race 911 --grid 1 --name "BotAdmin" \
    --chat-start-tick 60 \
    --chat /admin_admin \
    --chat /next >bot_next_race.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

sleep 12

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

if ! grep -q 'admin: /next' next_race.log; then
    echo "FAIL: accd did not process '/next' admin command"
    grep -iE 'admin|chat' next_race.log | head -10
    exit 1
fi
echo "  PASS: '/next' command logged"

# The pre-green race must reach PHASE_ADVANCE (it could not before the fix).
if ! grep -qE '\-> ADVANCE' next_race.log; then
    echo "FAIL: schedule never reached PHASE_ADVANCE after /next (stuck pre-green)"
    grep -E 'session 0:|Forwarding|admin: /next' next_race.log | tail -10
    exit 2
fi
echo "  PASS: schedule reached ADVANCE"

# session_advance must have fired the weekend wrap back to session 0.
if ! grep -qiE 'weekend complete, resetting to session 0|Resetting race weekend' next_race.log; then
    echo "FAIL: /next did not advance/wrap the last (race) session"
    grep -E 'session 0:|weekend|Resetting' next_race.log | tail -10
    exit 3
fi
echo "  PASS: weekend wrapped (session_advance fired)"
echo "RESULT: PASS (/next advances a pre-green race — issue #17)"
