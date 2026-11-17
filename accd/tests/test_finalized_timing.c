/* Final results and the qualifying archive must not diverge during aftercare. */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "handlers.h"
#include "handshake.h"
#include "io.h"
#include "prim.h"
#include "session.h"
#include "state.h"

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

static struct Server *
setup(uint8_t type)
{
	struct Server *s = calloc(1, sizeof(*s));
	int i;
	if (s == NULL)
		exit(2);
	server_init(s);
	s->max_connections = 4;
	s->session_count = 2;
	s->sessions[0].session_type = type;
	s->sessions[0].duration_min = 30;
	s->sessions[1].session_type = 10;
	s->sessions[1].duration_min = 30;
	s->session.phase = PHASE_SESSION;
	s->session.green_fired = 1;
	for (i = 0; i < 2; i++) {
		struct CarEntry *car = &s->cars[i];
		car->used = 1;
		car->driver_count = 1;
		car->car_id = (uint16_t)(ACC_CAR_ID_BASE + i);
		car->race.lap_count = 3;
		car->race.best_lap_ms = 100000 + i * 1000;
		car->race.race_time_ms = 300000 + i * 1000;
		car->race.formation_lap_done = 1;
		car->race.on_track = 1;
	}
	session_recompute_standings(s);
	return s;
}

static void
split(struct Server *s, struct Conn *c)
{
	struct ByteBuf b;
	bb_init(&b);
	wr_u8(&b, 0x20);
	wr_i32(&b, 30000);
	wr_u8(&b, 0);
	wr_i32(&b, 330000);
	wr_u16(&b, 0);
	h_sector_split_bulk(s, c, b.data, b.wpos);
	bb_free(&b);
}

static void
lap(struct Server *s, struct Conn *c)
{
	struct ByteBuf b;
	bb_init(&b);
	wr_u8(&b, 0x21);
	wr_i32(&b, 90000);
	wr_i32(&b, 400000);
	wr_u8(&b, 0);
	wr_u16(&b, 0);
	wr_u8(&b, 0);
	h_sector_split_single(s, c, b.data, b.wpos);
	bb_free(&b);
}

static void
check_finalized(uint8_t type, uint8_t phase)
{
	struct Server *s = setup(type);
	struct Conn c;
	struct CarRaceState saved;
	struct ByteBuf before, after;

	memset(&c, 0, sizeof(c));
	c.car_id = 1;
	c.conn_id = 1;
	c.state = CONN_AUTH;
	session_archive_snapshot(s);
	s->session.results_written = 1;
	s->session.phase = phase;
	saved = s->cars[1].race;
	bb_init(&before);
	bb_init(&after);
	expect(write_leaderboard_section(&before, s) == 0,
	    "could not serialize initial leaderboard");

	/* Neither message carries the optional client-side isSessionOver bit. */
	split(s, &c);
	expect(memcmp(&saved, &s->cars[1].race, sizeof(saved)) == 0,
	    "late sector changed finalized race state");
	lap(s, &c);
	expect(memcmp(&saved, &s->cars[1].race, sizeof(saved)) == 0,
	    "late lap changed finalized race state");
	expect(s->results_lap_count == 0, "late lap appended to final results");
	expect(memcmp(&saved, s->cars[1].race_archive[0], sizeof(saved)) == 0,
	    "final archive changed");
	expect(write_leaderboard_section(&after, s) == 0,
	    "could not serialize final leaderboard");
	expect(before.wpos == after.wpos &&
	    memcmp(before.data, after.data, before.wpos) == 0,
	    "late timing changed the client leaderboard");
	bb_free(&before);
	bb_free(&after);

	if (type == 4) {
		session_reset(s, 1);
		expect(s->cars[0].race.grid_position == 0 &&
		    s->cars[1].race.grid_position == 1,
		    "race grid disagrees with frozen qualifying order");
		expect(!s->session.results_written, "next session still frozen");
		lap(s, &c);
		expect(s->cars[1].race.best_lap_ms == 90000,
		    "next session rejected its first timing update");
	}
	server_free(s);
	free(s);
}

static void
check_overtime(void)
{
	struct Server *s = setup(4);
	struct Conn c;
	memset(&c, 0, sizeof(c));
	c.car_id = 1;
	c.conn_id = 1;
	c.state = CONN_AUTH;
	s->session.phase = PHASE_OVERTIME;
	s->cars[1].race.quali_eligible_to_finish = 1;
	split(s, &c);
	lap(s, &c);
	expect(s->cars[1].race.best_lap_ms == 90000 &&
	    s->cars[1].race.lap_count == 4,
	    "valid final flying lap was discarded during overtime");
	expect(s->cars[1].race.finished, "final flying lap did not finish the car");
	expect(s->cars[1].race.completed_lap_flags & 0x0400,
	    "final flying lap lost its finish flag");
	server_free(s);
	free(s);
}

int
main(void)
{
	uint8_t types[] = {0, 4, 10};
	uint8_t phases[] = {PHASE_COMPLETED, PHASE_ADVANCE, PHASE_RESULTS};
	size_t i, j;
	for (i = 0; i < sizeof(types); i++)
		for (j = 0; j < sizeof(phases); j++)
			check_finalized(types[i], phases[j]);
	check_overtime();
	if (failed)
		return 1;
	puts("PASS: finalized timing is immutable; overtime and next session still accept laps");
	return 0;
}
