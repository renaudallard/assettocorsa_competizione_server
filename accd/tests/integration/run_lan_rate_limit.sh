#!/bin/sh
# LAN discovery rate-limit regression.
#
# accd's lan.c rate-limits 0xc0 replies to one per second per source
# IP — without this the 6 B probe -> 270 B reply asymmetry is a
# ~45x UDP-reflection amplifier.  This test fires two probes <1s
# apart from the same source and asserts only the first elicits a
# reply.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd

pkill -KILL -f 'accd -c ' >/dev/null 2>&1 || true
sleep 1

rm -f accd.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
trap "kill -TERM $ACCD_PID 2>/dev/null || true" EXIT INT TERM
for i in 1 2 3 4 5; do
    if ss -uln 2>/dev/null | grep -q ':8999'; then break; fi
    sleep 0.3
done

python3 - <<'PY'
import socket, struct, sys, time

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(1.5)

probe = bytes([0xbf, 0x48]) + struct.pack('<I', 0xdeadbeef)

# First probe: expect a reply.
sock.sendto(probe, ('127.0.0.1', 8999))
try:
    reply1, _ = sock.recvfrom(1024)
    print(f'probe 1: {len(reply1)} B reply')
except socket.timeout:
    print('FAIL: probe 1 timed out (no reply at all)')
    sys.exit(1)
if not reply1 or reply1[0] != 0xc0:
    print(f'FAIL: probe 1 reply byte was 0x{reply1[0]:02x}, expected 0xc0')
    sys.exit(2)

# Second probe < 1 s later: expect NO reply.
time.sleep(0.2)
sock.sendto(probe, ('127.0.0.1', 8999))
sock.settimeout(0.8)  # well under the 1 s rate window
try:
    reply2, _ = sock.recvfrom(1024)
    print(f'FAIL: probe 2 got a {len(reply2)} B reply within 1 s')
    sys.exit(3)
except socket.timeout:
    print('probe 2: rate-limited (no reply within 800 ms) — good')

# Third probe AFTER the rate window: expect a reply again.
time.sleep(1.2)
sock.sendto(probe, ('127.0.0.1', 8999))
sock.settimeout(1.5)
try:
    reply3, _ = sock.recvfrom(1024)
    print(f'probe 3 (after 1s wait): {len(reply3)} B reply')
except socket.timeout:
    print('FAIL: probe 3 timed out after the rate window expired')
    sys.exit(4)
if not reply3 or reply3[0] != 0xc0:
    print(f'FAIL: probe 3 reply byte was 0x{reply3[0]:02x}')
    sys.exit(5)

print('RESULT: PASS (rate limit allows 1, drops 2, allows 3 after refill)')
PY

if ! grep -q 'lan: rate-limited reply' accd.log; then
    echo "WARN: accd.log lacks 'lan: rate-limited reply' (log_debug, may need -d)"
fi
