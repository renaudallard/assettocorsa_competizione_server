/*
 * Every bop.json line for the server track and the car model is added
 * to the entrylist ballast and restrictor when a car joins, and the
 * sum is clamped. A rejoin starts again from the entrylist values, so
 * the lines never add up twice and an admin /ballast or /restrictor
 * lasts only until the car rejoins. /manual entrylist saves the
 * entrylist values, not the live ones.
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/socket.h>

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chat.h"
#include "entrylist.h"
#include "handshake.h"
#include "io.h"
#include "msg.h"
#include "prim.h"
#include "session.h"
#include "state.h"

/* Defined in main.c, which the unit tests do not link. */
volatile sig_atomic_t g_stop;

static int failed;
static char dir[] = "tests/test_bop.XXXXXX";

static const char entrylist[] =
    "{\"entries\": [\n"
    "{\"drivers\": [{\"playerID\": \"S100\"}], \"raceNumber\": 11,"
    " \"ballastKg\": 5, \"restrictor\": 2},\n"
    "{\"drivers\": [{\"playerID\": \"S101\"}], \"raceNumber\": 12},\n"
    "{\"drivers\": [{\"playerID\": \"S102\"}], \"raceNumber\": 13,"
    " \"ballastKg\": 20, \"restrictor\": 10},\n"
    "{\"drivers\": [{\"playerID\": \"S103\"}], \"raceNumber\": 14,"
    " \"ballastKg\": 7}\n"
    "], \"forceEntryList\": 1}\n";

static const char bop[] =
    "{\"entries\": [\n"
    "{\"track\": \"monza\", \"carModel\": 35, \"ballastKg\": 10,"
    " \"restrictor\": 5},\n"
    "{\"track\": \"monza\", \"carModel\": 35, \"ballastKg\": 15,"
    " \"restrictor\": 3},\n"
    "{\"track\": \"misano\", \"carModel\": 35, \"ballastKg\": 30},\n"
    "{\"track\": \"monza\", \"carModel\": 30, \"ballastKg\": 20},\n"
    "{\"track\": \"monza\", \"carModel\": 31, \"ballastKg\": -25},\n"
    "{\"track\": \"monza\", \"carModel\": 32, \"ballastKg\": 30},\n"
    "{\"track\": \"monza\", \"carModel\": 32, \"ballastKg\": 30,"
    " \"restrictor\": 15},\n"
    "{\"track\": \"monza\", \"carModel\": 33, \"ballastKg\": 100,"
    " \"restrictor\": 5},\n"
    "{\"track\": \"monza\", \"carModel\": 33, \"ballastKg\": -30,"
    " \"restrictor\": -10},\n"
    "{\"track\": \"monza_2019\", \"carModel\": 35, \"ballastKg\": 40},\n"
    "{\"track\": \"Monza\", \"carModel\": 35, \"ballastKg\": 40}\n"
    "]}\n";

static void
fail(const char *what)
{
	fprintf(stderr, "FAIL: %s\n", what);
	failed = 1;
}

static void
write_cfg(const char *name, const char *text)
{
	char path[64];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if ((fp = fopen(path, "w")) == NULL || fputs(text, fp) == EOF ||
	    fclose(fp) == EOF) {
		perror(path);
		exit(1);
	}
}

static struct Server *
new_server(void)
{
	struct Server *s;

	if ((s = calloc(1, sizeof(*s))) == NULL) {
		perror("calloc");
		exit(1);
	}
	server_init(s);
	s->max_connections = 8;
	s->max_car_slots = 8;
	s->session_count = 1;
	s->sessions[0].session_type = 0;
	s->sessions[0].duration_min = 10;
	snprintf(s->track, sizeof(s->track), "monza");
	return s;
}

/* Discard whatever the server sent on the client end of a socketpair. */
static void
drain(int fd)
{
	char buf[8192];

	while (read(fd, buf, sizeof(buf)) > 0)
		;
}

