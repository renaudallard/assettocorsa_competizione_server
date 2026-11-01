/*
 * A car whose driver left keeps its slot and wire id. The pool is every
 * slot, not maxConnections. A new car takes an empty slot first, then
 * the slot of a departed car with no result row, then the slot of the
 * driver who left first, under an id no other car of the event had.
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "state.h"

/* Defined in main.c, which the unit tests do not link. */
volatile sig_atomic_t g_stop;

static int failed;

static void
expect(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", what);
		failed = 1;
	}
}

static int
join(struct Server *s, const char *steam)
{
	int i;

	if ((i = server_alloc_car(s)) < 0)
		return -1;
	s->cars[i].driver_count = 1;
	snprintf(s->cars[i].drivers[0].steam_id,
	    sizeof(s->cars[i].drivers[0].steam_id), "%s", steam);
	return i;
}

static void
leave(struct Server *s, int i, uint64_t when)
{
	s->cars[i].used = 0;
	s->cars[i].left_ms = when;
}

int
main(void)
{
	struct Server *s;
	char steam[16];
	int a, b, c, d, e, i;

	if ((s = calloc(1, sizeof(*s))) == NULL) {
		perror("calloc");
		return 1;
	}
	server_init(s);
	s->max_connections = 3;

	a = join(s, "A");
	b = join(s, "B");
	expect(a == 0 && s->cars[a].car_id == ACC_CAR_ID_BASE,
	    "first car takes slot 0 as car 1001");
	leave(s, a, 100);

	c = join(s, "C");
	expect(c == 2, "new car skips the departed car's slot");
	expect(s->cars[a].driver_count == 1 &&
	    strcmp(s->cars[a].drivers[0].steam_id, "A") == 0,
	    "departed car keeps its driver");
	expect(server_car_slot(s, ACC_CAR_ID_BASE) == a,
	    "departed car keeps its id");

	/* Departed cars do not count against maxConnections. */
	for (i = 3; i < ACC_MAX_CARS; i++) {
		snprintf(steam, sizeof(steam), "P%d", i);
		expect(join(s, steam) == i, "pool ends at maxConnections");
	}

	/* No empty slot left: the driver who left first gives way. */
	leave(s, b, 200);
	s->cars[a].race_archive[0] = calloc(1, sizeof(struct CarRaceState));
	s->cars[a].ballast_kg = 30;
	d = join(s, "D");
	expect(d == a, "new car takes the slot of the oldest departure");
	expect(s->cars[d].car_id == ACC_CAR_ID_BASE + ACC_MAX_CARS,
	    "reused slot gets an id above the slot range");
	expect(server_car_slot(s, ACC_CAR_ID_BASE) < 0,
	    "id of the replaced car is not reused");
	expect(s->cars[d].race_archive[0] == NULL &&
	    s->cars[d].ballast_kg == 0,
	    "reused slot starts clean");

	e = join(s, "E");
	expect(e == b &&
	    s->cars[e].car_id == ACC_CAR_ID_BASE + ACC_MAX_CARS + 1,
	    "next reused slot gets the next id");
	expect(join(s, "F") < 0, "no slot while every car is connected");

	/* A car with a result row gives way last, even if it left first. */
	leave(s, 3, 300);
	s->cars[3].race.lap_count = 1;
	leave(s, 4, 400);
	expect(join(s, "G") == 4, "car with a result row gave way first");
	expect(join(s, "H") == 3, "car with a result row gave way last");

	/* After a wrap the counter skips ids a car still holds. */
	leave(s, c, 500);
	s->next_car_id = 0xfffe;
	(void)join(s, "I");
	expect(s->cars[c].car_id == 0xfffe, "last id before the wrap");
	leave(s, c, 600);
	(void)join(s, "J");
	expect(s->cars[c].car_id == ACC_CAR_ID_BASE + ACC_MAX_CARS + 4,
	    "wrapped counter skips ids in use");

	server_free(s);
	free(s);
	if (failed)
		return 1;
	printf("PASS: departed cars keep their id, reused slots get "
	    "new ones\n");
	return 0;
}
