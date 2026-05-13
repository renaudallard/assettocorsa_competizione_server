#!/bin/sh
# Lobby 0xf4 remote-DQ chat-targeting regression.
#
# Drives the kson -> accd 0xf4 remote-DQ path with two bots connected
# and verifies:
#   - the chat 0x2b is sent to ONLY the targeted driver's connection,
#     not broadcast;
#   - sender is the literal "Server" (NOT "Race Control");
#   - chat-type is 3 (NOT 5);
#   - body is the s2 reason verbatim (no "[kson]" prefix).
# Mirrors kunos FUN_1400251b0.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot
FAKE_PORT=11910
PCAP=/tmp/accd_f4_target.pcap

ACCD_PID=""
FAKE_PID=""
BOT_PIDS=""
DPID=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
    [ -n "$FAKE_PID" ] && kill -TERM "$FAKE_PID" 2>/dev/null || true
    [ -n "$DPID" ] && sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
    for f in cfg/settings.json.bak; do
        [ -f "$f" ] || continue
        mv "$f" "${f%.bak}" || true
    done
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT
for f in cfg/*.bak; do
    [ -f "$f" ] || continue
    mv "$f" "${f%.bak}"
done

cp cfg/settings.json cfg/settings.json.bak
cp cfg_lobby/local/settings.json cfg/settings.json

echo "==> capturing TCP egress from accd (port 9302)"
sudo -n rm -f "$PCAP"
sudo -n dumpcap -i lo -w "$PCAP" -f 'tcp port 9302' -q >/dev/null 2>&1 &
DPID=$!
sleep 1

# Bot derives steam_id as "S7656119900<race>" (bot.c line 1274), so we
# need to match the race-number form the bot will send.
BOT1_SID="S76561199000000911"
BOT2_SID="S76561199000000922"

echo "==> spin up fake kson lobby on 127.0.0.1:$FAKE_PORT"
python3 - <<PY &
import sys, time, struct
sys.path.insert(0, ".")
from fake_kson import FakeKsonLobby, write_kson_str
lobby = FakeKsonLobby(port=$FAKE_PORT)
lobby.start()
got = lobby.wait_for_type(0xc8, timeout=8.0)
print(f"  got 0xc8 register: {got is not None}", flush=True)
lobby.send(b"\\xef\\x00")
# Wait long enough for both bots to handshake + accd to push 0xd1
time.sleep(8)
# Send 0xf4: cmd + kson_str(steam_id) + kson_str(reason) + i32 + u8
body = bytearray()
body.append(0xf4)
write_kson_str(body, "$BOT1_SID")
write_kson_str(body, "rammed bot2 turn 3")
body.extend(struct.pack("<i", 0))
body.append(0)
lobby.send(bytes(body))
print("  sent 0xf4 targeting bot1", flush=True)
time.sleep(5)
lobby.shutdown()
PY
FAKE_PID=$!
sleep 1

echo "==> spin up accd targeting fake lobby"
rm -f accd.log
ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=$FAKE_PORT \
    $ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
sleep 2

echo "==> spawn 2 bots with distinct steam_ids"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "BotOne" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"
sleep 0.5
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 922 --grid 2 --name "BotTwo" >bot2.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"
sleep 15

echo "==> shutdown"
for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
kill -TERM "$FAKE_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
sleep 1
sudo -n chown "$(id -u):$(id -g)" "$PCAP" 2>/dev/null || true

echo "==> log + pcap verification"
if ! grep -q "0xf4 remote DQ for car" accd.log; then
    echo "FAIL: accd did not log 0xf4 remote DQ"
    tail -30 accd.log
    exit 1
fi
echo "  PASS: log shows 0xf4 remote DQ"

python3 - <<'PY'
import sys, struct
from scapy.all import rdpcap, TCP, Raw

pcap = "/tmp/accd_f4_target.pcap"
# Collect accd->bot streams (per-conn keyed on bot ephemeral port)
streams = {}  # dport -> {payloads}
for p in rdpcap(pcap):
    if TCP not in p or Raw not in p:
        continue
    if p[TCP].sport != 9302:
        continue
    dport = p[TCP].dport
    streams.setdefault(dport, []).append((p[TCP].seq, bytes(p[Raw].load)))

# Walk each conn for SRV_CHAT_OR_STATE (0x2b) frames
hits = []  # (dport, sender, text, ctype)
for dport, segs in streams.items():
    segs.sort()
    stream = b"".join(b for _, b in segs)
    off = 0
    while off + 2 <= len(stream):
        n = struct.unpack("<H", stream[off:off + 2])[0]
        if off + 2 + n > len(stream):
            break
        body = stream[off + 2:off + 2 + n]
        off += 2 + n
        if not body or body[0] != 0x2b:
            continue
        # 0x2b ACP_CHAT_OR_STATE uses wr_str_a encoding:
        #   u8 0x2b
        #   u8 cp_count + N x u32 codepoint  (= sender)
        #   u8 cp_count + N x u32 codepoint  (= text)
        #   i32 ts + u8 ctype
        try:
            p = 1
            def read_str_a(b, p):
                n = b[p]; p += 1
                cps = []
                for _ in range(n):
                    cps.append(struct.unpack("<I", b[p:p + 4])[0])
                    p += 4
                return "".join(chr(c) for c in cps), p
            sender, p = read_str_a(body, p)
            text, p = read_str_a(body, p)
            ts = struct.unpack("<i", body[p:p + 4])[0]; p += 4
            ctype = body[p]
            del ts
            hits.append((dport, sender, text, ctype))
        except Exception:
            pass

# Filter out unrelated chat (e.g. server-sent welcome banners).  We
# care about the f4-triggered one whose body matches the reason.
target_hits = [h for h in hits if h[2] == "rammed bot2 turn 3"]

# Print all hits for debugging
print(f"  total 0x2b chats observed: {len(hits)}")
for h in hits[-10:]:
    print(f"    dport={h[0]} sender={h[1]!r} text={h[2][:40]!r} ctype={h[3]}")
print(f"  target-matched (text=='rammed bot2 turn 3'): {len(target_hits)}")

if len(target_hits) != 1:
    print(f"FAIL: expected exactly 1 target-matched chat, got {len(target_hits)}")
    sys.exit(1)
h = target_hits[0]
if h[1] != "Server":
    print(f"FAIL: sender is {h[1]!r}, expected 'Server'")
    sys.exit(1)
if h[3] != 3:
    print(f"FAIL: chat_type is {h[3]}, expected 3")
    sys.exit(1)
print("RESULT: PASS (0xf4 -> 0x2b targeted, sender 'Server', type 3)")
PY
