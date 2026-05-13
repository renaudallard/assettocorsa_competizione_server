#!/bin/sh
# 0x46 dirt-relay must NOT broadcast regression.
#
# Per handlers.c:1295-1301, kunos accServer.exe never relays 0x46
# (verified via pcap).  Instead it stores the latest dirt values
# from inbound 0x45 ACP_CAR_DIRT_UPDATE per-car so late joiners
# see accumulated weathering in the welcome spawnDef tail.  accd
# mirrors this: h_car_dirt only memcpys into car_dirt[], no
# broadcast.  This test runs 2 bots, captures all accd TCP egress,
# and asserts that zero 0x46 frames appear despite both bots
# emitting 0x45 updates.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tmp/bot/bot
PCAP=/tmp/accd_no_46.pcap

ACCD_PID=""
BOT_PIDS=""
DPID=""
cleanup_on_exit() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
    [ -n "$DPID" ] && sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT

echo "==> capturing TCP egress on lo:9302"
sudo -n rm -f "$PCAP"
sudo -n dumpcap -i lo -w "$PCAP" -f 'tcp port 9302' -q >/dev/null 2>&1 &
DPID=$!
sleep 1

rm -f accd.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "Bot1" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"
sleep 0.5
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 922 --grid 2 --name "Bot2" >bot2.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

# Drive long enough that each bot has emitted at least one 0x45
# (bot tx 0x45 a few seconds into running).
sleep 18

for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
sleep 1
sudo -n chown "$(id -u):$(id -g)" "$PCAP" 2>/dev/null || true

python3 - <<'PY'
import sys, struct
from scapy.all import rdpcap, TCP, Raw

# Tally inbound 0x45 (client -> accd) and outbound 0x46 (accd -> client)
ingress_45 = 0
egress_46 = 0
egress_per_conn = {}

for p in rdpcap("/tmp/accd_no_46.pcap"):
    if TCP not in p or Raw not in p: continue
    raw = bytes(p[Raw].load)
    # Direction by port: sport=9302 -> egress (accd -> client)
    #                    dport=9302 -> ingress (client -> accd)
    if p[TCP].dport == 9302:
        # Walk u16-LE-len frames in this raw blob.  Best-effort:
        # bots send small packets so most segments are whole-frame.
        off = 0
        while off + 2 <= len(raw):
            n = struct.unpack("<H", raw[off:off+2])[0]
            if off + 2 + n > len(raw): break
            body = raw[off+2:off+2+n]
            if body and body[0] == 0x45:
                ingress_45 += 1
            off += 2 + n
    elif p[TCP].sport == 9302:
        # Aggregate per dport in order
        egress_per_conn.setdefault(p[TCP].dport, []).append(
            (p[TCP].seq, raw))

# Walk reassembled egress streams for 0x46
for dport, segs in egress_per_conn.items():
    segs.sort()
    stream = b"".join(s for _, s in segs)
    off = 0
    while off + 2 <= len(stream):
        n = struct.unpack("<H", stream[off:off+2])[0]
        if off + 2 + n > len(stream): break
        body = stream[off+2:off+2+n]
        if body and body[0] == 0x46:
            egress_46 += 1
        off += 2 + n

print(f"  inbound  0x45 ACP_CAR_DIRT_UPDATE: {ingress_45}")
print(f"  outbound 0x46 SRV_CAR_DIRT_RELAY:  {egress_46}")
if ingress_45 == 0:
    print("FAIL: no 0x45 from bots; test isn't exercising the gate")
    sys.exit(1)
if egress_46 != 0:
    print(f"FAIL: accd emitted {egress_46} 0x46 frame(s); kunos parity"
          " requires 0 (per pcap)")
    sys.exit(2)
print("RESULT: PASS (accd absorbed 0x45 + emitted 0 x 0x46 -- matches kunos)")
PY
