#!/bin/sh
# Verify BoP matching and repeated application without starting a server.
set -eu
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -ffunction-sections -fdata-sections -I. \
    tests/test_bop_apply.c entrylist.c log.c \
    -Wl,--gc-sections -lm -o "$tmpdir/test_bop_apply"
"$tmpdir/test_bop_apply"
