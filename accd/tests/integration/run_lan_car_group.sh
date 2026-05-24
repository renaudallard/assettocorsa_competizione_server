#!/bin/sh
# carGroup wire byte regression.
#
# GitHub issue #1 (thomasbourimech, 2026-05-24) noted that the
# 5 three-character carGroup labels in lobby.c + lan.c had been
# decoded from the kunos exe with the labels rotated, so an
# operator setting "carGroup": "GT3" got listed as GT4 in the
# ACC browser, breaking joins for clients without the GT4 Pack.
#
# This test pins the mapping the ACC client actually uses, against
# the LAN-discovery reply's last byte (which carries the same
# carGroup enum as the lobby registration body).  For every label,
# we tweak cfg/settings.json, restart accd, probe LAN, and check
# the trailing byte.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
TMPDIR=$(mktemp -d -p . cargroup.XXXXXX)

cleanup() {
    rc=$?
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    if [ -f cfg/settings.json.bak ]; then
        mv cfg/settings.json.bak cfg/settings.json
    fi
    rm -rf "$TMPDIR"
    exit $rc
}
trap cleanup EXIT INT TERM

pkill -KILL -f 'accd -c ' >/dev/null 2>&1 || true
sleep 1
cp cfg/settings.json cfg/settings.json.bak

run_probe() {
    label="$1"
    expected_byte="$2"
    # Patch cfg, restart accd, probe.
    python3 -c "
import json
o = json.load(open('cfg/settings.json'))
o['carGroup'] = '$label'
json.dump(o, open('cfg/settings.json', 'w'), indent=4)
"
    $ACCD -c cfg >"$TMPDIR/accd_$label.log" 2>&1 &
    ACCD_PID=$!
    for i in 1 2 3 4 5; do
        if ss -uln 2>/dev/null | grep -q ':8999'; then break; fi
        sleep 0.3
    done
    REPLY=$(python3 lan_probe.py --host 127.0.0.1 --port 8999 --raw)
    kill -TERM "$ACCD_PID" 2>/dev/null || true
    wait "$ACCD_PID" 2>/dev/null || true
    ACCD_PID=""

    # Last byte of the reply hex = carGroup.
    GOT_BYTE=$(printf '%s' "$REPLY" | tail -c 2)
    EXPECTED_HEX=$(printf '%02x' "$expected_byte")
    if [ "$GOT_BYTE" != "$EXPECTED_HEX" ]; then
        echo "FAIL: carGroup=$label expected 0x$EXPECTED_HEX got 0x$GOT_BYTE"
        return 1
    fi
    echo "  PASS: $label -> 0x$GOT_BYTE"
}

run_probe "FreeForAll" $((0xfa))
run_probe "GT3"        $((0x00))
run_probe "GT4"        $((0x07))
run_probe "GT2"        $((0x0b))
run_probe "TCX"        $((0x0c))
run_probe "GTC"        $((0xf9))

echo "RESULT: PASS (every carGroup label maps to the byte the ACC browser expects)"
