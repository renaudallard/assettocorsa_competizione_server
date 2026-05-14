#!/bin/sh
# SMPR (ServerMonitor protobuf) end-to-end smoke.
#
# Wire-format reference: §12B of notebook-b/NOTEBOOK_B.md.  The
# server demultiplexes the SMPR vs sim lanes at the first byte of
# the framed body: 0x0a is the protobuf tag (field 1, wire 2 =
# length-delimited) that starts every ServerMonitorConnectionRequest.
#
# The test:
#   1.  Start accd locally.
#   2.  Connect a sim bot so the SMPR stream has cars / conns to
#       describe.
#   3.  Open a TCP connection from a hand-rolled Python "monitor"
#       client, send a ServerMonitorConnectionRequest, then read
#       2.5 seconds of framed protobuf.
#   4.  Verify all 7 message types arrived and the REALTIME_UPDATE
#       cadence matches the requested 250 ms interval.
set -e
cd "$(dirname "$0")"
pkill -KILL -f 'accd -c cfg' >/dev/null 2>&1 || true
sleep 1

rm -f smpr_accd.log smpr_bot.log
/home/r/code/assettocorsa/accd/accd -c cfg > smpr_accd.log 2>&1 &
APID=$!
sleep 1

/home/r/code/assettocorsa/tools/bot/bot --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name SmprBot > smpr_bot.log 2>&1 &
B=$!
sleep 2

python3 - <<'PY'
import socket, struct, sys, time

# Build a ServerMonitorConnectionRequest:
#   field 1 (displayName, string)             'probe'
#   field 2 (realtimeCarUpdateInterval, i32)   250
#   field 3 (sendSelfcontainingLeaderboards)   true
#   field 4 (sendExtendedLeaderboards)         true
#   field 5 (registerToAllEvents)              true
body = b'\x0a\x05probe' + b'\x10\xfa\x01' + b'\x18\x01' + b'\x20\x01' + b'\x28\x01'
hdr = struct.pack('<H', len(body))

sock = socket.create_connection(('127.0.0.1', 9302), timeout=3)
sock.sendall(hdr + body)
sock.settimeout(0.2)

# Read for 2.5 s, walking u16-length-prefixed protobuf frames.
counts = {}
buf = b''
deadline = time.time() + 2.5
while time.time() < deadline:
    try:
        chunk = sock.recv(8192)
    except socket.timeout:
        continue
    if not chunk:
        break
    buf += chunk
    while len(buf) >= 2:
        flen = buf[0] | (buf[1] << 8)
        if len(buf) < 2 + flen:
            break
        msg = buf[2:2+flen]
        if msg:
            counts[msg[0]] = counts.get(msg[0], 0) + 1
        buf = buf[2+flen:]

labels = {
    0x01: 'REGISTRATION_RESULT',
    0x02: 'SERVER_CONFIGURATION',
    0x03: 'SESSION_STATE',
    0x04: 'CAR_ENTRY',
    0x05: 'CONNECTION_ENTRY',
    0x06: 'REALTIME_UPDATE',
    0x07: 'LEADERBOARD_UPDATE',
}
for mt in sorted(counts):
    print(f'  0x{mt:02x} {labels.get(mt, "?")}: {counts[mt]}')

required = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}
missing = [mt for mt in required if counts.get(mt, 0) == 0]
if missing:
    print(f'FAIL missing message types: {", ".join(f"0x{m:02x}" for m in missing)}')
    sys.exit(1)

# At 250 ms interval over ~2.5 s we should see ~9-11 REALTIME_UPDATEs.
# Allow 7..14 for jitter.
rt = counts.get(0x06, 0)
if rt < 7 or rt > 14:
    print(f'FAIL REALTIME cadence: expected 7..14 over 2.5s, got {rt}')
    sys.exit(2)

print('RESULT: PASS (all 7 SMPR message types observed, cadence within bounds)')
PY

rc=$?
kill -TERM $B $APID 2>/dev/null || true
wait 2>/dev/null || true
rm -f smpr_accd.log smpr_bot.log
exit $rc
