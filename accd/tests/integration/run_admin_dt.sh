#!/bin/sh
# Admin /dt — issue Drive-Through to car by race number.  Test the
# chat path now that the wire fix is in (commit ed9985b).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

./run_paired.sh admin_dt 30 "--race 911 --grid 1 --name BotAdminDT --chat-start-tick 60 --chat /admin_admin --chat /dt_911"

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_dt.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_admin_dt.legacy.pcap', 19298)
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
