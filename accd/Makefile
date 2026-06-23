# accd -- ACC dedicated server reimplementation
#
# Portable Makefile.  Works with both BSD make and GNU make.
# iconv detection is automatic: /usr/local paths are always
# searched, and -liconv is linked when iconv is not in libc.

PROG =		accd

OBJS =		main.o \
		bans.o \
		bcast.o \
		chat.o \
		config.o \
		console.o \
		dispatch.o \
		entrylist.o \
		handlers.o \
		handshake.o \
		io.o \
		json.o \
		lan.o \
		lobby.o \
		log.o \
		monitor.o \
		net.o \
		pb.o \
		penalty.o \
		prim.o \
		ratings.o \
		results.o \
		sandbox.o \
		session.o \
		smpr.o \
		state.o \
		tick.o \
		weather.o

# Read the project VERSION file and bake it into the binary as
# ACCD_VERSION_STR.  Prefers ./VERSION (deployed-flat layout, where
# the deploy script copies VERSION next to the Makefile) over
# ../VERSION (source-tree layout, where VERSION is at the project
# root) so a stale parent-dir VERSION on a target host can't
# shadow the freshly-shipped one.  Falls back to "dev".  The `!=`
# shell-expansion syntax is supported by both BSD make and GNU
# make >= 4.0.
ACCD_VERSION !=	(cat VERSION 2>/dev/null || cat ../VERSION 2>/dev/null \
		    || echo dev) | tr -d '[:space:]'

# Internal flags always applied.  User-supplied CFLAGS / LDFLAGS
# / LIBS are appended after these so platform-specific paths can
# override without losing the compiler warnings.
#
# Hardening flags (all probed for compiler support before use; see
# the probe block in the ${PROG} recipe so older toolchains still
# build cleanly even if a flag is missing):
#   -D_FORTIFY_SOURCE=2     glibc/musl/OpenBSD runtime checks on
#                           memcpy/strcpy/printf-family inputs
#   -fstack-protector-strong  per-frame canary on functions with any
#                             buffer/local-address-taken
#   -Wformat -Wformat-security  reject non-literal printf format args
#   -Wnull-dereference        catch missing NULL checks (gcc 6+)
#   -fPIE / -pie              build a position-independent executable
#                             so the loader can ASLR the text segment
#   -Wl,-z,relro,-z,now        full RELRO + BIND_NOW (read-only GOT)
#   -Wl,-z,noexecstack         non-executable stack
ACCD_CFLAGS =	-std=c99 -Wall -Wextra -Wpedantic -O2 -g \
		-D_FORTIFY_SOURCE=2 -fstack-protector-strong \
		-Wformat -Wformat-security -Wnull-dereference \
		-fPIE \
		-DACCD_VERSION_STR='"${ACCD_VERSION}"' \
		-I/usr/local/include
ACCD_LDFLAGS =	-L/usr/local/lib -pie \
		-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
ACCD_LIBS =	-lm

all: ${PROG}

