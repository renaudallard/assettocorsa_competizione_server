#!/bin/sh
# /admin + /track regression.  Bot elevates with /admin <password>
# then issues "/track misano".  accd's chat_do_track sets s->track,
# session-resets the weekend, then calls chat_weekend_reset_broadcast
# which emits:
#
#  - 0x40 SRV_RACE_WEEKEND_RESET (weather body) once
#  - 0x4b SRV_WELCOME_REDELIVERY (full welcome trailer) to every peer
#  - 0x2b chat banner "Event changed to misano" with chat_type=4
#
# We test only the structural cascade because byte-exact welcome
# bytes drift between runs (weather PRNG, conn timestamps).  The
# point is to pin that /track does trigger the redelivery + banner.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

BOT="--race 911 --grid 1 --name BotAdmin --chat-start-tick 90 \
    --chat /admin_admin --chat /track_misano"
TEST_DURATION=20

echo "==> accd + bot, /admin then /track misano"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_track_command.pcap

echo "==> diff"
rm -f accd_track_command.legacy.pcap
editcap -F pcap accd_track_command.pcap accd_track_command.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_track_command.legacy.pcap', 9302)
frames = list(walk_acc_frames(ab))

m40 = [b for o,l,b in frames if b and b[0]==0x40]
m4b = [b for o,l,b in frames if b and b[0]==0x4b]
m2b = [b for o,l,b in frames if b and b[0]==0x2b]
print(f'0x40 race-weekend-reset: {len(m40)}')
print(f'0x4b welcome-redelivery: {len(m4b)}')
print(f'0x2b chat banners:       {len(m2b)}')

if not m40:
    print('FAIL: no 0x40 SRV_RACE_WEEKEND_RESET after /track')
    sys.exit(1)
if not m4b:
    print('FAIL: no 0x4b SRV_WELCOME_REDELIVERY after /track')
    sys.exit(2)

# Welcome redelivery payload must be the full trailer, not a stub —
# a real welcome runs into the multi-KB range; a stub would be < 64 B.
big = [b for b in m4b if len(b) >= 1024]
print(f'0x4b frames >= 1024 B: {len(big)} (full trailer)')
if not big:
    print(f'FAIL: 0x4b frames all under 1024 B (sizes: {[len(b) for b in m4b]})')
    sys.exit(3)

# Banner check: locate the 'Event changed to misano' banner.
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

want = 'Event changed to misano'
hit = None
for b in m2b:
    s, off = rd_str_a(b, 1)
    body, off = rd_str_a(b, off) if s is not None else (None, 0)
    if body is None or off + 5 > len(b):
        continue
    if s == 'Race Control' and body == want and b[off + 4] == 4:
        hit = body
        break

if hit is None:
    print(f'FAIL: no banner matched body={want!r}')
    for b in m2b[:5]:
        s, o = rd_str_a(b, 1)
        t, o2 = rd_str_a(b, o) if s is not None else (None, 0)
        print(f'  candidate: sender={s!r} body={t!r}')
    sys.exit(4)

print('RESULT: PASS (/track emits 0x40 weather reset + 0x4b welcome '
      'redelivery + banner)')
"
