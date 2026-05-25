#!/bin/sh
# `say <message>` stdin-console regression.  PR #5 added a console arm
# that emits 0x2b chat under sender "SERVER" on chat_type=0 (player
# panel), distinct from the in-game /say which aliases /broadcast.
# This pins the wire shape of the new path.
#
# accd's console is gated on isatty(STDIN_FILENO).  A python pty
# wrapper allocates a pseudo-terminal for accd's stdin so we can feed
# the `say` line from this script.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot
cd "$HERE"

pkill -KILL -f 'accd -c '       >/dev/null 2>&1 || true
pkill -KILL -f 'tools/bot/bot ' >/dev/null 2>&1 || true
sleep 1
rm -f cfg/current/*.json log/*.log accd.pcap accd.log bot1.log 2>/dev/null || true

NEEDLE="hello-from-say-test"
PCAP_TMP=/tmp/penalty_diff_accd.pcap

if ! sudo -n true 2>/dev/null; then
    echo "FAIL: sudo -n not allowed" >&2; exit 90
fi
sudo -n rm -f "$PCAP_TMP" accd_say.pcap accd_say.legacy.pcap
sudo -n dumpcap -i lo -w "$PCAP_TMP" -f 'tcp port 9302' -q >/dev/null 2>&1 &
TPID=$!
sleep 1

echo "==> accd under pty, bot, then 'say'"
python3 - "$ACCD" "$NEEDLE" >accd.log 2>&1 <<'PY' &
import os, pty, select, signal, sys, time

accd, needle = sys.argv[1], sys.argv[2]
m, s = pty.openpty()
pid = os.fork()
if pid == 0:
    os.setsid()
    os.dup2(s, 0); os.dup2(s, 1); os.dup2(s, 2)
    os.close(m); os.close(s)
    os.execv(accd, [accd, "-c", "cfg"])

os.close(s)
# Wait long enough for bot to handshake before sending 'say'.
deadline_say = time.monotonic() + 6.0
deadline_kill = time.monotonic() + 12.0
sent = False
while time.monotonic() < deadline_kill:
    r, _, _ = select.select([m], [], [], 0.2)
    if r:
        try:
            chunk = os.read(m, 4096)
            if chunk:
                sys.stdout.buffer.write(chunk); sys.stdout.buffer.flush()
        except OSError:
            break
    if not sent and time.monotonic() >= deadline_say:
        os.write(m, f"say {needle}\n".encode())
        sent = True
os.kill(pid, signal.SIGTERM)
os.waitpid(pid, 0)
PY
PYPID=$!

# Wait for accd to bind before launching the bot, otherwise the bot
# hits "Connection refused" and no peer is connected when `say` fires.
for i in 1 2 3 4 5 6 7 8 9 10; do
    if ss -tlnp 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.5
done

# Bot just needs to be connected when 'say' fires.
"$BOT" --host 127.0.0.1 --tcp 9302 --race 911 --grid 1 --name BotSay \
    >bot1.log 2>&1 &
BPID=$!

wait $PYPID 2>/dev/null || true
kill -TERM $BPID 2>/dev/null || true
wait $BPID 2>/dev/null || true

sudo -n pkill -INT -f "dumpcap -i lo -w $PCAP_TMP" 2>/dev/null || true
wait $TPID 2>/dev/null || true
sleep 1
sudo -n cp "$PCAP_TMP" accd_say.pcap
sudo -n chown "$(id -u):$(id -g)" accd_say.pcap

echo "==> diff"
rm -f accd_say.legacy.pcap
editcap -F pcap accd_say.pcap accd_say.legacy.pcap

python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_say.legacy.pcap', 9302)
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

need_body = '$NEEDLE'
hits = []
for b in chat:
    s, off = rd_str_a(b, 1)
    body, off = rd_str_a(b, off) if s is not None else (None, 0)
    if body is None or off + 5 > len(b):
        continue
    ctype = b[off + 4]
    if s == 'SERVER' and body == need_body:
        hits.append((s, body, ctype))

print(f'SERVER frames with body={need_body!r}: {len(hits)}')
if not hits:
    print('FAIL: no 0x2b broadcast carried sender=SERVER + the operator text')
    for b in chat[:8]:
        s, o = rd_str_a(b, 1)
        t, o2 = rd_str_a(b, o) if s is not None else (None, 0)
        print(f'  candidate: sender={s!r} body={t!r}')
    sys.exit(1)

if not any(c == 0 for _,_,c in hits):
    print(f'FAIL: chat_type was not 0; got {[c for _,_,c in hits]}')
    sys.exit(2)

print('RESULT: PASS (console say emits sender=SERVER on chat_type=0)')
"
