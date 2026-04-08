# accd -- ACC dedicated server reimplementation (phase 0 skeleton)
#
# Portable Makefile.  Works with both BSD make and GNU make.
# No conditionals, no pattern-substitution tricks.

PROG =		accd
OBJS =		main.o

CFLAGS +=	-std=c99 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS +=

all: ${PROG}

${PROG}: ${OBJS}
	${CC} ${LDFLAGS} -o ${PROG} ${OBJS}

main.o: main.c
	${CC} ${CFLAGS} -c main.c

clean:
	rm -f ${PROG} ${OBJS}

.PHONY: all clean