/* Drop every connection a rejoin marked for teardown. */
static void
reap(struct Server *s)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS; i++)
		if (s->conns[i] != NULL &&
		    s->conns[i]->state == CONN_DISCONNECT)
			conn_drop(s, s->conns[i]);
}

/*
 * Send a real client format 0x09 for this driver and car model and
 * return the accepted connection.
 */
static struct Conn *
join(struct Server *s, const char *steam, uint8_t model, int32_t rnum,
    int *peer_fd)
{
	struct sockaddr_in peer;
	struct ByteBuf bb;
	struct Conn *c;
	int i, sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1 ||
	    fcntl(sv[1], F_SETFL, O_NONBLOCK) == -1) {
		perror("socketpair");
		exit(1);
	}
	memset(&peer, 0, sizeof(peer));
	if ((c = conn_new(s, sv[0], &peer)) == NULL) {
		fprintf(stderr, "conn_new failed\n");
		exit(1);
	}

	bb_init(&bb);
	(void)wr_u8(&bb, ACP_REQUEST_CONNECTION);
	(void)wr_u16(&bb, ACC_PROTOCOL_VERSION);
	(void)wr_str_a(&bb, "");		/* password */
	/* DriverInfo: 5 strings, 41 byte block, steam id. */
	(void)wr_str_a(&bb, "Bop");
	(void)wr_str_a(&bb, "");
	(void)wr_str_a(&bb, "Test");
	(void)wr_str_a(&bb, "");
	(void)wr_str_a(&bb, "BOP");
	for (i = 0; i < 41; i++)
		(void)wr_u8(&bb, 0);
	(void)wr_str_a(&bb, steam);
	for (i = 0; i < 8; i++)
		(void)wr_u8(&bb, 0);
	/* CarInfo. */
	(void)wr_u32(&bb, 0);
	(void)wr_u32(&bb, 0);
	(void)wr_i32(&bb, rnum);
	for (i = 0; i < 33; i++)
		(void)wr_u8(&bb, 0);
	(void)wr_str_a(&bb, "");		/* customSkinName */
	(void)wr_u8(&bb, 0);			/* bannerKey */
	(void)wr_str_a(&bb, "");		/* teamName */
	(void)wr_u16(&bb, 0);			/* nationality */
	(void)wr_str_a(&bb, "");		/* displayName */
	(void)wr_str_a(&bb, "");		/* competitorName */
	for (i = 0; i < 3; i++)
		(void)wr_u8(&bb, 0);
	(void)wr_u8(&bb, model);		/* carModelType */
	for (i = 0; i < 5; i++)
		(void)wr_u8(&bb, 0);
	/* The server takes the short probe format at 200 bytes or less. */
	while (bb.wpos <= 200)
		(void)wr_u8(&bb, 0);

	(void)handshake_handle(s, c, bb.data, bb.wpos);
	bb_free(&bb);
	drain(sv[1]);
	reap(s);
	if (c->state != CONN_AUTH || c->car_id < 0) {
		fprintf(stderr, "FAIL: %s was not accepted\n", steam);
		exit(1);
	}
	*peer_fd = sv[1];
	return c;
}

static void
leave(struct Server *s, struct Conn *c, int peer_fd)
{
	conn_drop(s, c);
	close(peer_fd);
}

static int
near(float a, float b)
{
	float d = a - b;

	return d > -1e-6f && d < 1e-6f;
}

/*
 * Check the live values and the ballast and restrictor the welcome
 * spawnDef carries for this car, its last 6 bytes.
 */
