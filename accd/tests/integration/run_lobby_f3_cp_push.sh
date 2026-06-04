#!/bin/sh
# Lobby 0xf3 CP (Competition) event push: parse + apply regression.
#
# A CP-enrolled server receives 0xf3 to switch to its next scheduled
# event: the stock server (FUN_140029eb0) parses a full event descriptor,
# disconnects all players, swaps in the new track + session list, and runs
# the weekend reset.  accd mirrors this in lobby_apply_cp_event, parsing
# every field defensively so a desync aborts before touching state.
#
# This test fires bodies at the dispatcher and asserts:
#   - well-formed full CP event: logs "0xf3 CP event ... applying" AND the
#     apply reaches the weekend reset (track/sessions swapped).
#   - truncated (just 0xf3): logs "prefix parse failed", no apply.
#   - oversize cp_id: parse aborts, no apply.
#   - mid-session truncation: logs "session N parse failed", no apply.
#   - the dispatcher survives every malformed body (0xf1 -> 0xd1).
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

def cp_event(cp_id, track, sessions):
    """Full 0xf3 body in accd's CP wire layout."""
    body = bytearray([0xf3])
    write_kson_str(body, cp_id)
    body.extend(b"\\x01\\x01")              # flag_a, flag_b
    write_kson_str(body, track)
    body.extend(bytes(12))                  # 12 B scalar config
    body.append(len(sessions))              # session count
    for stype, name, dur in sessions:
        body.extend(bytes([stype, 0, 0]))   # type, cat, rules
        write_kson_str(body, name)
        body.extend(struct.pack("<H", dur)) # duration_min
    body.extend(bytes(20))                  # EventRules (20 B)
    body.extend(b"\\x00\\x00")              # entrylist flag + count
    return bytes(body)

# 1) well-formed full CP event -> apply
lobby.send(cp_event("test_cup", "spa",
                    [(0, "Practice", 20), (4, "Qualy", 15)]))
print("  sent well-formed 0xf3 CP event", flush=True)
time.sleep(0.8)

# 2) truncated (just the cmd byte) -> prefix parse fails
lobby.send(b"\\xf3")
print("  sent truncated 0xf3", flush=True)
time.sleep(0.4)

# 3) oversize cp_id (1000 'X') -> kson read aborts
body = bytearray([0xf3])
write_kson_str(body, "X" * 1000)
lobby.send(bytes(body))
print("  sent oversize 0xf3", flush=True)
time.sleep(0.4)

# 4) valid prefix but truncated mid-session -> session parse fails
body = bytearray([0xf3])
write_kson_str(body, "cup")
body.extend(b"\\x01\\x01")
write_kson_str(body, "monza")
body.extend(bytes(12))
body.append(3)                              # claims 3 sessions
body.extend(bytes([0, 0, 0]))               # session 0 header, then cut
lobby.send(bytes(body))
print("  sent mid-session-truncated 0xf3", flush=True)
time.sleep(0.4)

# 5) dispatcher still alive: 0xf1 must still yield a 0xd1
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

sleep 0.3
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

if [ $FRC -ne 0 ]; then exit $FRC; fi

# Well-formed event must be applied (parsed + weekend reset).
if ! grep -q '0xf3 CP event "test_cup" @ "spa": 2 sessions' accd.log; then
    echo "FAIL: well-formed 0xf3 not applied"
    grep "0xf3" accd.log || true
    exit 1
fi
echo "  PASS: well-formed CP event parsed and applied"
# The apply reaches the two-phase weekend reset.
if ! grep -q "resetting weekend to friday night" accd.log; then
    echo "FAIL: apply did not reach the weekend reset"
    exit 1
fi
echo "  PASS: apply ran the weekend reset"

# Truncated + oversize + mid-session must all abort without applying.
n_fail=$(grep -c "0xf3.*parse failed.*dropped" accd.log || true)
if [ "${n_fail:-0}" -lt 3 ]; then
    echo "FAIL: expected >= 3 'parse failed - dropped' logs, got $n_fail"
    grep "0xf3" accd.log || true
    exit 1
fi
echo "  PASS: malformed bodies aborted without applying (${n_fail} drops)"

# Only ONE apply should have happened (the well-formed one).
n_apply=$(grep -c "0xf3 CP event.*applying" accd.log || true)
if [ "${n_apply:-0}" -ne 1 ]; then
    echo "FAIL: expected exactly 1 apply, got $n_apply"
    exit 1
fi
echo "  PASS: exactly one apply (malformed bodies left state untouched)"
echo "RESULT: PASS (0xf3 CP push parses + applies; malformed bodies abort safely)"
