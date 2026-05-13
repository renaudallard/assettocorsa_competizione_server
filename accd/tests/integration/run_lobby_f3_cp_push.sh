#!/bin/sh
# Lobby 0xf3 CP_PUSH parse-robustness regression.
#
# accd is non-CP: it parses the leading kson_string event_id, logs
# "0xf3 CP data push for event %s", and drops the rest.  This test
# fires three flavours of body at the dispatcher and asserts accd
# does not crash AND logs the expected branch for each:
#   - well-formed: kson_str("test_event") + trailing junk
#   - truncated:   just the 0xf3 byte (no string)
#   - oversize:    kson_str of 1000 'X' bytes (exceeds 128 B event_id
#                  buffer in lobby.c)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
FAKE_PORT=11931

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
import sys, time, struct
sys.path.insert(0, ".")
from fake_kson import FakeKsonLobby, write_kson_str
lobby = FakeKsonLobby(port=$FAKE_PORT)
lobby.start()
lobby.wait_for_type(0xc8, timeout=8.0)
lobby.send(b"\\xef\\x00")
time.sleep(1)

# 1) well-formed
body = bytearray([0xf3])
write_kson_str(body, "test_event")
body.extend(b"\\x00\\x01trailing junk")
lobby.send(bytes(body))
print("  sent well-formed 0xf3", flush=True)
time.sleep(0.5)

# 2) truncated (just the cmd byte)
lobby.send(b"\\xf3")
print("  sent truncated 0xf3", flush=True)
time.sleep(0.5)

# 3) oversize event_id (1000 'X')
body = bytearray([0xf3])
write_kson_str(body, "X" * 1000)
lobby.send(bytes(body))
print("  sent oversize 0xf3", flush=True)
time.sleep(0.5)

# 4) Verify accd is still alive + responsive: send 0xf1 and wait for
# 0xd1, proving the dispatcher loop survived all three bodies.
pre = lobby.count(0xd1)
lobby.send(b"\\xf1")
time.sleep(1.5)
post = lobby.count(0xd1)
lobby.shutdown()
if post <= pre:
    print(f"FAIL: dispatcher dead after malformed 0xf3 (pre={pre}, post={post})")
    sys.exit(1)
print(f"  dispatcher alive after fuzz: +{post - pre} 0xd1 from 0xf1")
sys.exit(0)
PY
FAKE_PID=$!
sleep 0.5

rm -f accd.log
ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=$FAKE_PORT \
    $ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!

wait "$FAKE_PID"
FRC=$?

# Snapshot before tearing accd down so the log is stable
sleep 0.3
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

# Verify the log branches accd took
if [ $FRC -ne 0 ]; then exit $FRC; fi
if ! grep -q '0xf3 CP data push for event "test_event"' accd.log; then
    echo "FAIL: no log for well-formed 0xf3"
    grep "0xf3" accd.log
    exit 1
fi
echo "  PASS: well-formed body logged with event_id"
if ! grep -q "0xf3 CP data push.*short parse" accd.log; then
    echo "FAIL: no 'short parse' log for truncated 0xf3"
    grep "0xf3" accd.log
    exit 1
fi
echo "  PASS: truncated body took short-parse branch"
# The oversize body should take the short-parse branch because the
# read_kson_string sees (size_t)slen + 1 > outsz (128) and returns -1.
# Count how many "short parse" appearances there are.
n_short=$(grep -c "short parse" accd.log || true)
if [ "${n_short:-0}" -lt 2 ]; then
    echo "FAIL: expected >= 2 'short parse' logs (truncated + oversize), got $n_short"
    exit 1
fi
echo "  PASS: oversize body also took short-parse branch (${n_short} total)"
echo "RESULT: PASS (0xf3 parse robust + dispatcher survives malformed bodies)"
