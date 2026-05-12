#!/bin/sh
# LAN discovery regression.
# Sends a 0xbf 0x48 <nonce> UDP probe to accd and validates the 0xc0
# reply structure (server_name str_a + clients + has_password +
# tcp_port + nonce echo + carGroup byte).
#
# Kunos cross-check is opportunistic: if the wine accServer is up and
# bound to 8999 on the RE host, we probe and compare structural
# fields.  Wine flakiness (server doesn't stay up after a backgrounded
# ssh) often masks the wine probe — we report it but don't fail.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd

echo "==> spin up accd locally and probe UDP 8999"
rm -f log/*.log accd.log 2>/dev/null || true
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
sleep 1
for i in 1 2 3 4 5 6 7 8 9 10; do
    if ss -uln 2>/dev/null | grep -q ':8999'; then break; fi
    sleep 0.3
done
ACCD_REPLY=$(python3 lan_probe.py --host 127.0.0.1 --port 8999 --raw)
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

echo "accd reply: $ACCD_REPLY"
python3 lan_probe.py --validate-hex "$ACCD_REPLY"

echo "==> opportunistic kunos probe (skipped if wine not up)"
KUNOS_REPLY=$(python3 lan_probe.py --host 172.20.0.66 --port 8999 --raw \
    --timeout 2 2>/dev/null || echo "")
if [ -n "$KUNOS_REPLY" ]; then
    echo "kunos reply: $KUNOS_REPLY"
    python3 lan_probe.py --compare-hex "$ACCD_REPLY" "$KUNOS_REPLY"
else
    echo "kunos: no reply (wine not running or fw block — accd-only PASS)"
fi
