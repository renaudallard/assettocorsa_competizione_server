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
 * state.h -- per-connection and per-car / global server state.
 *
 * Modeled on what the binary stores in its server state struct,
 * but flat and minimal.  Just enough to support phase 1 (handshake)
 * and to be a credible foundation for later phases.
 */

#ifndef ACCD_STATE_H
#define ACCD_STATE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <netinet/in.h>

#include "io.h"
#include "lobby.h"
#include "msg.h"

/*
 * mono_ms / mono_us: monotonic-clock "milliseconds since boot".
 * Used everywhere — phase scheduling, RTT computation, debounce
 * gates, log timestamps.  Kept as a static inline in this
 * universally-included header so the body has exactly one
 * source of truth (avoids the type-drift the previous four
 * file-local copies in bcast.c, session.c, tick.c, main.c
 * were starting to develop).
 */
static inline uint64_t
mono_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull +
	    (uint64_t)ts.tv_nsec / 1000ull;
}

static inline uint64_t
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull +
	    (uint64_t)ts.tv_nsec / 1000000ull;
}

#define ACC_MAX_BANS		256
#define ACC_MAX_CARS		64
#define ACC_CAR_ID_BASE		1001
#define ACC_MAX_DRIVERS_PER_CAR	4

/*
 * Sentinel u32 written for "no lap recorded yet" / "best sector
 * absent" slots in the 0x36 leaderboard record and the welcome
 * trailer.  Matches FUN_140034210's INT32_MAX wire literal.
 */
#define LAP_TIME_INVALID	0x7FFFFFFFu

/*
 * Default ambient temperature in degrees Celsius, used as a
 * fallback whenever event.json doesn't set one (or sets it to
 * zero).  Lifted to a single named constant so the 5 wire
 * emission sites that fall back to a literal 22 / 22.0f can't
 * drift in either value or type.
 */
#define ACC_DEFAULT_AMBIENT_C	22
#define ACC_MAX_NAME_LEN	64
#define ACC_TRACK_NAME_LEN	48
#define ACC_MAX_SESSIONS	16
#define ACC_MAX_PENALTIES	8
#define RTT_RING_SLOTS		50	/* pong RTT sliding-window size
					 * (exe FUN_1400420e0 ring) */
#define ACC_LAP_HISTORY		16
/*
 * Hard ceiling on the per-session results lap log (results.json laps[]).
 * Sized well above any real race (24h x 30 cars x ~2 min laps ~= 21600);
 * beyond it new laps are dropped with a one-shot warning so a client
 * spamming lap-complete cannot grow the log without bound.
 */
#define ACC_RESULTS_LAP_MAX	100000
#define ACC_RATINGS_MAX		256

/*
 * Rating-requirement sentinel.  settings.json maps -1 (or missing key)
 * to uint8_t 0xff = "unset" / no filter; 0 and positive values are
 * thresholds.  Any gate that reads track_medals_required, safety_
 * rating_required or racecraft_rating_required must use this macro
 * instead of bare `< N` comparisons — otherwise the 0xff sentinel
 * compares as "minimum 255 required" and the wrong arm runs.
 */
#define ACC_RATING_UNSET	0xffu
#define ACC_RATING_REQUIRED(v)	((v) > 0 && (v) != ACC_RATING_UNSET)

/*
 * Session phase machine.
 *
 * Matches the accServer.exe internal 7-level phase model
 * (FUN_14012e810 computeCurrentPhase).  Transitions are purely
 * time-driven via 7 scheduled timestamps populated in
 * session_start() when the first driver connects.  Session type
 * (P/Q/R) is metadata in SessionDef, not a separate phase.
 */
enum session_phase {
	PHASE_WAITING     = 1,	/* no driver connected yet */
	PHASE_FORMATION   = 2,	/* race pre-formation (intermediate) */
	PHASE_PRE_SESSION = 3,	/* countdown intro */
	PHASE_SESSION     = 4,	/* active session (P, Q, or R) */
	PHASE_OVERTIME    = 5,	/* past scheduled end, grace period */
	PHASE_COMPLETED   = 6,	/* aftercare / results pending */
	PHASE_ADVANCE     = 7,	/* sentinel: triggers session-advance */
	PHASE_RESULTS     = 8,	/* terminal: weekend over */
};

/* Penalty kinds matching the chat command set.
 *
 * PEN_TP30, PEN_TP40, PEN_TP50, PEN_TP60 are not directly issuable
 * by an admin command; they are materialised by penalty_convert_race_end()
 * at session-over for unserved DT / SG10 / SG20 / SG30 entries
 * respectively (handbook V.1.8.11, exe FUN_140127440).  They serialise
 * to ServerMonitorPenaltyShortcut wire value 14 (PostRaceTime) just
 * like PEN_TP5 / PEN_TP15.
 */
enum penalty_kind {
	PEN_NONE = 0,
	PEN_RBL,	/* RemoveBestLaptime (kunos kind=7) */
	PEN_TP5, PEN_TP15,
	PEN_DT, PEN_DTC,
	PEN_SG10, PEN_SG10C,
	PEN_SG20, PEN_SG20C,
	PEN_SG30, PEN_SG30C,
	PEN_DQ,
	PEN_TP30, PEN_TP40, PEN_TP50, PEN_TP60	/* race-end conversions */
};

/*
 * Reason a penalty was issued.  Combined with penalty_kind this maps
 * to one of the 36 ServerMonitorPenaltyShortcut values (notebook-b
 * §12B.4) via penalty_wire_value() for on-wire serialization.
 */
enum penalty_reason {
	REASON_NONE = 0,
	REASON_CUTTING,
	REASON_PIT_SPEEDING,
	REASON_IGNORED_MANDATORY_PIT,
	REASON_RACE_CONTROL,
	REASON_PIT_ENTRY,
	REASON_PIT_EXIT,
	REASON_WRONG_WAY,
	REASON_LIGHTS_OFF,
	REASON_IGNORED_DRIVER_STINT,
	REASON_EXCEEDED_DRIVER_STINT_LIMIT,
	REASON_DRIVER_RAN_NO_STINT,
	REASON_DAMAGED_CAR,
	REASON_SPEEDING_ON_START,
	REASON_WRONG_POSITION_ON_START
};

struct PenaltyEntry {
	uint8_t		kind;		/* enum penalty_kind */
	uint8_t		collision;	/* /tp5c vs /tp5 */
	uint8_t		served;
	uint8_t		reason;		/* enum penalty_reason (drives wire code) */
	uint8_t		category;	/* AC2 cat enum (drives results.json label) */
	int32_t		laps_remaining;	/* drive-through countdown */
	uint64_t	issued_ms;
	uint8_t		pending;	/* 1 = client-reported, awaits server validation */
	uint8_t		admin;		/* 1 = admin chat-issued, hidden from 0x36 wire */
	uint8_t		race_end_tp;	/* race-end DT/SG->TP30..TP60 conversion target */
	uint32_t	race_end_tp_ms;	/* converted time in ms (base * timeMultiplier) */
	uint8_t		driver_index;	/* driver active when the penalty was
					 * incurred; for results.json attribution */
	int16_t		violation_lap;	/* 0-based lap index when the penalty was
					 * incurred (= lap_count at issue time) */
	int16_t		cleared_lap;	/* 0-based lap index when served/cleared
					 * (= lap_count at serve/race-end time);
					 * -1 while still open */
};

struct PenaltyQueue {
	struct PenaltyEntry	slots[ACC_MAX_PENALTIES];
	uint8_t			count;
};

