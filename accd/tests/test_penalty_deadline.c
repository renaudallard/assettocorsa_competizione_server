/* A promoted DT/SG entry with a zero-lap deadline must not underflow. */
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "handlers.h"
#include "penalty.h"
#include "state.h"

volatile sig_atomic_t g_stop;

static void
cross(struct Server *s, struct Conn *c)
{
	unsigned char b[13];

	memset(b, 0, sizeof(b));
	b[0] = 0x21;
	b[1] = 0x90;
	b[2] = 0x5f;
	b[3] = 0x01; /* 90 seconds */
	(void)h_sector_split_single(s, c, b, sizeof(b));
}

int
main(void)
{
	struct Server *s = calloc(1, sizeof(*s));
	struct sockaddr_in peer;
	struct Conn *c;
	struct CarRaceState *race;
	int ok;

	if (s == NULL)
		return 1;
	server_init(s);
	s->max_connections = 2;
	s->session_count = 1;
	s->sessions[0].session_type = 10; /* race */
	s->sessions[0].duration_min = 30;
	s->session.session_index = 0;
	s->session.phase = PHASE_SESSION;
	s->session.green_fired = 1;
	memset(&peer, 0, sizeof(peer));
	c = conn_new(s, -1, &peer);
	if (c == NULL)
		return 1;
	c->state = CONN_AUTH;
	c->car_id = 0;
	s->cars[0].used = 1;
	s->cars[0].car_id = ACC_CAR_ID_BASE;
	s->cars[0].driver_count = 1;
	s->cars[0].race.on_track = 1;
	race = &s->cars[0].race;

	/* The second report promotes DT to SG and resets the deadline to 0. */
	(void)penalty_enqueue(s, 0, EXE_DT, 0, 3, 0, 0, 0);
	(void)penalty_enqueue(s, 0, EXE_SG10, 0, 3, 0, 0, 0);
	ok = race->pen.count == 1 && race->pen.slots[0].laps_remaining == 0;
	cross(s, c);
	ok = ok && race->disqualified &&
	    race->pen.slots[0].laps_remaining == 0;
	/* Further lap messages must not decrement the expired entry. */
	cross(s, c);
	ok = ok && race->pen.slots[0].laps_remaining == 0;
	conn_drop(s, c);
	free(s);
	if (!ok) {
		fprintf(stderr, "FAIL: zero-lap DT/SG deadline wrapped\n");
		return 1;
	}
	puts("PASS: zero-lap DT/SG deadline expires once");
	return 0;
}
