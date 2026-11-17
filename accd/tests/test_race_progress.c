/* The live race order follows completed sectors, not the previous lap order. */
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
setup(void)
{
	struct Server *s = calloc(1, sizeof(*s));
	int i;
	if (s == NULL)
		exit(2);
	server_init(s);
	s->max_connections = 4;
	s->session_count = 1;
	s->sessions[0].session_type = 10;
	s->sessions[0].duration_min = 30;
	s->session.phase = PHASE_SESSION;
	s->session.green_fired = 1;
	for (i = 0; i < 2; i++) {
		struct CarEntry *car = &s->cars[i];
		car->used = 1;
		car->driver_count = 1;
		car->car_id = (uint16_t)(ACC_CAR_ID_BASE + i);
		car->race.lap_count = 3;
		car->race.grid_position = (int16_t)i;
		car->race.race_time_ms = 300000 + i * 1000;
		car->race.best_lap_ms = 100000 + i * 1000;
		car->race.formation_lap_done = 1;
	}
	session_recompute_standings(s);
	return s;
}

static void
split(struct Server *s, int car, uint8_t sector, int32_t clock)
{
	struct Conn c;
	struct ByteBuf b;
	memset(&c, 0, sizeof(c));
	c.car_id = car;
	c.conn_id = (uint16_t)car;
	c.state = CONN_AUTH;
	c.clock_base_ms = (int64_t)session_green_ms(s);
	bb_init(&b);
	wr_u8(&b, 0x20);
	wr_i32(&b, 30000);
	wr_u8(&b, sector);
	wr_i32(&b, clock);
	wr_u16(&b, 0);
	h_sector_split_bulk(s, &c, b.data, b.wpos);
	bb_free(&b);
}

static void
check_live_order(void)
{
	struct Server *s = setup();
	struct ByteBuf board;
	uint16_t first;

	split(s, 0, 0, 330000);
	split(s, 1, 1, 360000);
	/* No explicit recompute here: the real sector handler must update it. */
	expect(s->cars[1].race.position == 1 && s->cars[0].race.position == 2,
	    "car past sector 2 did not pass car still in sector 2");
	bb_init(&board);
	expect(write_leaderboard_section(&board, s) == 0 && board.wpos > 21,
	    "could not serialize live order");
	if (board.wpos > 21) {
		/* Fixed leaderboard header before the first car record: 20 bytes. */
		first = (uint16_t)(board.data[20] | (board.data[21] << 8));
		expect(first == ACC_CAR_ID_BASE + 1,
		    "client leaderboard still emits the previous leader first");
	}
	bb_free(&board);

	/* Earlier sector arrival wins even when the prior S/F order differs. */
	split(s, 0, 1, 361000);
	expect(s->cars[1].race.position == 1,
	    "same-sector order fell back to the previous lap's crossing");
	/* A delayed message with an earlier crossing time then leads B. */
	split(s, 0, 1, 359000);
	expect(s->cars[0].race.position == 1,
	    "same-sector order did not use the split crossing time");
	{
		struct Conn c;
		struct ByteBuf lap;
		memset(&c, 0, sizeof(c));
		c.car_id = 1;
		c.conn_id = 1;
		c.state = CONN_AUTH;
		c.clock_base_ms = (int64_t)session_green_ms(s);
		bb_init(&lap);
		wr_u8(&lap, 0x21);
		wr_i32(&lap, 99000);
		wr_i32(&lap, 400000);
		wr_u8(&lap, 0);
		wr_u16(&lap, 0);
		wr_u8(&lap, 0);
		h_sector_split_single(s, &c, lap.data, lap.wpos);
		bb_free(&lap);
	}
	expect(s->cars[1].race.lap_count == 4 &&
	    s->cars[1].race.last_split_id == 0,
	    "finish-line crossing did not reset sector progress");
	expect(s->cars[1].race.position == 1, "sector progress overrode a completed lap");
	server_free(s);
	free(s);
}

static void
check_boundaries(void)
{
	struct Server *s = setup();
	int i;
	for (i = 0; i < 2; i++) {
		s->cars[i].race.lap_count = 0;
		s->cars[i].race.race_time_ms = 0;
		s->cars[i].race.last_split_id = SPLIT_ID_NONE;
	}
	session_recompute_standings(s);
	expect(s->cars[0].race.position == 1, "unstarted cars lost their grid order");
	split(s, 1, 0, 30000);
	expect(s->cars[1].race.position == 1, "first racing lap cannot change order");
	s->session.green_fired = 0;
	session_recompute_standings(s);
	expect(s->cars[0].race.position == 1, "formation reordered the starting grid");
	s->session.green_fired = 1;
	s->cars[1].race.disqualified = 1;
	session_recompute_standings(s);
	expect(s->cars[0].race.position == 1, "sector progress overrode disqualification");
	s->cars[1].race.disqualified = 0;
	s->cars[1].race.car_field = 0x40;
	session_recompute_standings(s);
	expect(s->cars[0].race.position == 1, "sector progress overrode retirement");
	s->cars[1].race.car_field = 0;
	s->sessions[0].session_type = 4;
	session_recompute_standings(s);
	expect(s->cars[0].race.position == 1, "qualifying stopped using best lap");
	server_free(s);
	free(s);
}

static void
check_missing_clock(void)
{
	struct Server *s = setup();
	int i;
	s->cars[2] = s->cars[0];
	s->cars[2].car_id = ACC_CAR_ID_BASE + 2;
	for (i = 0; i < 3; i++) {
		s->cars[i].race.last_split_id = 2;
		s->cars[i].race.race_time_ms = 300000 + i * 1000;
	}
	s->cars[0].race.last_split_ms = 360000;
	s->cars[1].race.last_split_ms = 0;
	s->cars[2].race.last_split_ms = 359000;
	session_recompute_standings(s);
	expect(s->cars[2].race.position == 1 &&
	    s->cars[0].race.position == 2 && s->cars[1].race.position == 3,
	    "missing clock made same-sector ordering depend on the previous lap");
	server_free(s);
	free(s);
}

static void
check_finish(void)
{
	struct Server *s = setup();
	s->session.phase = PHASE_OVERTIME;
	s->cars[0].race.finished = 1;
	s->cars[1].race.last_split_id = 2;
	s->cars[1].race.last_split_ms = 360000;
	session_recompute_standings(s);
	expect(s->cars[0].race.position == 1, "unfinished car passed a finisher on equal laps");
	s->cars[1].race.finished = 1;
	s->cars[1].race.race_time_ms = 299000;
	session_recompute_standings(s);
	expect(s->cars[1].race.position == 1, "finishers no longer use finish-line time");
	s->session.results_written = 1;
	s->session.phase = PHASE_ADVANCE;
	s->cars[0].race.finished = s->cars[1].race.finished = 0;
	s->cars[1].race.race_time_ms = 301000;
	session_recompute_standings(s);
	expect(s->cars[0].race.position == 1, "aftercare reordered cars by cooldown sectors");
	server_free(s);
	free(s);
}

int
main(void)
{
	check_live_order();
	check_boundaries();
	check_missing_clock();
	check_finish();
	if (failed)
		return 1;
	puts("PASS: live race order follows sector progress and preserves classification boundaries");
	return 0;
}
