#!/bin/sh
# Welcome trailer byte-by-byte regression.
#
# tests/fake_client.py walks the 0x0b welcome trailer the same way
# AC2-Win64-Shipping.exe does (mirroring the 11 client-side log
# anchors).  This wrapper runs it as a regression test so the suite
# catches silent byte-count drift in any of the 11 sections
# (CircuitEntity / GraphicsRules / CarSet / RaceEntity / EventRules /
# WeatherStatus / WeatherData / TrackConditions / DeltaThreshold /
# TrackRecords / cppResults) without needing a live Windows client.
#
# Failure modes the underlying walker catches:
#   - 'Consumed M of N B (leftover K)' where K > 0
#   - 'expected frames missing' beyond the documented 0x4e warning
#   - any unhandled exception during section traversal
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/.."/..

echo "==> running fake_client.py against a transient accd"
out=$(python3 tests/fake_client.py 2>&1)
echo "$out" | tail -25

if echo "$out" | grep -q "PASS: welcome parses cleanly to EOF"; then
    if echo "$out" | grep -qE "^FAIL|leftover [1-9]"; then
        echo "RESULT: FAIL (PASS line present but unexpected FAIL/leftover above)"
        exit 1
    fi
    echo "RESULT: PASS (welcome trailer + follow-up burst walk cleanly)"
    exit 0
fi
echo "RESULT: FAIL (fake_client.py did not emit PASS)"
exit 2
