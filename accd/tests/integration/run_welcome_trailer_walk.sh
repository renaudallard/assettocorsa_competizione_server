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

echo "==> running fake_client.py (driver) against a transient accd"
out=$(python3 tests/fake_client.py 2>&1)
echo "$out" | tail -25

if ! echo "$out" | grep -q "PASS: welcome parses cleanly to EOF"; then
    echo "RESULT: FAIL (driver fake_client.py did not emit PASS)"
    exit 2
fi
if echo "$out" | grep -qE "^FAIL|leftover [1-9]"; then
    echo "RESULT: FAIL (driver PASS present but unexpected FAIL/leftover)"
    exit 1
fi

echo "==> running fake_client.py --spectator (carless welcome)"
sout=$(python3 tests/fake_client.py --spectator 2>&1)
echo "$sout" | tail -25

if ! echo "$sout" | grep -q "PASS: spectator welcome"; then
    echo "RESULT: FAIL (spectator: car_index != 0xffffffff or welcome bad)"
    exit 3
fi
if echo "$sout" | grep -qE "^FAIL|leftover [1-9]"; then
    echo "RESULT: FAIL (spectator PASS present but unexpected FAIL/leftover)"
    exit 1
fi

echo "RESULT: PASS (driver + spectator welcome trailers walk cleanly)"
exit 0
