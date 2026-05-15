#!/bin/sh
# Mid-race join handshake regression.
#
# accd's handshake.c rejects mid-race joins with REJECT_BAD_SESSION
# when is_race_locked=1 (kunos default 'unsafeRejoin=0').  When
# isRaceLocked=0 the join must succeed even after green has fallen.
# This test drives BOTH paths through the gate:
#
#   PART A (unsafeRejoin=1, isRaceLocked=0): join is ACCEPTED.
#     - cfg_autodq overlay (5-min Race, isRaceLocked=0)
#     - bot1 connects + drives, race transitions WAITING -> ... -> SESSION
#     - bot2 connects AFTER PHASE_SESSION with --mid-race, asserts
#       welcome trailer + car slot.
#
#   PART B (unsafeRejoin=0, isRaceLocked=1): join is REJECTED with
#     code 12 BAD_SESSION (handshake.c:2287, "unsafeRejoin=0 and
#     race in progress").  Uses --expect-reject so the bot exits 0
#     on receiving 0x0c.
#
# Slow test (~120 s for the accept path; ~60 s for the reject path).
# Skip via SKIP=1 in CI smoke runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot

ACCD_PID=""
BOT_PIDS=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
    for f in cfg/settings.json.bak cfg/event.json.bak; do
        [ -f "$f" ] || continue
        mv "$f" "${f%.bak}" || true
    done
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT
for f in cfg/*.bak; do
    [ -f "$f" ] || continue
    mv "$f" "${f%.bak}"
done

cp cfg/settings.json cfg/settings.json.bak
cp cfg/event.json    cfg/event.json.bak
cp cfg_autodq/local/settings.json cfg/settings.json
cp cfg_autodq/local/event.json    cfg/event.json

rm -f accd.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

echo "==> bot1 connects + drives until accd reaches PHASE_SESSION"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "Bot1" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

# Wait until accd transitions to SESSION (formation + pre_race).
echo "==> waiting for PHASE_SESSION..."
for i in $(seq 1 90); do
    sleep 1
    if grep -q 'PRE_SESSION -> SESSION' accd.log 2>/dev/null; then
        echo "  PHASE_SESSION reached after ${i}s"
        break
    fi
done
if ! grep -q 'PRE_SESSION -> SESSION' accd.log 2>/dev/null; then
    echo "FAIL: accd never reached PHASE_SESSION (race start failed)"
    exit 1
fi

sleep 3
echo "==> bot2 joins mid-race"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 922 --grid 2 --name "Bot2" \
    --mid-race >bot2.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

sleep 8

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

echo "==> verifying bot2's handshake outcome"
if grep -q "welcome ok: conn=" bot2.log; then
    bot2_welcome=$(grep "welcome ok" bot2.log | head -1)
    echo "  PASS: bot2 received welcome: $bot2_welcome"
elif grep -qE 'reject|0x0c' bot2.log; then
    echo "FAIL: bot2 was REJECTED on mid-race join"
    grep -E 'reject|0x0c' bot2.log | head -3
    exit 2
else
    echo "FAIL: bot2 produced no recognized handshake outcome"
    tail -10 bot2.log
    exit 3
fi

# Sanity: accd should have logged the 2nd handshake during PHASE_SESSION
accept_count=$(grep -c 'handshake accepted' accd.log || true)
echo "  accd handshake accepted count: $accept_count"
if [ "${accept_count:-0}" -lt 2 ]; then
    echo "FAIL: accd only saw $accept_count handshakes, expected >= 2"
    exit 4
fi
echo "RESULT: PART A PASS (mid-race join accepted with isRaceLocked=0)"

# -------------------------------------------------------------------
# PART B: same scenario but isRaceLocked=1.  bot2 must be REJECTED.
# -------------------------------------------------------------------
echo
echo "==> PART B: relaunch accd with isRaceLocked=1; expect bot2 REJECT"

# Tweak the overlay copy already in cfg/ to flip the lock.
python3 -c "
import json
p = 'cfg/settings.json'
o = json.load(open(p))
o['isRaceLocked'] = 1
json.dump(o, open(p, 'w'), indent=4)
print('  cfg/settings.json: isRaceLocked = 1')
"

ACCD_PID=""
BOT_PIDS=""

rm -f accd.log bot1.log bot2.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

echo "==> bot1 reconnects + drives until PHASE_SESSION"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "Bot1" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

for i in $(seq 1 90); do
    sleep 1
    if grep -q 'PRE_SESSION -> SESSION' accd.log 2>/dev/null; then
        echo "  PHASE_SESSION reached after ${i}s"
        break
    fi
done
if ! grep -q 'PRE_SESSION -> SESSION' accd.log 2>/dev/null; then
    echo "FAIL: PART B: accd never reached PHASE_SESSION"
    exit 1
fi

sleep 3
echo "==> bot2 attempts mid-race join; should be REJECTED"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 922 --grid 2 --name "Bot2" \
    --mid-race --expect-reject >bot2.log 2>&1 &
BOT2_PID=$!
BOT_PIDS="$BOT_PIDS $BOT2_PID"

# --expect-reject makes bot2 exit cleanly on 0x0c; give it 10 s and
# then bring down everything regardless of bot1's state.
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if ! kill -0 "$BOT2_PID" 2>/dev/null; then break; fi
    sleep 1
done

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

if grep -qE "reject|0x0c" bot2.log; then
    echo "  bot2 received 0x0c REJECT as expected"
elif grep -q "welcome ok: conn=" bot2.log; then
    echo "FAIL: PART B: bot2 was ACCEPTED but isRaceLocked=1"
    exit 5
else
    echo "FAIL: PART B: bot2 produced no recognized outcome"
    tail -10 bot2.log
    exit 6
fi

if grep -q "unsafeRejoin=0 and race in progress" accd.log; then
    echo "  accd logged the unsafeRejoin=0 reject reason"
else
    echo "WARN: accd reject log line not found; check accd.log"
fi

echo "RESULT: PART B PASS (mid-race join REJECTED with isRaceLocked=1)"
echo "RESULT: PASS (mid-race accept + reject paths both exercised)"
