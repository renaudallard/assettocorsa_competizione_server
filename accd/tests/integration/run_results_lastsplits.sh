#!/bin/sh
# results.json `lastSplits` content regression (PR #6, 0.3.76).
#
# results.c reads car->race.last_lap_splits_ms which is snapshotted
# from sector_ms[] just before the per-lap reset in
# h_sector_split_single.  Before PR #6 results.c read sector_ms[]
# directly which was zeroed by the time the JSON was written, so
# `lastSplits` was always [0, 0, 0] for any driver who completed a
# lap.  This test runs a Q with one bot completing >=2 laps, then
# asserts results.json::lastSplits has at least S1 and S2 non-zero
# (sector 2 stays at 0 because kunos's 0x20 only carries sectors
# 0 and 1 — kunos exhibits the same shape).
#
# Slow test (~95 s).  Skip via SKIP=1 in CI smoke runs.
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
cp cfg_phase_collapse/local/event.json    cfg/event.json
sed 's/"dumpLeaderboards": 0/"dumpLeaderboards": 1/' \
    cfg_phase_collapse/local/settings.json > cfg/settings.json

rm -rf results
rm -f accd.log

$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

"$BOT" --host 127.0.0.1 --tcp 9302 --race 911 --grid 1 \
    --name SplitTester --laps 0 >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

# 60 s of driving (Q is 1 min, ends naturally); then wait through
# OVERTIME + COMPLETED + WRAP for results_write to fire.
sleep 95

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
for pid in $BOT_PIDS; do wait "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

res=$(ls results/*_Q.json 2>/dev/null | head -1)
if [ -z "$res" ]; then
    echo "FAIL: no Q results.json was produced"
    ls -la results 2>&1 | head
    exit 1
fi
echo "==> $res"

python3 - "$res" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    j = json.load(f)
cars = j.get("sessionResult", {}).get("leaderBoardLines", [])
if not cars:
    print("FAIL: no leaderBoardLines")
    sys.exit(2)
c0 = cars[0]
t = c0.get("timing", {})
last_lap   = t.get("lastLap", 0)
last_splits = t.get("lastSplits", [0, 0, 0])
print(f"  lastLap={last_lap}ms lastSplits={last_splits}")
if last_lap <= 0:
    print("FAIL: lastLap=0; no lap was scored")
    sys.exit(3)
if last_splits[0] <= 0 or last_splits[1] <= 0:
    print("FAIL: lastSplits[S1/S2] not populated; PR #6 snapshot missing")
    sys.exit(4)
# S3 staying 0 is expected per kunos: 0x20 only carries sectors 0/1.
print("RESULT: PASS (lastSplits S1/S2 populated from snapshot)")
PY
