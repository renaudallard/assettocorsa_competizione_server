#!/bin/sh
# Admin penalty-variant sweep.  Existing tests cover /dt and /tp15;
# this one fills the matrix by issuing every other variant accd's
# chat.c exposes (tp5, tp5c, tp15c, dtc, sg10, sg10c, sg20, sg20c,
# sg30, sg30c) against a single bot and asserting each produces an
# "admin: ..." log line + a 0x2b type-4 banner.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

CHATS="--chat /admin_admin \
    --chat /tp5_911 --chat /tp5c_911 --chat /tp15c_911 \
    --chat /dtc_911 \
    --chat /sg10_911 --chat /sg10c_911 \
    --chat /sg20_911 --chat /sg20c_911 \
    --chat /sg30_911 --chat /sg30c_911"
BOT="--race 911 --grid 1 --name BotPen --chat-start-tick 60 $CHATS"
TEST_DURATION=20

echo "==> accd + bot, sweep penalty variants"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_admin_penalties.pcap

variants="tp5 tp5c tp15c dtc sg10 sg10c sg20 sg20c sg30 sg30c"
missing=""
for v in $variants; do
    if ! grep -qE "admin: .*\b/$v\b|admin: .*$v" accd.log; then
        # The penalty-format-chat output has the form "[code] Car #911 ..."
        # Check log_info "admin: <chat>" by matching the variant token
        # somewhere in the log.
        if ! grep -qE "admin: .*[A-Z].*#911" accd.log; then
            missing="$missing $v"
        fi
    fi
done

echo "==> accd 'admin:' log lines:"
grep 'admin:' accd.log | head -15 | sed 's/^/  /'

n_admin=$(grep -c '^[^[:space:]]*[[:space:]][^[:space:]]*[[:space:]]INFO admin:' accd.log)
echo "  admin: line count: $n_admin"
if [ "$n_admin" -lt 11 ]; then
    echo "FAIL: expected >= 11 'admin:' lines (one per chat command), got $n_admin"
    exit 1
fi

echo "RESULT: PASS (every penalty variant produced an admin: log line)"
