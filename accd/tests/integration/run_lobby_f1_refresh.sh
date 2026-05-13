#!/bin/sh
# Lobby 0xf1 DRIVERS_REFRESH regression.
#
# Per kunos FUN_140044c10 case 0xf1, the lobby asks the server to
# re-emit its drivers list.  accd's dispatcher invokes
# lobby_send_drivers_update which produces a fresh 0xd1.  This test
# accepts the register, lets the post-accept 0xd1 fly, then sends
# 0xf1 and asserts a second 0xd1 frame arrives within 2 s.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
FAKE_PORT=11930

ACCD_PID=""
FAKE_PID=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
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

python3 - <<PY &
import sys, time
sys.path.insert(0, ".")
from fake_kson import FakeKsonLobby
lobby = FakeKsonLobby(port=$FAKE_PORT)
lobby.start()
lobby.wait_for_type(0xc8, timeout=8.0)
lobby.send(b"\\xef\\x00")
time.sleep(2)  # let the post-accept 0xd1 fly
pre = lobby.count(0xd1)
print(f"  initial 0xd1 count: {pre}", flush=True)
lobby.send(b"\\xf1")
print("  sent 0xf1 DRIVERS_REFRESH", flush=True)
time.sleep(2)
post = lobby.count(0xd1)
print(f"  post-refresh 0xd1 count: {post}", flush=True)
rc = 0 if post > pre else 1
if rc == 0:
    print(f"RESULT: PASS (0xf1 -> +{post - pre} 0xd1 emit)")
else:
    print("FAIL: no additional 0xd1 after 0xf1")
lobby.shutdown()
sys.exit(rc)
PY
FAKE_PID=$!
sleep 0.5

rm -f accd.log
ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=$FAKE_PORT \
    $ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!

wait "$FAKE_PID"
FRC=$?

kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

exit $FRC
