#!/bin/sh
set -eu
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
cc=${CC:-cc}
libs=
if printf '%s\n' '#include <bsd/stdlib.h>' 'int main(void){return (int)arc4random_uniform(1);}' | "$cc" -x c - -o "$tmpdir/probe" -lbsd 2>/dev/null; then libs="$libs -lbsd"; fi
if printf '%s\n' '#include <seccomp.h>' 'int main(void){scmp_filter_ctx c=seccomp_init(0);seccomp_release(c);return 0;}' | "$cc" -x c - -o "$tmpdir/probe" -lseccomp 2>/dev/null; then libs="$libs -lseccomp"; fi
if ! printf '%s\n' '#include <iconv.h>' 'int main(void){iconv_t c=iconv_open("","");iconv_close(c);return 0;}' | "$cc" -x c - -o "$tmpdir/probe" 2>/dev/null; then libs="$libs -liconv"; fi
# The objects are built by the parent make target; intentionally omit main.o.
# shellcheck disable=SC2046,SC2086
"$cc" -std=c99 -I. tests/test_disconnect_notify.c $(find . -maxdepth 1 -name '*.o' ! -name main.o ) -lm $libs -o "$tmpdir/test_disconnect_notify"
"$tmpdir/test_disconnect_notify"
