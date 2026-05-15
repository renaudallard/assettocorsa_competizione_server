#!/bin/sh
# Damage propagation regression: --damage T:z1,z2,z3,z4,z5 makes the
# bot emit a 0x43 ACP_DAMAGE_ZONES_UPDATE with five zone bytes.
# accd's h_damage_zones (handlers.c) re-broadcasts as 0x44 SRV_
# DAMAGE_ZONES_RELAY over UDP carrying those same five bytes to
# every other peer.  This test pins the five-byte payload survives
# the relay.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Bot 1 emits damage; bot 2 receives.  Pick recognisable byte values
# (0x10, 0x20, 0x30, 0x40, 0x50) so they're easy to find in the pcap.
BOT1="--race 911 --grid 1 --name BotHit --damage 60:16,32,48,64,80"
BOT2="--race 912 --grid 2 --name BotObs"
TEST_DURATION=12

echo "==> accd + 2 bots, damage on tick 60"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_damage_flow.pcap

echo "==> diff"
rm -f accd_damage_flow.legacy.pcap
editcap -F pcap accd_damage_flow.pcap accd_damage_flow.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import read_pcap

# h_damage_zones emits SRV_DAMAGE_ZONES_RELAY (0x44) via bcast_all_udp.
# Walk UDP egress from port 9303 and look for 0x44 frames.
frames = []
for ts, pkt in read_pcap('accd_damage_flow.legacy.pcap'):
    if len(pkt) < 28 or (pkt[0] & 0xf0) >> 4 != 4 or pkt[9] != 17:
        continue
    ihl = (pkt[0] & 0xf) * 4
    sport = (pkt[ihl] << 8) | pkt[ihl + 1]
    ulen = (pkt[ihl + 4] << 8) | pkt[ihl + 5]
    payload = pkt[ihl + 8 : ihl + ulen]
    if sport == 9303 and payload and payload[0] == 0x44:
        frames.append(bytes(payload))

print(f'accd 0x44 SRV_DAMAGE_ZONES_RELAY frames: {len(frames)}')
if not frames:
    print('FAIL: accd never emitted 0x44 damage broadcast')
    sys.exit(1)

# Wire body: u8 msg + u16 car_id + 5 u8 zones = 8 bytes total
need = bytes([16, 32, 48, 64, 80])
match = [b for b in frames if len(b) >= 8 and b[-5:] == need]
print(f'frames carrying the expected zone bytes: {len(match)}')
if not match:
    print('FAIL: no 0x44 frame carries the expected zone bytes')
    for b in frames[:3]:
        print(f'  hex: {b.hex()}')
    sys.exit(2)

# Body shape: msg=0x44 + u16 car_id + 5 u8 zones = 8 bytes.
sizes = sorted({len(b) for b in match})
print(f'0x44 frame sizes: {sizes}')
if sizes != [8]:
    print(f'FAIL: 0x44 frame size {sizes}, expected [8]')
    sys.exit(3)

print(f'PASS: {len(match)}/{len(frames)} frames carry zones {list(need)}')
print('RESULT: PASS (bot 0x43 -> accd 0x44 zone-byte fan-out works)')
"