/*
 * Per-car PenaltySheet state matching FUN_140125f50 in accServer.exe.
 * Indexed by exe penalty kind (1=DT, 2=SG10, 3=SG20, 4=SG30, 5=TP,
 * 6=DQ; slot 0 unused).  Reports accumulate the `counter` field; when
 * it reaches 0x100 the timing module escalates via a new Penalty
 * append + a ladder step on `severity`.
 */
struct PenaltySheetState {
	int32_t		counter;
	uint8_t		severity;
	uint8_t		category;	/* exe local_res20, typically 8 */
	uint64_t	issued_ms;
	uint8_t		reason;
};

/*
 * Per-car race-state runtime info: laps, position, sector splits,
 * pit status.  Updated by 0x19/0x20/0x21/0x32 handlers and read
 * by the leaderboard sort + result writer.
 */
struct CarRaceState {
	int16_t		position;		/* 1-based, 0 = unset */
	int16_t		grid_position;
	int32_t		lap_count;
	int32_t		best_lap_ms;
	uint64_t	best_lap_set_at_ms;	/* mono_ms when best_lap_ms was
						 * set; tiebreaker in P/Q sort
						 * (exe FUN_140120c90:74 compares
						 * a session-time-of-best double
						 * at lap+0x58) */
	int32_t		last_lap_ms;
	int32_t		current_lap_ms;
	int32_t		sector_ms[3];
	/*
	 * Arrival-ordered splits accumulated this lap, mirroring the exe's
	 * per-lap split vector (car+0x1d0..+0x1d8).  Appended on each
	 * recorded 0x20 split and reset at lap-complete, so the 0x3a relay
	 * count matches the exe even on the formation lap where the first
	 * split was dropped (sector_ms[] is sector-indexed and would carry
	 * a stale leading 0 there).
	 */
	int32_t		lap_split_buf[3];
	uint8_t		lap_split_n;
	/*
	 * Last-known inbound lap-states / car_field word from the client's
	 * 0x20/0x21 splits (exe car+0x54, car+0x1e8 for the 0x3a/0x3c
	 * trailing field).  Bits per FUN_140f8e8d0: 0x01 HasCut, 0x02 HasPenalty,
	 * 0x04 IsOutLap, 0x08 IsInLap, 0x40 IsRetired, 0x80 IsDisqualified,
	 * 0x400 IsSessionOver, 0x800 NextLapHasCut, etc.
	 */
	uint16_t	car_field;
	/*
	 * Lap-states word of the most recently completed non-out-lap.
	 * Set at lap-end before the per-lap car_field reset.  Used as
	 * the 0x36 status: exe FUN_140128a80:441 sets LL+0x1d0 from the
	 * last completed lap history entry, not the in-progress car_field.
	 * HasCut 0x01 tells AC2 to display the cut lap time in last_lap
	 * but inhibit the timing-tower new-best commit.
	 */
	uint16_t	completed_lap_flags;
	/*
	 * Snapshot of sector_ms[] taken at lap-completion just before
	 * the per-lap reset, so results.c's "lastSplits" field reflects
	 * the splits of the most recently completed lap (matches kunos
	 * exe semantics) instead of always reading 0 after the reset.
	 * Captures both valid and invalid (cut / out-lap) completions.
	 */
	int32_t		last_lap_splits_ms[3];
	int32_t		best_sectors_ms[3];
	int32_t		race_time_ms;
	int32_t		lap_history_ms[ACC_LAP_HISTORY];
	/*
	 * Per-lap sector splits captured at lap-completion time
	 * (same ring-buffer index as lap_history_ms).  Indexed
	 * [slot][sector] where sector ∈ {0,1,2}.  0 = no data for
	 * that split.  Populated by h_sector_split_bulk so the 0x56
	 * garage reply can include real per-lap splits instead of
	 * always emitting split_count = 0.
	 */
	int32_t		lap_splits_ms[ACC_LAP_HISTORY][3];
	/*
	 * Driver index active when each ring lap completed (same slot as
	 * lap_history_ms), so results.json laps[] attributes a lap to the
	 * driver who actually set it on a multi-driver entry rather than
	 * the current driver after a swap.  0 for single-driver cars.
	 */
	uint8_t		lap_history_driver[ACC_LAP_HISTORY];
	/*
	 * Monotonically increasing lap counter — used by handlers.c to
	 * pick the ring slot, and by handshake.c / handlers.c / results.c
	 * to compute the wrap-aware first-lap index.  uint32_t (not
	 * uint8_t) so an endurance race with > 255 laps doesn't freeze
	 * the ring with every later lap pinned to slot 255 % 16 = 15.
	 */
	uint32_t	lap_history_count;
	/*
	 * Cut-inclusive lap history for the 0x56 garage Previous-Laps reply.
	 * The exe 0x56 (FUN_1400328f0) emits EVERY lap with a real laptime
	 * (cut laps included, only the INT32_MAX sentinel filtered) plus a
	 * per-lap quality byte (lap+0x4c lap-states low byte).  The valid-only
	 * lap_history_ms ring above feeds the monitor display and must stay
	 * valid-only, so 0x56 keeps a separate ring that records cut laps too.
	 */
	int32_t		lap56_ms[ACC_LAP_HISTORY];
	int32_t		lap56_splits[ACC_LAP_HISTORY][3];
	uint8_t		lap56_entry_idx[ACC_LAP_HISTORY];
	uint16_t	lap56_lapstates[ACC_LAP_HISTORY];
	uint32_t	lap56_count;
	uint8_t		in_pit;
	uint8_t		mandatory_pit_served;	/* count of 0x54
						 * ACP_MANDATORY_PITSTOP_
						 * SERVED messages received
						 * this session; compared to
						 * Server.mandatory_pit_count
						 * at session end */
	uint8_t		current_tyres;
	uint8_t		car_dirt[5];		/* last 0x46 payload per
						 * zone — emitted in the
						 * welcome spawnDef so a
						 * late joiner sees the
						 * same body weathering
						 * as everyone else */
	uint8_t		damage[5];		/* last 0x43 zone payload
						 * — same rationale */
	uint8_t		out_of_track_latched;
	uint8_t		cuts_this_lap;		/* per-lap counted cuts;
						 * log only, no wire effect */
	uint8_t		formation_lap_done;	/* exe car+0x200 flag */
	uint8_t		disqualified;		/* PEN_DQ terminal flag */
	uint8_t		race_end_short_circuit;	/* set when stint_check_
						 * violations applied a stint /
						 * mandatory-pit / no-stint
						 * penalty; tells the race-end
						 * DT/SG->TP conversion to skip
						 * this car, mirroring the exe's
						 * FUN_14012ae10-returns-1 ->
						 * skip FUN_140127440 short-
						 * circuit (one race-end penalty
						 * per car) */
	uint8_t		on_track;		/* mirrors exe car+0x153:
						 * last ACP_CAR_LOCATION_UPDATE
						 * (0x32) had location==Track.
						 * Gates race-start leader pick. */
	uint8_t		finished;		/* crossed S/F after the race
						 * clock expired (set in the
						 * overtime lap-finish path);
						 * excluded from the phase-6
						 * end-detection hold, mirroring
						 * exe car+0x1d1 bit 0x04 */
	uint8_t		car_location;		/* raw 0x32 location enum
						 * (NONE=0 Track=1 Pitlane=2
						 * PitEntry=3 PitExit=4); exe
						 * car+0x153, emitted in the
						 * spawnDef so a late joiner
						 * sees where each car is */
	uint8_t		formation_mid_passed;	/* exe car+0x204 latch:
						 * one-shot set when rt position
						 * passes through [0.6, 0.7]
						 * during the formation lap.
						 * See FUN_1400431e0.  Bulk-true
						 * for manual formation. */
	uint8_t		quali_eligible_to_finish;
						/* Set per-car at the moment a
						 * Quali session timer expires
						 * (= phase enters OVERTIME) for
						 * cars that were on-track and
						 * outside the pit at that
						 * instant.  Cleared when the
						 * car either completes a lap
						 * or invalidates one during
						 * overtime, mirroring the
						 * "right to finish" / "instant
						 * drop" rules.  When no car
						 * still has the flag set the
						 * Quali overtime can collapse
						 * even before
						 * sessionOverTimeSeconds
						 * elapses. */
	struct PenaltyQueue	pen;
	struct PenaltySheetState	pen_state[7];	/* exe kind 1..6 */
	/*
	 * Per-car DT/SG escalation ladder.  Kunos FUN_140125f50 keys
	 * its PenaltySheet by carId (entry+0x28); category is stored
	 * at +0x5c as metadata, NOT as a search key.  A second DT/SG
	 * report for the same car steps the single per-car ladder
	 * regardless of the incoming category.  dtsg_ladder_cat holds
	 * the first report's category for the results.json label only.
	 * Both are zeroed by penalty_clear and session reset.
	 */
	uint8_t		dtsg_ladder_sev;
	uint8_t		dtsg_ladder_cat;
	/*
	 * Driver-stint tracking for FUN_14012ae10-style enforcement.
	 * stint_start_ms = monotonic ms when the current driver most
	 * recently entered the track (0 = not accumulating).  On any
	 * transition off-track (pit entry, driver swap, session end)
	 * the delta is flushed into driver_stint_ms[current_driver].
	 */
	uint64_t	stint_start_ms;
	int32_t		driver_stint_ms[ACC_MAX_DRIVERS_PER_CAR];
};

