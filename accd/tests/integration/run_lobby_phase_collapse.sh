#!/bin/sh
# Lobby 0xcb phase-collapse regression.
#
# accd's lobby_sample_session intentionally maps PHASE_OVERTIME (internal
# 6) to wire-byte 5 (SESSION) and PHASE_COMPLETED (internal 7) to wire-
# byte 1 (WAITING) so the kson backend never sees the transient
# end-of-session phases that delist the server.  This test runs a short
# race session, drives accd through the full state machine, and asserts
# the captured 0xcb stream contains only phase bytes in {1..5}.
#
# Slow test (~90 s).  Skip via SKIP=1 in CI smoke runs.
# Cycle wall time for a quali session = 3 s pre + 60 s quali + 10 s
# overtime + 15 s post ~ 88 s; +5 s buffer for the 0xcb drain loop.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot
FAKE_PORT=11913

ACCD_PID=""
FAKE_PID=""
BOT_PIDS=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
    [ -n "$FAKE_PID" ] && kill -TERM "$FAKE_PID" 2>/dev/null || true
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
cp cfg_phase_collapse/local/settings.json cfg/settings.json
cp cfg_phase_collapse/local/event.json    cfg/event.json

echo "==> spin up fake kson lobby + 0xcb collector on 127.0.0.1:$FAKE_PORT"
RESULT_FILE=$(mktemp)
python3 - "$RESULT_FILE" <<PY &
import sys, time, struct
sys.path.insert(0, ".")
from fake_kson import FakeKsonLobby
RESULT = sys.argv[1]
lobby = FakeKsonLobby(port=$FAKE_PORT)
lobby.start()
lobby.wait_for_type(0xc8, timeout=8.0)
lobby.send(b"\\xef\\x00")
print("  fake-lobby accepted accd register", flush=True)
# Ack every keepalive so accd stays REGISTERED through the cycle
last_count = 0
end = time.time() + 90
phase_bytes_seen = []
while time.time() < end:
    time.sleep(0.5)
    f2 = lobby.count(0xf2)
    while last_count < f2:
        lobby.send(b"\\xfd")
        last_count += 1
    # Drain 0xcb frames out of inbox
    for entry in lobby.all_inbox():
        if entry[0] == 0xcb and entry not in phase_bytes_seen:
            phase_bytes_seen.append(entry)
print(f"  collected {len(phase_bytes_seen)} 0xcb frames", flush=True)
phases = []
for _, body in phase_bytes_seen:
    if len(body) >= 4:
        phases.append(body[1])
print(f"  phase bytes: {phases}", flush=True)
with open(RESULT, "w") as f:
    for p in phases:
        f.write(f"{p}\\n")
lobby.shutdown()
PY
FAKE_PID=$!
sleep 1

echo "==> spin up accd"
rm -f accd.log
ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=$FAKE_PORT \
    $ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
sleep 2

echo "==> spawn 1 bot so accd doesn't reset to first session"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "BotPhase" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

echo "==> wait for full quali -> overtime -> completed cycle (~90 s)"
wait "$FAKE_PID"
FRC=$?

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

echo "==> verify accd actually traversed OVERTIME / COMPLETED internally"
if ! grep -qE "session [0-9]+: SESSION -> OVERTIME|session [0-9]+: .* -> OVERTIME" accd.log; then
    echo "FAIL: accd never entered PHASE_OVERTIME internally"
    grep "session" accd.log | head
    exit 1
fi
echo "  PASS: accd entered PHASE_OVERTIME internally"
if ! grep -qE "session [0-9]+: .* -> COMPLETED" accd.log; then
    echo "FAIL: accd never entered PHASE_COMPLETED internally"
    exit 1
fi
echo "  PASS: accd entered PHASE_COMPLETED internally"

echo "==> verify lobby never saw phase 6 or 7"
PHASES=$(cat "$RESULT_FILE" 2>/dev/null || true)
if echo "$PHASES" | grep -q '^[67]$'; then
    echo "FAIL: lobby saw phase 6 or 7 (collapse broken):"
    echo "$PHASES"
    exit 1
fi
echo "  lobby phase bytes: $(echo "$PHASES" | tr '\n' ' ')"
echo "  PASS: no phase=6 (OVERTIME) and no phase=7 (COMPLETED) reached the lobby"
rm -f "$RESULT_FILE"
echo "RESULT: PASS (phase-collapse in lobby_sample_session is wired)"
exit $FRC
