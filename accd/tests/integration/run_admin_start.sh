#!/bin/sh
# /admin + /start regression.  Bot elevates with /admin <pw> then
# issues /start.  accd's chat.c:807 logs "admin: /start" and
# broadcasts the "Session started by administrator" type-4 banner.
# /go is a synonym for /start; both share the same handler arm.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotAdmin --chat-start-tick 60 \
    --chat /admin_admin --chat /start"
TEST_DURATION=12

echo "==> accd + bot, /admin then /start"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_admin_start.pcap

echo "==> diff"
rm -f accd_admin_start.legacy.pcap
editcap -F pcap accd_admin_start.pcap accd_admin_start.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_start.legacy.pcap', 9302)
chat = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x2b]
def rd_str_a(buf, off):
    if off >= len(buf): return None, off
    n = buf[off]; off += 1
    out = bytearray()
    for _ in range(n):
        if off + 4 > len(buf): return None, off
        cp = buf[off] | (buf[off+1]<<8) | (buf[off+2]<<16) | (buf[off+3]<<24)
        if cp < 0x80: out.append(cp)
        off += 4
    return out.decode('ascii','replace'), off

want = 'Session started by administrator'
ok = False
for b in chat:
    s, off = rd_str_a(b, 1)
    body, off = rd_str_a(b, off) if s else (None, 0)
    if body == want and off + 5 <= len(b) and b[off + 4] == 4:
        ok = True; break
print(f'banner with body={want!r} type=4: {ok}')
if not ok:
    print('FAIL'); sys.exit(1)
print('RESULT: PASS (/start emits the type-4 session-started banner)')
"

if ! grep -q 'admin: /start' accd.log; then
    echo "FAIL: accd.log missing 'admin: /start'"
    exit 2
fi
echo "  accd.log has 'admin: /start' line"
