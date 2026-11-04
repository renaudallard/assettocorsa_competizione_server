#!/bin/sh
# isSessionOver in the 0x36 status (github issue #73).
#
# The client draws the checkered flag next to a car from bit 0x0400 of
# its leaderboard status word. In multiplayer only the server sets it:
# the exe stamps the bit into the lap word of a lap that ends the
# session for the car (FUN_14012b380) before the lap is stored, so the
# 0x36 status carries it from then on. accd used to put the bit on the
# 0x3b relay only and kept the raw client word in the 0x36 status, so
# the flag never showed.
#
# Config: one 1 min Practice with 120 s overtime, so the bot (about
# 31 s a lap) completes laps both before and during overtime. Every
# lap done in a practice overtime ends the session for the car, and
# the overtime is released right after it. The test checks that no
# 0x36 status carries 0x0400 before the first overtime lap and that
# one does after it.
#
# Slow test (~2 min). Skip via SKIP=1 in CI smoke runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot
PCAP=accd_session_over.pcap

ACCD_PID=""
BOT_PIDS=""
CAP_PID=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
    [ -n "$CAP_PID" ] && sudo -n kill -TERM "$CAP_PID" 2>/dev/null || true
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
cp cfg_session_over/local/event.json    cfg/event.json
cp cfg_session_over/local/settings.json cfg/settings.json

rm -rf results
rm -f accd.log "$PCAP"

# dumpcap cannot open a file under the home directory, so write the
# capture to stdout and let the shell store it.
sudo -n dumpcap -q -F pcap -i lo -w - -f 'tcp port 9302' \
    >"$PCAP" 2>/dev/null &
CAP_PID=$!
sleep 1

$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "BotFlag" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

# 5 s pre + 60 s P, then one lap into the overtime.
echo "==> waiting 110 s for practice and overtime laps"
sleep 110

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
ACCD_PID=""
sleep 1
sudo -n kill -TERM "$CAP_PID" 2>/dev/null || true
wait "$CAP_PID" 2>/dev/null || true
CAP_PID=""

# The capture is nanosecond pcap, diff_pcap reads microsecond pcap.
LEGACY=accd_session_over.legacy.pcap
rm -f "$LEGACY"
editcap -F pcap "$PCAP" "$LEGACY"

set +e
python3 - "$LEGACY" <<'PY'
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames
from decode36 import decode

_, buf, _ = reassemble_server_tx(sys.argv[1], 9302)
frames = [b for _, _, b in walk_acc_frames(buf) if b and b[0] == 0x36]
if not frames:
    print("FAIL: no 0x36 frame captured")
    sys.exit(2)

# Walk the 0x36 stream in order up to the first status carrying
# 0x0400. Before it the lap count must have climbed with the bit
# clear, so the bit comes from the overtime lap and not from any lap.
seen_over = False
laps_clear = 0
for f in frames:
    for car in decode(f)['cars']:
        st = car['status']
        print(f"  car {car['car_id']}: lap {car['lap_count']} "
              f"status 0x{st:04x}")
        if st & 0x0400:
            seen_over = True
        else:
            laps_clear = max(laps_clear, car['lap_count'])
    if seen_over:
        break

if laps_clear == 0:
    print("FAIL: no lap completed before overtime")
    sys.exit(4)
if not seen_over:
    print("FAIL: no 0x36 status carried isSessionOver (0x0400)")
    sys.exit(5)
print("RESULT: PASS (0x36 status carries isSessionOver after the "
      "session-ending lap)")
PY
RC=$?
set -e
rm -rf results
exit $RC
