#!/bin/sh
# Lobby reconnect-backoff regression.
#
# accd's lobby_disconnect transitions state -> LOBBY_BACKOFF and the
# lobby_tick state machine then retries.  Per kunos FUN_140048660
# case 2, the interval is 10 s while consecutive_fails < 3 and 30 s
# after, capped at LOBBY_BACKOFF_MAX_MS.  This test points accd at a
# TCP listener that accepts the connection and immediately closes it,
# then counts accepts over a 60 s window to confirm at least 4
# retries with the first 2 gaps ~10 s and the 3rd ~30 s.
#
# Slow test (~65 s).  Skip via SKIP=1 in CI smoke runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
FAKE_PORT=11933

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

RESULT=$(mktemp)
python3 - "$RESULT" <<PY &
import sys, socket, time
RESULT = sys.argv[1]
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", $FAKE_PORT))
s.listen(8)
s.settimeout(0.5)
accepts = []
end = time.time() + 60
print(f"  listener up on 127.0.0.1:{$FAKE_PORT}, accept-and-close mode", flush=True)
while time.time() < end:
    try:
        c, _ = s.accept()
        accepts.append(time.time())
        c.close()
        print(f"  accepted at t={time.time():.1f}", flush=True)
    except socket.timeout:
        continue
s.close()
with open(RESULT, "w") as f:
    for t in accepts:
        f.write(f"{t}\\n")
print(f"  total accepts in 60 s: {len(accepts)}", flush=True)
PY
FAKE_PID=$!
sleep 0.5

rm -f accd.log
ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=$FAKE_PORT \
    $ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!

wait "$FAKE_PID"
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

python3 - "$RESULT" <<'PY'
import sys
RESULT = sys.argv[1]
times = [float(l.strip()) for l in open(RESULT) if l.strip()]
print(f"  accept timestamps: {len(times)}")
if len(times) < 4:
    print(f"FAIL: only {len(times)} accept(s) in 60 s; expected >= 4")
    sys.exit(1)
gaps = [times[i + 1] - times[i] for i in range(len(times) - 1)]
print(f"  gaps: " + ", ".join(f"{g:.1f}s" for g in gaps))
# First two gaps should be ~10 s (fails 1->2 and 2->3), 3rd gap ~30 s
# (fails 3+ -> LONG window).  Slack +/- 3 s for poll wake-up jitter.
for i, g in enumerate(gaps[:3]):
    expect = 10.0 if i < 2 else 30.0
    if not (expect - 3.0 <= g <= expect + 3.0):
        print(f"FAIL: gap {i+1} = {g:.1f}s, expected {expect:.0f} +/- 3s")
        sys.exit(2)
print(f"RESULT: PASS (accd retried {len(times)} times: 10/10/30 s growing backoff)")
PY
RC=$?
rm -f "$RESULT"
if [ $RC -ne 0 ]; then exit $RC; fi

if ! grep -q "lobby: disconnecting" accd.log; then
    echo "FAIL: accd never logged a disconnect"
    exit 1
fi
if ! grep -q "state .* -> BACKOFF" accd.log; then
    echo "FAIL: accd never entered BACKOFF state"
    grep "state " accd.log | head
    exit 1
fi
echo "  PASS: BACKOFF state observed in log"
