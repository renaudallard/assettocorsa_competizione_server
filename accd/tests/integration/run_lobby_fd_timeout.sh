#!/bin/sh
# Lobby 0xfd ack-timeout regression.
#
# Verifies that after accd sends 0xf2 keepalive, if the fake kson
# server withholds the 0xfd reply for >30 s, accd drops the connection
# with 'keepalive ack timeout' and transitions out of REGISTERED.
# Mirrors kunos's FUN_140048660 case 6 (byte at LC+0x30 still set
# 30 s after the send -> disconnect).
#
# Slow test (~65 s) because LOBBY_KEEPALIVE_MS=30 s and the timeout
# window is another 30 s.  Adjust SKIP=1 to bypass in CI smoke runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
FAKE_PORT=11909

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

# Enable lobby in cfg
cp cfg/settings.json cfg/settings.json.bak
cp cfg_lobby/local/settings.json cfg/settings.json

echo "==> spin up fake kson lobby on 127.0.0.1:$FAKE_PORT"
python3 - <<PY &
import sys, time
sys.path.insert(0, ".")
from fake_kson import FakeKsonLobby, build_preamble

lobby = FakeKsonLobby(port=$FAKE_PORT)
lobby.start()
print(f"  fake-lobby port={lobby.port} listening", flush=True)
# Accept the 0xc8 register, reply 0xef 0x00 to advance accd to
# REGISTERED.  After that send NOTHING — never reply 0xfd to the
# 0xf2 keepalive that accd will send at the 30 s mark.
got = lobby.wait_for_type(0xc8, timeout=8.0)
print(f"  got 0xc8 register: {got is not None}", flush=True)
lobby.send(b"\\xef\\x00")
# Stay alive ~70s so accd has time to time out + log disconnect.
time.sleep(70)
lobby.shutdown()
PY
FAKE_PID=$!
sleep 1

echo "==> spin up accd targeting fake lobby"
rm -f accd.log
ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=$FAKE_PORT \
    $ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!

echo "==> wait 65 s for accd to send keepalive (~30 s) + time out (~+30 s)"
sleep 65

kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
kill -TERM "$FAKE_PID" 2>/dev/null || true
wait "$FAKE_PID" 2>/dev/null || true

echo "==> verify accd logged the timeout + disconnect"
if grep -q "Sent keepalive" accd.log; then
    echo "  PASS: keepalive sent"
else
    echo "  FAIL: no keepalive in log"
    cat accd.log | tail -30
    exit 1
fi
if grep -q "keepalive ack timeout" accd.log; then
    echo "  PASS: timeout detected"
else
    echo "  FAIL: no 'keepalive ack timeout' in log"
    cat accd.log | tail -30
    exit 1
fi
if grep -q "DISCONNECTED\|BACKOFF" accd.log; then
    echo "  PASS: state transitioned out of REGISTERED"
else
    echo "  FAIL: no state transition after timeout"
    exit 1
fi
echo "RESULT: PASS (accd disconnects ~30 s after unacked keepalive)"
