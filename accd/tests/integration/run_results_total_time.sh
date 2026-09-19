#!/bin/sh
# results.json totalTime regression (github issue #20).
#
# The finishing time accd writes for a car is the elapsed time from the
# green flag to that car's last S/F crossing, so it can never be longer
# than the race.  It used to be built by adding a per-connection clock
# offset expressed against session.phase_started_ms, which is re-stamped
# at every phase and session boundary while the offset itself is only
# re-latched when the connection posts a new minimum RTT.  A driver who
# joined during practice therefore had their race dated from practice,
# and totalTime came out longer than the whole race.
#
# The bot has to run with --no-keepalive here.  Its default 1 Hz UDP
# 0x13 makes the server drop and re-latch the clock every second, which
# hides the staleness; a real ACC client sends exactly one 0x13 per
# connection and never refreshes it.
#
# Config: 1 min Practice + 2 min Race, so the stale anchor (if it comes
# back) inflates the race time by the practice session.
#
# Slow test (~4 min).  Skip via SKIP=1 in CI smoke runs.
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
cp cfg_total_time/local/event.json    cfg/event.json
cp cfg_total_time/local/settings.json cfg/settings.json

rm -rf results
rm -f accd.log

$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

"$BOT" --host 127.0.0.1 --tcp 9302 --no-keepalive \
    --race 911 --grid 1 --name "BotT" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

# 5 s pre + 60 s P + 10 s overtime + wrap, then 5 s pre + formation +
# 120 s R + 10 s overtime + aftercare.  240 s leaves headroom for the
# race results to land.
echo "==> waiting 240 s for the P -> R cycle"
sleep 240

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

FILE=$(ls results/*_R.json 2>/dev/null | head -1)
if [ -z "$FILE" ]; then
    echo "FAIL: no race results file in results/"
    exit 1
fi
echo "  found: $FILE"

set +e
python3 - "$FILE" accd.log <<'PY'
import json, re, sys
from datetime import datetime, timedelta

results_path, log_path = sys.argv[1], sys.argv[2]

TS = r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})"
green_re = re.compile(TS + r" INFO green flag \(\w+\).*fire_in=(\d+)ms")
lap_re = re.compile(TS + r" INFO lap completed: car=(\d+) ")

def parse(ts):
    return datetime.strptime(ts, "%Y-%m-%d %H:%M:%S.%f")

green = None
last_cross = {}
for line in open(log_path, errors="replace"):
    m = green_re.match(line)
    if m:
        green = parse(m.group(1)) + timedelta(milliseconds=int(m.group(2)))
        last_cross = {}
        continue
    m = lap_re.match(line)
    if m and green is not None:
        last_cross[int(m.group(2))] = parse(m.group(1))

if green is None:
    print("FAIL: no green flag line in accd.log")
    sys.exit(2)
if not last_cross:
    print("FAIL: no S/F crossing logged after the green flag")
    sys.exit(3)

data = json.load(open(results_path))
if data["sessionType"] != "R":
    print(f"FAIL: expected sessionType 'R', got {data['sessionType']!r}")
    sys.exit(4)

# Race length the server was told to run, plus overtime, plus a lap of
# slack: the finishing time can never exceed this.
race_ms = 2 * 60 * 1000 + 10 * 1000 + 60 * 1000

rc = 0
for line in data["sessionResult"]["leaderBoardLines"]:
    car_id = line["car"]["carId"]
    idx = car_id - 1001
    total = line["timing"]["totalTime"]
    laps = line["timing"]["lapCount"]
    if idx not in last_cross:
        print(f"FAIL: car {car_id} has no crossing in the log")
        rc = 5
        continue
    expect = int((last_cross[idx] - green).total_seconds() * 1000)
    delta = total - expect
    print(f"  car {car_id}: laps={laps} totalTime={total} "
          f"expected={expect} delta={delta:+d} ms")
    if total > race_ms:
        print(f"FAIL: totalTime {total} exceeds the race ({race_ms} ms)")
        rc = 6
    if abs(delta) > 1500:
        print(f"FAIL: totalTime is {delta:+d} ms off the measured race time")
        rc = 7

if rc == 0:
    print("RESULT: PASS (totalTime matches the green-to-crossing elapsed time)")
sys.exit(rc)
PY
RC=$?
set -e
rm -rf results
exit $RC
