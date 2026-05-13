#!/bin/sh
# Lobby 0xd1 drivers-content regression.
#
# accd's lobby_send_drivers_update emits, per connected car:
#   u32 car_id
#   kson_string driver_name  (u16 byte-length + UTF-8)
#   u8 current_driver_index
# preceded by u8 count.  Earlier coverage (run_lobby_f1_refresh.sh)
# only asserts the frame fires; this test decodes the body and
# checks the per-car shape against a real bot's identity.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tmp/bot/bot
FAKE_PORT=11950

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
cp cfg_lobby/local/settings.json cfg/settings.json

RESULT=$(mktemp)
python3 - "$RESULT" <<PY &
import sys, time, struct
sys.path.insert(0, ".")
from fake_kson import FakeKsonLobby
RESULT = sys.argv[1]
lobby = FakeKsonLobby(port=$FAKE_PORT)
lobby.start()
lobby.wait_for_type(0xc8, timeout=8.0)
lobby.send(b"\\xef\\x00")
# Wait for a 0xd1 with at least 1 driver (the empty post-accept one
# comes first with count=0).
target_body = None
for _ in range(20):
    time.sleep(0.5)
    for t, body in lobby.all_inbox():
        if t == 0xd1 and len(body) >= 1 and body[0] >= 1:
            target_body = body
            break
    if target_body is not None:
        break
if target_body is None:
    print("FAIL: no 0xd1 with count >= 1 within 10 s")
    open(RESULT, "w").write("")
    lobby.shutdown()
    sys.exit(1)
open(RESULT, "wb").write(target_body)
print(f"  captured 0xd1 body ({len(target_body)} B): {target_body.hex()}")
lobby.shutdown()
PY
FAKE_PID=$!
sleep 0.5

rm -f accd.log
ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=$FAKE_PORT \
    $ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "BotDoc" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

wait "$FAKE_PID"

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

python3 - "$RESULT" <<'PY'
import sys, struct
RESULT = sys.argv[1]
body = open(RESULT, "rb").read()
if not body:
    sys.exit(1)
count = body[0]
off = 1
rc = 0
print(f"  driver count: {count}")
for i in range(count):
    if off + 4 > len(body):
        print(f"FAIL: car {i} car_id truncated"); sys.exit(2)
    car_id = struct.unpack("<I", body[off:off+4])[0]; off += 4
    if off + 2 > len(body):
        print(f"FAIL: car {i} name_len truncated"); sys.exit(3)
    nlen = struct.unpack("<H", body[off:off+2])[0]; off += 2
    if off + nlen > len(body):
        print(f"FAIL: car {i} name body truncated"); sys.exit(4)
    name = body[off:off+nlen].decode("utf-8", "replace"); off += nlen
    if off >= len(body):
        print(f"FAIL: car {i} driver_idx missing"); sys.exit(5)
    di = body[off]; off += 1
    print(f"  [{i}] car_id={car_id} name={name!r} driver_idx={di}")
    if car_id != 1001:
        print(f"  FAIL: car_id {car_id} != 1001"); rc = 6
    if "BotDoc" not in name:
        print(f"  FAIL: name {name!r} missing 'BotDoc'"); rc = 7
    if di != 0:
        print(f"  FAIL: driver_idx {di} != 0"); rc = 8
if off != len(body):
    print(f"FAIL: {len(body) - off} trailing bytes")
    sys.exit(9)
if rc == 0:
    print("RESULT: PASS (0xd1 body byte-decodes cleanly + matches bot identity)")
sys.exit(rc)
PY
RC=$?
rm -f "$RESULT"
exit $RC
