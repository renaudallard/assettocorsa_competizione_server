# accd -- ACC dedicated server reimplementation
#
# Portable Makefile.  Works with both BSD make and GNU make.
# No conditionals, no pattern-substitution tricks.

PROG =		accd

OBJS =		main.o \
		bcast.o \
		chat.o \
		config.o \
		dispatch.o \
		handlers.o \
		handshake.o \
		io.o \
		json.o \
		lan.o \
		log.o \
		monitor.o \
		net.o \
		pb.o \
		prim.o \
		state.o \
		tick.o

CFLAGS +=	-std=c99 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS +=

all: ${PROG}

${PROG}: ${OBJS}
	${CC} ${LDFLAGS} -o ${PROG} ${OBJS}

main.o: main.c config.h dispatch.h io.h lan.h log.h msg.h net.h state.h tick.h
	${CC} ${CFLAGS} -c main.c

bcast.o: bcast.c bcast.h io.h log.h msg.h state.h
	${CC} ${CFLAGS} -c bcast.c

chat.o: chat.c chat.h log.h msg.h state.h
	${CC} ${CFLAGS} -c chat.c

config.o: config.c config.h json.h log.h state.h
	${CC} ${CFLAGS} -c config.c

dispatch.o: dispatch.c dispatch.h handlers.h handshake.h io.h log.h msg.h prim.h state.h
	${CC} ${CFLAGS} -c dispatch.c

handlers.o: handlers.c handlers.h bcast.h chat.h io.h log.h msg.h prim.h state.h
	${CC} ${CFLAGS} -c handlers.c

handshake.o: handshake.c handshake.h bcast.h io.h log.h monitor.h msg.h prim.h state.h
	${CC} ${CFLAGS} -c handshake.c

io.o: io.c io.h
	${CC} ${CFLAGS} -c io.c

json.o: json.c json.h
	${CC} ${CFLAGS} -c json.c

lan.o: lan.c lan.h io.h log.h msg.h prim.h state.h
	${CC} ${CFLAGS} -c lan.c

log.o: log.c log.h
	${CC} ${CFLAGS} -c log.c

monitor.o: monitor.c monitor.h bcast.h io.h log.h msg.h pb.h state.h
	${CC} ${CFLAGS} -c monitor.c

net.o: net.c net.h log.h
	${CC} ${CFLAGS} -c net.c

pb.o: pb.c pb.h io.h
	${CC} ${CFLAGS} -c pb.c

prim.o: prim.c prim.h io.h
	${CC} ${CFLAGS} -c prim.c

state.o: state.c state.h bcast.h io.h log.h msg.h prim.h
	${CC} ${CFLAGS} -c state.c

tick.o: tick.c tick.h log.h msg.h state.h
	${CC} ${CFLAGS} -c tick.c

clean:
	rm -f ${PROG} ${OBJS}

.PHONY: all clean
