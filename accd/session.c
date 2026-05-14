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

#ifdef __linux__
#include <bsd/stdlib.h>
#endif
#if defined(__OpenBSD__) || defined(__APPLE__)
/* See lobby.c header comment: __BSD_VISIBLE is forced off by
 * _POSIX_C_SOURCE so <stdlib.h> hides arc4random_uniform even
 * though libc has it.  macOS has the same gating problem. */
uint32_t arc4random_uniform(uint32_t);
#endif

#include "bans.h"
#include "bcast.h"
#include "handshake.h"
#include "log.h"
#include "msg.h"
#include "penalty.h"
#include "prim.h"
#include "results.h"
#include "session.h"
#include "state.h"
#include "tick.h"

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
	/*
	 * FUN_1401342d0: test whether pos is inside a [start, end)
	 * half-open segment on the 0..1 normalized track loop,
	 * handling the start/finish line wrap (end <= start means
	 * the range crosses position 0).  Exe rotates one bound by
	 * +/-1.0 based on which side of 0.5 pos sits, then tests
	 * (start <= pos) && (pos < end) — half-open at the upper
	 * bound.  We had it inclusive on both ends.
	 */
	if (end <= start) {
		if (pos < 0.5f)
			start -= 1.0f;
		else
			end += 1.0f;
	}
	return start <= pos && pos < end;
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
		p = (float)arc4random_uniform(1u << 24) / (float)(1u << 24);
		return start + p * span;
	}
	span = (end + 1.0f) - start;
	p = (float)arc4random_uniform(1u << 24) / (float)(1u << 24);
	p = start + p * span;
	if (p >= 1.0f)
		p -= 1.0f;
	return p;
}

/*
 * Auto-derive shortFormationLap from the session list.  Matches
 * exe FUN_140029eb0 line 261-270 which sets ServerState+0x230 = 1
 * unless at least one race session has duration > 0x99c (= 2460);
 * with duration in seconds that's a 41-minute threshold, so any
 * race ≥ 41 minutes flips the flag to 0 (standard formation).
 *
 * The JSON "shortFormationLap" key in settings.json lands on a
 * different struct (SettingsConfig+0x110 per FUN_140106300 line
 * 619-621) and is NOT used for the formation_start transform —
 * the transform reads only the auto-derived byte.  We keep
 * s->short_formation_lap loaded for parity but ignore it here.
 */
static uint8_t
auto_short_formation(const struct Server *s)
{
	int i;
	for (i = 0; i < s->session_count; i++) {
		const struct SessionDef *def = &s->sessions[i];
		if (def->session_type == 10 && def->duration_min >= 41)
			return 0;	/* long race exists */
	}
	return 1;	/* short formation */
}

/*
 * FUN_14012f640 transform.  When formation_lap_type is not 3 or 5
 * (i.e. anything other than the silent path), the exe shifts the
 * formation_start trigger earlier in the lap before storing it in
 * SessionManager+0x288.  The shift depends on the auto-derived
 * short-formation byte:
 *
 *   shortFormation == 0 (long): start -= 0.16, wrap at 0
 *   shortFormation != 0 AND start > 0.7: start -= 0.5
 *
 * Without this, brands_hatch (formation_start = 0.7299) on a
 * non-silent formation type fires the formation-end gate at 73 %
 * of the lap on our server while the windows server fires it at
 * 22-57 % depending on whether a 41+ min race is in the schedule.
 *
 * Exe call site: FUN_14002f710 line 148-150:
 *   if ((formation_lap_type - 3) & 0xfd) != 0
 *       fVar43 = FUN_14012f640(fVar43, ServerState+0x230);
 */
static float
effective_formation_start(const struct Server *s)
{
	float v = s->formation_trigger_start;

	if (s->formation_lap_type == 3 || s->formation_lap_type == 5)
		return v;	/* silent path — no transform */
	if (auto_short_formation(s) == 0) {
		v -= 0.16f;
		if (v < 0.0f)
			v += 1.0f;
		return v;
	}
	if (v > 0.7f)
		return v - 0.5f;
	return v;
}

uint8_t
session_cur_type(const struct Server *s)
{
	if (s->session.session_index >= s->session_count)
		return 0xff;
	return s->sessions[s->session.session_index].session_type;
}

int
session_is_race(const struct Server *s)
{
	return session_cur_type(s) == 10;
}

