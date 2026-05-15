#!/bin/sh
# legacy_netcode toggle: accd emits 0x39 SRV_PERCAR_SLOW_RATE in
# legacy-netcode mode (default; state.c:163 sets it to 1) and 0x1e
# SRV_PERCAR_FAST_RATE when an operator flips the toggle off.  This
# test pins the legacy-on case (the default-shipped behaviour) and
# proves accd routes per-car relays to 0x39 with the expected
# count=1 + per-car body shape.
#
# Future regression on the conditional (e.g. accidentally flipping
# the default, or swapping the opcodes) would surface here.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name BotL"
BOT2="--race 912 --grid 2 --name BotM"
TEST_DURATION=8

echo "==> accd + 2 bots (default legacy_netcode=1)"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_legacy_netcode.pcap

echo "==> diff"
rm -f accd_legacy_netcode.legacy.pcap
editcap -F pcap accd_legacy_netcode.pcap accd_legacy_netcode.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import read_pcap
from collections import Counter

# Walk UDP egress from port 9303; count opcodes in the payload[0].
opc = Counter()
for ts, pkt in read_pcap('accd_legacy_netcode.legacy.pcap'):
    if len(pkt) < 28 or (pkt[0] & 0xf0) >> 4 != 4 or pkt[9] != 17:
        continue
    ihl = (pkt[0] & 0xf) * 4
    sport = (pkt[ihl] << 8) | pkt[ihl + 1]
    ulen = (pkt[ihl + 4] << 8) | pkt[ihl + 5]
    payload = pkt[ihl + 8 : ihl + ulen]
    if sport != 9303 or not payload:
        continue
    opc[payload[0]] += 1

print(f'UDP egress opcode counts: {dict(opc)}')

# Legacy-netcode default => 0x39 dominates per-car relay; 0x1e
# should be absent (or close to it).  Allow a few 0x1e if accd's
# direct echo path leaks them, but the relay path is 0x39.
slow = opc.get(0x39, 0)
fast = opc.get(0x1e, 0)
if slow < 10:
    print(f'FAIL: only {slow} 0x39 relay frames in default legacy_netcode=1 mode')
    sys.exit(1)
if fast > slow / 10:
    print(f'FAIL: 0x1e count {fast} unexpectedly high (slow={slow})')
    sys.exit(2)

# Verify the 0x39 body shape: msg_id=0x39, count=1, then 63-byte
# per-car record => total 65 bytes on the wire.
sizes = []
for ts, pkt in read_pcap('accd_legacy_netcode.legacy.pcap'):
    if len(pkt) < 28 or (pkt[0] & 0xf0) >> 4 != 4 or pkt[9] != 17:
        continue
    ihl = (pkt[0] & 0xf) * 4
    sport = (pkt[ihl] << 8) | pkt[ihl + 1]
    ulen = (pkt[ihl + 4] << 8) | pkt[ihl + 5]
    payload = pkt[ihl + 8 : ihl + ulen]
    if sport == 9303 and payload and payload[0] == 0x39:
        sizes.append(len(payload))
        if len(sizes) >= 5:
            break

from collections import Counter as C
size_counts = C(sizes)
print(f'0x39 frame sizes (first 5): {sizes}')
# Expected: 1 (msg_id) + 1 (count) + 63 (record) = 65 B.
# accd emits one car per frame in this relay path.
if not all(s == 65 for s in sizes):
    print(f'FAIL: unexpected 0x39 sizes (expected 65, got {set(sizes)})')
    sys.exit(3)
# Verify count byte == 1
for ts, pkt in read_pcap('accd_legacy_netcode.legacy.pcap'):
    if len(pkt) < 28 or (pkt[0] & 0xf0) >> 4 != 4 or pkt[9] != 17:
        continue
    ihl = (pkt[0] & 0xf) * 4
    sport = (pkt[ihl] << 8) | pkt[ihl + 1]
    ulen = (pkt[ihl + 4] << 8) | pkt[ihl + 5]
    payload = pkt[ihl + 8 : ihl + ulen]
    if sport == 9303 and payload and payload[0] == 0x39 and payload[1] != 1:
        print(f'FAIL: 0x39 count byte was {payload[1]}, expected 1')
        sys.exit(4)

print('RESULT: PASS (legacy_netcode=1 routes per-car relay to 0x39, 65B, count=1)')
"
