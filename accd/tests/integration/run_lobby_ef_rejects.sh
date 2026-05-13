#!/bin/sh
# Lobby 0xef reject-code regression (codes 1..6).
#
# accd's lobby_dispatch_message + lobby_reject_reason map kunos's
# FUN_140048660 case 1 reject codes:
#   1 outdated server      5 unsupported platform (Wine?)
#   2 wrong version/port   6 did not respond on public IP
#   3 blocked by backend   (0 = accepted, tested elsewhere)
#   4 rejected (unknown)
# On any non-zero code accd must transition state -> PERMANENTLY_DISABLED
# and log "lobby: hard reject; disabling lobby client".  This test
# spins up a fresh accd for each code, asserts both the warn line
# and the state transition.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BASE_PORT=11920

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

run_one_code() {
    code=$1
    port=$((BASE_PORT + code))
    echo "==> code=$code  (port=$port)"

    python3 - "$port" "$code" <<'PY' &
import sys, time
sys.path.insert(0, ".")
from fake_kson import FakeKsonLobby
port = int(sys.argv[1])
code = int(sys.argv[2])
lobby = FakeKsonLobby(port=port)
lobby.start()
lobby.wait_for_type(0xc8, timeout=8.0)
lobby.send(bytes([0xef, code]))
time.sleep(2)
lobby.shutdown()
PY
    FAKE_PID=$!
    sleep 0.5

    rm -f accd.log
    ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=$port \
        $ACCD -c cfg >accd.log 2>&1 &
    ACCD_PID=$!
    sleep 3

    kill -TERM "$ACCD_PID" 2>/dev/null || true
    wait "$ACCD_PID" 2>/dev/null || true
    kill -TERM "$FAKE_PID" 2>/dev/null || true
    wait "$FAKE_PID" 2>/dev/null || true

    if ! grep -q "registration rejected code=$code" accd.log; then
        echo "  FAIL: no 'registration rejected code=$code' in log"
        tail -10 accd.log
        return 1
    fi
    if ! grep -q "hard reject; disabling lobby client" accd.log; then
        echo "  FAIL: no hard-reject log"
        return 1
    fi
    if ! grep -q "PERMANENTLY_DISABLED" accd.log; then
        echo "  FAIL: state never moved to PERMANENTLY_DISABLED"
        return 1
    fi
    echo "  PASS: code $code -> PERMANENTLY_DISABLED"
}

fails=0
for c in 1 2 3 4 5 6; do
    run_one_code "$c" || fails=$((fails + 1))
done

if [ $fails -eq 0 ]; then
    echo "RESULT: PASS (all 6 reject codes -> PERMANENTLY_DISABLED)"
else
    echo "RESULT: DIFFER ($fails failed)"
    exit 1
fi
