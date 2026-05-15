#!/bin/sh
# maxMonitors cfg toggle regression.  When the global cap is hit,
# accd's smpr_handle_connection_request (smpr.c:251) rejects the
# new SMPR conn with a HANDSHAKE_RESULT carrying success=0 and
# "monitor cap reached".  This pins the cap-rejection path.
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
o['maxMonitors'] = 1
json.dump(o, open('cfg/settings.json', 'w'), indent=4)
print('  cfg/settings.json: maxMonitors = 1')
"

rm -f accd.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

python3 - <<'PY'
import socket, struct, sys, time

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

# Conn 1: should be accepted.
s1 = socket.create_connection(('127.0.0.1', 9302), timeout=3)
s1.sendall(smpr_req('mon1'))
r1 = read_one(s1, 2.0)
print(f'conn1 first frame: msg=0x{r1[0]:02x}' if r1 else 'conn1 NO FRAME')
if not r1 or r1[0] != 0x01:
    print('FAIL: conn1 did not get HANDSHAKE_RESULT')
    sys.exit(1)
# success protobuf field 1, wire type 0: tag byte 0x08, then value.
ok1 = r1[1] == 0x08 and r1[2] == 1
print(f'conn1 success: {ok1}')
if not ok1:
    print('FAIL: conn1 should have success=1')
    sys.exit(2)

# Conn 2: should be rejected with monitor cap reached.
s2 = socket.create_connection(('127.0.0.1', 9302), timeout=3)
s2.sendall(smpr_req('mon2'))
r2 = read_one(s2, 2.0)
print(f'conn2 first frame: msg=0x{r2[0]:02x}' if r2 else 'conn2 NO FRAME')
if not r2 or r2[0] != 0x01:
    print('FAIL: conn2 did not get HANDSHAKE_RESULT')
    sys.exit(3)
# Look for any text 'monitor cap reached' anywhere in body.
if b'monitor cap reached' not in r2:
    print('FAIL: conn2 reply lacks monitor-cap-reached text')
    print(f'  hex: {r2.hex()}')
    sys.exit(4)
print('conn2 carries "monitor cap reached"')

s1.close(); s2.close()
print('RESULT: PASS (maxMonitors=1 accepts 1, rejects 2nd with cap message)')
PY

if ! grep -q 'global cap.*reached' accd.log; then
    echo "WARN: accd.log lacks the 'global cap reached' line (test still passed wire-side)"
fi
