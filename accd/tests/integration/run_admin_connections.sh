#!/bin/sh
# /connections regression.  Admin issues /connections; accd
# broadcasts "Active connections:" banner followed by one banner
# per peer ("conn=<id> car=<id>[ admin][ spectator]") .
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name BotAdm \
    --chat-start-tick 60 --chat /admin_admin --chat /connections"
BOT2="--race 912 --grid 2 --name BotPeer"
TEST_DURATION=12

echo "==> accd + 2 bots, admin /connections"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_admin_connections.pcap

rm -f accd_admin_connections.legacy.pcap
editcap -F pcap accd_admin_connections.pcap accd_admin_connections.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_connections.legacy.pcap', 9302)
chat = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x2b]
def rd_str_a(buf, off):
    if off >= len(buf): return None, off
    n = buf[off]; off += 1
    cps = []
    for _ in range(n):
        if off + 4 > len(buf): return None, off
        cps.append(buf[off] | (buf[off+1]<<8) | (buf[off+2]<<16) | (buf[off+3]<<24))
        off += 4
    return ''.join(chr(c) for c in cps), off

header = False
rows = 0
for b in chat:
    s, off = rd_str_a(b, 1)
    body, off = rd_str_a(b, off) if s else (None, 0)
    if body is None: continue
    if body == 'Active connections:':
        header = True
    elif body.lstrip().startswith('conn='):
        rows += 1

print(f'header observed: {header}, conn= rows: {rows}')
if not header:
    print('FAIL: no \"Active connections:\" header')
    sys.exit(1)
if rows < 2:
    print(f'FAIL: expected >= 2 conn= rows, got {rows}')
    sys.exit(2)
print('RESULT: PASS (/connections lists header + per-peer rows)')
"
