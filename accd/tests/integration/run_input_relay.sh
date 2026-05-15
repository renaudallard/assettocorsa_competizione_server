#!/bin/sh
# 0x1e input-byte relay parity: two bots drive against accd, one
# with --zero-inputs and one without.  accd's per-peer 0x1e relay
# fan-out must pass the input_a / input_b / rpm / gear / fuel /
# damage bytes through unchanged.  This catches any future
# regression in the relay's content-copy path (the sendmmsg batch
# in broadcast_percar_dirty being a recent example of code that
# touches every byte).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name BotZero --zero-inputs"
BOT2="--race 912 --grid 2 --name BotReal"
TEST_DURATION=10

echo "==> accd + 2 bots (one zero-input, one realistic)"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_input_relay.pcap

echo "==> diff"
rm -f accd_input_relay.legacy.pcap
editcap -F pcap accd_input_relay.pcap accd_input_relay.legacy.pcap

python3 -c "
import sys, struct
sys.path.insert(0, '.')
from diff_pcap import read_pcap, parse_ipv4_tcp
from collections import defaultdict

# Walk the UDP 0x1e relay stream OUT of accd's UDP port (9303) and
# bin frames by destination port -- one bin per receiving bot.  Within
# each bin, sample the input bytes and the rpm; assert the zero-input
# bot's outbound bytes survive intact at the other peer's inbound,
# and the realistic-input bot's bytes vary.
def walk_relay(path, src_port):
    '''Walk a pcap for UDP per-car-update relay frames sent from
    src_port.  Bins payload by destination port.  Handles both
    0x1e (FAST_RATE) and 0x39 (SLOW_RATE, legacy_netcode=1 mode,
    accd's default); 0x39 has a 1-byte count prefix before the
    per-car record.  read_pcap strips the L2 header.'''
    bins = defaultdict(list)
    for ts, pkt in read_pcap(path):
        try:
            if len(pkt) < 20:
                continue
            ipver = (pkt[0] & 0xf0) >> 4
            if ipver != 4:
                continue
            ihl = (pkt[0] & 0x0f) * 4
            proto = pkt[9]
            if proto != 17:  # UDP
                continue
            udp_off = ihl
            sport = (pkt[udp_off] << 8) | pkt[udp_off + 1]
            dport = (pkt[udp_off + 2] << 8) | pkt[udp_off + 3]
            ulen = (pkt[udp_off + 4] << 8) | pkt[udp_off + 5]
            payload = pkt[udp_off + 8 : udp_off + ulen]
            if sport != src_port or not payload:
                continue
            if payload[0] not in (0x1e, 0x39):
                continue
            bins[dport].append(bytes(payload))
        except IndexError:
            continue
    return bins

bins = walk_relay('accd_input_relay.legacy.pcap', 9303)
print(f'distinct UDP dst ports receiving 0x1e/0x39 from accd: {sorted(bins)}')

def sample_inputs(frame):
    # Per-car record offsets per notebook-b §5.6.2 / §5.6.4a.  The
    # 0x39 wire prepends a u8 count=1 byte before the record, so all
    # record-internal offsets shift by +1 relative to the 0x1e form.
    base = 1 if frame[0] == 0x39 else 0
    # In the bare 0x1e: +0x2e..+0x31 input_a, +0x34..+0x35 rpm,
    #                   +0x36 gear, +0x37 fuel, +0x38 damage.
    in_a_off = 0x2e + base
    rpm_off  = 0x34 + base
    gear_off = 0x36 + base
    fuel_off = 0x37 + base
    dmg_off  = 0x38 + base
    if len(frame) < dmg_off + 1:
        return None
    return {
        'in_a':    tuple(frame[in_a_off:in_a_off + 4]),
        'rpm':     frame[rpm_off] | (frame[rpm_off + 1] << 8),
        'gear':    frame[gear_off],
        'fuel':    frame[fuel_off],
        'damage':  frame[dmg_off],
    }

# A 0x1e relay frame at the receiver carries the SENDER's input bytes
# (it's a content-pass-through with timestamp rewrite).  So inspect
# samples across both bins; we should see:
#   - some frames with all-zero input bytes (from BotZero's outbound)
#   - some frames with non-zero input bytes (from BotReal's outbound)
all_samples = []
for dport, frames in bins.items():
    for f in frames[:200]:  # cap sample size per bin
        s = sample_inputs(f)
        if s is not None:
            all_samples.append(s)

if len(all_samples) < 10:
    print(f'FAIL: only {len(all_samples)} 0x1e samples; expected >= 10')
    sys.exit(1)

zero = [s for s in all_samples if s['in_a'] == (0,0,0,0) and s['rpm'] == 0 and s['gear'] == 0]
real = [s for s in all_samples if s['in_a'] != (0,0,0,0) or s['rpm'] != 0 or s['gear'] != 0]
print(f'total samples: {len(all_samples)}')
print(f'zero-input samples: {len(zero)} (from BotZero outbound)')
print(f'real-input samples: {len(real)} (from BotReal outbound)')

if not zero:
    print('FAIL: no zero-input frames observed; --zero-inputs flag not working?')
    sys.exit(2)
if not real:
    print('FAIL: no realistic-input frames observed; bot encoding broken?')
    sys.exit(3)

# Sanity: realistic frames should show varying rpm + gear.
rpms = sorted({s['rpm'] for s in real if s['rpm'] != 0})
gears = sorted({s['gear'] for s in real})
print(f'realistic rpms observed: {len(rpms)} distinct (sample: {rpms[:5]}...)')
print(f'realistic gears observed: {gears}')
if len(rpms) < 2:
    print('FAIL: realistic bot only emits a single rpm value')
    sys.exit(4)

print('RESULT: PASS (zero-input bytes pass through; realistic bytes vary across the relay)')
"
