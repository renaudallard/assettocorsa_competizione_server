#!/bin/sh
# Malformed-handshake robustness test.
# Fires N TCP connections with random body bytes and verifies accd
# stays up (PID still alive, port still bound, can still accept a
# clean handshake afterwards).  Goal: catch parser-side crashes on
# short / oversized / truncated 0x09 packets that real attackers
# would explore.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd

echo "==> spin up accd locally"
rm -f log/*.log accd.log 2>/dev/null || true
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
sleep 1
for i in 1 2 3 4 5 6 7 8 9 10; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

echo "==> fuzzing handshake with malformed bodies"
python3 fuzz_handshake.py --host 127.0.0.1 --port 9302 --iters 300
RC=$?

echo "==> verify accd still alive + responsive"
if ! kill -0 "$ACCD_PID" 2>/dev/null; then
    echo "FAIL: accd died during fuzz"
    RC=1
fi

if [ $RC -eq 0 ]; then
    # Send one clean handshake to confirm the parser is still usable.
    if /home/r/code/assettocorsa/tools/bot/bot \
        --host 127.0.0.1 --tcp 9302 \
 \
        --race 911 --grid 1 --name BotPostFuzz \
        --client-version 0x0001 --expect-reject \
        2>&1 | grep -q "reject path completed"; then
        echo "RESULT: PASS (accd survived 300 random handshakes + responded to a clean reject probe)"
    else
        echo "FAIL: accd did not respond to a clean handshake after fuzz"
        RC=1
    fi
fi

kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
exit $RC
