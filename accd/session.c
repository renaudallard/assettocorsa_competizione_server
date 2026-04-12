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
 *
 * Implements the Kunos 7-level phase model from FUN_14012e810
 * (computeCurrentPhase) and FUN_14012e970 (startSession) in
 * accServer.exe.  Phase transitions are purely time-driven via
 * 6 scheduled timestamps populated when the first driver connects.
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
		s->session.ts_valid = 0;
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
	s->session.phase = PHASE_WAITING;
	s->session.phase_started_ms = mono_ms();
	s->session.standings_seq = 1;
	s->session.ts_valid = 0;
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

/*
 * Populate the 6 schedule timestamps when the first driver
 * connects.  Matches FUN_14012e970 (startSession) in the exe:
 *   ts[0] = now - 1
 *   ts[1] = ts[0] + preSessionMs  (3000 non-race, config for race)
 *   ts[2] = ts[1]                 (non-race; race adds formation)
 *   ts[3] = ts[2] + durationMs
 *   ts[4] = ts[3] + overtimeMs    (120000 default)
 *   ts[5] = ts[4] + postSessionMs (5000 default; configurable)
 */
static void
session_start(struct Server *s)
{
	const struct SessionDef *def =
	    &s->sessions[s->session.session_index];
	uint64_t now = mono_ms();
	uint64_t pre_ms = def->session_type == 10 ? 10000 : 3000;
	uint64_t dur_ms = (uint64_t)def->duration_min * 60000ull;
	uint64_t ot_ms  = 120000;
	uint64_t post_ms = 5000;

	s->session.ts[0] = now - 1;
	s->session.ts[1] = s->session.ts[0] + pre_ms;
	s->session.ts[2] = s->session.ts[1];
	s->session.ts[3] = s->session.ts[2] + dur_ms;
	s->session.ts[4] = s->session.ts[3] + ot_ms;
	s->session.ts[5] = s->session.ts[4] + post_ms;
	s->session.ts_valid = 1;

	s->session.phase_started_ms = now;
	log_info("session_start: scheduled slots "
	    "pre=%llums dur=%llums ot=%llums post=%llums",
	    (unsigned long long)pre_ms, (unsigned long long)dur_ms,
	    (unsigned long long)ot_ms, (unsigned long long)post_ms);
}

/*
 * Pure function: compute phase from server_now and the 6
 * schedule timestamps.  Matches FUN_14012e810.
 */
static uint8_t
compute_phase(const struct SessionState *ss, uint64_t now)
{
	if (!ss->ts_valid)
		return PHASE_WAITING;
	if (now < ss->ts[0])
		return PHASE_WAITING;
	if (now < ss->ts[1])
		return PHASE_FORMATION;
	if (now < ss->ts[2])
		return PHASE_PRE_SESSION;
	if (now < ss->ts[3])
		return PHASE_SESSION;
	if (now < ss->ts[4])
		return PHASE_OVERTIME;
	if (now < ss->ts[5])
		return PHASE_COMPLETED;
	return PHASE_ADVANCE;
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
	case PHASE_WAITING:	return "WAITING";
	case PHASE_FORMATION:	return "FORMATION";
	case PHASE_PRE_SESSION:	return "PRE_SESSION";
	case PHASE_SESSION:	return "SESSION";
	case PHASE_OVERTIME:	return "OVERTIME";
	case PHASE_COMPLETED:	return "COMPLETED";
	case PHASE_ADVANCE:	return "ADVANCE";
	case PHASE_RESULTS:	return "RESULTS";
	default:		return "?";
	}
}

/*
 * Map our internal phase enum (1-8) to the ACC Broadcasting SDK
 * SessionPhase enum used on the wire and expected by clients:
 *
 *   SDK 0 = NONE           (our WAITING)
 *   SDK 1 = Starting
 *   SDK 2 = PreFormation   (our FORMATION)
 *   SDK 3 = FormationLap
 *   SDK 4 = PreSession     (our PRE_SESSION)
 *   SDK 5 = Session        (our SESSION and OVERTIME)
 *   SDK 6 = SessionOver    (our COMPLETED)
 *   SDK 7 = PostSession    (our ADVANCE)
 *   SDK 8 = ResultUI       (our RESULTS)
 */
uint8_t
session_phase_to_wire(uint8_t p)
{
	switch (p) {
	case PHASE_WAITING:	return 0;
	case PHASE_FORMATION:	return 2;
	case PHASE_PRE_SESSION:	return 4;
	case PHASE_SESSION:	return 5;
	case PHASE_OVERTIME:	return 5;
	case PHASE_COMPLETED:	return 6;
	case PHASE_ADVANCE:	return 7;
	case PHASE_RESULTS:	return 8;
	default:		return 0;
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

void
session_tick(struct Server *s)
{
	uint64_t now;
	uint8_t new_phase;
	const struct SessionDef *def;

	if (s->session_count == 0)
		return;
	if (s->session.phase == PHASE_RESULTS)
		return;
	if (s->session.session_index >= s->session_count)
		return;

	def = &s->sessions[s->session.session_index];
	now = mono_ms();

	/* Start the session clock when the first driver connects. */
	if (s->session.phase == PHASE_WAITING && s->nconns > 0) {
		session_start(s);
	}

	/* Reset to waiting when everyone disconnects mid-session.
	 * Only log once (WAITING check prevents re-logging each tick). */
	if (s->nconns == 0 && s->session.phase != PHASE_COMPLETED &&
	    s->session.phase != PHASE_RESULTS &&
	    s->session.phase != PHASE_WAITING) {
		log_info("no drivers, resetting session");
		session_reset(s, s->session.session_index);
		return;
	}

	/* Compute the new phase from schedule slots. */
	new_phase = compute_phase(&s->session, now);
	enter_phase(s, new_phase);

	/* Drive the in-game clock during the active session. */
	if (s->session.phase == PHASE_SESSION ||
	    s->session.phase == PHASE_OVERTIME) {
		uint64_t active_start = s->session.ts[2];
		uint64_t elapsed = now > active_start
		    ? now - active_start : 0;
		s->session.weekend_time_s =
		    (uint32_t)(def->hour_of_day * 3600 +
		    elapsed / 1000 * def->time_multiplier);
	}

	/* Phase 7 (ADVANCE) triggers session advance. */
	if (s->session.phase == PHASE_ADVANCE)
		session_advance(s);
}

void
session_advance(struct Server *s)
{
	uint8_t next = (uint8_t)(s->session.session_index + 1);

	if (!s->session.results_written) {
		(void)results_write(s);
		s->session.results_written = 1;
	}

	if (next >= s->session_count) {
		log_info("session: weekend complete, entering RESULTS");
		s->session.phase = PHASE_RESULTS;
		s->session.phase_started_ms = mono_ms();
		s->session.ts_valid = 0;
		return;
	}
	session_reset(s, next);
}
