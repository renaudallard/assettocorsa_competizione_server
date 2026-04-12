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
#include "msg.h"

#define ACC_MAX_BANS		256
#define ACC_MAX_CARS		64
#define ACC_CAR_ID_BASE		1001
#define ACC_MAX_DRIVERS_PER_CAR	4
#define ACC_MAX_NAME_LEN	64
#define ACC_TRACK_NAME_LEN	48
#define ACC_MAX_SESSIONS	16
#define ACC_MAX_PENALTIES	8

/*
 * Session phase machine.
 *
 * Mirrors the per-session lifecycle defined in HB §IV.  Each
 * configured session in event.json runs through PRE_SESSION ->
 * STARTING -> SESSION (one of P/Q/R) -> POST_SESSION before the
 * server advances to the next session, and finally RESULTS at
 * the end of the weekend.
 */
enum session_phase {
	PHASE_NONE = 0,
	PHASE_PRE_SESSION,
	PHASE_STARTING,
	PHASE_PRACTICE,
	PHASE_QUALIFYING,
	PHASE_PRE_RACE,
	PHASE_RACE,
	PHASE_POST_SESSION,
	PHASE_RESULTS
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

struct PenaltyEntry {
	uint8_t		kind;		/* enum penalty_kind */
	uint8_t		collision;	/* /tp5c vs /tp5 */
	uint8_t		served;
	int32_t		laps_remaining;	/* drive-through countdown */
	uint64_t	issued_ms;
};

struct PenaltyQueue {
	struct PenaltyEntry	slots[ACC_MAX_PENALTIES];
	uint8_t			count;
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
	uint8_t		in_pit;
	uint8_t		pit_crossing_latched;
	uint8_t		mandatory_pit_served;
	uint8_t		current_tyres;
	uint8_t		out_of_track_latched;
	struct PenaltyQueue	pen;
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
	float		grip_level;
	uint32_t	standings_seq;	/* bumps on leaderboard change */
	uint32_t	last_standings_seq;	/* tick.c: detect changes */
	uint8_t		last_phase;		/* tick.c: detect transitions */
	int		results_written;	/* one-shot guard */
	int		grid_announced;		/* one-shot guard */
};

/*
 * Per-server weather snapshot.
 */
struct WeatherStatus {
	float		clouds;		/* 0..1 */
	float		current_rain;	/* 0..1 */
	float		target_rain;	/* 0..1 */
	float		wetness;	/* 0..1 */
	float		dry_line_wetness;
	float		puddles;
	float		forecast_10m;
	float		forecast_30m;
	uint64_t	last_step_ms;
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
	struct ByteBuf	rx;		/* incoming TCP byte stream */
	struct ByteBuf	tx;		/* not yet used; for batched sends */
	unsigned char	*hs_echo;	/* raw handshake body to echo in trailer */
	size_t		 hs_echo_len;
	uint32_t	keepalive_sent_ms;	/* server mono ms when last 0x14 sent */
	uint32_t	avg_rtt_ms;		/* exponential avg round-trip (from 0x16 pong) */
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
	char		server_name[ACC_MAX_NAME_LEN];
	char		password[64];
	char		admin_password[64];
	char		spectator_password[64];
	char		track[ACC_TRACK_NAME_LEN];
	int		ignore_premature_disconnects;
	int		dump_leaderboards;
	int		force_entry_list;

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
	char			cfg_dir[256];	/* for saving bans */

	/* timing */
	uint64_t	tick_count;
	uint64_t	session_start_ms;
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

#endif /* ACCD_STATE_H */