int
session_is_qualy(const struct Server *s)
{
	return session_cur_type(s) == 4;
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
		uint8_t *lb = s->session.leaderboard_cache;
		size_t lb_cap = s->session.leaderboard_cache_cap;

		memset(&s->session, 0, sizeof(s->session));
		s->session.ambient_temp = at;
		s->session.track_temp = tt;
		/*
		 * Preserve the leaderboard cache allocation across the
		 * session reset; the next emit's deep-compare must see
		 * an empty cache_len so it fires the first 0x36 of the
		 * new session even when the payload bytes happen to
		 * match the prior session's last frame.
		 */
		s->session.leaderboard_cache = lb;
		s->session.leaderboard_cache_cap = lb_cap;
		s->session.leaderboard_cache_len = 0;
	}
	s->session.session_index = session_index;
	s->session.phase = PHASE_WAITING;
	s->session.phase_started_ms = mono_ms();
	s->session.ts_valid = 0;
	s->session.weekend_time_s =
	    (uint32_t)s->sessions[session_index].hour_of_day * 3600u;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct CarRaceState *r = &s->cars[i].race;
		/*
		 * Preserve on_track across the per-session memset.  It
		 * mirrors the most recent ACP_CAR_LOCATION_UPDATE
		 * (location == Track), which the client only re-sends
		 * when the location *changes* — so a driver who's
		 * already on the grid at session_start doesn't re-emit
		 * one, and zeroing on_track here would leave the new
		 * leader-pick gate (FUN_1400428d0 mirror in tick.c)
		 * permanently blocked.  The remaining race-state
		 * fields (laps, cuts, latches) all want the fresh
		 * zero, so memset stays.
		 */
		uint8_t saved_on_track = r->on_track;

		memset(r, 0, sizeof(*r));
		r->best_lap_ms = 0;
		r->last_lap_ms = 0;
		r->position = (int16_t)(i + 1);
		r->grid_position = -1;
		r->on_track = saved_on_track;
		car_runtime_reset_gate(&s->cars[i].rt);
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

/*
 * Configured post-session grace in milliseconds for the current
 * session.  Honors postRaceSeconds for race / postQualySeconds
 * for everything else; falls back to 5 s if unset.  Used wherever
 * we collapse ts[6] after the overtime hold releases — previously
 * a hardcoded 5 s ignored the operator's setting.
 */
