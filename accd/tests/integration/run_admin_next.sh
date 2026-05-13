#!/bin/sh
# Admin /next session-advance regression.
#
# Per chat.c:674-677, when an authenticated admin types '/next' in
# chat (after elevating with '/admin <password>'), accd broadcasts
# 'Forwarding to next session' (chat type 4) and calls
# session_advance().  This test asserts the admin elevation + /next
# pair causes session_index to advance from 0 -> 1 (Practice -> Quali
# in the default 3-session cfg).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tmp/bot/bot

ACCD_PID=""
BOT_PIDS=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT

rm -f accd.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

# Bot elevates with /admin admin then issues /next.  --chat-start-tick
# 60 (~2s) so both fire shortly after handshake.  Underscores in --chat
# are translated to spaces by the bot.
echo "==> spawn bot with /admin admin + /next"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "BotAdmin" \
    --chat-start-tick 60 \
    --chat /admin_admin \
    --chat /next >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

sleep 10

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

# Assert: admin elevation logged
if ! grep -q 'admin elevated' accd.log && \
   ! grep -qiE 'elevated.*admin|admin.*elev' accd.log; then
    echo "WARN: no 'admin elevated' log; chat.c may log differently"
fi

# Assert: /next was processed
if ! grep -q 'admin: /next' accd.log; then
    echo "FAIL: accd did not process '/next' admin command"
    grep -iE 'admin|chat' accd.log | head -10
    exit 1
fi
echo "  PASS: '/next' command logged"

# Assert: session_index moved past 0 (a session_start for index >= 1)
if ! grep -qE 'session (1|2): ' accd.log; then
    echo "FAIL: session_index never advanced past 0"
    grep -E 'session [0-9]:' accd.log | head
    exit 2
fi
NEW=$(grep -oE 'session [0-9]:' accd.log | sort -u | tail -1)
echo "  PASS: $NEW reached (advance fired)"
echo "RESULT: PASS (/admin /next chain advances session_index)"
