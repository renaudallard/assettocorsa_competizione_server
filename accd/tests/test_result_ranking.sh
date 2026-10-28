#!/bin/sh
set -eu
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -ffunction-sections -fdata-sections -I. \
    tests/test_result_ranking.c log.c -Wl,--gc-sections -lm -o "$tmpdir/test_result_ranking"
"$tmpdir/test_result_ranking"
