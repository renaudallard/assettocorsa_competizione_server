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
#include "handshake.h"
#include "log.h"
#include "msg.h"
#include "penalty.h"
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
void
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
	 * Race formation lap: the exe spans ts[2] → ts[3] with the
	 * formation-lap duration.  The exact value isn't in event.json
	 * — Kunos uses an internal constant; ~60 s matches most ACC
	 * circuits at rolling-start pace.  For P/Q we leave the span
	 * at zero since those sessions don't do a formation lap.
	 */
	uint64_t formation_ms = def->session_type == 10 ? 60000ull : 0;

	/*
	 * 7 schedule boundaries matching the exe's sub-objects
	 * at +0x70..+0x1c0.  For non-race (P/Q), ts[1]=ts[2]=ts[3]
	 * (no formation lap).  Session end is at ts[4], overtime
	 * at ts[5].  Verified against Kunos wire capture.
	 */
	s->session.ts[0] = now - 1;
	s->session.ts[1] = s->session.ts[0] + pre_ms;
	s->session.ts[2] = s->session.ts[1];
	s->session.ts[3] = s->session.ts[2] + formation_ms;
	s->session.ts[4] = s->session.ts[3] + dur_ms;
	if (def->session_type == 10) {
		/*
		 * Race: FUN_14012e970 (session_manager_advance) sets the
		 * SESSION→OVERTIME and OVERTIME→COMPLETED gate timestamps
		 * to -1.0 (never trigger by time).  Race ends via explicit
		 * triggers — last car completing its lap count, or the
		 * empty-overtime skip-grace path further down — not a
		 * clock gate.  Use UINT64_MAX as our equivalent sentinel;
		 * session_overtime_car_finished() / skip-grace set these
		 * to finite values when the race actually ends.
		 */
		s->session.ts[5] = UINT64_MAX;
		s->session.ts[6] = UINT64_MAX;
	} else {
		s->session.ts[5] = s->session.ts[4] + ot_ms;
		s->session.ts[6] = s->session.ts[5] + post_ms;
	}
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

	/*
	 * DQ'd cars always sort below non-DQ'd cars regardless of
	 * laps or time.  Among DQ'd cars, fall through to lap/time
	 * order so the relative ranking is stable.
	 */
	if (ra->disqualified != rb->disqualified)
		return ra->disqualified ? 1 : -1;

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
	lobby_notify_session_changed(&s->lobby);
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

	/*
	 * No drivers around: reset to the FIRST configured session
	 * (typically Practice) and go to WAITING.  This matches
	 * Kunos ("No drivers around, resetting session / Reset time
	 * to first session / sessionPhase <...> -> <waiting for
	 * drivers>").  Sends a 0xcb with phase=1 which keeps the
	 * public lobby listing as a joinable Practice server,
	 * instead of cycling through OVERTIME / COMPLETED where the
	 * server disappears from the public server list.
	 */
	if (s->nconns == 0 &&
	    (s->session.session_index != 0 ||
	     s->session.phase != PHASE_WAITING)) {
		log_info("no drivers, resetting to first session");
		session_reset(s, 0);
		lobby_notify_session_changed(&s->lobby);
		return;
	}

	/* Compute the new phase from schedule slots. */
	new_phase = compute_phase(&s->session, now);
	enter_phase(s, new_phase);

	/*
	 * Overtime entry check, applies to every session type: if
	 * nobody has a valid lap and is still out on track (not in
	 * pit/garage), waiting the overtime grace period serves no
	 * purpose.  Collapse ts[5] so compute_phase jumps straight to
	 * COMPLETED on the next tick.
	 *
	 * For race sessions with cars still on track, activate the
	 * overtime hold so the schedule freezes until everyone has
	 * crossed the finish line.
	 */
	if (new_phase == PHASE_OVERTIME &&
	    s->session.overtime_hold == 0) {
		int i, racing = 0;
		for (i = 0; i < ACC_MAX_CARS && i < s->max_connections;
		    i++) {
			if (!s->cars[i].used)
				continue;
			if (s->cars[i].race.lap_count > 0 &&
			    !s->cars[i].race.in_pit)
				racing++;
		}
		if (racing == 0) {
			log_info("overtime: no car racing on track, "
			    "skipping grace period");
			s->session.ts[5] = now;
			if (s->session.ts[6] <= now)
				s->session.ts[6] = now + 5000;
		} else if (def->session_type == 10) {
			s->session.overtime_hold = 1;
			s->session.cars_in_overtime = (int16_t)racing;
			log_info("overtime: hold active, %d cars racing",
			    racing);
		}
	}

	/*
	 * Release an active overtime hold when every on-track car
	 * has left (disconnect mid-overtime).  Without this the
	 * race session can be stuck in OVERTIME forever if the
	 * holding drivers vanish without completing another lap.
	 */
	if (s->session.overtime_hold) {
		int i, still_racing = 0;
		for (i = 0; i < ACC_MAX_CARS && i < s->max_connections;
		    i++) {
			if (!s->cars[i].used)
				continue;
			if (s->cars[i].race.lap_count > 0 &&
			    !s->cars[i].race.in_pit)
				still_racing++;
		}
		if (still_racing == 0) {
			log_info("overtime: all racing cars left, "
			    "releasing hold");
			s->session.overtime_hold = 0;
			s->session.cars_in_overtime = 0;
		}
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

	/*
	 * Time remaining in the active session, in ms.  Used by the
	 * lobby session-update message and the admin console.
	 * Computed as ts[4] (active end) minus now, clamped to 0.
	 */
	if (s->session.ts_valid && s->session.ts[4] > now)
		s->session.time_remaining_ms =
		    (int32_t)(s->session.ts[4] - now);
	else
		s->session.time_remaining_ms = 0;

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
		 * Weekend complete: loop back to session 0 silently.
		 * Kunos does NOT emit 0x40 on the automatic wrap (81-min
		 * replay saw 8 × 0x3e session-results and 0 × 0x40).
		 * 0x40 is reserved for the admin /resetWeekend command,
		 * not the natural end-of-sessions rollover.
		 */
		log_info("session: weekend complete, resetting to "
		    "session 0");
		session_reset(s, 0);
		if (s->nconns > 0)
			session_start(s);
		return;
	}
	session_reset(s, next);
	if (s->nconns > 0)
		session_start(s);
}