static uint64_t
post_grace_ms(const struct Server *s)
{
	uint16_t cfg;

	if (session_is_race(s))
		cfg = s->post_race_s;
	else
		cfg = s->post_qualy_s;
	return cfg > 0 ? (uint64_t)cfg * 1000ull : 5000ull;
}

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
		log_info("session_start: formation_lap_type=%u "
		    "shortFormation auto=%u (json=%u) "
		    "formation_start raw=%.4f effective=%.4f",
		    (unsigned)s->formation_lap_type,
		    (unsigned)auto_short_formation(s),
		    (unsigned)s->short_formation_lap,
		    (double)s->formation_trigger_start,
		    (double)effective_formation_start(s));
		/*
		 * Formation-lap mid latch (exe car+0x204) bulk-set to
		 * match FUN_1400197b0 at session-transition time: its
		 * else branch fires for every `formation_lap_type != 0`
		 * (i.e. default, short, manual, verbose) and sets fmp=1
		 * for all cars before the race begins.  FUN_1400431e0's
		 * 0.6-0.7 latch only matters for cars that weren't
		 * present at the transition — mid-race joiners get the
		 * ctor default fmp=0 and must drive through the band
		 * before FUN_1400428d0's gate admits them.
		 *
		 * Previously we only bulk-set for formation_lap_type==1
		 * (manual), which deadlocked solo type-3 races: the
		 * driver reaches the green-trigger position before
		 * crossing 0.6-0.7 on their way round, gate blocks
		 * green indefinitely.
		 */
		{
			uint8_t preset = (s->formation_lap_type != 0) ? 1 : 0;
			int i;

			for (i = 0; i < ACC_MAX_CARS; i++)
				s->cars[i].race.formation_mid_passed = preset;
		}
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
	uint64_t now, dur_ms, fire_delay_ms;
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
		float fstart = effective_formation_start(s);
		float pre_green = s->green_trigger_start -
		    FORMATION_PRE_GREEN_EPS;

		if (pre_green < 0.0f)
			pre_green += 1.0f;
		if (wrapped_range_contains(leader_pos, fstart, pre_green)) {
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
			    (double)leader_pos, (double)fstart,
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
		 * Phase 4 (= pre-race with grid lights) lasts from the
		 * green-position crossing until `now >= ts[3]`.  The exe
		 * stamps the deadline differently per path:
		 *
		 *   silent  (FUN_14012f300, formation_lap_type 3/5):
		 *           +0x1b0 = now + 1000 ms.  No red-lights
		 *           countdown — the 1 s grace just lets the 0x28
		 *           broadcast reach clients before the phase flip.
		 *
		 *   verbose (FUN_14012f4a0, formation_lap_type anything
		 *           else): +0x1b0 = now + rand_float * 0.07629628
		 *           + 3000 = uniform [3000, 5500] ms.  This 3–5.5
		 *           s window IS the client-side red-lights
		 *           countdown (no dedicated opcode; the client
		 *           renders 5 lights into the remaining time to
		 *           ts[3]).  A shorter window here makes the
		 *           HUD lights never appear.
		 */
		if (silent) {
			fire_delay_ms = 1000;
		} else {
			fire_delay_ms = 3000 +
			    (uint64_t)arc4random_uniform(2501);
		}
		ss->ts[3] = now + fire_delay_ms;
		ss->ts[4] = ss->ts[3] + dur_ms;
		ss->ts[5] = ss->ts[4] +
		    (uint64_t)s->session_overtime_s * 1000ull;
		/*
		 * Aftercare end: ts[6] must be a real time so
		 * compute_phase can transition PHASE_COMPLETED ->
		 * PHASE_ADVANCE and the session manager can wrap to
		 * the next weekend.  Without this ts[6] stays at the
		 * UINT64_MAX initial set by session_start (race branch),
		 * and a race that reaches PHASE_COMPLETED hangs there
		 * forever — session_advance never fires, the leaderboard
		 * never resets to session 0, and tail-cadence parity with
		 * kunos is broken.
		 */
		ss->ts[6] = ss->ts[5] +
		    (uint64_t)s->post_race_s * 1000ull;
		log_info("green flag (%s): leader norm_pos=%.3f trigger=%.3f "
		    "active_dur=%llums fire_in=%llums",
		    silent ? "silent" : "verbose",
		    (double)leader_pos, (double)green_end,
		    (unsigned long long)dur_ms,
		    (unsigned long long)fire_delay_ms);
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
	/*
	 * Effective race time = raw cumulative lap time + unserved
	 * DT/SG converted to +30/+40/+50/+60 s + admin TP5/TP15.
	 * penalty_total_ms() handles the conversion (penalty.c:358);
	 * folding it into the comparison here means the position sort
	 * reflects the spec's post-race classification rule even on the
	 * live leaderboard, so a driver carrying an unserved DT shows
	 * behind whoever served theirs in the pit.
	 */
	{
		int64_t ea = (int64_t)ra->race_time_ms +
		    (int64_t)penalty_total_ms(&ra->pen);
		int64_t eb = (int64_t)rb->race_time_ms +
		    (int64_t)penalty_total_ms(&rb->pen);
		if (ea != eb)
			return ea < eb ? -1 : 1;
	}
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
	int n = 0, i, j;

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

		s->cars[idx].race.position = (int16_t)(i + 1);
	}
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
 * Kunos-format phase name for stdout `Detected sessionPhase` log
 * line.  accweb's regex expects [A-Za-z ]+ (letters + spaces, no
 * underscore / dash), so we present the names in title case with
 * spaces where accd's internal names use underscores.
 */
const char *
session_phase_kname(uint8_t p)
{
	switch (p) {
	case PHASE_WAITING:	return "Waiting";
	case PHASE_FORMATION:	return "Formation";
	case PHASE_PRE_SESSION:	return "Pre Session";
	case PHASE_SESSION:	return "Session";
	case PHASE_OVERTIME:	return "Overtime";
	case PHASE_COMPLETED:	return "Session Over";
	case PHASE_ADVANCE:	return "Post Session";
	case PHASE_RESULTS:	return "Result UI";
	default:		return "Unknown";
	}
}

/*
 * Kunos-format session-type name for the same stdout lines.
 * The wire enum is 0=Practice, 4=Qualifying, 10=Race.
 */
