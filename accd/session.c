/*
 * session.c -- session phase machine.
 */

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "msg.h"
#include "session.h"
#include "state.h"

static uint64_t
mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull +
	    (uint64_t)ts.tv_nsec / 1000000ull;
}

void
session_reset(struct Server *s, uint8_t session_index)
{
	int i;

	if (session_index >= s->session_count) {
		s->session.phase = PHASE_RESULTS;
		s->session.session_index = session_index;
		s->session.phase_started_ms = mono_ms();
		log_info("session: no more sessions, entering RESULTS");
		return;
	}

	memset(&s->session, 0, sizeof(s->session));
	s->session.session_index = session_index;
	s->session.phase = PHASE_PRE_SESSION;
	s->session.phase_started_ms = mono_ms();
	s->session.standings_seq = 1;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct CarRaceState *r = &s->cars[i].race;

		memset(r, 0, sizeof(*r));
		r->best_lap_ms = 0;
		r->last_lap_ms = 0;
		r->position = (int16_t)(i + 1);
	}

	log_info("session %u: starting (type=%u)",
	    (unsigned)session_index,
	    (unsigned)s->sessions[session_index].session_type);
}

int
session_is_practice_or_qualy(const struct Server *s)
{
	uint8_t p = s->session.phase;

	return p == PHASE_PRACTICE || p == PHASE_QUALIFYING;
}

/*
 * Compare two cars for the current sort order.  Returns < 0 if
 * a should come before b, > 0 otherwise.
 */
static int
cmp_cars(const struct Server *s, const struct CarEntry *a,
    const struct CarEntry *b)
{
	const struct CarRaceState *ra = &a->race;
	const struct CarRaceState *rb = &b->race;

	if (!a->used)
		return 1;
	if (!b->used)
		return -1;

	if (session_is_practice_or_qualy(s)) {
		/* Sorted by best lap time ascending; 0 = no time
		 * yet, push to bottom. */
		int32_t la = ra->best_lap_ms;
		int32_t lb = rb->best_lap_ms;

		if (la == 0 && lb == 0)
			return 0;
		if (la == 0)
			return 1;
		if (lb == 0)
			return -1;
		return la < lb ? -1 : (la > lb ? 1 : 0);
	}
	/* Race: more laps first, then less race time. */
	if (ra->lap_count != rb->lap_count)
		return rb->lap_count - ra->lap_count;
	if (ra->race_time_ms != rb->race_time_ms)
		return ra->race_time_ms - rb->race_time_ms;
	return 0;
}

void
session_recompute_standings(struct Server *s)
{
	int order[ACC_MAX_CARS];
	int n = 0, i, j, changed = 0;

	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		if (s->cars[i].used)
			order[n++] = i;
	}
	/* Insertion sort: small N (<= 64) and we want stability. */
	for (i = 1; i < n; i++) {
		int key = order[i];
		j = i - 1;
		while (j >= 0 && cmp_cars(s, &s->cars[key],
		    &s->cars[order[j]]) < 0) {
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = key;
	}
	for (i = 0; i < n; i++) {
		int idx = order[i];

		if (s->cars[idx].race.position != (int16_t)(i + 1)) {
			s->cars[idx].race.position = (int16_t)(i + 1);
			changed = 1;
		}
	}
	if (changed)
		s->session.standings_seq++;
}

const char *
session_phase_name(uint8_t p)
{
	switch (p) {
	case PHASE_NONE:		return "NONE";
	case PHASE_PRE_SESSION:		return "PRE_SESSION";
	case PHASE_STARTING:		return "STARTING";
	case PHASE_PRACTICE:		return "PRACTICE";
	case PHASE_QUALIFYING:		return "QUALIFYING";
	case PHASE_PRE_RACE:		return "PRE_RACE";
	case PHASE_RACE:		return "RACE";
	case PHASE_POST_SESSION:	return "POST_SESSION";
	case PHASE_RESULTS:		return "RESULTS";
	default:			return "?";
	}
}

static void
enter_phase(struct Server *s, uint8_t new_phase)
{
	if (s->session.phase == new_phase)
		return;
	log_info("session %u: %s -> %s",
	    (unsigned)s->session.session_index,
	    session_phase_name(s->session.phase),
	    session_phase_name(new_phase));
	s->session.phase = new_phase;
	s->session.phase_started_ms = mono_ms();
}

/*
 * Map session_type byte (0=P, 4=Q, 10=R, see HB IX.6) to the
 * "active" phase.
 */
static uint8_t
type_to_active_phase(uint8_t t)
{
	switch (t) {
	case 0:		return PHASE_PRACTICE;
	case 4:		return PHASE_QUALIFYING;
	case 10:	return PHASE_RACE;
	default:	return PHASE_PRACTICE;
	}
}

void
session_tick(struct Server *s)
{
	uint64_t now;
	uint64_t elapsed;
	const struct SessionDef *def;

	if (s->session_count == 0)
		return;
	if (s->session.phase == PHASE_NONE) {
		session_reset(s, 0);
		return;
	}
	if (s->session.session_index >= s->session_count)
		return;

	def = &s->sessions[s->session.session_index];
	now = mono_ms();
	elapsed = now - s->session.phase_started_ms;

	switch (s->session.phase) {
	case PHASE_PRE_SESSION:
		/*
		 * Spend up to preRaceWaitingTimeSeconds (default 80)
		 * in PRE_SESSION before opening the active phase.
		 * For non-race sessions we go directly to active.
		 */
		if (elapsed >= 5000) {
			if (def->session_type == 10)
				enter_phase(s, PHASE_PRE_RACE);
			else
				enter_phase(s, type_to_active_phase(
				    def->session_type));
		}
		break;
	case PHASE_PRE_RACE:
		/*
		 * Race countdown: emit grid positions (0x3f) once,
		 * then transition to RACE.
		 */
		if (!s->session.grid_announced) {
			s->session.grid_announced = 1;
			/* Grid broadcast happens from tick.c. */
			log_info("session: race countdown -- grid positions"
			    " ready");
		}
		if (elapsed >= 10000)
			enter_phase(s, PHASE_RACE);
		break;
	case PHASE_PRACTICE:
	case PHASE_QUALIFYING:
	case PHASE_RACE:
		s->session.weekend_time_s = (uint32_t)(elapsed / 1000);
		if (def->duration_min > 0 &&
		    elapsed >= (uint64_t)def->duration_min * 60ull * 1000ull) {
			enter_phase(s, PHASE_POST_SESSION);
		}
		break;
	case PHASE_POST_SESSION:
		if (elapsed >= 5000)
			session_advance(s);
		break;
	case PHASE_RESULTS:
		/* Terminal: nothing more to do. */
		break;
	default:
		break;
	}
}

void
session_advance(struct Server *s)
{
	uint8_t next = (uint8_t)(s->session.session_index + 1);

	if (next >= s->session_count) {
		log_info("session: weekend complete, entering RESULTS");
		s->session.phase = PHASE_RESULTS;
		s->session.phase_started_ms = mono_ms();
		return;
	}
	session_reset(s, next);
}
