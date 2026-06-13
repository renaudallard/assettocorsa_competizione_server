#!/bin/sh
# maxMonitorsPerIp cfg toggle regression.  With a high global cap but a
# per-IP cap of 1, the second SMPR conn from the same source IP must be
# rejected by the per-IP path (smpr.c:260) -- a HANDSHAKE_RESULT with
# success=0 and "monitor cap reached", logged as "per-IP cap reached"
# (distinct from the global "global cap reached").  Both test conns are
# from 127.0.0.1, so they share a source IP and exercise the per-IP path
# without tripping the global cap.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd

ACCD_PID=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
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

pkill -KILL -f 'accd -c '       >/dev/null 2>&1 || true
pkill -KILL -f 'tools/bot/bot ' >/dev/null 2>&1 || true
sleep 1

cp cfg/settings.json cfg/settings.json.bak
python3 -c "
import json
o = json.load(open('cfg/settings.json'))
o['maxMonitors'] = 10
o['maxMonitorsPerIp'] = 1
json.dump(o, open('cfg/settings.json', 'w'), indent=4)
print('  cfg/settings.json: maxMonitors = 10, maxMonitorsPerIp = 1')
"

rm -f accd.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

python3 - <<'PY'
import socket, struct, sys

def smpr_req(name):
    body = b'\x0a' + bytes([len(name)]) + name.encode() + \
           b'\x10\xfa\x01' + b'\x18\x01' + b'\x20\x01' + b'\x28\x01'
    return struct.pack('<H', len(body)) + body

def read_one(sock, timeout):
    sock.settimeout(timeout)
    buf = b''
    while True:
        try:
            chunk = sock.recv(8192)
        except socket.timeout:
            return None
        if not chunk:
            return None
        buf += chunk
        if len(buf) < 2: continue
        flen = buf[0] | (buf[1] << 8)
        if len(buf) >= 2 + flen:
            return buf[2:2+flen]

# Conn 1 (127.0.0.1): under both caps, should be accepted.
s1 = socket.create_connection(('127.0.0.1', 9302), timeout=3)
s1.sendall(smpr_req('mon1'))
r1 = read_one(s1, 2.0)
if not r1 or r1[0] != 0x01:
    print('FAIL: conn1 did not get HANDSHAKE_RESULT')
    sys.exit(1)
if not (r1[1] == 0x08 and r1[2] == 1):
    print('FAIL: conn1 should have success=1')
    sys.exit(2)
print('conn1 accepted (success=1)')

# Conn 2 (same 127.0.0.1 IP): under the global cap (10) but over the
# per-IP cap (1), so the per-IP path must reject it.
s2 = socket.create_connection(('127.0.0.1', 9302), timeout=3)
s2.sendall(smpr_req('mon2'))
r2 = read_one(s2, 2.0)
if not r2 or r2[0] != 0x01:
    print('FAIL: conn2 did not get HANDSHAKE_RESULT')
    sys.exit(3)
if b'monitor cap reached' not in r2:
    print('FAIL: conn2 reply lacks monitor-cap-reached text')
    print(f'  hex: {r2.hex()}')
    sys.exit(4)
print('conn2 rejected with "monitor cap reached"')

s1.close(); s2.close()
print('RESULT: PASS (maxMonitorsPerIp=1 accepts 1, rejects 2nd from same IP)')
PY

# Confirm it was the per-IP path, not the global cap.
if ! grep -q 'per-IP cap' accd.log; then
    echo "FAIL: accd.log lacks the 'per-IP cap reached' line"
    exit 5
fi
if grep -q 'global cap' accd.log; then
    echo "FAIL: global cap fired (maxMonitors too low to isolate per-IP path)"
    exit 6
fi
echo "RESULT: PASS (rejection came from the per-IP cap, not the global cap)"