${PROG}: ${OBJS}
	@ICONV=; \
	if ! printf '%s\n%s\n' '#include <iconv.h>' \
	    'int main(){iconv_t c=iconv_open("","");iconv_close(c);return 0;}' \
	    | ${CC} ${ACCD_CFLAGS} ${CFLAGS} ${ACCD_LDFLAGS} ${LDFLAGS} \
	    -x c - -o /dev/null 2>/dev/null; then \
	    ICONV="-liconv"; \
	fi; \
	BSD=; \
	if printf '%s\n%s\n' '#include <bsd/stdlib.h>' \
	    'int main(){arc4random_uniform(1);return 0;}' \
	    | ${CC} ${ACCD_CFLAGS} ${CFLAGS} ${ACCD_LDFLAGS} ${LDFLAGS} \
	    -x c - -o /dev/null -lbsd 2>/dev/null; then \
	    BSD="-lbsd"; \
	fi; \
	SECCOMP=; \
	if printf '%s\n%s\n' '#include <seccomp.h>' \
	    'int main(){scmp_filter_ctx c=seccomp_init(0);seccomp_release(c);return 0;}' \
	    | ${CC} ${ACCD_CFLAGS} ${CFLAGS} ${ACCD_LDFLAGS} ${LDFLAGS} \
	    -x c - -o /dev/null -lseccomp 2>/dev/null; then \
	    SECCOMP="-lseccomp"; \
	fi; \
	echo ${CC} ${ACCD_LDFLAGS} ${LDFLAGS} -o ${PROG} ${OBJS} ${ACCD_LIBS} $$ICONV $$BSD $$SECCOMP ${LIBS}; \
	${CC} ${ACCD_LDFLAGS} ${LDFLAGS} -o ${PROG} ${OBJS} ${ACCD_LIBS} $$ICONV $$BSD $$SECCOMP ${LIBS}

# main.o embeds ACCD_VERSION_STR via -DACCD_VERSION_STR=...
# defined in ACCD_CFLAGS at make-invocation time.  After bumping
# VERSION, run `make clean && make` so the new value is baked in;
# touching main.c alone is not enough because the dependency
# system can't see the macro change.  The deploy script already
# does clean+make.
#
# Header dependencies below were generated with `cc -MM *.c` so a
# touch on any transitively-included header forces every dependent
# .o to rebuild.  After changing a #include in any .c or .h, run
# `make depend` and replace the block between the sentinel comments
# with the printed output.  The previous manual list was incomplete
# (e.g. nothing listed lobby.h even though state.h includes it),
# which let stale .o files survive a struct change in lobby.h and
# crashed accd at run time with a corrupted layout.

.SUFFIXES: .c .o
.c.o:
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c $<

# Print regenerated header-dep rules to stdout.  Copy the output over
# the block between '# --- header deps begin ---' and '# --- header
# deps end ---' below.  Print-only (not in-place edit) to keep the
# recipe portable across BSD/GNU make and to avoid awk/sed having to
# match its own sentinel markers in this same file.
depend:
	@for c in ${OBJS:.o=.c}; do \
	    ${CC} -MM ${ACCD_CFLAGS} ${CFLAGS} $$c || exit 1; \
	done

# --- header deps begin ---
main.o: main.c bans.h state.h io.h lobby.h msg.h ratings.h config.h \
 console.h dispatch.h bcast.h lan.h log.h net.h sandbox.h tick.h
bans.o: bans.c bans.h state.h io.h lobby.h msg.h log.h
bcast.o: bcast.c bcast.h state.h io.h lobby.h msg.h log.h
chat.o: chat.c bans.h state.h io.h lobby.h msg.h bcast.h chat.h \
 entrylist.h handlers.h handshake.h log.h penalty.h prim.h session.h \
 weather.h
config.o: config.c config.h state.h io.h lobby.h msg.h entrylist.h json.h \
 log.h weather.h
console.o: console.c chat.h state.h io.h lobby.h msg.h console.h log.h \
 penalty.h session.h
dispatch.o: dispatch.c bcast.h state.h io.h lobby.h msg.h dispatch.h \
 handlers.h handshake.h log.h prim.h smpr.h tick.h
entrylist.o: entrylist.c entrylist.h state.h io.h lobby.h msg.h json.h \
 log.h
handlers.o: handlers.c bcast.h state.h io.h lobby.h msg.h chat.h \
 handlers.h handshake.h log.h penalty.h prim.h ratings.h results.h \
 session.h tick.h
handshake.o: handshake.c bcast.h state.h io.h lobby.h msg.h ratings.h \
 bans.h entrylist.h handlers.h handshake.h log.h penalty.h prim.h \
 session.h smpr.h tick.h weather.h
