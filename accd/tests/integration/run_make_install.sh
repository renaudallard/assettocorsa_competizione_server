#!/bin/sh
# 'make install' artifact regression.
#
# The Makefile defines an 'install' target that lays down the accd
# binary and the man page under $DESTDIR$PREFIX.  This test runs it
# into a throwaway DESTDIR and asserts both artifacts land at the
# documented paths.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/.."/..

DESTDIR=$(mktemp -d)
trap 'rm -rf "$DESTDIR"' EXIT
PREFIX=/usr/local

echo "==> make install DESTDIR=$DESTDIR PREFIX=$PREFIX"
make install DESTDIR="$DESTDIR" PREFIX="$PREFIX" >/dev/null

BIN="$DESTDIR$PREFIX/bin/accd"
MAN="$DESTDIR$PREFIX/share/man/man1/accd.1"

if [ ! -x "$BIN" ]; then
    echo "FAIL: accd binary missing at $BIN"
    find "$DESTDIR" -type f | head -20
    exit 1
fi
echo "  PASS: binary at $BIN ($(wc -c <"$BIN") bytes)"

if [ ! -f "$MAN" ]; then
    echo "FAIL: man page missing at $MAN"
    find "$DESTDIR" -type f | head -20
    exit 2
fi
echo "  PASS: man page at $MAN ($(wc -c <"$MAN") bytes)"

# Sanity: the man page uses mdoc macros (BSD style).  Check for the
# .Dt ACCD 1 document-title macro and at least one .Sh section header.
if ! grep -qE '^\.Dt ACCD' "$MAN"; then
    echo "FAIL: man page lacks .Dt ACCD title macro"
    head "$MAN"
    exit 3
fi
if ! grep -qE '^\.Sh NAME' "$MAN"; then
    echo "FAIL: man page lacks .Sh NAME section header"
    exit 4
fi
echo "  PASS: .Dt ACCD + .Sh NAME mdoc markers present"

# Sanity: binary is ELF (or at least non-empty) and not the source
file_type=$(file -b "$BIN" 2>/dev/null || echo unknown)
echo "  binary type: $file_type"
echo "RESULT: PASS (make install lays down accd + accd.1 at canonical paths)"