/*
 * One configured session as parsed from event.json sessions[].
 */
struct SessionDef {
	uint8_t		session_type;	/* HB IX.6: P=0 Q=4 R=10 */
	uint16_t	duration_min;
	uint8_t		hour_of_day;
	uint8_t		date_minute;	/* dateMinute: weather time-base
					 * minute term (exe descriptor +0x2c) */
	uint8_t		day_of_weekend;
	uint8_t		time_multiplier;
	float		dynamic_track_multiplier; /* exe descriptor +0x4c;
						   * default 0.0 (exe default
						   * when absent from JSON) */
};

/*
 * Current session runtime state.
 */
struct SessionState {
	uint8_t		phase;		/* enum session_phase */
	uint8_t		session_index;	/* into Server.sessions[] */
	uint64_t	phase_started_ms;
	uint32_t	weekend_time_s;
	int32_t		time_remaining_ms;
	uint8_t		ambient_temp;
	uint8_t		track_temp;
	uint8_t		*leaderboard_cache;	/* last broadcast payload */
	size_t		leaderboard_cache_len;
	size_t		leaderboard_cache_cap;
	uint64_t	last_leaderboard_ms;	/* async-mode coarse cadence */
	uint8_t		leaderboard_pending;	/* event flag; tick loop
						 * drains.  Set by callers
						 * after state mutations
						 * that kunos emits 0x36
						 * for (handshake, penalty,
						 * phase boundary, peer
						 * leave). */
	uint8_t		last_phase;		/* tick.c: detect transitions */
	int		results_written;	/* one-shot guard */
	int		grid_announced;		/* one-shot guard */

	/*
	 * 7 scheduled timestamps (ms, monotonic clock) matching
	 * the SessionManager in accServer.exe.  Populated by
	 * session_start() when the first driver connects.
	 *   ts[0] = pre_start (phase 1→3)
	 *   ts[1] = phase2_boundary (phase 2→3, race formation)
	 *   ts[2] = active_start (phase 3→4)
	 *   ts[3] = active_end (phase 4→5)
	 *   ts[4] = overtime_end (phase 5→6)
	 *   ts[5] = aftercare_end (phase 6→7)
	 *   ts[6] = advance_end (phase 6→7, inner boundary)
	 */
	uint64_t	ts[7];
	int		ts_valid;	/* non-zero once populated */
	uint8_t		overtime_hold;	/* freeze phase at OVERTIME */
	int16_t		cars_in_overtime;/* cars still finishing */
	/*
	 * Wall-clock at which overtime_hold was first asserted.  Used as
	 * the anchor for the sessionOverTimeSeconds hard cap so a single
	 * AFK car can't hold the lobby past the configured grace.  Once
	 * (now - overtime_hold_started_ms) exceeds session_overtime_s,
	 * the hold is force-released regardless of cars_in_overtime.
	 */
	uint64_t	overtime_hold_started_ms;
	uint8_t		overtime_leader_armed;	/* SM+0x262: 1 = leader-finish
						 * one-shot not yet fired;
						 * exe FUN_14012ed70 fires it
						 * once and clears the flag */
	/*
	 * Mirrors exe's server+0x14180 aftercare counter: when an overtime
	 * hold releases early (not hard-stop), the phase machine jumps
	 * immediately to PHASE_ADVANCE (ts[5]=ts[6]=now) but session_advance
	 * is deferred until advance_at_ms.  Zero means "no deferral" (hard
	 * stop, or race paths where ts[6] already carries the grace).
	 */
	uint64_t	advance_at_ms;

	/*
	 * Race green-flag position gate (FUN_14012f4a0 in accServer.exe).
	 * For race sessions, ts[3]/ts[4] are held at UINT64_MAX until the
	 * leader's normalized track position crosses the configured
	 * trigger zone.  No time fallback — matches exe exactly.
	 *   formation_ended: leader entered the formation-end range
	 *   green_fired:     green flag has fired; "Race start initialized"
	 *                    system chat has been broadcast
	 *   green_trigger:   randomized point inside the green range, rolled
	 *                    once by session_start (FUN_14012ee60 equiv).
	 */
	uint8_t		formation_ended;
	uint8_t		green_fired;
	float		green_trigger;

	/*
	 * Snapshot of the last-emitted 0x28 phase descriptor state, used
	 * by tick.c to detect changes and re-emit the wire frame the way
	 * accServer.exe FUN_14002f710 does (line 716-749 + line 799-800):
	 * compare current ts[]/phase against the snapshot; on any change
	 * push 0x28 to every auth conn within one tick and refresh the
	 * snapshot.  Without this an event-driven phase transition (e.g.
	 * formation_end stamps ts[2]) is invisible to clients until the
	 * next scheduled emit, which delays the corresponding HUD state
	 * by up to one period.
	 */
	uint64_t	last_emit_ts[7];
	uint8_t		last_emit_phase;
	uint8_t		last_emit_valid;	/* 0 until first emit */
};

/*
 * Per-server weather snapshot.
 *
 * Wire order in 0x37 (verified against Kunos accServer.exe v1.10.2
 * capture, see weather_build_broadcast for the full 17-float layout):
 *   ambient, road, clouds, wind_dir, rain, wind_speed,
 *   dry_line_wetness, ...
 */
/*
 * Fourier weather model state, mirroring the WeatherData object the
 * exe's FUN_140116c50 builds and FUN_140116830 evolves each tick.
 * The wire layout (welcome trailer's Top-level WeatherData and 0x40
 * weekend reset) reads these fields directly.  See
 * reference_weather_algorithm.md for the full per-field mapping.
 */
