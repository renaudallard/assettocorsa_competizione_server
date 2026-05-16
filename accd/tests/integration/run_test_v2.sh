#!/bin/sh
# Run accd locally and N bots with arbitrary args.
# Usage: ./run_test_v2.sh "bot1_args" ["bot2_args" ...]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot

cd "$HERE"
mkdir -p log

# Kill any stale accd / bot left over from a previous crashed or
# interrupted dev session before we touch ports.  Without this a
# zombie bot still holding a car slot (typically slot 0 under a
# different display name) makes the fresh bot here arrive as
# "Recognized reconnect (zombie slot 0)", which loops handshake +
# disconnect and breaks any wire-byte comparison downstream.
pkill -KILL -f 'accd -c '       >/dev/null 2>&1 || true
pkill -KILL -f 'tools/bot/bot ' >/dev/null 2>&1 || true
sleep 1

rm -f cfg/current/*.json log/*.log accd.pcap accd.log bot*.log 2>/dev/null || true

PCAP_TMP=/tmp/penalty_diff_accd.pcap
rm -f "$PCAP_TMP"
# Verify sudo + dumpcap up front so a NOPASSWD misconfiguration
# fails the test with a clear message instead of having `set -e`
# trip on the later `sudo -n cp` and produce a misleading symptom.
if ! sudo -n true 2>/dev/null; then
    echo "FAIL: sudo -n not allowed — run_test_v2 needs passwordless sudo for dumpcap" >&2
    exit 90
fi
if ! command -v dumpcap >/dev/null 2>&1; then
    echo "FAIL: dumpcap not on PATH" >&2
    exit 91
fi
sudo -n dumpcap -i lo -w "$PCAP_TMP" -f 'tcp port 9302 or udp port 9303' \
    -q >/dev/null 2>&1 &
TCPDUMP_PID=$!
sleep 1
if ! kill -0 "$TCPDUMP_PID" 2>/dev/null; then
    echo "FAIL: dumpcap exited within 1s — capture filter or permission issue" >&2
    exit 92
fi

"$ACCD" -c cfg >accd.log 2>&1 &
ACCD_PID=$!

for i in 1 2 3 4 5 6 7 8 9 10; do
    if ss -tlnp 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.5
done

BOT_PIDS=""
i=0
for botargs in "$@"; do
    i=$((i + 1))
    "$BOT" --host 127.0.0.1 --tcp 9302 $botargs \
        >bot$i.log 2>&1 &
    BOT_PIDS="$BOT_PIDS $!"
    sleep 0.3
done

sleep "${TEST_DURATION:-30}"
# Disconnect bots one at a time with a small gap so 0x24 has a chance
# to fan out before subsequent peers also drop.
for pid in $BOT_PIDS; do
    kill -TERM $pid 2>/dev/null || true
    sleep 0.5
done
for pid in $BOT_PIDS; do
    wait $pid 2>/dev/null || true
done

kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true
sudo -n pkill -INT -f "dumpcap -i lo -w $PCAP_TMP" 2>/dev/null || true
wait "$TCPDUMP_PID" 2>/dev/null || true
sleep 1
sudo -n cp "$PCAP_TMP" accd.pcap
sudo -n chown "$(id -u):$(id -g)" accd.pcap

ls -la accd.pcap accd.log bot*.log
