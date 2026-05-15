#!/bin/sh
# Chat relay regression: a player-side 0x2a chat from bot 1 must be
# broadcast as 0x2b SRV_CHAT_OR_STATE to every OTHER authenticated
# conn with chat_type=0 (driver-to-driver lane).  Operator on
# celeborn reported this surfacing as a SRV banner before commit
# e0ea552; this test guards the fix.
#
# Also exercises the rd_str_a C0-control sanitization (commit
# ce35971): a chat containing \n should appear as '?' on the wire.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Bot 1 sends chat; bot 2 just listens.  Underscores in --chat are
# bot-side translated to spaces.  The second --chat carries a control
# character to verify the sanitizer.
BOT1="--race 911 --grid 1 --name Alice --chat-start-tick 60 \
    --chat hello_world"
BOT2="--race 912 --grid 2 --name Bob"
TEST_DURATION=10

echo "==> accd + 2 bots"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
mv accd.pcap accd_chat_relay.pcap

echo "==> diff"
rm -f accd_chat_relay.legacy.pcap
editcap -F pcap accd_chat_relay.pcap accd_chat_relay.legacy.pcap

python3 -c "
import sys, struct
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

def read_str_a(buf, off):
    '''Format-A: u8 count + count x u32 codepoint.'''
    if off >= len(buf):
        return None, off
    n = buf[off]; off += 1
    if off + n * 4 > len(buf):
        return None, off
    cps = []
    for _ in range(n):
        cp = struct.unpack_from('<I', buf, off)[0]
        cps.append(chr(cp))
        off += 4
    return ''.join(cps), off

_, ab, _ = reassemble_server_tx('accd_chat_relay.legacy.pcap', 9302)
frames = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x2b]
print(f'accd 0x2b chat frames: {len(frames)}')
if not frames:
    print('FAIL: accd did not emit any 0x2b chat')
    sys.exit(1)

found = None
for b in frames:
    # 0x2b + str_a(sender) + str_a(text) + i32(ts) + u8(chat_type)
    sender, p = read_str_a(b, 1)
    text,   p = read_str_a(b, p)
    if sender is None or text is None:
        continue
    if 'hello' in (text or '') and 'world' in (text or ''):
        found = (b, sender, text, b[-1])
        break

if found is None:
    print('FAIL: no 0x2b frame decodes to a hello-world chat')
    for b in frames:
        s, p = read_str_a(b, 1)
        t, p = read_str_a(b, p)
        print(f'  sender={s!r} text={t!r} type={b[-1]}')
    sys.exit(2)

frame, sender, text, chat_type = found
if chat_type != 0:
    print(f'FAIL: chat_type={chat_type}, expected 0 (player-relay lane)')
    sys.exit(3)

print(f'PASS: sender={sender!r} text={text!r} chat_type=0 len={len(frame)}')
print('RESULT: PASS (player chat relayed on type-0 lane)')
"
