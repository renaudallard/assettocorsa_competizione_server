#!/bin/sh
# Lobby 0xf2 keepalive cadence regression.
#
# accd's lobby_tick sends 0xf2 every 30 s (LOBBY_KEEPALIVE_MS) while
# in LOBBY_REGISTERED.  Without acks the cadence stops at the
# ack-timeout path (covered by run_lobby_fd_timeout.sh); this test
# acks every keepalive and asserts the steady-state spacing is
# ~30 s over at least 2 full cycles.
#
# Slow test (~75 s).  Skip via SKIP=1 in CI smoke runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

[ "$SKIP" = 1 ] && { echo "SKIP set, skipping slow test"; exit 0; }

ACCD=/home/r/code/assettocorsa/accd/accd
FAKE_PORT=11932

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
import sys, time
sys.path.insert(0, ".")
from fake_kson import FakeKsonLobby
RESULT = sys.argv[1]
lobby = FakeKsonLobby(port=$FAKE_PORT)
lobby.start()
lobby.wait_for_type(0xc8, timeout=8.0)
lobby.send(b"\\xef\\x00")
# Record arrival times of each 0xf2 and ack each one promptly.
arrivals = []
end = time.time() + 75
last_count = 0
while time.time() < end:
    time.sleep(0.2)
    n = lobby.count(0xf2)
    while last_count < n:
        arrivals.append(time.time())
        lobby.send(b"\\xfd")
        last_count += 1
print(f"  0xf2 arrivals: {len(arrivals)}", flush=True)
with open(RESULT, "w") as f:
    for t in arrivals:
        f.write(f"{t}\\n")
lobby.shutdown()
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
if len(times) < 2:
    print(f"FAIL: only {len(times)} 0xf2 frames in 75 s (need >= 2)")
    sys.exit(1)
gaps = [times[i + 1] - times[i] for i in range(len(times) - 1)]
print(f"  arrivals: {len(times)}, gaps: " +
      ", ".join(f"{g:.1f}s" for g in gaps))
# 30 s nominal +/- 4 s slack (poll wakeups + fake ack RTT)
bad = [g for g in gaps if not (26.0 <= g <= 34.0)]
if bad:
    print(f"FAIL: gap(s) outside [26, 34] s: {bad}")
    sys.exit(2)
print("RESULT: PASS (0xf2 cadence steady at ~30 s with acks flowing)")
PY
rm -f "$RESULT"
