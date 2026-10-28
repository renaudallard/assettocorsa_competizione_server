#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
volatile sig_atomic_t g_stop = 0;
/* Deterministic time for the real session machine; no wall-clock sleeps. */
#include "state.h"
static uint64_t lifecycle_now_ms(void);
#define mono_ms lifecycle_now_ms
#include "session.c"
#undef mono_ms
#include "handlers.h"
#include "lobby.h"

static uint64_t test_now = 1000000;
static uint64_t lifecycle_now_ms(void) { return test_now; }
static struct Server *race_server(void)
{
    struct Server *s = calloc(1, sizeof(*s));
    assert(s);
    server_init(s);
    s->max_connections = 25;
    s->session_count = 1;
    s->sessions[0].session_type = 10;
    s->sessions[0].duration_min = 10;
    s->session.session_index = 0;
    s->session.phase = PHASE_SESSION;
    s->session.ts_valid = 1;
    s->session.green_fired = 1;
    s->session_overtime_s = 120;
    s->post_race_s = 15;
    for (int i = 0; i < 4; i++) s->session.ts[i] = test_now - 60000;
    s->session.ts[4] = test_now;
    s->session.ts[5] = test_now + 120000;
    s->session.ts[6] = test_now + 135000;
    for (int i = 0; i < 2; i++) {
        struct CarEntry *c = &s->cars[i];
        c->used = 1;
        c->car_id = ACC_CAR_ID_BASE + i;
        c->driver_count = 1;
        c->race.position = i + 1;
        c->race.lap_count = 10 - i;
        c->race.on_track = 1;
        c->race.formation_lap_done = 1;
        c->rt.last_moved_ms = test_now;
    }
    return s;
}
static void lap(struct Server *s, int id)
{
    struct Conn c = {0};
    struct ByteBuf b;
    c.fd = -1; c.car_id = id; c.conn_id = id; c.state = CONN_AUTH;
    bb_init(&c.tx); bb_init(&c.rx); bb_init(&b);
    wr_u8(&b, 0x21); wr_i32(&b, 90000); wr_i32(&b, 200000);
    wr_u8(&b, 0); wr_u16(&b, 0); wr_u8(&b, 0);
    h_sector_split_single(s, &c, b.data, b.wpos);
    bb_free(&b); bb_free(&c.tx); bb_free(&c.rx);
}
static void finish_order(void)
{
    struct Server *s = race_server();
    session_tick(s);
    assert(s->session.phase == PHASE_OVERTIME);
    /* A lapped driver crosses before the leader: no chequered flag yet. */
    s->cars[1].race.lap_count = 5;
    lap(s, 1);
#ifdef RC_LIFECYCLE_BASELINE
    assert(s->cars[1].race.finished);
    puts("REPRODUCED: lapped car incorrectly finishes before leader");
    free(s); return;
#else
    assert(!s->cars[1].race.finished);
    assert(s->session.cars_in_overtime == 2);
    test_now += 1000;
    lap(s, 0);
    assert(s->cars[0].race.finished && !s->session.overtime_leader_armed);
    assert(s->session.cars_in_overtime == 1);
    struct ByteBuf record; bb_init(&record);
    assert(write_car_leaderboard_record(&record,s,&s->cars[0],1,0,NULL,0) == 0);
    assert(record.wpos > 8 && (record.data[7] & 4)); /* status 0x0400 */
    bb_free(&record);
    int laps = s->cars[0].race.lap_count;
    lap(s, 0); /* Cooldown lap must not count or consume another finish. */
    assert(s->cars[0].race.lap_count == laps);
    assert(s->session.cars_in_overtime == 1);
    test_now += 1000;
    lap(s, 1);
    assert(s->cars[1].race.finished && !s->session.overtime_hold);
    assert(s->session.ts[6] == test_now + 15000);
    session_tick(s);
    assert(s->session.phase == PHASE_COMPLETED);
    uint64_t end = s->session.ts[6];
    test_now = end - 1; session_tick(s);
    assert(s->session.phase == PHASE_COMPLETED && s->session.ts[6] == end);
    test_now = end; session_tick(s);
    assert(s->session.phase == PHASE_ADVANCE);
    puts("PASS: lapped crossing, leader finish, sender flag, duplicate/cooldown and full post-race grace");
    free(s);
#endif
}
#ifndef RC_LIFECYCLE_BASELINE
static void zero_lap_race(void)
{
    struct Server *s = race_server();
    s->cars[0].race.lap_count = s->cars[1].race.lap_count = 0;
    s->cars[0].rt.last_moved_ms = s->cars[1].rt.last_moved_ms = 0;
    session_tick(s);
#ifdef RC_OVERTIME_BASELINE
    assert(!s->session.overtime_hold);
    puts("REPRODUCED: race ends before the first on-track lap can finish");
#else
    assert(s->session.phase == PHASE_OVERTIME && s->session.overtime_hold);
    assert(s->session.cars_in_overtime == 2);
    lap(s,0);assert(s->cars[0].race.finished && s->session.overtime_hold);
    lap(s,1);assert(s->cars[1].race.finished && !s->session.overtime_hold);
    puts("PASS: zero-lap and temporarily stationary race cars may finish");
#endif
    free(s);
}
static void deadlines(void)
{
    struct Server *s = race_server();
    session_tick(s);
    uint64_t first_deadline = s->session.ts[5];
    test_now += 10000; session_overtime_car_finished(s, 0);
    uint64_t finish_deadline = s->session.ts[5];
    assert(finish_deadline == first_deadline);
    test_now = first_deadline - 1;
    s->cars[1].rt.last_moved_ms = test_now;
    session_tick(s);
    assert(s->session.overtime_hold && s->session.phase == PHASE_OVERTIME);
    test_now = finish_deadline;
    session_tick(s);
    assert(!s->session.overtime_hold && s->session.ts[6] == test_now + 15000);
    session_tick(s);
    assert(s->session.phase == PHASE_COMPLETED);
    test_now += 15000; session_tick(s);
    assert(s->session.phase == PHASE_ADVANCE);
    free(s);
    s = race_server();s->post_race_s = 0;session_tick(s);
    session_overtime_car_finished(s,0);session_overtime_car_finished(s,1);
    session_tick(s);assert(s->session.phase == PHASE_ADVANCE);free(s);
    s = race_server();session_tick(s);session_overtime_car_finished(s,0);
    s->cars[1].used = 0;
    session_tick(s);assert(!s->session.overtime_hold);
    assert(s->session.ts[6] == test_now + 15000);free(s);
    s = race_server();s->cars[0].race.in_pit = s->cars[1].race.in_pit = 1;
    session_tick(s);session_tick(s);
    assert(s->session.phase == PHASE_COMPLETED && s->session.ts[6] == test_now + 15000);
    free(s);
    puts("PASS: advertised deadline, timeout grace, zero post time and disconnect of remaining finisher");
}
#endif
int main(void)
{
#ifdef RC_OVERTIME_BASELINE
    zero_lap_race();
#else
    finish_order();
#ifndef RC_LIFECYCLE_BASELINE
    zero_lap_race();deadlines();
#endif
#endif
    return 0;
}
