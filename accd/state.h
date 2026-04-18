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
#include <netinet/in.h>

#include "io.h"
#include "lobby.h"
#include "msg.h"

#define ACC_MAX_BANS		256
#define ACC_MAX_CARS		64
#define ACC_CAR_ID_BASE		1001
#define ACC_MAX_DRIVERS_PER_CAR	4
#define ACC_MAX_NAME_LEN	64
#define ACC_TRACK_NAME_LEN	48
#define ACC_MAX_SESSIONS	16
#define ACC_MAX_PENALTIES	8
#define ACC_LAP_HISTORY		16
#define ACC_RATINGS_MAX		256

/*
 * Session phase machine.
 *
 * Matches the accServer.exe internal 7-level phase model
 * (FUN_14012e810 computeCurrentPhase).  Transitions are purely
 * time-driven via 6 scheduled timestamps populated in
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

/* Penalty kinds matching the chat command set. */
enum penalty_kind {
	PEN_NONE = 0,
	PEN_TP5, PEN_TP15,
	PEN_DT, PEN_DTC,
	PEN_SG10, PEN_SG10C,
	PEN_SG20, PEN_SG20C,
	PEN_SG30, PEN_SG30C,
	PEN_DQ
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
	uint8_t		reason;		/* enum penalty_reason */
	int32_t		laps_remaining;	/* drive-through countdown */
	uint64_t	issued_ms;
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
	int32_t		last_lap_ms;
	int32_t		current_lap_ms;
	int32_t		sector_ms[3];
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
	uint8_t		lap_history_count;
	uint8_t		in_pit;
	uint8_t		pit_crossing_latched;
	uint8_t		mandatory_pit_served;
	uint8_t		current_tyres;
	uint8_t		out_of_track_latched;
	uint8_t		cuts_this_lap;		/* 0x3c force=1 count */
	uint64_t	last_cut_ms;		/* mono_ms of last counted
						 * cut — debounce window
						 * so a sustained off-track
						 * episode (many force=1
						 * events from the client)
						 * counts as a single cut */
	uint8_t		formation_lap_done;	/* exe car+0x200 flag */
	uint8_t		out_lap_done;		/* first lap from pits */
	uint8_t		disqualified;		/* PEN_DQ terminal flag */
	struct PenaltyQueue	pen;
	struct PenaltySheetState	pen_state[7];	/* exe kind 1..6 */
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
	uint8_t		day_of_weekend;
	uint8_t		time_multiplier;
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
	uint32_t	standings_seq;	/* bumps on leaderboard change */
	uint32_t	last_standings_seq;	/* tick.c: detect changes */
	uint8_t		last_phase;		/* tick.c: detect transitions */
	int		results_written;	/* one-shot guard */
	int		grid_announced;		/* one-shot guard */

	/*
	 * 6 scheduled timestamps (ms, monotonic clock) matching
	 * the SessionManager in accServer.exe.  Populated by
	 * session_start() when the first driver connects.
	 *   ts[0] = pre_start (phase 1→3)
	 *   ts[1] = phase2_boundary (phase 2→3, race formation)
	 *   ts[2] = active_start (phase 3→4)
	 *   ts[3] = active_end (phase 4→5)
	 *   ts[4] = overtime_end (phase 5→6)
	 *   ts[5] = aftercare_end (phase 6→7)
	 */
	uint64_t	ts[7];
	int		ts_valid;	/* non-zero once populated */
	uint8_t		overtime_hold;	/* freeze phase at OVERTIME */
	int16_t		cars_in_overtime;/* cars still finishing */

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
};

/*
 * Per-server weather snapshot.
 *
 * Wire order in 0x37 (verified against Kunos accServer.exe v1.10.2
 * capture, see weather_build_broadcast for the full 17-float layout):
 *   ambient, road, clouds, wind_dir, rain, wind_speed,
 *   dry_line_wetness, ...
 */
struct WeatherStatus {
	float		wind_speed;	/* m/s or normalized */
	float		wind_direction;	/* degrees, unclamped */
	float		clouds;		/* 0..1 */
	float		current_rain;	/* 0..1 */
	float		target_rain;	/* 0..1 */
	float		track_wetness;	/* 0..1 */
	float		dry_line_wetness;
	float		puddles;
	uint64_t	last_step_ms;
	/*
	 * Configured baselines from event.json.  weather_step drifts
	 * around these (bounded) instead of running its own free-form
	 * sine.  randomness=0 holds them constant; 1+ allows drift.
	 */
	float		base_clouds;
	float		base_rain;
	uint8_t		randomness;
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
	uint8_t		stability_control_max;
	uint8_t		disable_autosteer;
	uint8_t		disable_auto_pit_limiter;
	uint8_t		disable_auto_gear;
	uint8_t		disable_auto_clutch;
	uint8_t		disable_ideal_line;
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
};

/*
 * Per-car record (entry list slot).
 */
