#!/bin/sh
# Lobby browser ping-probe echo regression.
# AC2 client sends a 7-byte 0x17 UDP probe (msg_id + u32 client_ts +
# u16 server_port) to every server in the lobby list to measure RTT.
# Kunoss accServer.exe FUN_140027f80 echoes the body verbatim;
# accd dispatch.c:273-283 mirrors this.  Without the echo the client
# shows "" in the PING column.
#
# This test sends the probe from an ephemeral UDP socket and asserts
# the reply matches byte-for-byte.
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

echo "==> spin up accd"
rm -f accd.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
sleep 1
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

echo "==> probe accd UDP/9303 with 7-byte 0x17"
python3 - <<'PY'
import socket, struct, sys, time

PROBE = bytes([0x17]) + struct.pack("<I", 0x12345678) + \
        struct.pack("<H", 9302)
assert len(PROBE) == 7

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2.0)
s.sendto(PROBE, ("127.0.0.1", 9303))
try:
    data, peer = s.recvfrom(64)
except socket.timeout:
    print("FAIL: no echo within 2.0s -- lobby browser would show '-'")
    sys.exit(1)

if data != PROBE:
    print(f"FAIL: echo mismatch")
    print(f"  sent: {PROBE.hex()}")
    print(f"  got:  {data.hex()}")
    sys.exit(2)

print(f"  echo OK: {data.hex()} (from {peer[0]}:{peer[1]})")
print("RESULT: PASS (accd echoes 7-byte 0x17 probe byte-exact)")
PY
