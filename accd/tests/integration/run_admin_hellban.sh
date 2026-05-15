#!/bin/sh
# /hellban regression.  Admin flips c->hellbanned on a target peer;
# dispatch.c:127 then silently drops every inbound msg (except the
# handshake/disconnect) from that peer.  Their chat doesn't relay.
#
# Test: bot1 admin /hellbans bot2 (#912).  bot2 then sends chat
# *after* the ban tick.  accd should:
#  - emit the "Car #912 has been hellbanned" banner
#  - NOT emit any 0x2b chat from bot2's later message
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT1="--race 911 --grid 1 --name BotAdm \
    --chat-start-tick 60 --chat /admin_admin --chat /hellban_912"
# bot2 starts chatting AFTER bot1's hellban lands (tick > 60+5).
BOT2="--race 912 --grid 2 --name BotVic \
    --chat-start-tick 150 --chat post-ban-message-should-vanish"
TEST_DURATION=14

echo "==> accd + 2 bots: admin hellbans victim, victim chats after"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_admin_hellban.pcap

if ! grep -qE 'admin: /hellban 912' accd.log; then
    echo "FAIL: no '/hellban 912' log line"
    exit 1
fi
echo "  /hellban logged"

rm -f accd_admin_hellban.legacy.pcap
editcap -F pcap accd_admin_hellban.pcap accd_admin_hellban.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_hellban.legacy.pcap', 9302)
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

ban_banner = False
victim_chat = False
for b in chat:
    s, off = rd_str_a(b, 1)
    body, off = rd_str_a(b, off) if s else (None, 0)
    if body is None: continue
    if 'has been hellbanned' in body:
        ban_banner = True
    if 'post-ban-message' in body or 'post-ban' in body:
        victim_chat = True

print(f'hellban banner observed: {ban_banner}')
print(f'victim chat leaked: {victim_chat}')
if not ban_banner:
    print('FAIL: no hellban banner')
    sys.exit(1)
if victim_chat:
    print('FAIL: victim chat relayed despite hellban')
    sys.exit(2)
print('RESULT: PASS (banner emitted + victim chat silently dropped)')
"
