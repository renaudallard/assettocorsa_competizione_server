#!/bin/sh
# Lobby 0xf6 CONFIG_REQUEST -> 0xd7 fingerprint response regression.
#
# Per kunos FUN_140044c10 case 0xf6, the server replies with 0xd7
# containing 3 kson_strings (server fingerprint, SC+0x1a8, SC+0x188).
# accd's port: 20-digit numeric fingerprint generated at lobby_init,
# fields [1]/[2] empty (semantic unverified).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
FAKE_PORT=11912

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

echo "==> spin up fake kson lobby + driver"
python3 - <<PY &
import sys, time, struct, json
sys.path.insert(0, ".")
from fake_kson import FakeKsonLobby
lobby = FakeKsonLobby(port=$FAKE_PORT)
lobby.start()
got = lobby.wait_for_type(0xc8, timeout=8.0)
print(f"  got 0xc8 register: {got is not None}", flush=True)
lobby.send(b"\\xef\\x00")
# Give accd a beat to enter REGISTERED + emit its 0xcb/0xd1 then ask for config
time.sleep(2)
lobby.send(b"\\xf6")  # 1-byte CONFIG_REQUEST body
print("  sent 0xf6 CONFIG_REQUEST", flush=True)
got = lobby.wait_for_type(0xd7, timeout=5.0)
if got is None:
    print("FAIL: no 0xd7 reply received")
    sys.exit(1)
payload = got[1]
print(f"  0xd7 payload bytes: {payload.hex()}")
# Walk 3 kson_strings (u16 LE len + N bytes)
off = 0
fields = []
for i in range(3):
    if off + 2 > len(payload):
        print(f"FAIL: payload truncated at field {i}")
        sys.exit(2)
    n = struct.unpack("<H", payload[off:off+2])[0]
    off += 2
    if off + n > len(payload):
        print(f"FAIL: field {i} length {n} exceeds payload")
        sys.exit(3)
    fields.append(payload[off:off+n].decode("utf-8", "replace"))
    off += n
print(f"  field[0]: {fields[0]!r}")
print(f"  field[1]: {fields[1]!r}")
print(f"  field[2]: {fields[2]!r}")
rc = 0
if len(fields[0]) != 20:
    print(f"FAIL: field[0] len {len(fields[0])}, expected 20")
    rc = 1
elif not fields[0].isdigit():
    print(f"FAIL: field[0] not all digits: {fields[0]!r}")
    rc = 1
else:
    print("  PASS: field[0] is 20 digits")
if fields[1] != "":
    print(f"FAIL: field[1] not empty: {fields[1]!r}")
    rc = 1
if fields[2] != "":
    print(f"FAIL: field[2] not empty: {fields[2]!r}")
    rc = 1
if rc == 0:
    print("  PASS: fields [1] and [2] empty")
    print("RESULT: PASS (0xd7 = 20-digit fpr + 2 empty strings)")
lobby.shutdown()
sys.exit(rc)
PY
FAKE_PID=$!
sleep 1

echo "==> spin up accd"
rm -f accd.log
ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=$FAKE_PORT \
    $ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!

# Wait for the fake-kson driver to exit (it does the assertions)
wait "$FAKE_PID"
FRC=$?

kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

exit $FRC