#define ACCD_WX_MAX_SINE	16
#define ACCD_WX_MAX_COSINE	4

struct WeatherStatus {
	/* Live values updated by weather_step from the Fourier model. */
	float		wind_speed;		/* current m/s */
	float		wind_direction;		/* current degrees, unclamped */
	float		clouds;			/* current 0..1 */
	float		current_rain;		/* current 0..1 */
	float		target_rain;		/* current 0..1 (= current_rain in this model) */
	float		track_wetness;		/* current 0..1 */
	float		dry_line_wetness;	/* exe WS+0x40, range ~ -1.2..0.8 */
	float		puddles;
	float		ambient_current;	/* current ambient °C */
	float		road_current;		/* current road °C */
	uint64_t	last_step_ms;

	/* Configured baselines from event.json. */
	float		base_clouds;
	float		base_rain;
	uint8_t		randomness;

	/* Fourier model state, matches WeatherData struct +0x28..+0x88. */
	uint8_t		is_dynamic;		/* +0x28/+0x90 (u32, but stored u8 here) */
	float		ambient_mean;		/* +0x30 */
	float		wind_speed_base;	/* +0x34 */
	float		wind_speed_mean;	/* +0x38 (informational) */
	float		wind_speed_dev;		/* +0x3c clamp >= 0.01 */
	float		wind_direction_base;	/* +0x40 */
	float		wind_direction_change;	/* +0x44 */
	int32_t		wind_harmonic;		/* +0x48 1-based index of dominant |coef| */
	int32_t		n_harmonics;		/* +0x4c sine-coefficient count */
	float		weather_base_mean;	/* +0x50 */
	float		weather_base_dev;	/* +0x54 */
	float		variability_dev;	/* +0x58 clamp >= 0.01 */
	float		sine_coeffs[ACCD_WX_MAX_SINE];	/* +0x60 vec body */
	uint8_t		n_sine;			/* sine_coeffs count */
	float		cosine_coeffs[ACCD_WX_MAX_COSINE];	/* +0x78 vec body */
	uint8_t		n_cosine;		/* cosine_coeffs count */

	uint32_t	start_time_s;		/* origin for dt = weekend_time - start */
};

/*
 * Optional cfg/weatherRules.json constraints.  The stock server
 * (FUN_140133770) re-draws the weekend weather until a generated
 * forecast satisfies every set rule (or abortAfterMs elapses).  Each
 * bound is optional: -1 (or -1.0) means "ignore this rule".  Field
 * names and defaults mirror the exe deserializer FUN_1400fd9d0 and
 * initializer FUN_14000d8f0.
 */
struct WeatherRules {
	uint8_t		active;		/* isActive: master enable (default 0) */
	uint8_t		verbose;	/* withLogging: log rejection reasons */
	int32_t		abort_after_ms;	/* abortSimulationsAfterMs (default 300) */
	int32_t		temp_min;	/* raceTempMin   °C, -1 = ignore */
	int32_t		temp_max;	/* raceTempMax   °C */
	int32_t		temp_max_diff;	/* maxTempDifference, -1 = ignore */
	float		rain_min;	/* raceRainMin   0..1, -1 = ignore */
	float		rain_max;	/* raceRainMax */
	float		rain_min_diff;	/* minRainDifference */
	float		rain_max_diff;	/* maxRainDifference */
	float		cloud_min;	/* minCloudLevel */
	float		cloud_max;	/* maxCloudLevel */
	int32_t		rain_changes;	/* raceRainChanges: >=1 require dry+wet */
};

/*
 * Assist rules from assistRules.json (subset of fields actually
 * carried in the wire protocol; everything else is server-side
 * enforcement only).
 */
struct BanList {
	char	entries[ACC_MAX_BANS][32];
	int	count;
};

/*
 * Local rating ledger entry.  Indexed by steam_id, persisted to
 * cfg/ratings.json.  Ratings are stored ×100 (5000 = 50.00) to
 * match the 0x4e wire encoding.
 */
struct RatingEntry {
	char		steam_id[32];
	uint16_t	sa_x100;
	uint16_t	tr_x100;
};

struct AssistRules {
	uint8_t		stability_control_max;	/* 0..100 cap */
	uint8_t		disable_autosteer;
	uint8_t		disable_auto_pit_limiter;
	uint8_t		disable_auto_gear;
	uint8_t		disable_auto_clutch;
	uint8_t		disable_ideal_line;
	uint8_t		disable_auto_engine_start;
	uint8_t		disable_auto_wiper;
	uint8_t		disable_auto_lights;
};

/*
 * Per-driver record loaded from entrylist.json (and / or sent by
 * the client during handshake).
 */
struct DriverInfo {
	char		first_name[ACC_MAX_NAME_LEN];
	char		last_name[ACC_MAX_NAME_LEN];
	char		short_name[8];
	uint8_t		driver_category;	/* Bronze=0..Platinum=3 */
	uint16_t	nationality;		/* see SDK enum */
	char		steam_id[32];
};

/*
 * Per-car runtime state, populated from ACP_CAR_UPDATE datagrams.
 * All fields are stored in the exact layout sent on the wire so
 * they can be re-broadcast byte-for-byte via the 0x1e / 0x39
 * fast / slow-rate per-car broadcasts.
 */
struct CarRuntime {
	/* position / orientation / velocity (three Vector3 floats
	 * per the sim protocol; see NOTEBOOK_B.md §5.6.2 0x1e) */
	float		vec_a[3];	/* probable world position */
	float		vec_b[3];	/* probable orientation */
	float		vec_c[3];	/* confirmed velocity */

	/* physical inputs (4 u8 values each, semantic TBD) */
	uint8_t		input_a[4];
	uint8_t		input_b[4];

	/* scalar state bytes (exact semantic TBD — relayed opaquely) */
	uint8_t		scalar_2c;
	uint8_t		scalar_32;
	uint8_t		scalar_33;
	uint16_t	scalar_36;
	uint8_t		scalar_34;
	uint8_t		scalar_35;
	uint32_t	scalar_44;
	uint8_t		scalar_4c;
	int16_t		scalar_1ec;

	/* header echo */
	uint8_t		packet_seq;		/* rolling counter */
	uint32_t	client_timestamp_ms;	/* most recent client ts */
	uint32_t	last_timestamp_ms;	/* for out-of-order drop */
	int		has_data;		/* ever received? */
	uint64_t	last_moved_ms;		/* mono_ms of the last 0x1e
						 * where |vec_c| > 5 km/h; exe
						 * car+0x158, gates the phase-6
						 * "still moving" end-detection
						 * hold (FUN_140042890) */
	uint8_t		dirty;			/* set by car_update ingest,
						 * cleared by periodic
						 * fan-out.  Matches the
						 * +1 byte on the exe's car
						 * struct used by
						 * FUN_14001a170. */
	uint16_t	last_src_conn_id;	/* exe lastDrivingConnectionID
						 * (+6 in CarRuntime).  Set on
						 * every accepted car_update so
						 * a packet from a different
						 * conn (post-reconnect)
						 * bypasses the timestamp gate.
						 * 0xffff = unset. */
};

/*
 * Per-car record (entry list slot).
 */