io.o: io.c io.h log.h
json.o: json.c json.h
lan.o: lan.c io.h lan.h state.h lobby.h msg.h log.h prim.h
lobby.o: lobby.c bcast.h state.h io.h lobby.h msg.h chat.h log.h \
 penalty.h prim.h session.h
log.o: log.c log.h
monitor.o: monitor.c bcast.h state.h io.h lobby.h msg.h log.h monitor.h \
 pb.h prim.h
net.o: net.c log.h net.h
pb.o: pb.c io.h pb.h
penalty.o: penalty.c log.h penalty.h state.h io.h lobby.h msg.h session.h \
 tick.h
prim.o: prim.c prim.h io.h
ratings.o: ratings.c io.h json.h log.h ratings.h state.h lobby.h msg.h
results.o: results.c io.h log.h penalty.h state.h lobby.h msg.h results.h
sandbox.o: sandbox.c sandbox.h log.h
session.o: session.c bans.h state.h io.h lobby.h msg.h bcast.h \
 handshake.h log.h penalty.h prim.h results.h session.h smpr.h tick.h
smpr.o: smpr.c bcast.h state.h io.h lobby.h msg.h log.h monitor.h pb.h \
 prim.h smpr.h
state.o: state.c bcast.h state.h io.h lobby.h msg.h ratings.h handlers.h \
 log.h prim.h results.h session.h smpr.h tick.h
tick.o: tick.c bcast.h state.h io.h lobby.h msg.h chat.h ratings.h \
 handshake.h log.h penalty.h prim.h entrylist.h results.h session.h \
 smpr.h tick.h weather.h
weather.o: weather.c io.h log.h msg.h prim.h state.h lobby.h weather.h
# --- header deps end ---

probe: probe.c
	${CC} ${ACCD_CFLAGS} ${CFLAGS} ${ACCD_LDFLAGS} ${LDFLAGS} -o probe probe.c

# Smoke test: spin up accd with a synthetic config, send
# ACP_REQUEST_CONNECTION variants, verify:
#   - the 0x0b welcome trailer has the expected size and tail
#     sentinel (catches size drift of any write_* block in
#     handshake.c that would otherwise make the real ACC client
#     reject with "failed with value 13")
#   - the 0x0c reject reply carries the right code for the right
#     reason (wrong-version=7, wrong-password=6, accept=0x0b)
#   - fake_client.py walks the welcome trailer byte-by-byte (driver +
#     spectator) the way the AC2 client does, catching size drift in
#     any of the 11 sections to EOF (this is what flagged the 84 B
#     TrackConditions overrun the smoke test alone missed)
test: ${PROG}
	python3 tests/smoke_handshake.py
	python3 tests/smoke_reject_codes.py
	sh tests/integration/run_welcome_trailer_walk.sh

clean:
	rm -f ${PROG} ${OBJS} probe

PREFIX ?=	/usr/local
DESTDIR ?=
MANDIR ?=	${PREFIX}/share/man

install: ${PROG}
	install -d ${DESTDIR}${PREFIX}/bin
	install -m 755 ${PROG} ${DESTDIR}${PREFIX}/bin/${PROG}
	install -d ${DESTDIR}${MANDIR}/man1
	install -m 644 accd.1 ${DESTDIR}${MANDIR}/man1/accd.1

# OpenBSD service install — puts the rc.d wrapper under /etc/rc.d
# and (if missing) creates the /var/accd run-state directory owned
# by the _accd user.  The wrapper assumes the _accd user already
# exists (created by useradd / port install).  Run as root.
install-openbsd-service:
	install -m 555 openbsd/rc.d.accd ${DESTDIR}/etc/rc.d/accd
	test -d ${DESTDIR}/var/accd || install -d -o _accd -g _accd -m 750 \
	    ${DESTDIR}/var/accd

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/${PROG}
	rm -f ${DESTDIR}${MANDIR}/man1/accd.1

.PHONY: all clean depend install install-openbsd-service uninstall probe test