/*
 * Driver-stint tracker — matches FUN_14012ae10 (ExceededDriverStintLimit
 * path) on a per-car, per-driver basis.  Accumulates on-track time into
 * driver_stint_ms[current_driver_index] and enqueues DQ on any driver
 * whose total exceeds the configured `driverStintTime` at session end.
 *
 * The other two FUN_14012ae10 DQs (IgnoredDriverStint, DriverRanNoStint)
 * require extra config state (mandatory-stint count, minimum-stint) that
 * we don't yet parse from eventRules.json.
 */

void
stint_start_tracking(struct Server *s, int car_id)
{
	struct CarRaceState *r;

	if (car_id < 0 || car_id >= ACC_MAX_CARS)
		return;
	r = &s->cars[car_id].race;
	if (r->stint_start_ms != 0)
		return;	/* already tracking */
	r->stint_start_ms = mono_ms();
}

void
stint_stop_tracking(struct Server *s, int car_id)
{
	struct CarRaceState *r;
	struct CarEntry *car;
	uint64_t now, delta;
	uint8_t d;

	if (car_id < 0 || car_id >= ACC_MAX_CARS)
		return;
	car = &s->cars[car_id];
	r = &car->race;
	if (r->stint_start_ms == 0)
		return;	/* not tracking */
	now = mono_ms();
	delta = now - r->stint_start_ms;
	r->stint_start_ms = 0;

	d = car->current_driver_index;
	if (d < ACC_MAX_DRIVERS_PER_CAR) {
		int64_t total = (int64_t)r->driver_stint_ms[d] + (int64_t)delta;
		if (total > INT32_MAX)
			total = INT32_MAX;
		r->driver_stint_ms[d] = (int32_t)total;
	}
}

void
stint_check_violations(struct Server *s)
{
	int i;
	uint32_t limit_s;

	if (s->driver_stint_time_s == 0)
		return;	/* no limit configured — skip */
	limit_s = s->driver_stint_time_s;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct CarEntry *car = &s->cars[i];
		struct CarRaceState *r;
		int d;
		int violated = 0;

		if (!car->used)
			continue;
		r = &car->race;
		/* Flush any in-progress stint before checking. */
		stint_stop_tracking(s, i);

		for (d = 0; d < car->driver_count &&
		    d < ACC_MAX_DRIVERS_PER_CAR; d++) {
			uint32_t stint_s = (uint32_t)(r->driver_stint_ms[d]
			    / 1000);
			if (stint_s > limit_s) {
				log_info("Car %d driver %d stint %us > "
				    "limit %us -> DQ", i, d,
				    (unsigned)stint_s, (unsigned)limit_s);
				violated = 1;
				break;
			}
		}
		if (violated)
			(void)penalty_enqueue(s, i, EXE_DQ, 12, 3, 1, 0,
			    REASON_EXCEEDED_DRIVER_STINT_LIMIT);
	}
}