struct CarEntry {
	uint16_t	car_id;			/* assigned by server */
	int32_t		race_number;
	uint8_t		car_model;		/* see HB §IX.3 */
	uint8_t		forced_car_model;	/* entrylist forcedCarModel;
						 * 0xff = any.  Used by
						 * handshake to reject when
						 * the joiners wire cmodel
						 * != this value. */
	uint8_t		cup_category;		/* derived from the current
						 * driver's driver_category, like
						 * the 0x36 leaderboard cup byte */
	uint8_t		override_car_model_custom; /* entrylist
						 * overrideCarModelForCustomCar
						 * boolean; round-trip only,
						 * unused on the wire */
	uint16_t	nationality;
	char		team_name[ACC_MAX_NAME_LEN];
	int32_t		default_grid_position;	/* 0-based; -1 = unset
						 * (JSON value-1 at load) */
	int8_t		ballast_kg;		/* kg; /ballast admin clamps
						 * -40..40 per exe FUN_14001dae0 */
	float		restrictor;		/* normalized 0..0.20 */
	char		custom_car[64];		/* entrylist customCar
						 * filename (handbook §VI.2);
						 * stored for round-trip and
						 * results.json output. */
	uint8_t		current_driver_index;
	uint8_t		driver_count;
	uint8_t		is_server_admin;	/* entrylist isServerAdmin:
						 * auto-elevate any driver
						 * matching this slot to admin
						 * without /admin <pw>,
						 * matching exe +0x6e check */
	struct DriverInfo drivers[ACC_MAX_DRIVERS_PER_CAR];
	uint8_t		swap_state[ACC_MAX_DRIVERS_PER_CAR]; /* 0=idle..5=done */
	int		used;			/* slot occupied? */
	/*
	 * Team-entry anchor: when a forceEntryList entrylist entry has
	 * multiple registered drivers, the loader expands it into N
	 * companion slots (one per driver, sharing entry-level fields:
	 * race_number, car_model, team_name, drivers[], driver_count,
	 * ballast_kg, restrictor, ...).  team_entry_id is the slot index
	 * of the first member (the anchor); every member of the group —
	 * including the anchor — has team_entry_id == anchor_slot.  -1 =
	 * standalone entry (the default path, single-driver scenarios,
	 * and forceEntryList=0 entries).  All group-iteration code paths
	 * gate on (team_entry_id >= 0) so standalone slots short-circuit
	 * to the legacy single-car behaviour.
	 */
	int8_t		team_entry_id;

	/* Runtime state updated every tick by ACP_CAR_UPDATE. */
	struct CarRuntime rt;

	/* Race state updated by lap/sector handlers and the
	 * leaderboard sort. */
	struct CarRaceState race;

	/* Last observed ACP_CAR_SYSTEM_UPDATE payload (u64 at
	 * +0x1b0 in the exe's car struct).  Stored so it can be
	 * replayed to newly-joined clients via a proactive 0x2e
	 * state sync, matching FUN_14002dcb0 in accServer.exe. */
	uint64_t	last_sys_data;

	/* Last ACP_ELO_UPDATE payload (u32 at car+0x1f8 in the
	 * exe).  FUN_140034210 emits it as u8-clamped in the
	 * leaderboard record tail; default 0 = no rating seen. */
	uint32_t	last_elo;

	/*
	 * Snapshot of race state at the end of each completed
	 * session, keyed by SessionDef index.  Used by the 0x56
	 * ACP_LOAD_SETUP reply when the client asks for laps from
	 * a session we've already moved past.  NULL entries mean
	 * the session is either not yet run or was reset.
	 */
	struct CarRaceState *race_archive[ACC_MAX_SESSIONS];
};

/*
 * Per-connection state.  One of these per accepted TCP socket.
 */
struct Conn {
	int		fd;
	struct sockaddr_in
			peer;
	enum conn_state	state;
	uint16_t	conn_id;	/* server-assigned, also "carIndex" */
	int32_t		car_id;		/* index into server.cars[], -1 if spectator */
	char		steam_id[32];	/* parsed at handshake; drives the eager
					 * same-SteamID dedup for spectators too */
	int		is_admin;
	uint64_t	last_admin_attempt_ms;	/* /admin rate limit anchor */
	int		is_spectator;
	int		hellbanned;	/* /hellban: drop inbound, skip in
					 * broadcasts.  Per-session only. */
	uint8_t		netcar_delta_mode;	/* &delta: netcar latency
						 * display mode (0 default,
						 * 1 error, 2 diff).  Stored
						 * for exe user-visible parity;
						 * accd has no netcar latency
						 * display, so it is inert. */
	struct ByteBuf	rx;		/* incoming TCP byte stream */
	struct ByteBuf	tx;		/* outbound queue, drained on POLLOUT */
	size_t		tx_peak_bytes;	/* max queue depth ever observed */
	uint64_t	tx_warn_ms;	/* rate-limit soft-cap warnings */
	unsigned char	*hs_echo;	/* raw handshake body to echo in trailer */
	size_t		 hs_echo_len;
	uint64_t	accepted_mono_ms;	/* mono_ms at TCP accept; unauth
						 * conns past CONN_UNAUTH_TIMEOUT_MS
						 * are reaped so port scanners don't
						 * pin the session-start gate */
	uint32_t	avg_rtt_ms;		/* windowed mean round-trip from
						 * 0x16 pong: arithmetic mean of
						 * the rtt_ring, mirroring exe
						 * FUN_1400420e0's 50-slot mean */
	int32_t		rtt_ring[RTT_RING_SLOTS]; /* RTT samples; -1 = empty */
	int32_t		rtt_ring_idx;		/* last-written slot (init -1) */
	int32_t		clock_offset_ms;	/* server_now - (avg_rtt/2 +
						 * client_ts); stat/CSV only, no
						 * wire consumer (exe's dead
						 * field 0x2802a) */
	int64_t		session_clock_offset_ms;	/* session-relative clock
							 * offset: session_now -
							 * rtt/2 - pong_client_ts.
							 * Updated on best-RTT
							 * pong (matches kunos
							 * FUN_1400420e0:23-37).
							 * Used by 0x4f force=1
							 * relay to mirror kunos's
							 * FUN_140042030 transform
							 * into a session-relative
							 * IEEE-754 double.
							 * This is the exe Mode-A
							 * base D_base (conn+0xa0310);
							 * Mode A is latency_mode
							 * != 0. */
	int64_t		session_avg_offset_ms;	/* session-relative average-RTT
						 * offset (exe I_avg): the slew
						 * target for i_fb_ms.  Recomputed
						 * every pong. */
	int64_t		i_fb_ms;		/* Mode-B relay-ts offset (exe
						 * I_fb conn+0xa00ac): slewed
						 * +-3 ms / 50 ms toward
						 * session_avg_offset_ms.  The
						 * default (latency_mode == 0)
						 * relay projection uses this. */
	uint8_t		i_fb_valid;		/* 1 after i_fb seeded (first pong) */
	uint32_t	best_rtt_ms;		/* lowest pong RTT seen so far */
	uint32_t	pong_threshold_ms;	/* 0x28 re-emit threshold: reset to
						 * avg_rtt after each ring update
						 * (exe FUN_1400420e0 stored_threshold);
						 * emit fires when rtt < this value */
	uint8_t		session_clock_seen;	/* 1 after first pong */
	double		drift_ms;		/* clock drift acc (FUN_1400419e0
						 * conn[0x280d0]): updated each 0x1e
						 * as drift += (server_delta -
						 * client_delta); reset to 0 on new
						 * best-RTT pong; Car+0x50 (wire
						 * +0x07) = (int)(best_rtt_ms +
						 * drift_ms) */
	double		drift_prev_server;	/* prev server_ms as double
						 * (conn[0x280cc]) */
	double		drift_prev_client;	/* prev client_ts as double
						 * (conn[0x280ce]) */
	int8_t		drift_valid;		/* 0 = skip drift update on first
						 * 0x1e after pong reset; 1 = prev
						 * timestamps are valid */
	uint32_t	last_pong_client_ts;	/* client_ts from most recent 0x16 pong;
						 * used only by the first-pong re-
						 * emit detection (latches non-zero
						 * on a real pong) */
	uint32_t	last_udp_client_ts;	/* client_ts from the freshest UDP
						 * packet (0x16 pong OR 0x1e car
						 * update) — pairs with last_udp_
						 * server_ms as the extrapolation
						 * pivot used by write_session_mgr_
						 * state.  Refreshed on every UDP
						 * packet so the resulting f32
						 * delta stays accurate even when
						 * the client clock drifts relative
						 * to the server (car updates arrive
						 * at ~18 Hz so the pivot is never
						 * more than ~55 ms stale). */
	uint32_t	last_udp_server_ms;
	uint64_t	last_keepalive_ms;	/* mono_ms of last 0x14 sent to
						 * this conn; 0 = never.  Exe
						 * FUN_140041e80 uses a per-conn
						 * field (conn+0xa0238) rather than
						 * a global timer. */
	uint32_t	welcome_bytes;		/* size of last 0x0b welcome
						 * trailer sent on this
						 * conn — reported on the
						 * kunos-compat `Sent
						 * handshake response …
						 * with N bytes` log line */
	/*
	 * SMPR (ServerMonitor) channel state.  is_smpr is set when the
	 * conn delivered a ServerMonitorConnectionRequest as its first
	 * frame; the conn never goes through the gameplay handshake and
	 * never claims a car slot.  The remaining fields capture the
	 * client's cadence + leaderboard preferences from that hello.
	 */
	uint8_t		is_smpr;
	uint8_t		smpr_self_contained;	/* sendSelfcontainingLeaderboards */
	uint8_t		smpr_extended;		/* sendExtendedLeaderboards */
	uint32_t	smpr_rt_interval_ms;	/* clamped REALTIME cadence */
	uint64_t	smpr_rt_last_ms;	/* mono_ms of last 0x06 push */
};

