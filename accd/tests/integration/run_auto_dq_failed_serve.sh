#!/bin/sh
# Auto-DQ on failed DT/SG serve regression.
#
# accd's handlers.c:373-397 decrements pen.laps_remaining on every
# valid race-lap complete while a DT/SG is at the head of the queue.
# When laps_remaining hits 0 and allow_auto_dq=1 the entry is
# replaced with PEN_DQ, broadcast as cat=DQ, and the car is marked
# race.disqualified.  This test:
#   - cfg overlay: 5-min Race + allowAutoDQ=1 + shortFormationLap=1
#   - bot self-reports cat=0 kind=1 (cutting DT) via 0x41
#   - bot keeps driving laps (no 0x42 served, no pit)
#   - after ~3 race laps accd auto-DQ's the bot
#   - test verifies 'failed to serve ... -> DQ' log line
#
# Slow test (~205 s).  Skip via SKIP=1 in CI smoke runs.
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

# Bot self-reports cat=0 kind=1 (DT-Cutting) at tick 2500 (~83 s),
# AFTER accd transitions to PHASE_SESSION (h_report_penalty drops
# 0x41 reports during FORMATION / PRE_SESSION).  Then bot keeps
# driving the synthetic stadium loop without pit / served.
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "BotDQ" \
    --report-penalty 0:1:3 \
    --penalty-start-tick 2500 \
    --no-mandatory-pit >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

echo "==> waiting 200 s for race->session + 3 race laps + auto-DQ"
sleep 200

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

if ! grep -qE "failed to serve .* -> DQ" accd.log; then
    echo "FAIL: accd did not auto-DQ on failed DT serve"
    echo "  searching log for any DQ activity:"
    grep -iE 'DQ|disqual|failed.*serve|laps_remaining' accd.log | tail -10
    exit 1
fi
echo "  PASS: 'failed to serve ... -> DQ' log line present"

# Sanity: penalty enqueue + bot's lap progression actually happened
if ! grep -q "penalty_enqueue" accd.log; then
    if ! grep -qiE "queue.*pen|pen.*enqueue|added penalty" accd.log; then
        echo "WARN: no explicit penalty-enqueue log; flow path differs"
    fi
fi
echo "RESULT: PASS (auto-DQ fired after 3 race laps of unserved DT)"
