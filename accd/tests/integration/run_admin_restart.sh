#!/bin/sh
# /admin + /restart regression.  Bot elevates with /admin <password>
# then issues /restart.  accd's chat.c:826 logs "admin: /restart",
# broadcasts the "Session restarted by administrator" type-4 banner
# (RC_SENDER = "Race Control") and calls session_reset() without
# bumping session_index.
#
# This pins the chat-banner side of the wire response.  A future
# regression that breaks the broadcast (wrong type, wrong RC sender,
# silent reset) would surface here.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# entry.json under run_test_v2.sh defaults to adminPassword=admin so
# `/admin admin` elevates the bot.
BOT="--race 911 --grid 1 --name BotAdmin --chat-start-tick 60 \
    --chat /admin_admin --chat /restart"
TEST_DURATION=15

echo "==> accd + bot, /admin then /restart"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_admin_restart.pcap

echo "==> diff"
rm -f accd_admin_restart.legacy.pcap
editcap -F pcap accd_admin_restart.pcap accd_admin_restart.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_admin_restart.legacy.pcap', 9302)
chat = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x2b]
print(f'accd 0x2b chat frames: {len(chat)}')
if not chat:
    print('FAIL: no 0x2b chat frames')
    sys.exit(1)

# str_a wire: u8 count + N x u32 codepoints
def rd_str_a(buf, off):
    if off >= len(buf):
        return None, off
    n = buf[off]; off += 1
    out = bytearray()
    for _ in range(n):
        if off + 4 > len(buf):
            return None, off
        cp = buf[off] | (buf[off+1]<<8) | (buf[off+2]<<16) | (buf[off+3]<<24)
        if cp < 0x80:
            out.append(cp)
        off += 4
    return out.decode('ascii','replace'), off

want_sender = 'Race Control'
want_body = 'Session restarted by administrator'

hits = []
for b in chat:
    sender, off = rd_str_a(b, 1)
    if sender is None:
        continue
    body, off = rd_str_a(b, off)
    if body is None or off + 5 > len(b):
        continue
    # i32 (4B) + u8 chat_type
    ctype = b[off + 4]
    if sender == want_sender and body == want_body:
        hits.append((sender, body, ctype))

print(f'banners matching sender={want_sender!r} body={want_body!r}: {len(hits)}')
for s,t,c in hits[:3]:
    print(f'  chat_type={c}')

if not hits:
    print('FAIL: no banner matched the expected sender + body')
    for b in chat[:5]:
        s, o = rd_str_a(b, 1)
        t, o = rd_str_a(b, o) if s is not None else (None, 0)
        print(f'  candidate: sender={s!r} body={t!r}')
    sys.exit(2)

if not any(c == 4 for s,t,c in hits):
    print(f'FAIL: banner chat_type was not 4; got {[c for _,_,c in hits]}')
    sys.exit(3)

print('RESULT: PASS (/restart emits the 0x2b type-4 banner from Race Control)')
"