const char *
session_type_kname(uint8_t t)
{
	switch (t) {
	case 0:	return "Practice";
	case 4:	return "Qualifying";
	case 10:return "Race";
	default:return "Unknown";
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
	/* accweb regex: ^Detected sessionPhase <X> -> <Y> (Z)$ */
	{
		uint8_t st = (s->session.session_index < s->session_count)
		    ? s->sessions[s->session.session_index].session_type
		    : 0;
		log_kunos("Detected sessionPhase <%s> -> <%s> (%s)",
		    session_phase_kname(s->session.phase),
		    session_phase_kname(new_phase),
		    session_type_kname(st));
	}
	s->session.phase = new_phase;
	s->session.phase_started_ms = mono_ms();
	lobby_notify_session_changed(&s->lobby);
	/*
	 * Kunos emits 0x36 on phase boundaries that change the
	 * leaderboard record's cvar8 / phase-driven tail bytes (Q->R
	 * grid seeding, race COMPLETED tail, weekend ADVANCE).  Flag
	 * the leaderboard dirty for these transitions; the next tick
	 * drains the pending bit and runs the gated broadcast.  Phases
	 * that don't change leaderboard bytes (WAITING, FORMATION,
	 * OVERTIME mid-stretch) skip the trigger to avoid spurious
	 * emits.
	 */
	switch (new_phase) {
	case PHASE_PRE_SESSION:
	case PHASE_SESSION:
	case PHASE_COMPLETED:
	case PHASE_ADVANCE:
		leaderboard_request_emit(s);
		break;
	default:
		break;
	}
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

	/*
	 * Start the session clock when the first driver connects.
	 * !ts_valid guard stops the double-call right after session_advance:
	 * session_advance itself calls session_start (ts_valid=1) but leaves
	 * phase at WAITING until the next compute_phase, so without this
	 * guard the next tick would re-call session_start and re-roll the
	 * race green trigger / nudge ts[0]/ts[1] by a few ms.  Matches the
	 * same guard in handshake.c:2035.
	 */
	if (s->session.phase == PHASE_WAITING && s->nconns > 0 &&
	    !s->session.ts_valid) {
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
				s->session.ts[6] = now + post_grace_ms(s);
		} else if (def->session_type == 10) {
			s->session.overtime_hold = 1;
			s->session.cars_in_overtime = (int16_t)racing;
			s->session.overtime_hold_started_ms = now;
			log_info("overtime: hold active, %d cars racing "
			    "(hard cap in %us)", racing,
			    (unsigned)s->session_overtime_s);
		} else if (def->session_type == 4) {
			/*
			 * Quali "Right to Finish": at the moment the
			 * timer hits 0:00, snapshot per-car eligibility.
			 * A car is eligible if it's on track and not in
			 * the pit / garage at this instant.  Cars in pit
			 * lose their right immediately (handled by
			 * eligible == 0 below).  Eligibility is dropped
			 * later by handlers (lap completion or
			 * invalidation during overtime).
			 */
			int eligible = 0;
			for (i = 0; i < ACC_MAX_CARS &&
			    i < s->max_connections; i++) {
				struct CarEntry *car = &s->cars[i];
				if (!car->used) {
					car->race.quali_eligible_to_finish = 0;
					continue;
				}
				if (car->race.on_track &&
				    !car->race.in_pit &&
				    car->race.lap_count >= 1) {
					car->race.quali_eligible_to_finish = 1;
					eligible++;
				} else {
					car->race.quali_eligible_to_finish = 0;
				}
			}
			if (eligible > 0) {
				s->session.overtime_hold = 1;
				s->session.cars_in_overtime =
				    (int16_t)eligible;
				s->session.overtime_hold_started_ms = now;
				log_info("quali overtime: %d eligible cars "
				    "still on flying lap (hard cap in %us)",
				    eligible,
				    (unsigned)s->session_overtime_s);
			} else {
				log_info("quali overtime: no eligible cars, "
				    "skipping grace");
				s->session.ts[5] = now;
				if (s->session.ts[6] <= now)
					s->session.ts[6] = now + post_grace_ms(s);
			}
		}
	}

	/*
	 * sessionOverTimeSeconds hard stop — close the session for
	 * everyone once the configured grace has elapsed since the hold
	 * began, regardless of how many cars are still on track.  Without
	 * this a single AFK / crashed driver in OVERTIME holds the lobby
	 * forever.  Spec: "Once this timer expires, the server executes a
	 * 'Hard Stop,' instantly closing the session for everyone".
	 */
	if (s->session.overtime_hold &&
	    s->session_overtime_s > 0 &&
	    s->session.overtime_hold_started_ms != 0 &&
	    now - s->session.overtime_hold_started_ms >=
		(uint64_t)s->session_overtime_s * 1000ull) {
		log_info("overtime: hard stop after %us, %d cars still "
		    "out — releasing hold", (unsigned)s->session_overtime_s,
		    (int)s->session.cars_in_overtime);
		s->session.overtime_hold = 0;
		s->session.cars_in_overtime = 0;
		s->session.ts[5] = now;
		if (s->session.ts[6] <= now)
			s->session.ts[6] = now + post_grace_ms(s);
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
	/*
	 * Quali tracks its eligible-car count via session_quali_drop_eligibility,
	 * which decrements cars_in_overtime once per car that uses its right
	 * to finish.  If we also decrement here on every Quali lap completion
	 * the counter collapses ~2x as fast as expected and the hold releases
	 * before the rest of the eligible field has crossed.
	 */
	if (!session_is_race(s))
		return;
	if (s->session.cars_in_overtime > 0)
		s->session.cars_in_overtime--;
	if (s->session.cars_in_overtime <= 0) {
		uint64_t now = mono_ms();
		uint64_t grace = post_grace_ms(s);
		s->session.overtime_hold = 0;
		s->session.ts[5] = now;
		if (s->session.ts[6] <= now)
			s->session.ts[6] = now + grace;
		log_info("overtime: all cars finished, releasing hold "
		    "(post=%llums)", (unsigned long long)grace);
	} else {
		log_info("overtime: %d cars still racing",
		    (int)s->session.cars_in_overtime);
	}
}

/*
 * Quali "Instant Drop" / lap-finish: drop a car's right-to-finish
 * during Quali overtime.  Called from h_out_of_track when a
 * flying lap is invalidated, and from the lap-completion path
 * when an eligible car crosses S/F.  No-op outside Quali
 * overtime.  When the eligibility set empties, behaves like
 * session_overtime_car_finished and lets the phase advance.
 */
void
session_quali_drop_eligibility(struct Server *s, int car_id)
{
	struct CarRaceState *r;

	if (s->session.phase != PHASE_OVERTIME)
		return;
	if (s->session.session_index >= s->session_count)
		return;
	if (s->sessions[s->session.session_index].session_type != 4)
		return;
	if (car_id < 0 || car_id >= ACC_MAX_CARS)
		return;
	r = &s->cars[car_id].race;
	if (!r->quali_eligible_to_finish)
		return;
	r->quali_eligible_to_finish = 0;
	if (s->session.cars_in_overtime > 0)
		s->session.cars_in_overtime--;
	if (s->session.cars_in_overtime <= 0 &&
	    s->session.overtime_hold) {
		s->session.overtime_hold = 0;
		s->session.ts[5] = mono_ms() + 1000;
		if (s->session.ts[6] <= s->session.ts[5])
			s->session.ts[6] = s->session.ts[5] + post_grace_ms(s);
		log_info("quali overtime: all eligible cars done, "
		    "releasing hold");
	} else {
		log_info("quali overtime: car %d dropped "
		    "(%d eligible left)", car_id,
		    (int)s->session.cars_in_overtime);
	}
}

/*
 * Push a 0x28 SRV_LARGE_STATE_RESPONSE to every authenticated conn
 * using the current session state.  Intended for the transient window
 * between session_reset (which clears ts_valid so every slot emits as
 * invalid) and session_start (which populates ts[] and sets ts_valid=1
 * again).  Kunos's reference capture shows a ~150 ms gap with one
 * all-INV 0x28 right at the sidx transition; emitting the same frame
 * signals the session-index change cleanly to the client so it can
 * reset its per-session state before the valid slot values come in.
 */
static void
broadcast_session_mgr_state_all(struct Server *s)
{
	int i;
	struct ByteBuf bb;
	uint64_t now_ms = mono_ms();

	bb_init(&bb);
	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];
		uint32_t client_ts_est;

		if (c == NULL || c->state != CONN_AUTH)
			continue;
		if (c->last_udp_server_ms != 0)
			client_ts_est = c->last_udp_client_ts +
			    ((uint32_t)now_ms - c->last_udp_server_ms);
		else
			client_ts_est = c->last_pong_client_ts;
		bb_clear(&bb);
		if (wr_u8(&bb, SRV_LARGE_STATE_RESPONSE) == 0 &&
		    write_session_mgr_state(&bb, s, client_ts_est,
			c->avg_rtt_ms) == 0)
			(void)conn_send_framed(c, bb.data, bb.wpos);
	}
	bb_free(&bb);
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
		int ci;
		uint8_t empty_results[2] = { SRV_SESSION_RESULTS, 0 };

		/*
		 * Weekend complete: loop back to session 0 silently.
		 * Kunos does NOT emit 0x40 on the automatic wrap (81-min
		 * replay saw 8 × 0x3e session-results and 0 × 0x40).
		 * 0x40 is reserved for the admin /resetWeekend command,
		 * not the natural end-of-sessions rollover.
		 *
		 * Emit a trailing empty 0x3e (count=0) to clear the
		 * client-side session-results widget before the wrap.
		 * Kunos's race-end pcap shows this 2-byte frame ~15 s
		 * after the main results (aftercare ts[5]->ts[6]); it
		 * signals "previous-weekend results no longer valid"
		 * so the widget collapses cleanly across the wrap.
		 */
		for (ci = 0; ci < ACC_MAX_CARS; ci++) {
			struct Conn *c = s->conns[ci];
			if (c == NULL || c->state != CONN_AUTH)
				continue;
			(void)conn_send_framed(c, empty_results,
			    sizeof(empty_results));
		}
		log_info("session: weekend complete, resetting to "
		    "session 0");
		/* accweb regex: ^Resetting race weekend$ */
		log_kunos("Resetting race weekend");
		/*
		 * Kicks are ephemeral and tied to the in-progress weekend;
		 * a fresh weekend gets a fresh kick list (kunos clears them
		 * on wrap too).  Bans persist across the wrap.
		 */
		bans_init(&s->kicks);
		session_reset(s, 0);
		/*
		 * Force-emit 0x36 after the wrap-to-session-0 reset.
		 * session_reset zeroes leaderboard_cache_len so the next
		 * gated emit would fire on the first byte difference, but
		 * kunos's pcap shows a 0x36 even when the fresh standings
		 * coincidentally match the prior frame (e.g. a 1-bot
		 * weekend wraps with identical row shape).  Force-emit
		 * here is belt-and-suspenders.
		 */
		(void)broadcast_leaderboard_force(s);
		/*
		 * Transient all-INV 0x28 between reset and start.  Matches
		 * the ~150 ms gap observed in Kunos's reference race-start
		 * capture; signals the session-index change to the client
		 * so it can clear per-session state before the valid slot
		 * timestamps come in.
		 */
		broadcast_session_mgr_state_all(s);
		if (s->nconns > 0)
			session_start(s);
		return;
	}
	{
		/*
		 * accweb regex: ^Session changed: X -> Y N$ where X and
		 * Y are session type names ("Practice", "Qualifying",
		 * "Race") and N is the new session index.
		 */
		uint8_t prev_t = (s->session.session_index < s->session_count)
		    ? s->sessions[s->session.session_index].session_type
		    : 0;
		uint8_t next_t = (next < s->session_count)
		    ? s->sessions[next].session_type : 0;
		log_kunos("Session changed: %s -> %s %u",
		    session_type_kname(prev_t),
		    session_type_kname(next_t),
		    (unsigned)next);
	}
	session_reset(s, next);
	/*
	 * Same belt-and-suspenders force-emit on the within-weekend
	 * session boundary (P->Q->R).  session_reset zeroed the cache;
	 * the new session's leaderboard may have identical byte shape
	 * (single bot, same race_number, same drivers[]) but kunos
	 * still emits.
	 */
	(void)broadcast_leaderboard_force(s);
	broadcast_session_mgr_state_all(s);
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
 * pit-entry driver is still active at pit-served time.  Repeated
 * misses step the DT → SG30 → DQ ladder inside penalty_enqueue
 * (penalty.c) — one visible Penalty per miss.
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
	int is_race = session_is_race(s);

	/*
	 * No global early-return: each per-check gate below handles its
	 * own "not configured" case.  The driver-ran-no-stint branch
	 * (line below) runs even when driverStintTime and
	 * mandatoryPitstopCount are both unset, since multi-driver
	 * entries always carry an implied swap obligation.
	 */

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
