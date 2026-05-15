#!/bin/sh
# /broadcast (alias: /say, /announce) regression.  Bot elevates with
# /admin <password> then sends "/broadcast <text>".  accd's chat.c:781
# parses the text after the command, logs "admin: /broadcast <text>",
# and emits a 0x2b chat_type=4 banner with RC_SENDER as the sender
# and the supplied <text> as the body.
#
# This pins the operator-broadcast wire shape.  Two assertions:
# (1) the body is a verbatim copy of the operator's text, and
# (2) the chat_type byte is 4 (info banner lane).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Pick a needle string that's unlikely to appear in any other banner.
NEEDLE="hello-from-accd-test"
BOT="--race 911 --grid 1 --name BotAdmin --chat-start-tick 60 \
    --chat /admin_admin --chat /broadcast_$NEEDLE"
TEST_DURATION=15

echo "==> accd + bot, /admin then /broadcast"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_broadcast_command.pcap

echo "==> diff"
rm -f accd_broadcast_command.legacy.pcap
editcap -F pcap accd_broadcast_command.pcap accd_broadcast_command.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_broadcast_command.legacy.pcap', 9302)
chat = [b for o,l,b in walk_acc_frames(ab) if b and b[0]==0x2b]
print(f'accd 0x2b chat frames: {len(chat)}')

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

# Bot sends '_' for spaces; accd should receive 'hello-from-accd-test'
# verbatim (no underscore conversion because the bot's underscore
# substitution converts ONLY the chat-arg text, the rest of the
# command is sent as-is).  Our needle has no underscores so the body
# will be literally NEEDLE.
need_body = '$NEEDLE'
hits = []
for b in chat:
    s, off = rd_str_a(b, 1)
    body, off = rd_str_a(b, off) if s is not None else (None, 0)
    if body is None or off + 5 > len(b):
        continue
    ctype = b[off + 4]
    if s == 'Race Control' and body == need_body:
        hits.append((s, body, ctype))

print(f'banners with body={need_body!r}: {len(hits)}')
if not hits:
    print('FAIL: no 0x2b broadcast carried the operator text')
    for b in chat[:8]:
        s, o = rd_str_a(b, 1)
        t, o2 = rd_str_a(b, o) if s is not None else (None, 0)
        print(f'  candidate: sender={s!r} body={t!r}')
    sys.exit(1)

if not any(c == 4 for _,_,c in hits):
    print(f'FAIL: chat_type was not 4; got {[c for _,_,c in hits]}')
    sys.exit(2)

print('RESULT: PASS (/broadcast emits the operator text as type-4 RC banner)')
"
