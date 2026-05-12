#!/bin/sh
# Lobby blob structural regression (accd-only).
# accd registers with the public kunos lobby (UDP 131.153.158.178:909)
# when registerToLobby=1.  Capture the outbound 0x3a-framed kson
# messages on the egress interface and validate the preamble + type
# bytes against the documented layout.  We dont assert against kunos
# because the lobby is the same host accd talks to; a parity test
# would require a kson decoder + a captured real kunos egress.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
PCAP=/tmp/accd_lobby.pcap

# Pick the egress interface for the lobby host so we capture the
# UDP packets accd sends out.
IFACE=$(ip -o route get 131.153.158.178 2>/dev/null | sed -n 's/.* dev \([^ ]*\) .*/\1/p')
if [ -z "$IFACE" ]; then
    echo "FAIL: no route to 131.153.158.178"
    exit 1
fi
echo "==> capturing UDP egress on $IFACE"
sudo -n rm -f "$PCAP"
sudo -n dumpcap -i "$IFACE" -w "$PCAP" \
    -f 'host 131.153.158.178 and tcp port 909' -q >/dev/null 2>&1 &
DPID=$!
sleep 1

echo "==> spin up accd with registerToLobby=1"
rm -f cfg/settings.json.bak 2>/dev/null
cp cfg/settings.json cfg/settings.json.bak
cp cfg_lobby/local/settings.json cfg/settings.json
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
sleep 6

kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
mv cfg/settings.json.bak cfg/settings.json
sudo -n pkill -INT -f "dumpcap.*$PCAP" 2>/dev/null || true
wait "$DPID" 2>/dev/null || true
sleep 1

sudo -n chown "$(id -u):$(id -g)" "$PCAP" 2>/dev/null || true

python3 lobby_decode.py "$PCAP"
