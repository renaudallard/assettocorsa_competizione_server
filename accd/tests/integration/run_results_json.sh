#!/bin/sh
# Session results.json regression.
#
# accd writes results/<YYYYMMDD_HHMMSS>_<sessiontype>.json at every
# session boundary (results.c:results_write).  This test runs the
# cfg_phase_collapse overlay (single 1-min Quali) with 1 bot, waits
# through SESSION -> OVERTIME -> COMPLETED -> wrap, and asserts:
#   - a Q-type results file appears in ./results/
#   - the JSON is well-formed and contains the expected top-level keys
#     (sessionType, trackName, serverName, sessionResult)
#   - sessionType matches the session that just ended
#
# Slow test (~95 s).  Skip via SKIP=1 in CI smoke runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tmp/bot/bot

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
# results_write is gated on dumpLeaderboards = 1 (tick.c:1139); the
# phase_collapse settings.json defaults to 0 so override here.
sed 's/"dumpLeaderboards": 0/"dumpLeaderboards": 1/' \
    cfg_phase_collapse/local/settings.json > cfg/settings.json

# Clean any prior results so we can distinguish what this run produced.
rm -rf results

rm -f accd.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "BotR" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

echo "==> waiting 90 s for full Q -> wrap cycle"
sleep 90

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

ls results/ 2>/dev/null || { echo "FAIL: no results/ dir"; exit 1; }
FILE=$(ls results/*.json 2>/dev/null | head -1)
if [ -z "$FILE" ]; then
    echo "FAIL: no .json file in results/"
    exit 2
fi
echo "  found: $FILE"

python3 - "$FILE" <<'PY'
import json, sys
path = sys.argv[1]
with open(path) as f:
    txt = f.read()
print(f"  body length: {len(txt)} B")
data = json.loads(txt)
required = ["sessionType", "trackName", "sessionIndex",
            "serverName", "sessionResult"]
missing = [k for k in required if k not in data]
if missing:
    print(f"FAIL: missing top-level keys: {missing}")
    sys.exit(3)
print(f"  sessionType: {data['sessionType']!r}")
print(f"  trackName:   {data['trackName']!r}")
print(f"  serverName:  {data['serverName']!r}")
if data["sessionType"] != "Q":
    print(f"FAIL: expected sessionType 'Q', got {data['sessionType']!r}")
    sys.exit(4)
sr = data["sessionResult"]
for k in ["bestlap", "bestSplits"]:
    if k not in sr:
        print(f"FAIL: sessionResult missing {k}")
        sys.exit(5)
print(f"  sessionResult.bestlap:    {sr['bestlap']}")
print(f"  sessionResult.bestSplits: {sr['bestSplits']}")
if not isinstance(sr["bestSplits"], list) or len(sr["bestSplits"]) != 3:
    print(f"FAIL: bestSplits is not a 3-element list")
    sys.exit(6)
print("RESULT: PASS (results.json well-formed, sessionType Q, structure OK)")
PY
RC=$?
rm -rf results
exit $RC
