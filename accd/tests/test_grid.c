/*
 * The race grid numbers the connected cars in qualifying order without
 * gaps, then the connected ones with no qualifying result, then the
 * departed ones while the grid has room, like FUN_140032400. Departed
 * cars that qualified ahead must not push a live car past the grid.
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "session.h"
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

/* Car in slot i, qualified in position qpos (0 = no result). */
static void
car(struct Server *s, int i, int used, int qpos)
{
	s->cars[i].driver_count = 1;
	s->cars[i].used = (uint8_t)used;
	if (qpos > 0) {
		s->cars[i].race_archive[0] =
		    calloc(1, sizeof(struct CarRaceState));
		if (s->cars[i].race_archive[0] == NULL) {
			perror("calloc");
			exit(1);
		}
		s->cars[i].race_archive[0]->position = (int16_t)qpos;
	}
}

static void
grid(int max_connections, int g[6])
{
	struct Server *s;
	int i;

	if ((s = calloc(1, sizeof(*s))) == NULL) {
		perror("calloc");
		exit(1);
	}
	server_init(s);
	s->max_connections = max_connections;
	s->session_count = 2;
	s->sessions[0].session_type = 4;
	s->sessions[0].duration_min = 10;
	s->sessions[1].session_type = 10;
	s->sessions[1].duration_min = 20;
	car(s, 0, 0, 1);	/* departed, P1 */
	car(s, 1, 0, 2);	/* departed, P2 */
	car(s, 2, 1, 4);	/* connected, P4 */
	car(s, 3, 1, 3);	/* connected, P3 */
	car(s, 4, 1, 0);	/* connected, no result */
	car(s, 5, 0, 5);	/* departed, P5 */
	session_reset(s, 1);
	for (i = 0; i < 6; i++)
		g[i] = s->cars[i].race.grid_position;
	server_free(s);
	free(s);
}

int
main(void)
{
	int g[6];

	grid(3, g);
	expect(g[3] == 0 && g[2] == 1, "connected cars not first in "
	    "qualifying order");
	expect(g[4] == 2, "connected car without a result not after them");
	expect(g[0] == -1 && g[1] == -1 && g[5] == -1,
	    "departed car given a slot past the grid");

	grid(8, g);
	expect(g[3] == 0 && g[2] == 1 && g[4] == 2,
	    "connected cars moved by the departed ones");
	expect(g[0] == 3 && g[1] == 4 && g[5] == 5,
	    "departed cars not numbered after the connected ones");

	if (failed)
		return 1;
	printf("PASS: race grid numbers the connected cars first\n");
	return 0;
}
