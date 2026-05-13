#!/bin/sh
# Lobby 0xf5 selector-driven unicast vs broadcast regression.
#
# 0xf5 carries u8 + kson_string s1 + kson_string s2 + i32 + u8.  Per
# kunos FUN_140025470:
#   - s1 non-empty  -> unicast 0x2b to the matching conn (sender
#                      'Server', chat_type 3, body = s2)
#   - s1 empty      -> broadcast 0x2b to all conns
# The test fires both flavours sequentially and verifies the chat
# fan-out via pcap.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot
FAKE_PORT=11911
PCAP=/tmp/accd_f5_selector.pcap

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

BOT1_SID="S76561199000000911"

echo "==> spin up fake kson lobby on 127.0.0.1:$FAKE_PORT"
python3 - <<PY &
import sys, time, struct
sys.path.insert(0, ".")
from fake_kson import FakeKsonLobby, write_kson_str
lobby = FakeKsonLobby(port=$FAKE_PORT)
lobby.start()
lobby.wait_for_type(0xc8, timeout=8.0)
lobby.send(b"\\xef\\x00")
time.sleep(8)  # wait for both bots up + 0xd1 drivers update
# 0xf5 unicast to bot1
body = bytearray([0xf5])
write_kson_str(body, "$BOT1_SID")
write_kson_str(body, "private message to bot1")
body.extend(struct.pack("<i", 0))
body.append(0)
lobby.send(bytes(body))
print("  sent 0xf5 unicast to bot1", flush=True)
time.sleep(2)
# 0xf5 broadcast (s1 empty)
body = bytearray([0xf5])
write_kson_str(body, "")
write_kson_str(body, "broadcast to all")
body.extend(struct.pack("<i", 0))
body.append(0)
lobby.send(bytes(body))
print("  sent 0xf5 broadcast", flush=True)
time.sleep(3)
lobby.shutdown()
PY
FAKE_PID=$!
sleep 1

echo "==> spin up accd"
rm -f accd.log
ACCD_LOBBY_HOST=127.0.0.1 ACCD_LOBBY_PORT=$FAKE_PORT \
    $ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
sleep 2

echo "==> spawn 2 bots"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "BotOne" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"
sleep 0.5
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 922 --grid 2 --name "BotTwo" >bot2.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"
sleep 17

echo "==> shutdown"
for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
kill -TERM "$FAKE_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
sleep 1
sudo -n chown "$(id -u):$(id -g)" "$PCAP" 2>/dev/null || true

python3 - <<'PY'
import sys, struct
from scapy.all import rdpcap, TCP, Raw

streams = {}
for p in rdpcap("/tmp/accd_f5_selector.pcap"):
    if TCP not in p or Raw not in p: continue
    if p[TCP].sport != 9302: continue
    streams.setdefault(p[TCP].dport, []).append((p[TCP].seq, bytes(p[Raw].load)))

def read_str_a(b, p):
    n = b[p]; p += 1
    cps = []
    for _ in range(n):
        cps.append(struct.unpack("<I", b[p:p + 4])[0])
        p += 4
    return "".join(chr(c) for c in cps), p

# Per-conn chat tally
hits = {}  # dport -> list of (sender, text, ctype)
for dport, segs in streams.items():
    segs.sort()
    stream = b"".join(b for _, b in segs)
    off = 0
    while off + 2 <= len(stream):
        n = struct.unpack("<H", stream[off:off + 2])[0]
        if off + 2 + n > len(stream): break
        body = stream[off + 2:off + 2 + n]
        off += 2 + n
        if not body or body[0] != 0x2b: continue
        try:
            p = 1
            sender, p = read_str_a(body, p)
            text, p = read_str_a(body, p)
            p += 4
            ctype = body[p]
            hits.setdefault(dport, []).append((sender, text, ctype))
        except Exception:
            pass

# Filter to messages from this test
def is_unicast_msg(h): return h[1] == "private message to bot1"
def is_broadcast_msg(h): return h[1] == "broadcast to all"

uni_conns = [d for d, hs in hits.items() if any(is_unicast_msg(h) for h in hs)]
bcast_conns = [d for d, hs in hits.items() if any(is_broadcast_msg(h) for h in hs)]
print(f"  conns observed: {sorted(hits.keys())}")
print(f"  unicast 'private message to bot1' delivered to: {uni_conns}")
print(f"  broadcast 'broadcast to all' delivered to: {sorted(bcast_conns)}")

rc = 0
if len(uni_conns) != 1:
    print(f"FAIL: unicast went to {len(uni_conns)} conns, expected 1")
    rc = 1
else:
    print("  PASS: unicast scope (1 conn)")
if len(bcast_conns) < 2:
    print(f"FAIL: broadcast went to {len(bcast_conns)} conns, expected >=2")
    rc = 1
else:
    print(f"  PASS: broadcast scope ({len(bcast_conns)} conns)")

# Validate sender + ctype on the unicast hit
for dport, hs in hits.items():
    for h in hs:
        if is_unicast_msg(h):
            if h[0] != "Server":
                print(f"FAIL: unicast sender {h[0]!r}, expected 'Server'")
                rc = 1
            if h[2] != 3:
                print(f"FAIL: unicast ctype {h[2]}, expected 3")
                rc = 1
        if is_broadcast_msg(h):
            if h[0] != "Server":
                print(f"FAIL: broadcast sender {h[0]!r}, expected 'Server'")
                rc = 1
            if h[2] != 3:
                print(f"FAIL: broadcast ctype {h[2]}, expected 3")
                rc = 1
print("PASS: sender + ctype shape correct" if rc == 0 else "FAIL")
sys.exit(rc)
PY
echo "RESULT: PASS"
