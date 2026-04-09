# accd -- ACC dedicated server reimplementation
#
# Portable Makefile.  Works with both BSD make and GNU make.
# No conditionals, no pattern-substitution tricks.
#
# OpenBSD users: iconv lives in ports at /usr/local.  Build with
#     make CFLAGS="-I/usr/local/include" \
#          LDFLAGS="-L/usr/local/lib" LIBS="-liconv"

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
		log.o \
		monitor.o \
		net.o \
		pb.o \
		penalty.o \
		prim.o \
		results.o \
		session.o \
		state.o \
		tick.o \
		weather.o

# Internal flags always applied.  User-supplied CFLAGS / LDFLAGS
# / LIBS are appended after these so platform-specific paths can
# override without losing the compiler warnings.
ACCD_CFLAGS =	-std=c99 -Wall -Wextra -Wpedantic -O2 -g
ACCD_LIBS =	-lm

all: ${PROG}

${PROG}: ${OBJS}
	${CC} ${LDFLAGS} -o ${PROG} ${OBJS} ${ACCD_LIBS} ${LIBS}

main.o: main.c bans.h config.h console.h dispatch.h io.h lan.h log.h msg.h net.h state.h tick.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c main.c

bans.o: bans.c bans.h log.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c bans.c

bcast.o: bcast.c bcast.h io.h log.h msg.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c bcast.c

chat.o: chat.c chat.h bans.h bcast.h handshake.h io.h log.h msg.h penalty.h prim.h session.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c chat.c

console.o: console.c console.h chat.h log.h penalty.h session.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c console.c

config.o: config.c config.h entrylist.h json.h log.h state.h weather.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c config.c

dispatch.o: dispatch.c dispatch.h handlers.h handshake.h io.h log.h msg.h prim.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c dispatch.c

entrylist.o: entrylist.c entrylist.h json.h log.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c entrylist.c

handlers.o: handlers.c handlers.h bcast.h chat.h io.h log.h msg.h penalty.h prim.h session.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c handlers.c

handshake.o: handshake.c handshake.h bans.h bcast.h io.h log.h msg.h prim.h state.h weather.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c handshake.c

io.o: io.c io.h log.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c io.c

json.o: json.c json.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c json.c

lan.o: lan.c lan.h io.h log.h msg.h prim.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c lan.c

log.o: log.c log.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c log.c

monitor.o: monitor.c monitor.h bcast.h io.h log.h msg.h pb.h prim.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c monitor.c

net.o: net.c net.h log.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c net.c

pb.o: pb.c pb.h io.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c pb.c

penalty.o: penalty.c penalty.h log.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c penalty.c

prim.o: prim.c prim.h io.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c prim.c

results.o: results.c results.h log.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c results.c

session.o: session.c session.h log.h msg.h results.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c session.c

state.o: state.c state.h bcast.h io.h log.h msg.h prim.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c state.c

tick.o: tick.c tick.h bcast.h io.h log.h msg.h prim.h results.h session.h state.h weather.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c tick.c

weather.o: weather.c weather.h io.h log.h msg.h prim.h state.h
	${CC} ${ACCD_CFLAGS} ${CFLAGS} -c weather.c

clean:
	rm -f ${PROG} ${OBJS}

PREFIX ?=	/usr/local
DESTDIR ?=

install: ${PROG}
	install -d ${DESTDIR}${PREFIX}/bin
	install -m 755 ${PROG} ${DESTDIR}${PREFIX}/bin/${PROG}

.PHONY: all clean install
