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

#include "bcast.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
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
	uint64_t pre_ms = def->session_type == 10
	    ? (uint64_t)s->pre_race_waiting_s * 1000ull : 3000;
	uint64_t dur_ms = (uint64_t)def->duration_min * 60000ull;
	uint64_t ot_ms  = (uint64_t)s->session_overtime_s * 1000ull;
	uint64_t post_ms = 5000;

	/*
	 * 7 schedule boundaries matching the exe's sub-objects
	 * at +0x70..+0x1c0.  For non-race (P/Q), ts[1]=ts[2]=ts[3]
	 * (no formation lap).  Session end is at ts[4], overtime
	 * at ts[5].  Verified against Kunos wire capture.
	 */
	s->session.ts[0] = now - 1;
	s->session.ts[1] = s->session.ts[0] + pre_ms;
	s->session.ts[2] = s->session.ts[1];
	s->session.ts[3] = s->session.ts[2];	/* race: + formation_ms */
	s->session.ts[4] = s->session.ts[3] + dur_ms;
	s->session.ts[5] = s->session.ts[4] + ot_ms;
	s->session.ts[6] = s->session.ts[5] + post_ms;
	s->session.ts_valid = 1;

	s->session_start_ms = now;
	s->session.phase_started_ms = now;
	log_info("session_start: scheduled slots "
	    "pre=%llums dur=%llums ot=%llums post=%llums",
	    (unsigned long long)pre_ms, (unsigned long long)dur_ms,
	    (unsigned long long)ot_ms, (unsigned long long)post_ms);
}

/*
 * Pure function: compute phase from server_now and the 7
 * schedule timestamps.  Matches FUN_14012e810.
 *
 * ts[0]: lobby start
 * ts[1]: pre-session end
 * ts[2]: formation end (= ts[1] non-race)
 * ts[3]: formation lap end (= ts[2] non-race)
 * ts[4]: session end (active duration)
 * ts[5]: overtime end
 * ts[6]: aftercare end
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
		return PHASE_PRE_SESSION;	/* race formation */
	if (now < ss->ts[4])
		return PHASE_SESSION;
	if (now < ss->ts[5])
		return PHASE_OVERTIME;
	/* Exe's flag_override_stop_at_5: hold overtime until all
	 * cars have finished their lap or the hold is released. */
	if (ss->overtime_hold)
		return PHASE_OVERTIME;
	if (now < ss->ts[6])
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

	/* Activate overtime hold for race sessions. */
	if (new_phase == PHASE_OVERTIME &&
	    s->session.overtime_hold == 0 &&
	    def->session_type == 10) {
		int i, n = 0;
		for (i = 0; i < ACC_MAX_CARS && i < s->max_connections;
		    i++)
			if (s->cars[i].used)
				n++;
		s->session.overtime_hold = 1;
		s->session.cars_in_overtime = (int16_t)n;
		log_info("overtime: hold active, %d cars racing", n);
	}

	/* Drive the in-game clock during the active session. */
	if (s->session.phase == PHASE_SESSION ||
	    s->session.phase == PHASE_OVERTIME) {
		uint64_t active_start = s->session.ts[3];
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

/*
 * Called from the lap completion handler when a car finishes
 * a lap during overtime.  Decrements the cars-in-overtime
 * counter; when it reaches 0, releases the overtime hold
 * and adjusts ts[5] to let the phase advance.
 */
void
session_overtime_car_finished(struct Server *s)
{
	if (!s->session.overtime_hold)
		return;
	if (s->session.cars_in_overtime > 0)
		s->session.cars_in_overtime--;
	if (s->session.cars_in_overtime <= 0) {
		s->session.overtime_hold = 0;
		s->session.ts[5] = mono_ms() + 5000;
		s->session.ts[6] = s->session.ts[5] + 5000;
		log_info("overtime: all cars finished, releasing hold");
	} else {
		log_info("overtime: %d cars still racing",
		    (int)s->session.cars_in_overtime);
	}
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
		/*
		 * Weekend complete: send 0x40 reset + loop back to
		 * session 0, matching the exe's resetRaceWeekend path.
		 * Capture shows 0x40 sent twice, then session resets.
		 */
		{
			struct ByteBuf bb;
			int k;

			bb_init(&bb);
			if (wr_u8(&bb, SRV_RACE_WEEKEND_RESET) == 0) {
				/* 12 x f32 weather scaling factors. */
				for (k = 0; k < 12; k++)
					(void)wr_f32(&bb, 0.0f);
				/* Two variable-length forecast vectors
				 * (count + samples).  Empty for reset. */
				(void)wr_u16(&bb, 0);
				(void)wr_u16(&bb, 0);
				(void)bcast_all(s, bb.data, bb.wpos,
				    0xFFFF);
				/* Exe sends it twice. */
				(void)bcast_all(s, bb.data, bb.wpos,
				    0xFFFF);
			}
			bb_free(&bb);
		}
		log_info("session: weekend complete, resetting to "
		    "session 0");
		session_reset(s, 0);
		return;
	}
	session_reset(s, next);
}