static void
expect(struct Server *s, struct Conn *c, int kg, float rest,
    const char *what)
{
	const struct CarEntry *car = &s->cars[c->car_id];
	struct ByteBuf bb;
	struct Reader r;
	uint16_t wkg = 0;
	int32_t wrace = 0;
	float wrest = 0.0f;
	char msg[128];

	if (car->ballast_kg != kg || !near(car->restrictor, rest)) {
		snprintf(msg, sizeof(msg), "%s: car has %d kg %.4f, "
		    "want %d kg %.4f", what, (int)car->ballast_kg,
		    (double)car->restrictor, kg, (double)rest);
		fail(msg);
	}
	bb_init(&bb);
	if (write_spawn_def(&bb, s, c->car_id) < 0 || bb.wpos < 16) {
		snprintf(msg, sizeof(msg), "%s: no spawnDef", what);
		fail(msg);
		bb_free(&bb);
		return;
	}
	/* spawnDef contains a four-byte race number after the two CarInfo ids.
	 * A duplicate client request must not leave the old number here. */
	rd_init(&r, bb.data + 12, 4);
	if (rd_i32(&r, &wrace) < 0 || wrace != car->race_number) {
		snprintf(msg, sizeof(msg), "%s: spawnDef number %d, assigned %d",
		    what, wrace, car->race_number);
		fail(msg);
	}
	rd_init(&r, bb.data + bb.wpos - 6, 6);
	if (rd_u16(&r, &wkg) < 0 || rd_f32(&r, &wrest) < 0 ||
	    (int16_t)wkg != kg || !near(wrest, rest)) {
		snprintf(msg, sizeof(msg), "%s: spawnDef has %d kg %.4f",
		    what, (int)(int16_t)wkg, (double)wrest);
		fail(msg);
	}
	bb_free(&bb);
}

static void
admin(struct Server *s, const char *args, int is_ballast)
{
	char reply[128];

	chat_do_bop(s, args, is_ballast, reply, sizeof(reply));
}

static void
entrylist_server(void)
{
	struct Server *s, *saved;
	struct Conn *c0, *c1, *c2, *c3;
	int p0, p1, p2, p3, pold;

	s = new_server();
	write_cfg("entrylist.json", entrylist);
	write_cfg("bop.json", bop);
	if (entrylist_load(s, dir) != 4)
		fail("entrylist.json did not load");
	(void)bop_load(s, dir);
	if (s->bop_count != 9)
		fail("bop lines for an unknown track were kept");

	/* Two matching lines on top of the entry, the rest ignored. */
	c0 = join(s, "S100", 35, 0, &p0);
	expect(s, c0, 5 + 10 + 15, 0.02f + 0.05f + 0.03f, "summed lines");
	/* Signed ballast, no entry values. */
	c1 = join(s, "S101", 31, 0, &p1);
	expect(s, c1, -25, 0.0f, "negative ballast");
	/* 20 + 60 kg and 10 + 15 % are clamped. */
	c2 = join(s, "S102", 32, 0, &p2);
	expect(s, c2, 40, 0.2f, "clamped sum");
	/* Each line is clamped before it is added. */
	c3 = join(s, "S103", 33, 0, &p3);
	expect(s, c3, 7 + 40 - 30, 0.05f, "clamped lines");

	/* Back while the old connection is still up, then after a drop. */
	pold = p0;
	c0 = join(s, "S100", 35, 0, &p0);
	close(pold);
	expect(s, c0, 30, 0.10f, "quick reconnect");
	leave(s, c0, p0);
	c0 = join(s, "S100", 35, 0, &p0);
	expect(s, c0, 30, 0.10f, "reconnect");

	/* Admin values hold until the car rejoins. */
	admin(s, "11 -12", 1);
	admin(s, "11 4", 0);
	drain(p0);
	expect(s, c0, -12, 0.04f, "admin override");

	/* The saved entry list keeps the entry values. */
	if (entrylist_save(s, dir) < 0)
		fail("entrylist_save");
	saved = new_server();
	if (entrylist_load(saved, dir) != 4)
		fail("saved entrylist.json did not load");
	if (saved->cars[0].ballast_kg != 5 ||
	    !near(saved->cars[0].restrictor, 0.02f) ||
	    saved->cars[2].ballast_kg != 20 ||
	    !near(saved->cars[2].restrictor, 0.10f))
		fail("entrylist_save wrote the live values");
	free(saved);

	leave(s, c0, p0);
	c0 = join(s, "S100", 35, 0, &p0);
	expect(s, c0, 30, 0.10f, "admin override after rejoin");

	leave(s, c0, p0);
	leave(s, c1, p1);
	leave(s, c2, p2);
	leave(s, c3, p3);
	server_free(s);
	free(s);
}

