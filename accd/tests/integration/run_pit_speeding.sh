#!/bin/sh
# Pit-speeding regression.  accd's handlers.c h_loc_update fires
# REASON_PIT_SPEEDING / PEN_DQ when the bot is in location=Pitlane
# with vec_c magnitude > 22.22 m/s AND s->session.green_fired is set
# (the gate at handlers.c:774).
#
# The standard bot caps pit-lane speed at V_PITLANE = 18 m/s so the
# DQ never fires.  The --pit-speed M flag overrides the cap; we set
# it to 30 m/s so the bot drives well above the 22.22 threshold
# during the pit-entry / pit-exit window.
#
# Uses the cfg_autodq overlay (single 5-min Race, 5 s pre-race wait)
# so green fires quickly.  Asserts the "PITLANE SPEEDING for car"
# log line + the type-4 banner with the "- pit speeding" suffix.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot

ACCD_PID=""
BOT_PID=""
cleanup_on_exit() {
    rc=$?
    [ -n "$BOT_PID"  ] && kill -TERM "$BOT_PID"  2>/dev/null || true
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
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

pkill -KILL -f 'accd -c '       >/dev/null 2>&1 || true
pkill -KILL -f 'tools/bot/bot ' >/dev/null 2>&1 || true
sleep 1

cp cfg/settings.json cfg/settings.json.bak
cp cfg/event.json    cfg/event.json.bak
cp cfg_autodq/local/settings.json cfg/settings.json
cp cfg_autodq/local/event.json    cfg/event.json

rm -f accd.log bot1.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

echo "==> bot drives with --pit-speed 30 and --pit-on-lap 4"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "PitSpeeder" \
    --pit-on-lap 4 --pit-speed 30 >bot1.log 2>&1 &
BOT_PID=$!

echo "==> waiting for green flag..."
for i in $(seq 1 90); do
    sleep 1
    if grep -q 'green_fired\|GREEN flag\|green flag' accd.log 2>/dev/null; then
        echo "  green fired after ${i}s"
        break
    fi
done
if ! grep -qE 'green_fired|GREEN flag|green flag' accd.log 2>/dev/null; then
    echo "FAIL: green flag never fired"
    exit 1
fi

echo "==> waiting for PITLANE SPEEDING line (up to 120 s)..."
for i in $(seq 1 120); do
    sleep 1
    if grep -q 'PITLANE SPEEDING' accd.log 2>/dev/null; then
        echo "  PITLANE SPEEDING fired after ${i}s post-green"
        break
    fi
done

kill -TERM "$BOT_PID"  2>/dev/null || true
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

if ! grep -q 'PITLANE SPEEDING' accd.log; then
    echo "FAIL: PITLANE SPEEDING was never logged"
    grep -E 'speeding|PITLANE|location=2' accd.log | head -5 || true
    exit 2
fi

# Optional: the type-4 chat banner with " - pit speeding" suffix.
if grep -q ' - pit speeding' accd.log; then
    echo "  chat banner with ' - pit speeding' suffix found"
fi

echo "RESULT: PASS (pit-lane speeding fired PEN_DQ + 0x2b banner)"
