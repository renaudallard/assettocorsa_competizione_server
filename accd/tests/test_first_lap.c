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

static void check(uint8_t type)
{
    struct Server *s = calloc(1, sizeof(*s));
    assert(s != NULL);
    server_init(s);
    s->max_connections = 4;
    s->session_count = 1;
    s->sessions[0].session_type = type;
    s->sessions[0].duration_min = 10;
    s->session.session_index = 0;
    s->session.phase = PHASE_SESSION;
    s->session.ts_valid = 1;
    s->session.green_fired = 1;
    s->session_overtime_s = 120;
    for (int i = 0; i < 4; i++) s->session.ts[i] = now - 60000;
    s->session.ts[4] = now;
    s->session.ts[5] = now + 120000;
    s->session.ts[6] = now + 135000;
    s->cars[0].used = 1;
    s->cars[0].race.on_track = 1;
    s->cars[0].race.lap_count = 0;
    session_tick(s);
    assert(s->session.phase == PHASE_OVERTIME);
    assert(s->session.overtime_hold);
    assert(s->cars[0].race.quali_eligible_to_finish);
    assert(s->session.cars_in_overtime == 1);
    session_quali_drop_eligibility(s, 0);
    assert(!s->session.overtime_hold);
    free(s);
}
int main(void)
{
    check(0);
    check(4);
    puts("PASS: practice and qualifying retain a first flying lap at 0:00");
    return 0;
}
