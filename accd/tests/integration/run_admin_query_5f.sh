#!/bin/sh
# UDP 0x5f ACP_ADMIN_QUERY regression.
#
# The kson lobby identity handshake: a caller echoes our 10-char
# token_b as a kson byte-string [0x5f][u16 byte_count][raw bytes];
# accd replies ONLY on a token match with a bare kson byte-string
# [u16 64][64 raw token_a bytes] (no opcode prefix, NOT Format-A),
# mirroring the exe (FUN_140027f80 -> writeKsonString FUN_14004d240).
#
# token_b is a random per-launch fingerprint we cannot predict from
# outside, so this test exercises the reachable-without-the-secret
# surface: that rd_str_raw parses a well-formed and a truncated query
# without over-reading, that a non-matching token draws no reply (the
# exe gates the same way), and that none of it crashes the server.
# The happy-path reply wire is verified by reverse engineering.
#
# We hand-build the UDP probes in Python so this doesn't need a bot.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd

ACCD_PID=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT

pkill -KILL -f 'accd -c '       >/dev/null 2>&1 || true
pkill -KILL -f 'tools/bot/bot ' >/dev/null 2>&1 || true
sleep 1

rm -f accd.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

python3 - <<'PY'
import socket, struct, sys

ADDR = ('127.0.0.1', 9303)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(1.0)

def expect_no_reply(body, label):
    sock.sendto(body, ADDR)
    try:
        reply, _ = sock.recvfrom(1024)
    except socket.timeout:
        print(f'  {label}: no reply (ok)')
        return
    print(f'FAIL: {label} drew an unexpected reply {reply.hex()}')
    sys.exit(1)

# Well-formed kson byte-string carrying a token that cannot match the
# random per-launch token_b -> token mismatch -> no reply.
wrong = b'ZZZZZZZZZZ'
expect_no_reply(bytes([0x5f]) + struct.pack('<H', len(wrong)) + wrong,
                'wrong-token')

# Truncated: declared length far exceeds the bytes present.  rd_str_raw
# must hit its rd_remaining bailout and the handler must drop it.
expect_no_reply(bytes([0x5f]) + struct.pack('<H', 100) + b'abc',
                'truncated')

# Header only (no length) and empty string: both must parse-fail / no-op.
expect_no_reply(bytes([0x5f]), 'header-only')
expect_no_reply(bytes([0x5f]) + struct.pack('<H', 0), 'empty')

# Liveness: a 7-byte 0x17 keepalive probe must still echo, proving the
# 0x5f packets did not wedge or crash the server.
sock.sendto(bytes([0x17]) + struct.pack('<Q', 0x1122334455)[:6], ADDR)
try:
    reply, _ = sock.recvfrom(64)
except socket.timeout:
    print('FAIL: server unresponsive after 0x5f probes (no 0x17 echo)')
    sys.exit(2)
if not reply or reply[0] != 0x17:
    print(f'FAIL: 0x17 echo malformed: {reply.hex()}')
    sys.exit(3)
print('  liveness: 0x17 echo ok')
print('RESULT: PASS (rd_str_raw parses/bails cleanly, mismatch drops, server alive)')
PY

if grep -qiE 'AddressSanitizer|runtime error|Segmentation' accd.log; then
    echo "FAIL: sanitizer/crash signature in accd.log"
    exit 4
fi
echo "  accd.log clean of crash signatures"
