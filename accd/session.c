/*
 * Copyright (c) 2025-2026 Renaud Allard
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
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
#include "results.h"
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

	{
		uint8_t at = s->session.ambient_temp;
		uint8_t tt = s->session.track_temp;

		memset(&s->session, 0, sizeof(s->session));
		s->session.ambient_temp = at;
		s->session.track_temp = tt;
	}
	s->session.session_index = session_index;
	s->session.phase = PHASE_NONE;
	s->session.phase_started_ms = mono_ms();
	s->session.standings_seq = 1;
	s->session.weekend_time_s =
	    (uint32_t)s->sessions[session_index].hour_of_day * 3600u;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct CarRaceState *r = &s->cars[i].race;

		memset(r, 0, sizeof(*r));
		r->best_lap_ms = 0;
		r->last_lap_ms = 0;
		r->position = (int16_t)(i + 1);
	}

{
		const char *sname = "PRACTICE";
		uint8_t st = s->sessions[session_index].session_type;
		if (st == 4) sname = "QUALIFYING";
		else if (st == 10) sname = "RACE";
		log_info("session %u: waiting for drivers (%s)",
		    (unsigned)session_index, sname);
	}
}

int
session_is_practice_or_qualy(const struct Server *s)
{
	uint8_t sidx = s->session.session_index;
	uint8_t st;

	if (sidx >= s->session_count)
		return 0;
	st = s->sessions[sidx].session_type;
	return st == 0 || st == 4;	/* P=0, Q=4 */
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
		if (s->nconns > 0)
			enter_phase(s, PHASE_PRE_SESSION);
		return;
	}
	if (s->session.session_index >= s->session_count)
		return;

	/* Reset to waiting when the last driver disconnects. */
	if (s->nconns == 0 && s->session.phase != PHASE_POST_SESSION &&
	    s->session.phase != PHASE_RESULTS) {
		log_info("no drivers, resetting session");
		session_reset(s, s->session.session_index);
		return;
	}

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
		s->session.weekend_time_s =
		    (uint32_t)(def->hour_of_day * 3600 +
		    elapsed / 1000 * def->time_multiplier);
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

	/* Write results for the current session before advancing. */
	if (!s->session.results_written) {
		(void)results_write(s);
		s->session.results_written = 1;
	}

	if (next >= s->session_count) {
		log_info("session: weekend complete, entering RESULTS");
		s->session.phase = PHASE_RESULTS;
		s->session.phase_started_ms = mono_ms();
		return;
	}
	session_reset(s, next);
}
