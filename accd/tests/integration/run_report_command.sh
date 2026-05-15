#!/bin/sh
# /report regression.  accd's chat.c:687 special-cases /report so any
# driver (admin or not) can issue it; the handler appends a line to
# <cfg_dir>/reports.txt and emits a "report (driver|admin)" log line.
# No chat broadcast goes out.
#
# Two assertions:
#   1. cfg/reports.txt has a fresh entry containing the report text.
#   2. accd.log has a "report (driver)" line carrying the same text.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

NEEDLE="reporting-conn-X"
BOT="--race 911 --grid 1 --name BotReporter --chat-start-tick 60 \
    --chat /report_$NEEDLE"
TEST_DURATION=10

# /report appends to <cfg_dir>/reports.txt.  Start with a clean file
# so we know what's freshly written this run.
rm -f cfg/reports.txt

echo "==> accd + bot, /report"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap accd.log
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
mv accd.pcap accd_report.pcap

echo "==> checks"
if ! [ -f cfg/reports.txt ]; then
    echo "FAIL: cfg/reports.txt was not created"
    exit 1
fi

echo "  cfg/reports.txt:"
head -5 cfg/reports.txt | sed 's/^/    /'

if ! grep -q "$NEEDLE" cfg/reports.txt; then
    echo "FAIL: cfg/reports.txt does not contain the report needle"
    exit 2
fi

if ! grep -qE "report \(driver\).*$NEEDLE" accd.log; then
    echo "FAIL: accd.log has no 'report (driver)' line with the needle"
    grep -E 'report' accd.log | head -5 || true
    exit 3
fi

echo "RESULT: PASS (/report appended to reports.txt and logged the line)"
