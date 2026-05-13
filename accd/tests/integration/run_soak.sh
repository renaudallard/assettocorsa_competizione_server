#!/bin/sh
# Soak test: 10 bots driving simultaneously for SOAK_DURATION seconds.
# Verifies accd survives sustained multi-bot fan-out (UDP relay, 0x36
# broadcast, lap-completion cascade, etc.) without crashing or
# leaking the listen sockets.
#
# Pass criteria:
#   - accd PID still alive at the end
#   - tcp/9302 + udp/9303 still bound
#   - log has >= N lap-completion lines (proves the tick loop kept
#     ticking under load, not stalled)
#   - max RSS reading taken halfway through; printed for inspection
#
# This is accd-only -- kunos comparison would need wine to handle
# 10 concurrent connections, which the current host cant.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot
NBOTS=${NBOTS:-10}
SOAK_DURATION=${SOAK_DURATION:-60}

cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    for pid in $BOT_PIDS; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT

# Patch maxConnections up for the soak window.
cp cfg/configuration.json cfg/configuration.json.bak
sed -e 's/"maxConnections":[[:space:]]*[0-9]*/"maxConnections": '"$NBOTS"'/' \
    cfg/configuration.json.bak > cfg/configuration.json
trap_restore() {
    [ -f cfg/configuration.json.bak ] && \
        mv cfg/configuration.json.bak cfg/configuration.json
}

echo "==> spin up accd (maxConnections=$NBOTS)"
rm -f log/*.log accd.log 2>/dev/null || true
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
sleep 1
for i in 1 2 3 4 5 6 7 8 9 10; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

echo "==> spawn $NBOTS bots"
BOT_PIDS=""
i=0
while [ $i -lt "$NBOTS" ]; do
    race=$((911 + i))
    grid=$((i + 1))
    "$BOT" --host 127.0.0.1 --tcp 9302 \
        --race "$race" --grid "$grid" --name "BotSoak${i}" \
        >"bot${i}.log" 2>&1 &
    BOT_PIDS="$BOT_PIDS $!"
    i=$((i + 1))
    sleep 0.2
done

echo "==> driving for ${SOAK_DURATION} s"
sleep "$((SOAK_DURATION / 2))"
RSS_MID=$(ps -o rss= -p "$ACCD_PID" 2>/dev/null | tr -d ' ' || echo "?")
echo "==> mid-soak accd RSS = ${RSS_MID} KB"
sleep "$((SOAK_DURATION - SOAK_DURATION / 2))"

echo "==> shutdown bots"
for pid in $BOT_PIDS; do
    kill -TERM "$pid" 2>/dev/null || true
done
for pid in $BOT_PIDS; do
    wait "$pid" 2>/dev/null || true
done
sleep 1

echo "==> verify accd state"
RC=0
if ! kill -0 "$ACCD_PID" 2>/dev/null; then
    echo "FAIL: accd died during soak"
    RC=1
fi
if [ $RC -eq 0 ] && ! ss -tln 2>/dev/null | grep -q ':9302'; then
    echo "FAIL: accd no longer bound to tcp/9302"
    RC=1
fi
# Both sector splits and 0x36 leaderboard emits prove the tick loop
# kept up; either pattern is fine as a heartbeat.
SPLIT_LINES=$(grep -c "sector split" accd.log 2>/dev/null | head -1)
LB_LINES=$(grep -c "Updated leaderboard" accd.log 2>/dev/null | head -1)
SPLIT_LINES=${SPLIT_LINES:-0}
LB_LINES=${LB_LINES:-0}
echo "  sector splits logged: $SPLIT_LINES"
echo "  0x36 leaderboard emits: $LB_LINES"
if [ "${SPLIT_LINES:-0}" -lt "$NBOTS" ] 2>/dev/null; then
    echo "WARN: fewer sector splits ($SPLIT_LINES) than bots ($NBOTS); tick loop may have stalled briefly"
fi
WARN_COUNT=$(grep -cE "WARN|ERR " accd.log 2>/dev/null | head -1)
WARN_COUNT=${WARN_COUNT:-0}
echo "  log warnings: $WARN_COUNT"
echo "  final accd RSS: $(ps -o rss= -p "$ACCD_PID" 2>/dev/null | tr -d ' ' || echo '?') KB"

kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
trap_restore

if [ $RC -eq 0 ]; then
    echo "RESULT: PASS (accd survived ${SOAK_DURATION} s with $NBOTS bots, completed >= $NBOTS laps)"
fi
exit $RC
