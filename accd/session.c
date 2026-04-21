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
#include <stdio.h>
#include <stdlib.h>
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

/*
 * Formation / green-flag position gate range.  The exe reads three
 * floats via a virtual deserializer at vtable slot 0x140142b70
 * ("formationTriggerNormalizedRangeStart", "greenFlagTriggerNormalized
 * RangeStart", "greenFlagTriggerNormalizedRangeEnd") and falls back to
 * the compiled-in constants at DAT_14014bccc (0.80) / DAT_14014bcd0
 * (0.89) / DAT_14014bcd8 (0.96) when the JSON is absent.  No per-track
 * file path is used — the same defaults apply to every ACC circuit,
 * overridable via event.json keys of the same names.  The epsilon
 * constant comes from DAT_14014bcac = 0.05.
 */
#define FORMATION_PRE_GREEN_EPS	0.05f

static int
wrapped_range_contains(float pos, float start, float end)
{
	/* FUN_1401342d0: test whether pos is inside a [start, end]
	 * segment on the 0..1 normalized track loop, handling the
	 * start/finish line wrap (start > end means the range crosses
	 * position 0). */
	if (start <= end)
		return pos >= start && pos <= end;
	return pos >= start || pos <= end;
}

static float
randomize_green_trigger(const struct Server *s)
{
	/* FUN_14012ee60: pick a random point inside
	 * [green_trigger_start, green_trigger_end] with wrap handling. */
	float start = s->green_trigger_start;
	float end = s->green_trigger_end;
	float span;
	float p;

	if (start <= end) {
		span = end - start;
		p = (float)rand() / (float)RAND_MAX;
		return start + p * span;
	}
	span = (end + 1.0f) - start;
	p = (float)rand() / (float)RAND_MAX;
	p = start + p * span;
	if (p >= 1.0f)
		p -= 1.0f;
	return p;
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

	/*
	 * A reset back to session 0 means the whole weekend is starting
	 * over (either operator /track or the empty-server auto-reset in
	 * session_tick).  Drop every car's race_archive so a later 0x56
	 * garage request doesn't serve laps from the previous weekend.
	 */
	if (session_index == 0)
		session_archive_clear(s);

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
		r->grid_position = -1;
	}

	/*
	 * Race grid assignment (FUN_140032400 equivalent).  The exe
	 * builds the grid from the most recent qualifying session's
	 * finishing order.  entrylist defaultGridPosition is a fallback
	 * that Kunos only honors when no qualifying session precedes
	 * the race (mixing the two logs a warning).
	 */
	if (s->sessions[session_index].session_type == 10) {
		int k, prior = -1;

		for (k = (int)session_index - 1; k >= 0; k--) {
			if (s->sessions[k].session_type == 4) {
				prior = k;
				break;
			}
		}
		if (prior < 0) {
			for (k = (int)session_index - 1; k >= 0; k--) {
				if (s->sessions[k].session_type == 0) {
					prior = k;
					break;
				}
			}
		}
		for (i = 0; i < ACC_MAX_CARS; i++) {
			struct CarEntry *car = &s->cars[i];
			int16_t g = -1;

			/*
			 * Assign grid to every slot that has an identity
			 * (driver_count > 0), not just currently-connected
			 * ones.  A driver who disconnected during qualy
			 * and reconnects after the race has started still
			 * gets their rightful grid position when the
			 * zombie-slot reclaim in handshake_handle re-binds
			 * them to this slot.  Unreclaimed zombies stay
			 * invisible because broadcast_grid iterates only
			 * `used` cars.
			 */
			if (car->driver_count == 0)
				continue;
			if (prior >= 0 && car->race_archive[prior] != NULL) {
				int16_t p = car->race_archive[prior]->position;
				if (p >= 1 && p <= ACC_MAX_CARS)
					g = p - 1;	/* 0-based slot */
			}
			if (g < 0 && prior < 0 &&
			    car->default_grid_position > 0)
				g = (int16_t)(car->default_grid_position - 1);
			if (g < 0) {
				int slot = server_find_grid_slot(s);
				if (slot >= 0)
					g = (int16_t)slot;
			}
			car->race.grid_position = g;
			log_info("grid: car %d -> %d (from session %d%s)",
			    i, (int)g, prior,
			    car->used ? "" : ", zombie");
		}
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
	uint64_t post_ms = def->session_type == 10
	    ? (uint64_t)s->post_race_s * 1000ull
	    : (uint64_t)s->post_qualy_s * 1000ull;

	/*
	 * 7 schedule boundaries matching the exe's sub-objects
	 * at +0x70..+0x1c0.  For non-race (P/Q), ts[1]=ts[2]=ts[3]
	 * (no formation lap).  For race, ts[3]..ts[6] are held at
	 * UINT64_MAX and only set when the leader's normalized track
	 * position triggers the green flag (FUN_14012f4a0).  No time
	 * fallback — matches exe exactly; session_overtime_car_finished
	 * and skip-grace collapse ts[5]/ts[6] when the race ends.
	 */
	s->session.ts[0] = now - 1;
	s->session.ts[1] = s->session.ts[0] + pre_ms;
	s->session.ts[2] = s->session.ts[1];
	if (def->session_type == 10) {
		/*
		 * Race uses position-triggered stamps for the formation-
		 * cross and green-cross boundaries, matching the exe's
		 * FUN_14012f300.  Hold ts[2]..ts[6] at UINT64_MAX until
		 * those triggers fire so the client's phase compute
		 * holds at phase 3 (formation lap running) through phase
		 * 4 (grid countdown / doubleFile) the same way Kunos's
		 * clients do.
		 */
		s->session.ts[2] = UINT64_MAX;
		s->session.ts[3] = UINT64_MAX;
		s->session.ts[4] = UINT64_MAX;
		s->session.ts[5] = UINT64_MAX;
		s->session.ts[6] = UINT64_MAX;
		s->session.formation_ended = 0;
		s->session.green_fired = 0;
		s->session.green_trigger = randomize_green_trigger(s);
		log_info("session_start: race green trigger rolled at "
		    "pos=%.3f (range [%.3f, %.3f])",
		    (double)s->session.green_trigger,
		    (double)s->green_trigger_start,
		    (double)s->green_trigger_end);
	} else {
		s->session.ts[3] = s->session.ts[2];
		s->session.ts[4] = s->session.ts[3] + dur_ms;
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

	/*
	 * Open a per-session latency-dump CSV if writeLatencyFileDumps=1.
	 * Closed by server_free or the next session_start (rotating).
	 * One row per authenticated conn per keepalive tick is appended
	 * from tick_run; see the consumer comment there.
	 */
	if (s->latency_dump_fp != NULL) {
		fclose((FILE *)s->latency_dump_fp);
		s->latency_dump_fp = NULL;
	}
	if (s->write_latency_dumps) {
		char path[384];
		time_t t = time(NULL);
		struct tm tm;
		const char *stype = def->session_type == 4 ? "Q"
		    : def->session_type == 10 ? "R" : "P";

		localtime_r(&t, &tm);
		snprintf(path, sizeof(path),
		    "results/latency_%04d%02d%02d_%02d%02d%02d_%s.csv",
		    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		    tm.tm_hour, tm.tm_min, tm.tm_sec, stype);
		s->latency_dump_fp = fopen(path, "w");
		if (s->latency_dump_fp != NULL) {
			fprintf((FILE *)s->latency_dump_fp,
			    "mono_ms,conn_id,steam_id,avg_rtt_ms,"
			    "clock_offset_ms\n");
			fflush((FILE *)s->latency_dump_fp);
			log_info("latency dump: writing to %s", path);
		} else {
			log_warn("latency dump: fopen %s failed", path);
		}
	}
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
session_advance_race_triggers(struct Server *s, float leader_pos)
{
	struct SessionState *ss = &s->session;
	const struct SessionDef *def;
	uint64_t now, dur_ms;
	float green_end;

	if (!ss->ts_valid || ss->session_index >= s->session_count)
		return 0;
	def = &s->sessions[ss->session_index];
	if (def->session_type != 10)
		return 0;
	if (ss->green_fired)
		return 0;

	now = mono_ms();
	if (now < ss->ts[1])
		return 0;	/* still in pre-race waiting countdown */

	if (!ss->formation_ended) {
		float pre_green = s->green_trigger_start -
		    FORMATION_PRE_GREEN_EPS;

		if (pre_green < 0.0f)
			pre_green += 1.0f;
		if (wrapped_range_contains(leader_pos,
		    s->formation_trigger_start, pre_green)) {
			ss->formation_ended = 1;
			/*
			 * FUN_14012f300 stamps +0x178 = now + 1000ms on
			 * formation crossing; the exe's phase-compute
			 * advances to phase 4 (doubleFile / grid countdown)
			 * only once that deadline passes.  Mirror it on
			 * ts[2] so the client runs through phase 3 → 4
			 * instead of jumping straight past formation.
			 */
			ss->ts[2] = now + 1000;
			log_info("formation end: leader norm_pos=%.3f "
			    "range=[%.3f, %.3f] doubleFile_at=%llums",
			    (double)leader_pos,
			    (double)s->formation_trigger_start,
			    (double)pre_green,
			    (unsigned long long)ss->ts[2]);
		}
		return 0;
	}

	/*
	 * formationLapType dictates which green-flag variant the exe
	 * runs (server_tick_tail at FUN_14002f710 line 290):
	 *   3 / 5 -> FUN_14012f300 silent path: fire when leader
	 *            crosses the RANDOMISED trigger point (+0x294 rolled
	 *            at session_start), never broadcast.  Default for
	 *            public servers.
	 *   else  -> FUN_14012f4a0 verbose path: fire anywhere in the
	 *            static [green_start, green_end] range; broadcast
	 *            "Race start initialized" on fire.  Used on private
	 *            servers with manual formation (type 1).
	 */
	{
		int silent = (s->formation_lap_type == 3 ||
		    s->formation_lap_type == 5);
		float lo, hi;

		if (silent) {
			/*
			 * FUN_14012f300 fires when the leader is in
			 * [green_trigger, green_end + 0.2 * |green_end -
			 * green_start|] (with wrap).  With defaults
			 * 0.89..0.96 that's a 0.037-wide window past the
			 * rolled point.  Our prior 0.02 single-point
			 * window was narrower than the exe's and often
			 * missed the leader on the first lap when the
			 * position update fell outside the 2 %
			 * slot — green wouldn't fire until the next full
			 * lap.
			 */
			float span = s->green_trigger_end -
			    s->green_trigger_start;
			if (span < 0.0f)
				span = -span;
			lo = ss->green_trigger;
			hi = s->green_trigger_end + 0.2f * span;
			if (hi >= 1.0f)
				hi -= 1.0f;
			if (!wrapped_range_contains(leader_pos, lo, hi))
				return 0;
		} else {
			lo = s->green_trigger_start;
			hi = s->green_trigger_end;
			if (!wrapped_range_contains(leader_pos, lo, hi))
				return 0;
		}

		green_end = silent ? ss->green_trigger : hi;
		ss->green_fired = 1;
		dur_ms = (uint64_t)def->duration_min * 60000ull;
		/*
		 * FUN_14012f300 stamps the green_fire_time deadline at
		 * +0x1b0 = now + 1000ms, and the phase-compute
		 * FUN_14012e810 only advances to race once (flag &&
		 * now >= deadline) both hold.  That 1-second grace lets
		 * the 0x28 phase-broadcast reach every client before the
		 * session phase flips, so the visible green light lands
		 * at the same track position the real server produces.
		 * Skipping the delay made our green fire ~1% of a lap
		 * earlier (~50 m at racing speed) than Kunos.
		 */
		ss->ts[3] = now + 1000;
		ss->ts[4] = ss->ts[3] + dur_ms;
		ss->ts[5] = ss->ts[4] +
		    (uint64_t)s->session_overtime_s * 1000ull;
		log_info("green flag (%s): leader norm_pos=%.3f trigger=%.3f "
		    "active_dur=%llums fire_in=1000ms",
		    silent ? "silent" : "verbose",
		    (double)leader_pos, (double)green_end,
		    (unsigned long long)dur_ms);
		return silent ? 0 : 1;
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
	/*
	 * Race tiebreak: before any lap is complete (and between sector
	 * splits), lap_count and race_time_ms match for every car.  Fall
	 * back to grid_position so the pole sitter leads the formation
	 * lap and the green-flag trigger picks position==1 correctly.
	 */
	if (ra->grid_position != rb->grid_position) {
		int16_t pa = ra->grid_position >= 0
		    ? ra->grid_position : INT16_MAX;
		int16_t pb = rb->grid_position >= 0
		    ? rb->grid_position : INT16_MAX;
		return pa < pb ? -1 : (pa > pb ? 1 : 0);
	}
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
 * Driver-stint tracker — matches FUN_14012ae10 on a per-car,
 * per-driver basis.  Accumulates on-track time into
 * driver_stint_ms[current_driver_index] and enqueues a DQ at
 * session end when any of the FUN_14012ae10 conditions fires:
 *   ExceededDriverStintLimit  (some driver's total > driverStintTime)
 *   DriverRanNoStint          (a registered driver has 0 ms)
 * The IgnoredDriverStint DT fires from h_mandatory_pitstop_served
 * (handlers.c) when isMandatoryPitstopSwapDriverRequired=1 and the
 * pit-entry driver is still active at pit-served time.  The exe's
 * full DT → SG30 → DQ escalation on repeated misses is not yet
 * implemented; we emit the single DT on each miss.
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
	int is_race =
	    s->session.session_index < s->session_count &&
	    s->sessions[s->session.session_index].session_type == 10;

	if (s->driver_stint_time_s == 0 && s->mandatory_pit_count == 0)
		return;	/* no enforcement configured */

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct CarEntry *car = &s->cars[i];
		struct CarRaceState *r;
		int d;

		/*
		 * Enforce on any slot with an identity + lap data so a
		 * driver who committed a violation and then disconnected
		 * still shows as DQ'd in the results file.  conn_drop
		 * flushes stint_start_ms on disconnect, so the pending-
		 * stint accumulation is accurate.
		 */
		if (car->driver_count == 0 || car->race.lap_count == 0)
			continue;
		r = &car->race;
		/* Flush any in-progress stint before checking. */
		stint_stop_tracking(s, i);
		if (r->disqualified)
			continue;	/* already DQ'd, skip */

		/*
		 * Per-driver stint-time violation (FUN_14012ae10 third
		 * DQ branch, ExceededDriverStintLimit).  Only runs when
		 * eventRules.driverStintTime is set.
		 */
		if (s->driver_stint_time_s != 0) {
			int violated = 0;
			uint32_t limit_s = s->driver_stint_time_s;

			for (d = 0; d < car->driver_count &&
			    d < ACC_MAX_DRIVERS_PER_CAR; d++) {
				uint32_t stint_s = (uint32_t)(
				    r->driver_stint_ms[d] / 1000);
				if (stint_s > limit_s) {
					log_info("Car %d driver %d stint "
					    "%us > limit %us -> DQ", i, d,
					    (unsigned)stint_s,
					    (unsigned)limit_s);
					violated = 1;
					break;
				}
			}
			if (violated) {
				(void)penalty_enqueue(s, i, EXE_DQ, 27, 3,
				    1, 0,
				    REASON_EXCEEDED_DRIVER_STINT_LIMIT);
				continue;	/* already DQ'd */
			}
		}

		/*
		 * Mandatory-pitstop violation at race end.  Only runs
		 * in race sessions where the car actually raced.  The
		 * exe uses Disqualified_IgnoredMandatoryPit (Server
		 * MonitorPenaltyShortcut 13, our REASON_IGNORED_
		 * MANDATORY_PIT).
		 */
		if (is_race && s->mandatory_pit_count > 0 &&
		    r->lap_count > 0 &&
		    r->mandatory_pit_served < s->mandatory_pit_count) {
			log_info("Car %d ignored mandatory pit (need %u, "
			    "served %u) -> DQ", i,
			    (unsigned)s->mandatory_pit_count,
			    (unsigned)r->mandatory_pit_served);
			(void)penalty_enqueue(s, i, EXE_DQ, 13, 3, 1, 0,
			    REASON_IGNORED_MANDATORY_PIT);
			if (r->disqualified)
				continue;
		}

		/*
		 * Driver-ran-no-stint violation (endurance races): if
		 * the car has multiple registered drivers (driver_count
		 * > 1) and at least one never took a turn on track,
		 * DQ with Disqualified_DriverRanNoStint (ServerMonitor
		 * PenaltyShortcut 28).  Single-driver entries are
		 * exempt — there's no implied swap obligation.
		 */
		if (is_race && car->driver_count > 1 && r->lap_count > 0) {
			int d, skipped = -1;
			for (d = 0; d < car->driver_count &&
			    d < ACC_MAX_DRIVERS_PER_CAR; d++) {
				if (r->driver_stint_ms[d] == 0) {
					skipped = d;
					break;
				}
			}
			if (skipped >= 0) {
				log_info("Car %d driver %d never took a "
				    "stint -> DQ", i, skipped);
				(void)penalty_enqueue(s, i, EXE_DQ, 28, 3, 1, 0,
				    REASON_DRIVER_RAN_NO_STINT);
			}
		}
	}
}

/*
 * Take a snapshot of every used car's CarRaceState and store it
 * in the corresponding race_archive[] slot so future 0x56
 * ACP_LOAD_SETUP requests can serve laps from this session after
 * we've moved on to the next one.  A stale slot (should not
 * normally happen but defensive) is freed first.
 */
void
session_archive_snapshot(struct Server *s)
{
	uint8_t idx = s->session.session_index;
	int j;

	if (idx >= ACC_MAX_SESSIONS)
		return;
	for (j = 0; j < ACC_MAX_CARS; j++) {
		struct CarEntry *car = &s->cars[j];
		struct CarRaceState *snap;

		if (!car->used)
			continue;
		if (car->race_archive[idx] != NULL) {
			free(car->race_archive[idx]);
			car->race_archive[idx] = NULL;
		}
		snap = malloc(sizeof(*snap));
		if (snap == NULL) {
			log_warn("session_archive_snapshot: oom for car %d "
			    "session %u", j, (unsigned)idx);
			continue;
		}
		*snap = car->race;
		car->race_archive[idx] = snap;
	}
	log_info("session_archive_snapshot: session %u archived",
	    (unsigned)idx);
}

void
session_archive_clear(struct Server *s)
{
	int j, k;

	for (j = 0; j < ACC_MAX_CARS; j++) {
		struct CarEntry *car = &s->cars[j];
		for (k = 0; k < ACC_MAX_SESSIONS; k++) {
			if (car->race_archive[k] != NULL) {
				free(car->race_archive[k]);
				car->race_archive[k] = NULL;
			}
		}
	}
}