struct CarEntry {
	uint16_t	car_id;			/* assigned by server */
	int32_t		race_number;
	uint8_t		car_model;		/* see HB §IX.3 */
	uint8_t		cup_category;
	uint16_t	nationality;
	char		team_name[ACC_MAX_NAME_LEN];
	int32_t		default_grid_position;	/* -1 = unset */
	uint8_t		ballast_kg;		/* clamped 0..40 */
	float		restrictor;		/* normalized 0..0.99 */
	uint8_t		current_driver_index;
	uint8_t		driver_count;
	struct DriverInfo drivers[ACC_MAX_DRIVERS_PER_CAR];
	uint8_t		swap_state[ACC_MAX_DRIVERS_PER_CAR]; /* 0=idle..5=done */
	int		used;			/* slot occupied? */

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
	int		is_admin;
	int		is_spectator;
	int		hellbanned;	/* /hellban: drop inbound, skip in
					 * broadcasts.  Per-session only. */
	struct ByteBuf	rx;		/* incoming TCP byte stream */
	struct ByteBuf	tx;		/* outbound queue, drained on POLLOUT */
	size_t		tx_peak_bytes;	/* max queue depth ever observed */
	uint64_t	tx_warn_ms;	/* rate-limit soft-cap warnings */
	unsigned char	*hs_echo;	/* raw handshake body to echo in trailer */
	size_t		 hs_echo_len;
	uint32_t	keepalive_sent_ms;	/* server mono ms when last 0x14 sent */
	uint32_t	avg_rtt_ms;		/* exponential avg round-trip (from 0x16 pong) */
	int32_t		clock_offset_ms;	/* server_now - (rtt/2 + client_ts) */
	uint32_t	last_pong_client_ts;	/* client_ts from most recent 0x16 pong */
};

/*
 * Global server state.
 */
struct Server {
	/* config */
	int		tcp_port;
	int		udp_port;
	int		max_connections;
	int		lan_discovery;
	/*
	 * UDP port on 127.0.0.1 for 0xbe periodic state snapshots.  0 =
	 * disabled (default).  Mirrors accServer.exe's optional stats
	 * push channel (FUN_14002e8d0 gates on the short at +0x112).
	 * Intended for local monitoring tools; never routed off-host.
	 */
	int		stats_udp_port;
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
	char		track[ACC_TRACK_NAME_LEN];
	int		ignore_premature_disconnects;
	int		dump_leaderboards;
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
	 * useAsyncLeaderboard from settings.json (default 1).  The exe
	 * gates whether the 0x36 leaderboard broadcast is deferred to
	 * a CONCRT worker.  We have no worker, so in async mode we
	 * coalesce: broadcast only on CADENCE_LEADERBOARD ticks, not
	 * on every standings_seq bump.  In sync mode we broadcast on
	 * every standings change plus the cadence tick.
	 */
	uint8_t		use_async_leaderboard;

	/*
	 * unsafeRejoin from settings.json (default 1 = allow late joins).
	 * Mirrors the exe's +0x228 byte read in FUN_140117300 and gates
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
	struct AssistRules	assist;
	struct BanList		bans;
	uint8_t			bop_version;
	uint16_t		pre_race_waiting_s; /* preRaceWaitingTimeSeconds */
	uint16_t		session_overtime_s; /* sessionOverTimeSeconds */
	uint32_t		driver_stint_time_s; /* eventRules.driverStintTime*60 (0 = no limit) */
	uint8_t			mandatory_pit_count; /* eventRules.mandatoryPitstopCount (0 = none) */
	char			cfg_dir[256];	/* for saving bans */

	/* timing */
	uint64_t	tick_count;
	uint64_t	session_start_ms;

	/* Per-steam_id rating ledger (see ratings.c). */
	struct RatingEntry	ratings[ACC_RATINGS_MAX];
	uint8_t			ratings_dirty;
	uint64_t		ratings_last_emit_ms;
};

void	server_init(struct Server *s);
void	server_free(struct Server *s);

/* Allocate a new Conn for an accepted fd.  Returns NULL on full. */
struct Conn *
	conn_new(struct Server *s, int fd, const struct sockaddr_in *peer);

/* Drop a connection: close the fd, free the buffers, slot returns
 * to the free pool. */
void	conn_drop(struct Server *s, struct Conn *c);

/* Find a connection by its (server-assigned) conn_id. */
struct Conn *
	server_find_conn(struct Server *s, uint16_t conn_id);

/*
 * Allocate a free CarEntry slot and return its index, or -1 if
 * the entry list is full.  car_id matches the index by design.
 */
int	server_alloc_car(struct Server *s);

/*
 * Pick the next grid slot for a joining car: max existing +1 if
 * that fits, else walk back from max_connections-1 looking for
 * an unoccupied slot.  Returns -1 if the grid is full.  Mirrors
 * accServer.exe FUN_140021090.
 */
int	server_find_grid_slot(struct Server *s);

#endif /* ACCD_STATE_H */