/*
 * One completed lap in the per-session results log (results.json laps[]).
 * Every closed lap is appended in completion order, valid or not, mirroring
 * the exe's OfficialTiming closed-laps vector (FUN_140125c60).  is_valid is
 * the exe's results.json isValidForBest verdict (FUN_140129b10), which is
 * stricter than the live best_lap mask: invalid if any lap-states bit in
 * 0x100f is set or the laptime is outside [1, 0x7ffffffe].
 */
struct ResultsLap {
	uint16_t	car_id;		/* wire car id (ACC_CAR_ID_BASE + slot) */
	uint8_t		driver_index;
	uint8_t		is_valid;	/* isValidForBest */
	int32_t		lap_time_ms;
	int32_t		splits_ms[3];
};

/*
 * Track grip / rubber integrator state (port of gripState in accServer.exe
 * ServerState+0xa0868).  Persists across sessions; constructed once at
 * server start by weather_grip_init() and evolved every weather tick by
 * weather_grip_step().  Field names follow the exe offset convention
 * (G## = gripState+0x##) so the RE notes remain traceable.
 *
 * Wire head map (FUN_1400330e0 RAW path, simracer_weather==0):
 *   G0c=head[0] grip-now, G10=head[1] grip-green, G14=head[2] wet-top,
 *   G18=head[3] wet-sub, G1c=head[4] clamp(G14-G3c), G20=head[5], G24=head[6].
 */
struct GripState {
	float	G04;	/* base grip; init 0.96 */
	float	G08;	/* rubber-build coeff; init 1.0 */
	float	G0c;	/* head[0] grip-now; init 1.0, recomputed each step */
	float	G10;	/* head[1] grip-green/base; init 0.5, recomputed */
	float	G14;	/* head[2] wet-top; init 0.0 */
	float	G18;	/* head[3] wet-sub; init 0.0 */
	float	G1c;	/* head[4] clamped wet; init 0.0 */
	float	G20;	/* head[5]; init 0.0, recomputed */
	float	G24;	/* head[6]; init 0.0, recomputed */
	float	G28;	/* wet-accum coeff; init 0.0020 */
	float	G2c;	/* dry-top rate; init 0.00030 */
	float	G30;	/* wet-sub-accum coeff; init 0.0010 */
	float	G34;	/* dry-sub rate; init 0.00020 */
	float	G38;	/* rubber accumulation; init 0.0 */
	float	G3c;	/* wet-surface drag; init 0.0 */
	float	G40;	/* last-tick seed (ms); init 0.0 */
};

/*
 * Global server state.
 */
struct Server {
	/* config */
	int		tcp_port;
	int		udp_port;
	int		max_connections;
	int		max_monitors;	/* SMPR observer cap; settings.json
					 * "maxMonitors"; defaults to
					 * max_connections / 4 (>=2) */
	int		max_monitors_per_ip;	/* settings.json
						 * "maxMonitorsPerIp"; default 2 */
	int		lan_discovery;
	/*
	 * UDP port on 127.0.0.1 for 0xbe periodic state snapshots.  0 =
	 * disabled (default).  Mirrors accServer.exe's optional stats
	 * push channel (FUN_14002e8d0 gates on the short at +0x112).
	 * Intended for local monitoring tools; never routed off-host.
	 */
	int		stats_udp_port;
	uint64_t	last_stats_udp_ms;	/* mono_ms of last 0xbe emit */
	/*
	 * Admin chat toggles mirroring the exe's server struct bytes.
	 * legacy_netcode at +0x22 (/mp), log_conditions at +0x116
	 * (/debug conditions), log_bandwidth at +0x114 (/debug
	 * bandwidth), log_qos at +0x117 (/debug qos), latency_mode at
	 * +0x1419b (/latencymode).  All default 0.
	 */
	uint8_t		legacy_netcode;
	uint8_t		log_conditions;
	uint8_t		log_bandwidth;
	uint8_t		log_qos;
	uint8_t		latency_mode;
	char		server_name[ACC_MAX_NAME_LEN];
	char		password[64];
	char		admin_password[64];
	char		spectator_password[64];
	/*
	 * settings.json carGroup — "FreeForAll" (default) / GT2 / GT3 /
	 * GT4 / GTC / TCX.  FUN_140116480 maps this to the trailing byte
	 * of the LAN-discovery reply (0xfa for FreeForAll) which the ACC
	 * server browser reads to categorise the server and to validate
	 * the probe reply for the ping-column measurement.  Invalid
	 * values fall back to FreeForAll in the exe's logic.
	 */
	char		car_group[16];
	char		track[ACC_TRACK_NAME_LEN];
	int		ignore_premature_disconnects;
	/*
	 * Rating thresholds from settings.json (handbook III.2.2).
	 * Use ACC_RATING_REQUIRED() to test — 0xff is the unset sentinel
	 * (default since 0.3.70).  Drivers below the configured floor
	 * get the handshake rejected before they reach a car slot.
	 *   trackMedalsRequirement      0..3
	 *   safetyRatingRequirement     0..99 (×100 stored on the wire)
	 *   racecraftRatingRequirement  0..99 (×100 stored on the wire)
	 */
	uint8_t		track_medals_required;
	uint8_t		safety_rating_required;
	uint8_t		racecraft_rating_required;
	uint8_t		is_cp_server;		/* settings.isCPServer: gate on
						 * the competition-rating window
						 * and restrict joins to FP */
	uint8_t		is_cp_inv_server;	/* settings.isCPInvServer: same
						 * public-MP gate suppression as
						 * is_cp_server (exe +0xe3) */
	uint8_t		simracer_weather;	/* settings.simracerWeatherConditions
						 * (exe +0x315): "Snowflake" vs
						 * "Standard" /wt header */
	uint8_t		fixed_condition_qualy;	/* event.json isFixedConditionQualification
						 * (exe +0xac): when 1, suppresses
						 * weather_step during qualifying so
						 * conditions stay fixed (FUN_1400330e0) */
	int32_t		competition_rating_min;	/* settings.competitionRatingMin */
	int32_t		competition_rating_max;	/* settings.competitionRatingMax */
	uint8_t		is_race_locked;		/* settings.json
						 * isRaceLocked: 1 (default)
						 * blocks mid-race joins,
						 * 0 allows them.  Inverse
						 * of unsafe_rejoin. */
	uint8_t		randomize_track_when_empty;
						/* settings.json
						 * randomizeTrackWhenEmpty:
						 * when 1, the track resets to
						 * a random pool pick each time
						 * the server empties (mirrors
						 * exe FUN_14002f710). */
	uint8_t		use_igt_dlc_tracks;	/* settings.json
						 * useIgtDlcTracks: include
						 * kyalami / mount_panorama /
						 * suzuka / laguna_seca in the
						 * random pool */
	uint8_t		use_bgt_dlc_tracks;	/* settings.json
						 * useBgtDlcTracks: include
						 * oulton_park / snetterton /
						 * donington in the random
						 * pool */
	char		meta_data[256];		/* event.json metaData,
						 * passed through to
						 * results.json header. */
	int		dump_leaderboards;
	int		dump_entry_list;	/* settings.json
						 * dumpEntryList: write
						 * cfg/entrylist.json at
						 * each session COMPLETED
						 * with final positions
						 * baked into
						 * defaultGridPosition. */
	int		force_entry_list;
	int		register_to_lobby;	/* settings.json knob */
	int		max_car_slots;		/* settings.json maxCarSlots,
						 * clamped per rating reqs;
						 * sent to lobby (Kunos clamps
						 * to 10 without rating reqs) */
	struct LobbyClient	lobby;
	/*
	 * allowAutoDQ from settings.json (default 1).  When set to 0,
	 * auto-DQ for failure to serve a DT/SG within 3 laps is
	 * downgraded to a 30-second stop&go so race control can
	 * review.  Reckless-driving and pit-speeding DQs are not
	 * affected (matches Kunos 1.8.11+ behavior).
	 */
	int		allow_auto_dq;

