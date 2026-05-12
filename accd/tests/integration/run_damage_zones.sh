#!/bin/sh
# Damage zones (0x43 in / 0x44 out) UDP relay regression.
# Bot1 sends ACP_DAMAGE_ZONES_UPDATE (0x43) over TCP with five zone
# bytes; server fans out SRV_DAMAGE_ZONES_RELAY (0x44) over UDP to
# every peer (bot2 in this scenario) with body
# u8 0x44 + u16 car_id + 5 × u8 zone.  Compare the UDP relay.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name BotDmg --damage 100:5,10,15,20,25"
BOT2="--race 922 --grid 2 --name BotPeer"
TEST_DURATION=15

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT1\" \"$BOT2\" >/dev/null 2>&1"
scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_damage.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_damage.pcap

echo "==> diff"
rm -f accd_damage.legacy.pcap kunos_damage.legacy.pcap
editcap -F pcap accd_damage.pcap accd_damage.legacy.pcap
editcap -F pcap kunos_damage.pcap kunos_damage.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

# 0x44 is UDP — diff_pcap currently only reassembles TCP.  Read the
# pcap directly with scapy or fall back to a raw UDP parse.
import struct
try:
    from scapy.all import rdpcap, UDP, Raw
except ImportError:
    print('FAIL: scapy required for UDP frame extraction')
    sys.exit(1)

def find_0x44(pcap_path, udp_port, car_id_filter=None):
    rels = []
    for p in rdpcap(pcap_path):
        if UDP not in p or Raw not in p:
            continue
        if p[UDP].sport != udp_port:
            continue
        payload = bytes(p[Raw].load)
        if len(payload) >= 8 and payload[0] == 0x44:
            cid = struct.unpack('<H', payload[1:3])[0]
            if car_id_filter is None or cid == car_id_filter:
                rels.append(payload)
    return rels

# Both servers assign car_id=1001 to the first bot.
ar = find_0x44('accd_damage.legacy.pcap', 9303, 1001)
kr = find_0x44('kunos_damage.legacy.pcap', 19299, 1001)
print(f'accd 0x44 frames (car=1001): {len(ar)}')
print(f'kunos 0x44 frames (car=1001): {len(kr)}')
if not ar or not kr:
    print('FAIL NO_FRAMES')
    sys.exit(1)
# Use the LAST frame so initial-state (zeros) frames don't mask the
# real damage update.
a, k = ar[-1], kr[-1]
print(f'accd[last]:  {a.hex()}')
print(f'kunos[last]: {k.hex()}')
if a == k:
    print('RESULT: IDENTICAL (0x44 relay body byte-exact)')
else:
    a_zones = list(a[3:8])
    k_zones = list(k[3:8])
    print(f'accd zones={a_zones} kunos zones={k_zones}')
    print('RESULT: DIFFER')
    sys.exit(2)
"
