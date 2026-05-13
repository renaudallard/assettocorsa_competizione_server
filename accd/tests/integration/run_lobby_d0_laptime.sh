#!/bin/sh
# Lobby 0xd0 laptime emit regression.
#
# accd's handlers.c invokes lobby_notify_lap on every valid (non-cut,
# non-out, non-invalidated) lap-complete, which builds a 23-byte
# 0xd0 body: u16 car_id + u16 race_number + u32 lap_ms + u32
# race_time_ms.  This test drives 1 bot through ~3 laps of the
# default ~10-s stadium loop and asserts the fake kson lobby
# received >= 1 0xd0 with sane bytes.
#
# Slow test (~50 s).  Skip via SKIP=1 in CI smoke runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot
FAKE_PORT=11940

ACCD_PID=""
FAKE_PID=""
BOT_PIDS=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
    [ -n "$FAKE_PID" ] && kill -TERM "$FAKE_PID" 2>/dev/null || true
    for f in cfg/settings.json.bak; do
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
# Enable registerToLobby; keep the default 3-session event so accd
# stays in Practice for the test window (default Practice is 2 min).
cp cfg_lobby/local/settings.json cfg/settings.json

RESULT=$(mktemp)
python3 - "$RESULT" <<PY &
import sys, time
sys.path.insert(0, ".")
from fake_kson import FakeKsonLobby
RESULT = sys.argv[1]
lobby = FakeKsonLobby(port=$FAKE_PORT)
lobby.start()
lobby.wait_for_type(0xc8, timeout=8.0)
lobby.send(b"\\xef\\x00")
# ACK every keepalive so accd stays REGISTERED through the run.
end = time.time() + 60
last_ka = 0
while time.time() < end:
    time.sleep(0.3)
    n = lobby.count(0xf2)
    while last_ka < n:
        lobby.send(b"\\xfd")
        last_ka += 1
d0 = [body for t, body in lobby.all_inbox() if t == 0xd0]
print(f"  collected {len(d0)} 0xd0 frames", flush=True)
with open(RESULT, "w") as f:
    for body in d0:
        f.write(body.hex() + "\\n")
lobby.shutdown()
PY
FAKE_PID=$!
sleep 0.5

rm -f accd.log
ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=$FAKE_PORT \
    $ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!

# Wait until accd is actually listening on tcp/9302 before launching
# the bot (the bot has no retry and dies on Connection refused).
for i in 1 2 3 4 5 6 7 8 9 10; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

echo "==> spawn 1 bot (default stadium loop, ~10 s/lap)"
# No --track override -> default stadium loop = ~903 m at V_RACE 85 m/s
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "BotLap" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

wait "$FAKE_PID"

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

python3 - "$RESULT" <<'PY'
import sys, struct
RESULT = sys.argv[1]
bodies = [bytes.fromhex(l.strip()) for l in open(RESULT) if l.strip()]
if not bodies:
    print("FAIL: no 0xd0 frame received from accd")
    sys.exit(1)
print(f"  decoding {len(bodies)} 0xd0 body(ies)")
ok = 0
for i, b in enumerate(bodies):
    # body[1:] (preamble stripped already by fake_kson.inbox).
    # Layout: u16 car_id + u16 race_number + u32 lap_ms + u32 race_time_ms
    if len(b) != 12:
        print(f"  [{i}] FAIL: body len {len(b)}, expected 12")
        continue
    car_id, race_no, lap_ms, race_ms = struct.unpack("<HHII", b)
    print(f"  [{i}] car_id={car_id} race#={race_no} "
          f"lap_ms={lap_ms} race_ms={race_ms}")
    if race_no != 911:
        print(f"  [{i}] FAIL: race_number {race_no}, expected 911")
        continue
    if lap_ms <= 0:
        print(f"  [{i}] FAIL: lap_ms <= 0")
        continue
    ok += 1
if ok == 0:
    print("FAIL: no 0xd0 frame validated")
    sys.exit(2)
print(f"RESULT: PASS ({ok} of {len(bodies)} 0xd0 frames pass byte-decode)")
PY
RC=$?
rm -f "$RESULT"
exit $RC
