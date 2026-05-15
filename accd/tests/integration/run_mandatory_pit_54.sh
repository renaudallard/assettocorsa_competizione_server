#!/bin/sh
# 0x54 ACP_MANDATORY_PITSTOP_SERVED regression.  Bot enters pit on
# lap 4 (well after green) and emits 0x54 after pit traversal.
# accd's h_mandatory_pitstop_served reads u16 car_id and emits the
# "Served Mandatory Pitstop: <id>" log line.  No wire broadcast.
#
# This test pins the inbound handler's log line.  A regression that
# broke car-id validation or the log shape would surface here.
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

echo "==> bot with --pit-on-lap 4"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "PitBot" \
    --pit-on-lap 4 --laps 5 >bot1.log 2>&1 &
BOT_PID=$!

echo "==> waiting for 0x54 (up to 150 s)..."
for i in $(seq 1 150); do
    sleep 1
    if grep -q 'Served Mandatory Pitstop' accd.log 2>/dev/null; then
        echo "  served at ${i}s"
        break
    fi
done

kill -TERM "$BOT_PID"  2>/dev/null || true
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

if ! grep -q 'Served Mandatory Pitstop' accd.log; then
    echo "FAIL: 'Served Mandatory Pitstop' never logged"
    tail -10 accd.log >&2
    exit 1
fi

n=$(grep -c 'Served Mandatory Pitstop' accd.log)
echo "  count: $n"
echo "RESULT: PASS (h_mandatory_pitstop_served accepted bot 0x54)"
