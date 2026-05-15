#!/bin/sh
# UDP 0x5f ACP_ADMIN_QUERY regression.  Client sends a Format-B
# (u16 count + N x u16) string carrying an identifier; accd replies
# with 0x5f + Format-A (u8 count + N x u32) server_name and logs
# "udp 0x5f admin query from <ip>:<port>".
#
# We hand-build the UDP probe in Python so this doesn't need a bot.
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
import socket, struct, sys, time

probe_id = 'whoami'
# Format-B = u16 count + N x u16 chars.
body = bytes([0x5f]) + struct.pack('<H', len(probe_id))
for ch in probe_id:
    body += struct.pack('<H', ord(ch))

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(2.0)
sock.sendto(body, ('127.0.0.1', 9303))

try:
    reply, peer = sock.recvfrom(1024)
except socket.timeout:
    print('FAIL: no UDP reply within 2s')
    sys.exit(1)

print(f'reply {len(reply)}B: {reply.hex()}')
if not reply or reply[0] != 0x5f:
    print(f'FAIL: reply msg byte was 0x{reply[0]:02x}, expected 0x5f')
    sys.exit(2)

# Format-A = u8 count + N x u32.
n = reply[1]
expected = 2 + 4 * n
if len(reply) != expected:
    print(f'FAIL: reply length {len(reply)} != expected {expected}')
    sys.exit(3)

name = ''
for i in range(n):
    cp = (reply[2 + 4*i]
         | (reply[3 + 4*i] << 8)
         | (reply[4 + 4*i] << 16)
         | (reply[5 + 4*i] << 24))
    name += chr(cp)
print(f'server_name in reply: {name!r}')
if not name:
    print('FAIL: empty server_name')
    sys.exit(4)
print('RESULT: PASS (0x5f UDP probe replies with server name)')
PY

if ! grep -q 'udp 0x5f admin query' accd.log; then
    echo "FAIL: accd.log missing the 0x5f log line"
    exit 5
fi
echo "  accd.log carries the 0x5f admin-query line"
