CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c99
LDLIBS  ?= -lm

bot: bot.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f bot *.o

.PHONY: clean