	/*
	 * useAsyncLeaderboard from settings.json (default 0 = fire-on-
	 * dirty, matching the exe's FUN_14002f710 which broadcasts 0x36
	 * whenever the leaderboard-dirty flag is set with no cadence
	 * gate).  When 1, we coalesce to the CADENCE_LEADERBOARD tick
	 * instead, trading up to 75 s of staleness for less fan-out CPU.
	 */
	uint8_t		use_async_leaderboard;

	/*
	 * unsafeRejoin from settings.json (default 1 = allow late joins).
	 * Mirrors the exe's +0x228 byte read in FUN_140023700 and gates
	 * the mid-race / late-qualy rejection in FUN_140025690.  When 0,
	 * 0x09 handshakes that land during a race session (or late qualy,
	 * or a locked preparation phase) are rejected with 0x0c code 12.
	 * Default matches the exe's "Joining during race is allowed"
	 * startup log when the key is absent.
	 */
	uint8_t		unsafe_rejoin;

	/*
	 * Admin-toggled preparation lock (FUN_140025690 bVar4 path using
	 * exe +0x229).  When set AND the current session is in FORMATION
	 * or PRE_SESSION, fresh handshakes are rejected with 0x0c code
	 * 12 regardless of unsafeRejoin — lets an operator freeze the
	 * grid once qualifying is in its countdown.  Toggled by the
	 * /lockprep /unlockprep admin chat commands.
	 */
	uint8_t		preparation_locked;

	/*
	 * Race green-flag position gate (event.json override, defaults
	 * match the exe's vtable fallback constants DAT_14014bccc/bcd0/
	 * bcd8).  Populate from event.json keys formationTriggerNormalized
	 * RangeStart / greenFlagTriggerNormalizedRangeStart /
	 * greenFlagTriggerNormalizedRangeEnd; else server_init sets the
	 * exe defaults below.
	 */
	float		formation_trigger_start;
	float		green_trigger_start;
	float		green_trigger_end;

	/*
	 * formationLapType from settings.json (exe +0x1dc), default 3
	 * (matches the exe ctor at FUN_14000de10:32, u16 0x1e03 stored
	 * at config+0xbc -> byte 0x03).  The exe dispatches green-fire
	 * on this byte (FUN_14002f710:277, test
	 * `((type - 3) & 0xfd) == 0`):
	 *   3 or 5 -> FUN_14012f300 silent path: fires at the rolled
	 *             trigger +0x294 with a 1 s phase-4 window, no chat.
	 *   else   -> FUN_14012f4a0 verbose path: fires anywhere in the
	 *             green range with a uniform 3000-5500 ms phase-4
	 *             window; broadcasts "Race start initialized" chat.
	 * On public servers the exe forces value 1 back to 3
	 * (FUN_140023700:538) — manual formation is private-server only.
	 */
	uint8_t		formation_lap_type;

	/*
	 * Additional settings.json knobs read by FUN_140106300 in the
	 * exe.  Parsed but mostly informational for accd — the CP-
	 * server, DLC-gating, and track-rotation keys are out of our
	 * scope so we don't bother storing them.
	 */
	uint8_t		short_formation_lap;	/* shortFormationLap: exe
						 * forces 1 on public
						 * servers (+0x230) */
	uint8_t		write_latency_dumps;	/* writeLatencyFileDumps:
						 * latency diagnostics
						 * file output toggle */
	void		*latency_dump_fp;	/* FILE* for the current
						 * session's latency CSV
						 * (void* to avoid leaking
						 * stdio into this header);
						 * NULL when disabled or
						 * between sessions */
	uint8_t		do_driver_swap_broadcast; /* doDriverSwapBroadcast:
						 * gate 0x47 / 0x48 driver-
						 * swap state broadcasts */
	uint32_t	config_version;		/* configVersion from
						 * settings.json */
	uint32_t	configuration_version;	/* configVersion from
						 * configuration.json */
	uint32_t	event_version;		/* configVersion from
						 * event.json */
	uint32_t	entrylist_version;	/* configVersion from
						 * entrylist.json */

	/* runtime */
	int		tcp_fd;
	int		udp_fd;
	int		lan_fd;
	struct Conn	*conns[ACC_MAX_CARS];	/* indexed by conn_id */
	int		nconns;

	struct CarEntry	cars[ACC_MAX_CARS];

