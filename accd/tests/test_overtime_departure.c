#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include "state.h"
volatile sig_atomic_t g_stop = 0;
static uint64_t fake_time(void);
#define mono_ms fake_time
#include "session.c"
#undef mono_ms
static uint64_t now = 1000000;
static uint64_t fake_time(void) { return now; }

static void check(uint8_t type, int departure)
{
    struct Server *s = calloc(1, sizeof(*s));
    assert(s != NULL);
    server_init(s);
    s->max_connections = 4;
    s->session_count = 1;
    s->sessions[0].session_type = type;
    s->sessions[0].duration_min = 10;
    s->session.phase = PHASE_SESSION;
    s->session.ts_valid = 1;
    s->session.green_fired = 1;
    s->session_overtime_s = 120;
    for (int i = 0; i < 4; i++) s->session.ts[i] = now - 60000;
    s->session.ts[4] = now;
    s->session.ts[5] = now + 120000;
    s->session.ts[6] = now + 135000;
    for (int i = 0; i < 2; i++) {
        s->cars[i].used = 1;
        s->cars[i].race.on_track = 1;
        s->cars[i].race.lap_count = 1;
    }
    session_tick(s);
    assert(s->session.overtime_hold && s->session.cars_in_overtime == 2);
    if (departure == 0) s->cars[1].used = 0;
    if (departure == 1) s->cars[1].race.in_pit = 1;
    if (departure == 2) s->cars[1].race.car_field |= 1;
    session_quali_drop_eligibility(s, 0);
    session_tick(s);
    assert(!s->session.overtime_hold);
    assert(s->session.cars_in_overtime == 0);
    free(s);
}
int main(void)
{
    for (int departure = 0; departure < 3; departure++) {
        check(0, departure);
        check(4, departure);
    }
    puts("PASS: overtime releases after eligible drivers finish, leave, pit or invalidate");
    return 0;
}
