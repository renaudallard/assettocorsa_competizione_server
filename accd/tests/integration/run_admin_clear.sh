#!/bin/sh
# Admin /clear <raceNumber> wipes pending penalties for the car.
# Bot self-reports a DT first (cat=0:1:3 via 0x41), then admin /clear
# wipes it.  Final 0x36 should show no penalty in tail.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

./run_paired.sh admin_clear 30 "--race 911 --grid 1 --name BotClr \
    --report-penalty 0:1:3 \
    --chat-start-tick 300 --chat /admin_admin --chat /clear_911"

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_clear.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_admin_clear.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]

print(f'accd 0x36: {len(af)} tails {[b[-4:].hex() for b in af]}')
print(f'kunos 0x36: {len(kf)} tails {[b[-4:].hex() for b in kf]}')
if af and kf:
    a, k = af[-1], kf[-1]
    if a == k:
        print('LAST 0x36: IDENTICAL')
    else:
        d = sum(1 for j in range(min(len(a), len(k))) if a[j] != k[j])
        print(f'LAST 0x36: DIFFER ({d} bytes, a={len(a)} k={len(k)})')
"