	/* sessions parsed from event.json */
	struct SessionDef	sessions[ACC_MAX_SESSIONS];
	uint8_t			session_count;
	struct SessionState	session;
	struct WeatherStatus	weather;
	struct WeatherRules	weather_rules;	/* cfg/weatherRules.json (optional) */
	uint32_t		weather_draw_seq;	/* increments per weekend re-draw to vary the seed */
	struct GripState	grip;		/* track grip integrator; persists across sessions */
	uint32_t		grip_forecast_s;	/* weekend seconds the grip forecast has reached; rewound to 0 on weekend reset */
	struct AssistRules	assist;
	struct BanList		bans;
	struct BanList		kicks;	/* ephemeral; cleared on weekend wrap */
	uint8_t			bop_version;
	uint16_t		pre_race_waiting_s; /* preRaceWaitingTimeSeconds */
	uint16_t		session_overtime_s; /* sessionOverTimeSeconds */
	uint16_t		post_qualy_s;	    /* postQualySeconds (EventConfig+0x90) */
	uint16_t		post_race_s;	    /* postRaceSeconds  (EventConfig+0x94) */
	uint32_t		driver_stint_time_s; /* eventRules.driverStintTimeSec, with the maxTotalDrivingTime fallback (0 = no limit) */
	uint8_t			mandatory_pit_count; /* eventRules.mandatoryPitstopCount (0 = none) */
	uint8_t			mandatory_swap_required; /* eventRules.isMandatoryPitstopSwapDriverRequired */
	/*
	 * Remaining eventRules.json fields per handbook III.2.4.
	 * 0xff / 0xffff sentinels mean "unset" on the wire.
	 */
	uint8_t			qualify_standing_type;	/* 0=bestlap, 1=superpole */
	int32_t			pit_window_length_s;	/* -1 = unset */
	int32_t			max_total_driving_time_s;/* -1 = unset */
	uint8_t			max_drivers_count;	/* default 1 */
	uint8_t			refuelling_allowed;	/* 1=allowed (default) */
	uint8_t			refuelling_time_fixed;	/* 0=variable (default) */
	uint8_t			pit_refuelling_required;
	uint8_t			pit_tyre_change_required;
	uint8_t			tyre_set_count;		/* default 50 (Kunos) */

	/*
	 * Per (track, carModel) BoP ballast / restrictor loaded from
	 * cfg/bop.json (handbook VI.3).  Parsed, clamped and logged for
	 * operator visibility but NOT applied to any car -- this matches
	 * the exe, which likewise loads bop.json into a vector it never
	 * reads back onto a car (verified 2026-06-13).  Up to ACC_MAX_BOP
	 * entries; unused slots zero-initialised.
	 */
#define ACC_MAX_BOP 256
	struct BoPEntry {
		char		track[ACC_TRACK_NAME_LEN];
		uint8_t		car_model;
		int8_t		ballast_kg;	/* kg, clamped [-40, 40] */
		uint8_t		restrictor_pct;
	} bop[ACC_MAX_BOP];
	int			bop_count;
	char			cfg_dir[256];	/* for saving bans */

	/* timing */
	uint64_t	tick_count;
	uint64_t	session_start_ms;

	/*
	 * Per-tick CPU-load ring for the 0x14 keepalive bytes 11/12
	 * (avg/max CPU load percent), mirroring the exe's per-tick ring at
	 * ServerState+0x14185 reduced ~1 Hz into +0xa0c14 (Avg Cpu) and
	 * +0xa0c18 (Max Cpu).  Each sample = tick_run work_us / tick
	 * interval; reduced to percent (x100) at keepalive time.  Window is
	 * the last <=40 ticks like the exe (trimmed to 0x28).
	 */
	float		cpu_ring[40];
	uint8_t		cpu_ring_count;
	uint8_t		cpu_ring_head;
	uint8_t		cpu_avg_pct;
	uint8_t		cpu_max_pct;

	/* Per-steam_id rating ledger (see ratings.c). */
	struct RatingEntry	ratings[ACC_RATINGS_MAX];
	uint8_t			ratings_dirty;
	uint64_t		ratings_last_emit_ms;

	/*
	 * Per-source-IP /admin retry table.  The per-Conn rate limit
	 * (Conn.last_admin_attempt_ms) is lost when conn_drop frees
	 * the struct, so a hostile client can re-handshake to reset
	 * the gate.  Keying the cooldown by source IP closes the
	 * bypass.  Small LRU; 32 entries cover any plausible
	 * concurrent attacker pool while staying tiny on the wire.
	 */
	struct AdminRetry {
		uint32_t	ip;	/* network-order, 0 == empty slot */
		uint64_t	last_ms;
	} admin_retry[32];

	/*
	 * Per-session results lap log: every completed lap (valid and
	 * invalid) in global completion order, the source for the
	 * results.json top-level laps[] array.  Mirrors the exe's
	 * OfficialTiming closed-laps vector (FUN_140125c60) -- uncapped and
	 * chronological, unlike the 16-slot per-car ring (lap_history_ms)
	 * that drives the 0x36 leaderboard and the 0x56 garage panel.
	 * Grown on demand up to ACC_RESULTS_LAP_MAX; reset per session,
	 * freed at shutdown.
	 */
	struct ResultsLap	*results_laps;
	uint32_t		results_lap_count;
	uint32_t		results_lap_cap;
	uint8_t			results_lap_overflow;	/* one-shot warn flag */
};

void	server_init(struct Server *s);
void	server_free(struct Server *s);

/*
 * Seed s->formation_trigger_start / green_trigger_start / green_trigger_end
 * from a per-track table matching exe FUN_14012c510.  Caller must have
 * populated s->track (the track-id string after year-suffix stripping)
 * before invoking.  Tracks not in the table keep the current values —
 * call from config_load after the track string is ready and before
 * event.json's formation/green override block runs so JSON still wins.
 */
void	track_zones_apply(struct Server *s);
int	track_pit_count(const char *track, int is_private);
void	track_random_pick(struct Server *s);
int64_t	conn_clock_offset(const struct Server *s, const struct Conn *c);

/* Allocate a new Conn for an accepted fd.  Returns NULL on full. */
struct Conn *
	conn_new(struct Server *s, int fd, const struct sockaddr_in *peer);

/* Drop a connection: close the fd, free the buffers, slot returns
 * to the free pool. */
void	conn_drop(struct Server *s, struct Conn *c);

/* Find a connection by its (server-assigned) conn_id. */
struct Conn *
	server_find_conn(struct Server *s, uint16_t conn_id);

/* Reset the car_update timestamp gate fields for a CarRuntime.  Called
 * from server_init, session_reset and conn_drop so a fresh session, a
 * reconnect or a slot-reuse driver starts clean — without this the
 * stored last_timestamp_ms blocks every new packet until it climbs
 * past the previous high-water mark. */
void	car_runtime_reset_gate(struct CarRuntime *rt);

/*
 * Allocate a free CarEntry slot and return its index, or -1 if
 * the entry list is full.  car_id matches the index by design.
 */
int	server_alloc_car(struct Server *s);
/* Count used car slots = "drivers present" (excludes carless spectators). */
int	server_used_car_count(const struct Server *s);

/*
 * Pick the next grid slot for a joining car: max existing +1 if
 * that fits, else walk back from max_connections-1 looking for
 * an unoccupied slot.  Returns -1 if the grid is full.  Mirrors
 * accServer.exe FUN_140021090.
 */
int	server_find_grid_slot(struct Server *s);
int	server_validate_default_grid(struct Server *s, int car_id, int dgp);

/*
 * Pick a unique race number for a joining car.  Mirrors the loop
 * inside accServer.exe FUN_140025690: try requested, requested+1,
 * ..., requested+9; if all collide, walk 1..999 picking the
 * smallest free; if even that is full, return 999.  The "taken"
 * set is every car slot except `my_slot` that holds a driver
 * record (used == 1 or driver_count > 0), matching the exe's
 * persistent saved-driver registry.
 */
int	server_alloc_race_number(struct Server *s, int my_slot,
	    int requested);

#endif /* ACCD_STATE_H */