/*
 * Without an entry list a car made by a join starts from zero, even
 * in a slot an earlier car with an admin ballast left behind. A new
 * car only takes over a departed car's slot when no slot is empty,
 * so give the server a single one.
 */
static void
open_server(void)
{
	struct Server *s;
	struct Conn *c;
	int p;

	s = new_server();
	s->max_connections = 1;
	(void)bop_load(s, dir);
	c = join(s, "S200", 35, 21, &p);
	expect(s, c, 25, 0.08f, "car made by a join");
	admin(s, "21 40", 1);
	leave(s, c, p);
	c = join(s, "S201", 35, 22, &p);
	if (c->car_id != 0)
		fail("second driver did not reuse the slot");
	expect(s, c, 25, 0.08f, "new driver in a used slot");
	leave(s, c, p);
	server_free(s);
	free(s);
}

static void
duplicate_race_number(void)
{
	struct Server *s = new_server();
	struct Conn *first, *second;
	int p0, p1;

	first = join(s, "S300", 35, 5, &p0);
	second = join(s, "S301", 35, 5, &p1);
	if (s->cars[first->car_id].race_number != 5 ||
	    s->cars[second->car_id].race_number != 6)
		fail("duplicate client race numbers were not disambiguated");
	expect(s, first, 0, 0.0f, "first race number");
	expect(s, second, 0, 0.0f, "reassigned race number");
	leave(s, first, p0);
	leave(s, second, p1);
	server_free(s);
	free(s);
}

static int
live_board_count(struct Server *s, uint32_t *best)
{
	struct ByteBuf bb;
	struct Reader r;
	uint16_t count = 0;

	bb_init(&bb);
	if (write_leaderboard_section(&bb, s) < 0 || bb.wpos < 20) {
		fail("could not build live leaderboard");
		bb_free(&bb);
		return -1;
	}
	if (best != NULL) {
		rd_init(&r, bb.data, 4);
		if (rd_u32(&r, best) < 0)
			fail("could not read live session best");
	}
	/* best lap (4), split count (1), three splits (12), flag (1) */
	rd_init(&r, bb.data + 18, 2);
	if (rd_u16(&r, &count) < 0)
		fail("could not read live leaderboard count");
	bb_free(&bb);
	return count;
}

static void
departed_live_board(void)
{
	struct Server *s = new_server();
	struct Conn *first, *second;
	int p0, p1, first_slot;
	uint32_t best = 0;

	first = join(s, "S400", 35, 40, &p0);
	second = join(s, "S401", 35, 41, &p1);
	first_slot = first->car_id;
	s->cars[first_slot].race.lap_count = 2;
	s->cars[first_slot].race.best_lap_ms = 90000;
	s->cars[second->car_id].race.lap_count = 1;
	s->cars[second->car_id].race.best_lap_ms = 100000;
	session_recompute_standings(s);
	if (live_board_count(s, &best) != 2 || best != 90000)
		fail("connected drivers missing from live leaderboard");
	leave(s, first, p0);
	if (live_board_count(s, &best) != 1 || best != 100000)
		fail("departed driver remained on live leaderboard");
	if (s->cars[first_slot].race.lap_count != 2 ||
	    s->cars[first_slot].race.best_lap_ms != 90000)
		fail("departed driver's result was lost");
	leave(s, second, p1);
	server_free(s);
	free(s);
}

static void
cleanup(void)
{
	char path[64];

	snprintf(path, sizeof(path), "%s/entrylist.json", dir);
	(void)unlink(path);
	snprintf(path, sizeof(path), "%s/bop.json", dir);
	(void)unlink(path);
	(void)rmdir(dir);
}

int
main(void)
{
	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	entrylist_server();
	open_server();
	duplicate_race_number();
	departed_live_board();
	cleanup();
	if (failed)
		return 1;
	printf("PASS: bop.json is added to the entry list on every join\n");
	return 0;
}
