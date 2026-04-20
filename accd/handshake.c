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
 * handshake.c -- ACP_REQUEST_CONNECTION parser and 0x0b response.
 *
 * The 0x09 request body, after the msg id byte, contains:
 *
 *     u16          client_version    (must == ACC_PROTOCOL_VERSION)
 *     string_a     password          (Format A)
 *     ... DriverInfo + CarInfo substructures ...
 *
 * Phase 1 only validates the first two fields and ignores the
 * trailing CarInfo until later phases.  This is enough to make
 * the server respond with either accept or reject.
 *
 * The 0x0b response body is:
 *
 *     u8           msg_id = 0x0b
 *     u16          server protocol version
 *     u8           server flags        (0 for now)
 *     u16          connection_id       (0xFFFF on reject)
 *     ... welcome trailer on accept ...
 *
 * For phase 1 we send the minimum-viable trailer documented in
 * §5.6.4c: carId + trackName + eventId + 0 sessions + empty
 * sub-records + 0 cars.  This is enough for some clients to
 * proceed; if the real client demands more we'll fix it then.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "bcast.h"
#include "ratings.h"
#include "bans.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "penalty.h"
#include "prim.h"
#include "session.h"
#include "state.h"
#include "weather.h"

/*
 * Walk a DriverInfo blob inside hs_echo and return its length.
 * Layout (from FUN_14011cea0): 5 Format-A wstrings + 41 fixed bytes
 * + 1 Format-A wstring (long steam_id).  Each Format-A wstring is
 * u8(len) + len*4 bytes.  Returns 0 on parse error.
 */
static size_t
parse_driverinfo_len(const unsigned char *buf, size_t len)
{
	size_t pos = 0;
	int i;

	for (i = 0; i < 5; i++) {
		if (pos >= len)
			return 0;
		pos += 1 + (size_t)buf[pos] * 4;
		if (pos > len)
			return 0;
	}
	pos += 41;
	if (pos > len)
		return 0;
	if (pos >= len)
		return 0;
	pos += 1 + (size_t)buf[pos] * 4;
	if (pos > len)
		return 0;
	return pos;
}

/*
 * Write the 104-byte SeasonEntity block that follows the per-car
 * spawnDefs in the welcome trailer.  Layout from FUN_14011e2a0
 * (SeasonEntity::writeToBuf) in accServer.exe:
 *
 *   HudRules      (7 u8)
 *   AssistRules   (2 u8 flags + 2 f32 + 6 u8 flags)
 *   GraphicsRules (6 u8)
 *   RealismRules  (2 u8 + f32 grip + u8 + f32 fuel + f32 tyre + 9 u8)
 *   GameplayRules (5 u8 + u32)
 *   OnlineRules   (13 u8 including u16 post_race_seconds)
 *   RaceDirectorRules (3 u8 + 4 u32)
 *   5 x u16 vector counts (4 empty + 1 event_count=1)
 *
 * Most values are server-wide defaults shared by all Kunos setups;
 * assist/online fields come from the AssistRules and server config.
 */
static int
write_season_entity(struct ByteBuf *bb, struct Server *s)
{
	/* HudRules: 1 configured slot + 6 unset sentinels. */
	if (wr_u8(bb, 1) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;

	/* AssistRules. */
	if (wr_u8(bb, s->assist.disable_ideal_line ?
	    s->assist.disable_ideal_line : 2) < 0) return -1;
	if (wr_u8(bb, s->assist.disable_autosteer ?
	    s->assist.disable_autosteer : 2) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, s->assist.stability_control_max > 0
	    ? (float)s->assist.stability_control_max / 100.0f
	    : 1.0f) < 0) return -1;
	if (wr_u8(bb, s->assist.disable_auto_pit_limiter ?
	    s->assist.disable_auto_pit_limiter : 2) < 0) return -1;
	if (wr_u8(bb, s->assist.disable_auto_gear ?
	    s->assist.disable_auto_gear : 2) < 0) return -1;
	if (wr_u8(bb, s->assist.disable_auto_clutch ?
	    s->assist.disable_auto_clutch : 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;	/* disable_auto_engine */
	if (wr_u8(bb, 2) < 0) return -1;	/* disable_auto_wiper */
	if (wr_u8(bb, 2) < 0) return -1;	/* disable_auto_lights */

	/* GraphicsRules: stable defaults. */
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 5) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 5) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 4) < 0) return -1;

	/* RealismRules: observed defaults from captures (grip 0.8,
	 * fuel 1.0, tyre 0.5, 9 misc flags). */
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_f32(bb, 0.8f) < 0) return -1;
	if (wr_u8(bb, 1) < 0) return -1;
	if (wr_f32(bb, 1.0f) < 0) return -1;
	if (wr_f32(bb, 0.5f) < 0) return -1;
	if (wr_u8(bb, 1) < 0) return -1;
	if (wr_u8(bb, 1) < 0) return -1;
	if (wr_u8(bb, 1) < 0) return -1;
	if (wr_u8(bb, 1) < 0) return -1;
	if (wr_u8(bb, 1) < 0) return -1;
	if (wr_u8(bb, 1) < 0) return -1;
	if (wr_u8(bb, 1) < 0) return -1;
	if (wr_u8(bb, 1) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;

	/* GameplayRules: defaults (100, 100, 15). */
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 100) < 0) return -1;
	if (wr_u8(bb, 100) < 0) return -1;
	if (wr_u32(bb, 15) < 0) return -1;

	/*
	 * OnlineRules (14 bytes observed in Kunos capture2):
	 *   u8 u8 formationLapType shortFormationLap
	 *   u16 postRaceSeconds u8 weatherRandomness u8 trackMedals
	 *   6 x u8 flag (all 0x02)
	 */
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;	/* formationLapType */
	if (wr_u8(bb, 2) < 0) return -1;	/* shortFormationLap */
	if (wr_u16(bb, 300) < 0) return -1;	/* postRaceSeconds */
	if (wr_u8(bb, s->weather.randomness) < 0) return -1; /* weatherRandomness */
	if (wr_u8(bb, 3) < 0) return -1;	/* trackMedalsRequirement */
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;

	/*
	 * RaceDirectorRules (18 bytes observed):
	 *   5 x u8 + u32 tickrate + u32 + u32 + u8
	 */
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 2) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u32(bb, 100) < 0) return -1;
	if (wr_u32(bb, 3000) < 0) return -1;
	if (wr_u32(bb, 15) < 0) return -1;
	if (wr_u8(bb, 3) < 0) return -1;

	/* Empty alt-rule vectors (u16 count each), then one
	 * EventEntity follows so the count is 1. */
	if (wr_u16(bb, 0) < 0) return -1;
	if (wr_u16(bb, 0) < 0) return -1;
	if (wr_u16(bb, 0) < 0) return -1;
	if (wr_u16(bb, 0) < 0) return -1;
	if (wr_u16(bb, 1) < 0) return -1;
	return 0;
}

/*
 * EventEntity body after the str_a trackName (136 bytes).
 * The block mixes CircuitInfo, GraphicsInfo, CarSet, RaceRules
 * and WeatherRules.  Structural bytes (graphics indices, race
 * rule sentinels) come from a reference template; weather and
 * temperature fields are filled from live server state so the
 * EventEntity is valid for any track and weather configuration.
 */
static int
write_event_entity_rest(struct ByteBuf *bb, struct Server *s)
{
	float ambient, road, rain;
	int i;

	ambient = s->session.ambient_temp > 0
	    ? (float)s->session.ambient_temp : 22.0f;
	road = s->session.track_temp > 0
	    ? (float)s->session.track_temp : ambient + 4.0f;
	rain = s->weather.current_rain > 0
	    ? s->weather.current_rain : 0.0f;

	/* CircuitInfo header (3 u8 + 4 f32 = 19 bytes). */
	if (wr_u8(bb, 0x01) < 0) return -1;
	if (wr_u8(bb, 0x20) < 0) return -1;
	if (wr_u8(bb, 0x03) < 0) return -1;
	if (wr_f32(bb, 0.9f) < 0) return -1;
	if (wr_f32(bb, s->weather.clouds * 0.1f) < 0) return -1;
	if (wr_f32(bb, rain * 0.1f) < 0) return -1;
	if (wr_f32(bb, 1.0f) < 0) return -1;

	/*
	 * GraphicsInfo — 9 bytes of protocol-tier constants.
	 * Each pair is (current_tier, max_tier) where current=0 means
	 * "server doesn't dictate" and max_tier is the protocol
	 * version's upper bound.  Kunos bumps max_tier only in game
	 * patches, so these are genuine constants, not runtime state.
	 *   0x00 0x05  -- reflection quality (max 5)
	 *   0x00 0x05  -- shadow quality (max 5)
	 *   0x00 0x04  -- LOD quality (max 4)
	 *   u16 0      -- reserved
	 *   0xFF       -- anisotropic filtering = auto
	 */
	if (wr_u8(bb, 0x00) < 0) return -1;
	if (wr_u8(bb, 0x05) < 0) return -1;
	if (wr_u8(bb, 0x00) < 0) return -1;
	if (wr_u8(bb, 0x05) < 0) return -1;
	if (wr_u8(bb, 0x00) < 0) return -1;
	if (wr_u8(bb, 0x04) < 0) return -1;
	if (wr_u16(bb, 0x0000) < 0) return -1;
	if (wr_u8(bb, 0xFF) < 0) return -1;

	/*
	 * CarSet — vtable[0x20] = FUN_14011ccc0.  The serializer
	 * emits just `u16 count` of the CarSelection vector at
	 * CarSet+0x28..+0x30 (stride 0x138) followed by N ×
	 * CarEntity::writeToBuf entries.  EventEntity's default
	 * CarSet has an empty vector, so the wire payload is u16 0.
	 * We had skipped this sub-block entirely, which silently
	 * shifted every subsequent EventEntity byte by two on the
	 * wire — the client tolerated it because the following
	 * RaceRules/WeatherRules leading bytes are small constants
	 * that still parse to something, but the layout was
	 * mis-aligned vs the exe.
	 */
	if (wr_u16(bb, 0) < 0) return -1;

	/*
	 * RaceRules — 16-byte block written field-by-field, matching
	 * the exe's FUN_14011d230 wire serializer (vtable slot 0x20
	 * of the RaceRules sub-object at EventEntity+0xf8).  Field
	 * names pinned from JSON serializer FUN_14010e390.  Kunos
	 * struct offsets in the comment; widths match the wire's
	 * compact packing, which differs from the JSON / internal
	 * struct sizes (several int32 fields are truncated to u8/u16
	 * on the wire).  Unset fields use -1 / 0xFFFF as the sentinel.
	 */
	{
		int32_t stint_s = (int32_t)s->driver_stint_time_s;
		uint16_t stint_wire = stint_s > 0 && stint_s <= 0xfffe
		    ? (uint16_t)stint_s : (uint16_t)0xffff;
		uint8_t mandatory_pits = s->mandatory_pit_count;

		/* +0x28 qualifyStandingType (0=best lap, 1=superpole).
		 * Kunos default = 1; we don't implement superpole so
		 * keep the exe's default value. */
		if (wr_u8(bb, 0x01) < 0) return -1;
		/* +0x2c superpoleMaxCar, signed-int-to-u8 with 0xff as
		 * "unset" sentinel.  No superpole => -1. */
		if (wr_u8(bb, 0xff) < 0) return -1;
		/* +0x30 pitWindowLengthSec — u16 on wire, -1 unset. */
		if (wr_u16(bb, 0xffff) < 0) return -1;
		/* +0x34 driverStintTimeSec from eventRules.driverStintTime,
		 * truncated to u16, -1 when unset. */
		if (wr_u16(bb, stint_wire) < 0) return -1;
		/* +0x38 isRefuellingAllowedInRace (bool).  Default on. */
		if (wr_u8(bb, 0x01) < 0) return -1;
		/* +0x39 isRefuellingTimeFixed (bool).  Default off. */
		if (wr_u8(bb, 0x00) < 0) return -1;
		/* +0x3a maxDriversCount — u8 on wire. */
		if (wr_u8(bb, 0x01) < 0) return -1;
		/* +0x3c mandatoryPitstopCount — u8 on wire. */
		if (wr_u8(bb, mandatory_pits) < 0) return -1;
		/* +0x40 maxTotalDrivingTime — u16 on wire, -1 unset. */
		if (wr_u16(bb, 0xffff) < 0) return -1;
		/* +0x44..+0x46 three mandatory-pit boolean toggles. */
		if (wr_u8(bb, 0x00) < 0) return -1;
		if (wr_u8(bb, 0x00) < 0) return -1;
		if (wr_u8(bb, 0x00) < 0) return -1;
		/* Trailing tyreSetCount (u8) — default 1. */
		if (wr_u8(bb, 0x01) < 0) return -1;
	}

	/* WeatherRules header (4 u8 + 7 f32 = 32 bytes). */
	if (wr_u8(bb, 0x01) < 0) return -1;
	if (wr_u8(bb, 0x32) < 0) return -1;
	if (wr_u8(bb, 0x03) < 0) return -1;
	if (wr_u8(bb, 0x00) < 0) return -1;
	if (wr_f32(bb, ambient) < 0) return -1;
	if (wr_f32(bb, road) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, rain) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 1.0f) < 0) return -1;

	/* WeatherRules forecast table (15 f32 = 60 bytes). */
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, ambient) < 0) return -1;
	if (wr_f32(bb, -1.0f) < 0) return -1;
	if (wr_f32(bb, 5.0f) < 0) return -1;
	if (wr_f32(bb, 15.0f) < 0) return -1;
	if (wr_f32(bb, -1.0f) < 0) return -1;
	for (i = 0; i < 3; i++)
		if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, rain) < 0) return -1;
	if (wr_f32(bb, rain) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;

	return 0;
}

/*
 * session_mgr_state from FUN_140033890.
 *
 * Layout:
 *   u8 session_index (from +0x14122, NOT the phase)
 *   7 x per-session record (FUN_140035130, variable-length):
 *     u8 valid
 *     if valid: f32 (timestamp - base)
 *   23-byte tail (FUN_140034f60):
 *     u8 hour_of_day (+0x28)
 *     u8 0           (+0x2c)
 *     i8 time_multiplier - 1 (+0x30)
 *     f32 grip       (+0x34)
 *     u16 sched_field (+0x38)
 *     u32 duration_s  (+0x3c)
 *     u32 overtime_s  (+0x40)
 *     u8 0           (+0x44)
 *     u8 0           (+0x48)
 *     f32 1.0        (+0x4c)
 */
int
write_session_tail(struct ByteBuf *bb, const struct SessionDef *def,
    uint16_t session_overtime_s)
{
	uint16_t sched_field = def->session_type == 10 ? 80 : 3;
	uint32_t duration_s = (uint32_t)def->duration_min * 60u;

	if (wr_u8(bb, def->hour_of_day) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, def->session_type == 10 ? 1 : 0) < 0) return -1;
	if (wr_f32(bb, 1.0f) < 0) return -1;
	if (wr_u16(bb, sched_field) < 0) return -1;
	if (wr_u32(bb, duration_s) < 0) return -1;
	if (wr_u32(bb, session_overtime_s > 0 ? session_overtime_s : 120) < 0)
		return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, def->session_type) < 0) return -1;
	if (wr_f32(bb, 1.0f) < 0) return -1;
	return 0;
}

int
write_session_mgr_state(struct ByteBuf *bb, struct Server *s,
    uint32_t conn_client_ts, uint32_t conn_rtt)
{
	const struct SessionDef *def;
	int k;

	if (s->session_count == 0)
		return -1;
	def = &s->sessions[s->session.session_index];

	/* First byte: session index (NOT phase). */
	if (wr_u8(bb, s->session.session_index) < 0)
		return -1;

	/*
	 * 7 per-session-slot records (FUN_140035130).
	 * Each: u8 valid + conditional f32 timestamp.
	 *
	 * The exe computes per-connection:
	 *   (float)(schedule_ts - FUN_1400418b0(conn))
	 *   = (float)(ts - server_now + RTT/2 + client_ts)
	 *
	 * This gives an absolute timestamp in the CLIENT's
	 * game clock.  The client computes remaining time as:
	 *   remaining = f32_value - my_current_time
	 *
	 * conn_client_ts + conn_rtt/2 is the server's estimate
	 * of what the client's clock reads right now.
	 */
	if (s->session.ts_valid) {
		struct timespec _ts;
		double now;
		double client_adj;

		clock_gettime(CLOCK_MONOTONIC, &_ts);
		now = (double)_ts.tv_sec * 1000.0 +
		    (double)_ts.tv_nsec / 1000000.0;
		client_adj = (double)conn_client_ts +
		    (double)(conn_rtt / 2);

		/*
		 * Slots 0-5 valid + slot 6 invalid.  Kunos emits all
		 * six as soon as session_start has run, regardless of
		 * whether each boundary has been reached — the f32
		 * carries the scheduled time projected into the
		 * client's clock so the client can anticipate upcoming
		 * phase changes.  Verified against the
		 * kunos_wine_full_race capture (P session; all six
		 * slots valid at welcome 0x28).
		 */
		for (k = 0; k < 6; k++) {
			if (wr_u8(bb, 1) < 0) return -1;
			if (wr_f32(bb,
			    (float)((double)s->session.ts[k]
			    - now + client_adj)) < 0)
				return -1;
		}
		if (wr_u8(bb, 0) < 0) return -1;
	} else {
		/* No schedule yet: all slots invalid. */
		for (k = 0; k < 7; k++)
			if (wr_u8(bb, 0) < 0) return -1;
	}

	return write_session_tail(bb, def, s->session_overtime_s);
}

/*
 * assist_rules + leaderboard section from FUN_140034a40.
 *   u32 (0x7fffffff)
 *   u8 int_vec.count (= 3)
 *   3 x u32 (0x7fffffff)
 *   u8 cVar8 (= 0 when no session has a +0x204 >= 0)
 *   u16 entry_count
 *   per entry: FUN_140034210 leaderboard_record_appender_0x220
 *   u8 tail1 (= 0)
 *   u8 tail2 (= 0)
 */
int
write_leaderboard_section(struct ByteBuf *bb, struct Server *s)
{
	int j, d, nc = 0;
	uint8_t cvar8 = 0;
	int32_t sess_best_lap = INT32_MAX;
	int32_t sess_best_sec[3] = { INT32_MAX, INT32_MAX, INT32_MAX };

	/*
	 * Session-best counters scan ALL cars, including those whose
	 * slot is currently unused: conn_drop preserves race state
	 * on disconnect so a driver who set the fastest lap and
	 * then left still contributes to the 'session best' shown
	 * on the standings sidebar.  Entry count (nc) and cvar8
	 * still only consider live cars.
	 */
	for (j = 0; j < ACC_MAX_CARS; j++) {
		struct CarRaceState *r = &s->cars[j].race;

		/*
		 * Session-best scan gated on used && !disqualified.
		 * A DQ'd car keeps its personal best_lap_ms but that
		 * time should not contribute to the session best that
		 * every client sees.  Also skips unused slots whose
		 * best_lap_ms is stale from a prior session.
		 */
		if (s->cars[j].used && !r->disqualified) {
			if (r->best_lap_ms > 0 &&
			    r->best_lap_ms < sess_best_lap)
				sess_best_lap = r->best_lap_ms;
			for (d = 0; d < 3; d++)
				if (r->best_sectors_ms[d] > 0 &&
				    r->best_sectors_ms[d] < sess_best_sec[d])
					sess_best_sec[d] =
					    r->best_sectors_ms[d];
		}

		if (!s->cars[j].used)
			continue;
		nc++;
		if (r->formation_lap_done)
			cvar8 = 1;
	}

	/*
	 * Outer FUN_140034a40 prefix.
	 *   u32 session-best lap time ms (0x7fffffff = unset)
	 *   u8  3 (count of u32s following)
	 *   3 × u32: session-best sector splits ms (0x7fffffff = unset)
	 *   u8  cvar8 (any-car-active flag)
	 *   u16 entry count
	 *
	 * Kunos populates these as live counters across all entries.
	 * We compute them by scanning per-car best_lap / best_sectors.
	 */
	if (wr_u32(bb, sess_best_lap == INT32_MAX
	    ? 0x7FFFFFFFu : (uint32_t)sess_best_lap) < 0) return -1;
	if (wr_u8(bb, 3) < 0) return -1;
	for (d = 0; d < 3; d++)
		if (wr_u32(bb, sess_best_sec[d] == INT32_MAX
		    ? 0x7FFFFFFFu : (uint32_t)sess_best_sec[d]) < 0)
			return -1;
	if (wr_u8(bb, cvar8) < 0) return -1;
	if (wr_u16(bb, (uint16_t)nc) < 0) return -1;

	/*
	 * Emit records in ranked order — the client infers each car's
	 * position from the record order (the exe's leaderboard record
	 * carries no explicit position byte; FUN_140034a40 iterates a
	 * pre-sorted vector).  Cars with equal/unset positions fall
	 * back to car_id order.
	 */
	{
		int pos, emitted = 0;

		for (pos = 1; pos <= ACC_MAX_CARS && emitted < nc; pos++) {
			for (j = 0; j < ACC_MAX_CARS; j++) {
				if (!s->cars[j].used)
					continue;
				if (s->cars[j].race.position != pos)
					continue;
				if (write_car_leaderboard_record(bb,
				    &s->cars[j], cvar8) < 0)
					return -1;
				emitted++;
			}
		}
		/* Emit any stragglers whose position wasn't in [1..n]. */
		for (j = 0; j < ACC_MAX_CARS && emitted < nc; j++) {
			int16_t p = s->cars[j].race.position;
			if (!s->cars[j].used)
				continue;
			if (p >= 1 && p <= ACC_MAX_CARS)
				continue;
			if (write_car_leaderboard_record(bb,
			    &s->cars[j], cvar8) < 0)
				return -1;
			emitted++;
		}
	}

	/* assist_rules tail. */
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	return 0;
}

/*
 * Per-car leaderboard record, byte-exact to FUN_140034210 in
 * accServer.exe.  Extracted from write_leaderboard_section so the
 * 0x56 garage reply can append a single-car record at its tail the
 * same way the exe does.
 *
 * cvar8 controls the 1-byte gated block (only emitted when the
 * caller says any car has an active lap; for a single-car context
 * the caller typically passes the car's own formation_lap_done).
 */
int
write_car_leaderboard_record(struct ByteBuf *bb,
    const struct CarEntry *ec, uint8_t cvar8)
{
	const struct CarRaceState *race = &ec->race;
	const struct PenaltyQueue *pq = &race->pen;
	int pi, d;

	if (wr_u16(bb, ec->car_id) < 0) return -1;
	if (wr_u16(bb, (uint16_t)ec->race_number) < 0) return -1;
	/*
	 * FUN_140034210 writes u8 at car+0x58 (cupCategory) then u8 at
	 * car+0x5c (current_driver_index) — NOT car_model.  The client
	 * already has car_model from the spawnDef in 0x0b; repeating it
	 * here shifts the two following fields out of alignment on the
	 * HUD timing tower.
	 */
	if (wr_u8(bb, ec->cup_category) < 0) return -1;
	if (wr_u8(bb, ec->current_driver_index) < 0) return -1;
	if (wr_u16(bb, 0) < 0) return -1;

	if (pq->count > 0 && !pq->slots[0].served) {
		float remaining = (float)pq->slots[0].laps_remaining;
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_u16(bb, penalty_wire_value(pq->slots[0].kind,
		    pq->slots[0].reason)) < 0) return -1;
		if (wr_f32(bb, remaining) < 0) return -1;
	} else {
		if (wr_u8(bb, 0) < 0) return -1;
	}

	if (cvar8) {
		if (wr_u8(bb, race->formation_lap_done) < 0) return -1;
	}

	if (wr_u8(bb, pq->count) < 0) return -1;
	for (pi = 0; pi < pq->count; pi++)
		if (wr_i32(bb, (int32_t)penalty_wire_value(
		    pq->slots[pi].kind,
		    pq->slots[pi].reason)) < 0) return -1;

	{
		uint8_t dcount = ec->driver_count;
		if (dcount == 0) dcount = 1;
		if (dcount > ACC_MAX_DRIVERS_PER_CAR)
			dcount = ACC_MAX_DRIVERS_PER_CAR;
		if (wr_u8(bb, dcount) < 0) return -1;
		for (d = 0; d < dcount; d++) {
			const struct DriverInfo *dd = &ec->drivers[d];
			if (wr_str_a(bb, dd->steam_id) < 0) return -1;
			if (wr_str_a(bb, dd->short_name) < 0) return -1;
			if (wr_str_a(bb, dd->first_name) < 0) return -1;
			if (wr_str_a(bb, dd->last_name) < 0) return -1;
			if (wr_u8(bb, dd->driver_category) < 0) return -1;
			if (wr_u16(bb, dd->nationality) < 0) return -1;
		}
	}

	/*
	 * Six per-car fields after the driver list.  FUN_140034210 reads
	 * these at fixed car-struct offsets; we traced each to its
	 * source via FUN_1400f0090 (car → LeaderboardLine copier) and
	 * cross-referenced with case 0x2e/0x51 handlers:
	 *
	 *   +0x180  u16   reserved / zero in all observed captures
	 *   +0x1d4  u32   best-lap-ms
	 *   +0x1b0  u32   low 4 bytes of car_system u64 (FUN_1400142f0
	 *                 case 0x2e writes u64 at car+0x1b0)
	 *   +0x1f4  u16   lap count (u32 in struct, u16 truncated on wire)
	 *   +0x1f0  u32   race-time-ms
	 *   +0x1f8  u8    ELO, clamped to u8 (FUN_1400142f0 case 0x51
	 *                 writes u32 at car+0x1f8 on ACP_ELO_UPDATE)
	 *
	 * We used to put last_lap_ms at +0x1b0 and clamp lap_count to u8
	 * at +0x1f8, both of which wrote real timing data into wire slots
	 * the client reads as car-system + ELO.  Emit the correct
	 * semantics.  last_lap_ms has no dedicated slot in this record —
	 * the client carries it via 0x1b lap broadcasts instead.
	 */
	if (wr_u16(bb, 0) < 0) return -1;
	if (wr_u32(bb, race->best_lap_ms > 0
	    ? (uint32_t)race->best_lap_ms : 0x7FFFFFFFu) < 0) return -1;
	if (wr_u32(bb, (uint32_t)ec->last_sys_data) < 0) return -1;
	if (wr_u16(bb, (uint16_t)race->lap_count) < 0) return -1;
	if (wr_u32(bb, race->race_time_ms > 0
	    ? (uint32_t)race->race_time_ms : 0x7FFFFFFFu) < 0) return -1;
	if (wr_u8(bb, ec->last_elo < 0xff
	    ? (uint8_t)ec->last_elo : 0xff) < 0) return -1;

	{
		int si;
		int l1_n = 0;
		int l2_n;
		uint8_t wide_flag = 0;
		int32_t l2_buf[ACC_LAP_HISTORY];

		for (si = 0; si < 3; si++)
			if (race->sector_ms[si] > 0)
				l1_n = si + 1;

		if (race->lap_history_count == 0) {
			l2_n = 3;
			l2_buf[0] = (int32_t)0x7FFFFFFF;
			l2_buf[1] = (int32_t)0x7FFFFFFF;
			l2_buf[2] = (int32_t)0x7FFFFFFF;
		} else {
			int nh = race->lap_history_count < ACC_LAP_HISTORY
			    ? race->lap_history_count : ACC_LAP_HISTORY;
			int start = race->lap_history_count
			    <= ACC_LAP_HISTORY ? 0
			    : race->lap_history_count % ACC_LAP_HISTORY;
			int k;

			l2_n = nh;
			for (k = 0; k < nh; k++) {
				int idx = (start + k) % ACC_LAP_HISTORY;
				l2_buf[k] = race->lap_history_ms[idx];
			}
		}

		/*
		 * FUN_140034210 scans both sector lists; if ANY value
		 * >= 0x10000, it switches BOTH lists to u32 encoding.
		 * Otherwise each value is written as u16 capped at 0xffff.
		 * The sentinel 0x7FFFFFFF for empty laps forces wide mode
		 * naturally, so narrow mode only kicks in when every
		 * sector is a real sub-65 s split.
		 */
		for (si = 0; si < l1_n; si++)
			if ((uint32_t)race->sector_ms[si] >= 0x10000u)
				wide_flag = 1;
		for (si = 0; si < l2_n; si++)
			if ((uint32_t)l2_buf[si] >= 0x10000u)
				wide_flag = 1;

		if (wr_u8(bb, wide_flag) < 0) return -1;
		if (wr_u8(bb, (uint8_t)l1_n) < 0) return -1;
		for (si = 0; si < l1_n; si++) {
			uint32_t v = (uint32_t)race->sector_ms[si];
			if (wide_flag) {
				if (wr_u32(bb, v) < 0) return -1;
			} else {
				if (wr_u16(bb,
				    v >= 0x10000u ? 0xffffu
				    : (uint16_t)v) < 0) return -1;
			}
		}
		if (wr_u8(bb, (uint8_t)l2_n) < 0) return -1;
		for (si = 0; si < l2_n; si++) {
			uint32_t v = (uint32_t)l2_buf[si];
			if (wide_flag) {
				if (wr_u32(bb, v) < 0) return -1;
			} else {
				if (wr_u16(bb,
				    v >= 0x10000u ? 0xffffu
				    : (uint16_t)v) < 0) return -1;
			}
		}
	}

	/*
	 * Final two bytes: FUN_140034210 writes car+0x200 then
	 * car+0x201.  car+0x200 is the session-final latch (set only
	 * once a car has crossed the finish line in a completed
	 * session); car+0x201's semantic is unknown but also quiescent
	 * during normal play.  formation_lap_done (car+0x204) was
	 * already emitted in the cvar8-gated block above — don't
	 * repeat it here.
	 */
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	return 0;
}

/*
 * WeatherData::serialize body (FUN_14011e660, vtable[0x20] on the
 * WeatherData object stored at param_1[0x1410e]).  JSON counterpart
 * at vtable[0x18] = FUN_140113b00 pinned the field names.
 *
 * Wire layout (12 × u32 + two variable-length vectors):
 *
 *   +0x28  u32  isDynamic              (bool — weatherRandomness>0)
 *   +0x30  f32  ambientTemperatureMean
 *   +0x34  f32  windSpeed              (current)
 *   +0x38  f32  windSpeedMean
 *   +0x3c  f32  windSpeedDeviation
 *   +0x40  f32  windDirection          (current, degrees)
 *   +0x44  f32  windDirectionChange
 *   +0x48  u32  windHarmonic
 *   +0x4c  u32  nHarmonics
 *   +0x50  f32  weatherBaseMean
 *   +0x54  f32  weatherBaseDeviation
 *   +0x58  f32  variabilityDeviation
 *   (+0x2c skipped in both serializers)
 *   i16  list1_count
 *   list1_count × f32  sineCoefficients    (+0x60..+0x68 vector)
 *   i16  list2_count
 *   list2_count × f32  cosineCoefficients  (+0x78..+0x80 vector)
 *
 * The two lists are Fourier coefficients: the client reconstructs
 * the weather variability curve as
 *     value(t) = weatherBaseMean + SUM_k (sineCoef[k] * sin(k*w*t)
 *                                       + cosineCoef[k] * cos(k*w*t))
 * Empty lists + nHarmonics=0 == "no variability model, static
 * weather", which is what our deterministic sin/cos simulator
 * reduces to.  Populate the remaining current-state fields so the
 * HUD forecast page shows the right starting ambient / wind, and
 * leave the stochastic-model slots (means / deviations / harmonics)
 * at their quiescent values.
 */
int
write_trailer_weather_data(struct ByteBuf *bb, const struct Server *s)
{
	float ambient = s->session.ambient_temp > 0
	    ? (float)s->session.ambient_temp : 22.0f;
	float wind_speed = s->weather.wind_speed;
	float wind_dir = s->weather.wind_direction;
	uint32_t is_dynamic = s->weather.randomness > 0 ? 1 : 0;

	if (wr_u32(bb, is_dynamic) < 0) return -1;	/* +0x28 */
	if (wr_f32(bb, ambient) < 0) return -1;		/* +0x30 ambientMean */
	if (wr_f32(bb, wind_speed) < 0) return -1;	/* +0x34 wind now */
	if (wr_f32(bb, wind_speed) < 0) return -1;	/* +0x38 windMean */
	if (wr_f32(bb, 0.0f) < 0) return -1;		/* +0x3c windDev  */
	if (wr_f32(bb, wind_dir) < 0) return -1;	/* +0x40 */
	if (wr_f32(bb, 0.0f) < 0) return -1;		/* +0x44 chg  */
	if (wr_u32(bb, 0) < 0) return -1;		/* +0x48 windHarmonic */
	/*
	 * +0x4c nHarmonics — FUN_140116830 drives its Fourier
	 * synthesis off this.  nHarmonics==0 or empty sineCoef
	 * disables variability (static curve, matching our old
	 * behavior).  Expose at least one harmonic when the server
	 * is configured dynamic so the client's forecast page
	 * picks up accd's long-period sin/cos drift instead of
	 * flat-lining across the weekend.
	 */
	if (wr_u32(bb, is_dynamic ? 1u : 0u) < 0) return -1;
	/*
	 * +0x50 weatherBaseMean is the CLOUD/RAIN variability baseline,
	 * NOT a temperature.  We were emitting the ambient temperature
	 * (typically ~22.0), which made the client read a 22-unit cloud
	 * baseline on the forecast page.  For a static weather model
	 * with zero harmonics, the meaningful baseline is the current
	 * cloud level — that's what the variability curve oscillates
	 * around when dynamic mode flips on.
	 */
	if (wr_f32(bb, s->weather.clouds) < 0) return -1;	/* +0x50 */
	if (wr_f32(bb, 0.0f) < 0) return -1;		/* +0x54 baseDev  */
	if (wr_f32(bb, 0.0f) < 0) return -1;		/* +0x58 varDev   */
	/*
	 * Fourier coefficients.  FUN_140116830 treats sineCoef[0] as
	 * a per-day linear drift rate and uses sineCoef[k>=1] paired
	 * with cosineCoef[k-1] as oscillator amplitude/phase for
	 * harmonic k.  accd's weather.c drifts clouds and rain at
	 * ~0.1 amplitude over 24 h, so a single sineCoef[0] sized
	 * to match the randomness level gives the client a
	 * reasonable forecast curve.  Static weather emits no
	 * coefficients (same as the exe).
	 */
	if (is_dynamic) {
		float drift = 0.1f * (float)s->weather.randomness;
		if (wr_i16(bb, 1) < 0) return -1;
		if (wr_f32(bb, drift) < 0) return -1;
	} else {
		if (wr_i16(bb, 0) < 0) return -1;
	}
	if (wr_i16(bb, 0) < 0) return -1;	/* cosineCoeffs: empty */
	return 0;
}

/*
 * trailer_additional_state (FUN_1400330e0) — 68 bytes total
 * (17 f32).  Identical wire layout to the periodic 0x37 weather
 * broadcast — see weather_build_broadcast and the v1.10.2 capture
 * comment there.  The cockpit/HUD reads the welcome's value, the
 * external view picks up live 0x37, so keeping these in lockstep
 * matters or the two views show different weather.
 */
static int
write_trailer_additional_state(struct ByteBuf *bb, struct Server *s)
{
	float ambient, road;
	int dyn = s->weather.randomness > 0;
	float rain = dyn ? tanhf(tanhf(s->weather.current_rain) * 0.9f)
	    : s->weather.current_rain;
	float clouds = dyn ? tanhf(tanhf(s->weather.clouds) * 0.9f)
	    : s->weather.clouds;
	float wet = dyn ? tanhf(tanhf(s->weather.track_wetness) * 0.9f)
	    : s->weather.track_wetness;
	float dry = dyn ? tanhf(tanhf(s->weather.dry_line_wetness) * 0.9f)
	    : s->weather.dry_line_wetness;

	ambient = s->session.ambient_temp > 0
	    ? (float)s->session.ambient_temp : 22.0f;
	road = s->session.track_temp > 0
	    ? (float)s->session.track_temp : ambient + 4.0f;

	if (wr_f32(bb, 1.0f - clouds * 0.3f) < 0) return -1;
	/* Green-flag grip baseline; constant DAT_14014bcd8 = 0.96. */
	if (wr_f32(bb, 0.96f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, wet) < 0) return -1;
	if (wr_f32(bb, wet) < 0) return -1;

	if (wr_f32(bb, ambient) < 0) return -1;
	if (wr_f32(bb, road) < 0) return -1;
	if (wr_f32(bb, s->weather.wind_speed) < 0) return -1;
	if (wr_f32(bb, s->weather.wind_direction) < 0) return -1;
	if (wr_f32(bb, clouds) < 0) return -1;
	if (wr_f32(bb, rain) < 0) return -1;
	if (wr_f32(bb, dry) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;

	if (wr_f32(bb, (float)s->session.weekend_time_s) < 0)
		return -1;
	return 0;
}

/*
 * track_records vector (FUN_140033980 lines 166-247) — the list
 * of all configured sessions for the weekend.  One 23-byte record
 * per session; the byte layout matches the 0x70 session struct.
 */
static int
write_track_records(struct ByteBuf *bb, struct Server *s)
{
	int k;
	uint8_t n;

	n = (uint8_t)s->session_count;
	if (n == 0)
		n = 1;
	if (wr_u8(bb, n) < 0) return -1;

	for (k = 0; k < (int)n && k < ACC_MAX_SESSIONS; k++) {
		const struct SessionDef *def = &s->sessions[k];
		uint16_t sched_field =
		    def->session_type == 10 ? 80 : 3;
		uint32_t duration_s =
		    (uint32_t)def->duration_min * 60u;

		if (wr_u8(bb, def->hour_of_day) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, (uint8_t)(def->time_multiplier > 0
		    ? def->time_multiplier - 1 : 0)) < 0) return -1;
		if (wr_f32(bb, 1.0f) < 0) return -1;
		if (wr_u16(bb, sched_field) < 0) return -1;
		if (wr_u32(bb, duration_s) < 0) return -1;
		if (wr_u32(bb, s->session_overtime_s > 0
	    ? s->session_overtime_s : 120) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, def->session_type) < 0) return -1;
		if (wr_f32(bb, 1.0f) < 0) return -1;
	}
	return 0;
}

/*
 * MultiplayerTrackRecord::writeToPacket (FUN_14011da70) — 19 bytes.
 * No direct decomp; the field purpose is a "best lap record" for the
 * circuit (signed sentinels when none recorded).  Bytes 6..11 carry
 * the 0xffd0ffffffff sentinel verified against a v1.10.2 welcome
 * capture (resp_00_0x0b.bin).
 */
static int
write_mtr(struct ByteBuf *bb, struct Server *s)
{
	(void)s;
	if (wr_u32(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0xff) < 0) return -1;
	if (wr_u8(bb, 0xd0) < 0) return -1;
	if (wr_u32(bb, 0xffffffff) < 0) return -1;
	if (wr_u32(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	return 0;
}

/*
 * MultiplayerCommunityCompetitionRatingSeries (37 bytes).
 *   str_raw "Standard"
 *   str_raw ""
 *   u8 1 (ratingline.first_u8)
 *   24 x u8 zeros (ratingline.payload)
 */
static int
write_rating_series(struct ByteBuf *bb, struct Server *s)
{
	int k;

	(void)s;
	if (wr_str_raw(bb, "Standard") < 0) return -1;
	if (wr_str_raw(bb, "") < 0) return -1;
	if (wr_u8(bb, 1) < 0) return -1;
	for (k = 0; k < 24; k++)
		if (wr_u8(bb, 0) < 0) return -1;
	return 0;
}

/*
 * Emit one spawnDef for car slot `car_slot`.  Layout matches
 * FUN_140032c90 in accServer.exe:
 *
 *   u16 car_id (+0x150)
 *   u8  grid_slot + 1   (from +2 in session car struct)
 *   u8  grid_slot + 1   (from +3)
 *   CarInfo::writeToPacket (193 bytes from the stored handshake echo)
 *   u8  driver_count
 *   DriverInfo::writeToPacket x driver_count
 *   u8  active_driver_idx
 *   u64 timestamp
 *   u8  flag (+0x153)
 *   u8  flag (+0x152)
 *   5 x u8 car_dirt
 *   5 x u8 damage_zones
 *   u16 elo
 *   u32 stability
 *
 * Returns 0 on success, -1 if the car has no valid handshake echo
 * (in which case nothing is written and the caller should skip the
 * car or fail the containing message build).
 */
int
write_spawn_def(struct ByteBuf *bb, struct Server *s, int car_slot)
{
	struct CarEntry *ec;
	struct Conn *owner = NULL;
	size_t drv_len, ci_off, ci_len;
	int k;
	uint8_t slot1;

	if (car_slot < 0 || car_slot >= ACC_MAX_CARS)
		return -1;
	ec = &s->cars[car_slot];
	if (!ec->used)
		return -1;

	for (k = 0; k < ACC_MAX_CARS; k++) {
		if (s->conns[k] != NULL &&
		    s->conns[k]->car_id == car_slot) {
			owner = s->conns[k];
			break;
		}
	}
	if (owner == NULL || owner->hs_echo == NULL ||
	    owner->hs_echo_len == 0)
		return -1;

	drv_len = parse_driverinfo_len(owner->hs_echo,
	    owner->hs_echo_len);
	if (drv_len == 0 || drv_len + 8 > owner->hs_echo_len)
		return -1;
	ci_off = drv_len + 8;
	ci_len = owner->hs_echo_len - ci_off;

	slot1 = (uint8_t)(car_slot + 1);
	if (wr_u16(bb, ec->car_id) < 0) return -1;
	if (wr_u8(bb, slot1) < 0) return -1;
	/*
	 * FUN_140032c90 writes `car+0x3 + 1` here — the 1-based
	 * gridNumber.  The exe's own debug log confirms it:
	 *   "Assigning gridNumber %d to new carId %d".
	 * Using slot+1 instead made the race start position HUD
	 * number disagree with the actual pit/grid slot the client
	 * spawned the car on, visible when defaultGridPosition or a
	 * qualy archive overrode the natural slot order.
	 */
	{
		int16_t g = ec->race.grid_position;
		uint8_t grid_wire = (g >= 0 && g < 0xff)
		    ? (uint8_t)(g + 1) : slot1;
		if (wr_u8(bb, grid_wire) < 0) return -1;
	}

	if (bb_append(bb, owner->hs_echo + ci_off, ci_len) < 0)
		return -1;

	/*
	 * Driver list.  FUN_140032c90 iterates the entry's DriverInfo
	 * vector and serializes each via FUN_14011cea0:
	 *   5 × str_a (first, last, short, ???, ???)
	 *   + 41 fixed bytes (category, nationality, 12 ratings/flags)
	 *   + 1 × str_a (long steam_id)
	 *
	 * We have the connecting driver's full blob in hs_echo and
	 * emit it verbatim.  Additional drivers of a multi-driver
	 * entry (endurance) aren't part of the handshake — synthesize
	 * minimal but well-formed blobs from entrylist data so the
	 * client sees the full roster at pre-session.  The two
	 * unidentified wstring slots (positions 3-4 of the 5) are
	 * emitted empty; the rating fields are zero until live data
	 * arrives via driver-swap / ACP_ELO_UPDATE.
	 */
	{
		uint8_t dc = ec->driver_count > 0 ? ec->driver_count : 1;
		int di;

		if (dc > ACC_MAX_DRIVERS_PER_CAR)
			dc = ACC_MAX_DRIVERS_PER_CAR;
		if (wr_u8(bb, dc) < 0) return -1;
		if (bb_append(bb, owner->hs_echo, drv_len) < 0) return -1;
		for (di = 1; di < dc; di++) {
			const struct DriverInfo *dd = &ec->drivers[di];

			/*
			 * Five wstrings at DriverInfo +0x28/+0x48/+0x68/
			 * +0x88/+0xa8.  We know the first three (first,
			 * last, short) from our entrylist.  The final two
			 * are initialized to empty std::string by
			 * FUN_140041290 (DriverInfo ctor) so a fresh
			 * synthetic driver matches the exe's default.
			 */
			if (wr_str_a(bb, dd->first_name) < 0) return -1;
			if (wr_str_a(bb, dd->last_name) < 0) return -1;
			if (wr_str_a(bb, dd->short_name) < 0) return -1;
			if (wr_str_a(bb, "") < 0) return -1;
			if (wr_str_a(bb, "") < 0) return -1;

			/*
			 * 41 fixed bytes.  FUN_140041290 seeds each of
			 * these to specific non-zero defaults that the
			 * real-driver client carries through handshake;
			 * mirroring those defaults gives a synthetic
			 * driver the same resting rating shape as an exe-
			 * built one (category '\x52'='R' reserve tag, an
			 * 0x1f7/0x1f8 rating seed, etc.).  We previously
			 * zeroed the block, which looks like "rating 0"
			 * on the client's UI.
			 */
			if (wr_u8(bb, dd->driver_category
			    ? dd->driver_category : 0x52) < 0) return -1;
			if (wr_u16(bb, dd->nationality) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u32(bb, 0x1f7u) < 0) return -1;	/* +0xd0 */
			if (wr_u32(bb, 0x11u) < 0) return -1;	/* +0xd4 */
			if (wr_u32(bb, 0xf3u) < 0) return -1;	/* +0xd8 */
			if (wr_u8(bb, 0) < 0) return -1;	/* +0xdc */
			if (wr_u32(bb, 0) < 0) return -1;	/* +0xe0 lo */
			if (wr_u32(bb, 0) < 0) return -1;	/* +0xe4 hi */
			if (wr_u32(bb, 200u) < 0) return -1;	/* +0xe8 */
			if (wr_u32(bb, 0x1f8u) < 0) return -1;	/* +0xec */
			if (wr_u32(bb, 0xf3u) < 0) return -1;	/* +0xf0 */
			if (wr_u32(bb, 0x155u) < 0) return -1;	/* +0xf4 */
			if (wr_str_a(bb, dd->steam_id) < 0) return -1;
		}
	}

	/* spawnDef tail: active, u64 ts, 2 u8, 5 dirt, 5 damage,
	 * u16 elo, u32 stability.  All zero for a fresh spawn. */
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u32(bb, 0) < 0) return -1;
	if (wr_u32(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	for (k = 0; k < 5; k++)
		if (wr_u8(bb, 0) < 0) return -1;
	for (k = 0; k < 5; k++)
		if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u16(bb, 0) < 0) return -1;
	if (wr_u32(bb, 0) < 0) return -1;
	return 0;
}

int
build_welcome_trailer(struct ByteBuf *bb, struct Server *s, struct Conn *c)
{
	int i, j;

	(void)c;
	/* Server name + track name (raw strings). */
	if (wr_str_raw(bb, s->server_name) < 0)
		return -1;
	if (wr_str_raw(bb, s->track) < 0)
		return -1;

	/*
	 * SpawnDefs: one per connected car.  Layout per car (from
	 * FUN_140032c90 in accServer.exe):
	 *   u16 car_id, u8 flag1, u8 flag2,
	 *   CarInfo (193 bytes from handshake),
	 *   u8 driver_count,
	 *   DriverInfo (183 bytes from handshake) per driver,
	 *   u8 active_driver_idx,
	 *   u64 timestamp, u8 flag, u8 flag,
	 *   5 tire bytes, 5 damage bytes,
	 *   u16 elo, u32 stability.
	 *
	 * The handshake stores DriverInfo before CarInfo (with 8
	 * intermediate bytes); the spawnDef wants CarInfo first.
	 * We split hs_echo and emit in the correct order.
	 */
	{
		int nc = 0;

		for (j = 0; j < ACC_MAX_CARS; j++)
			if (s->cars[j].used)
				nc++;
		if (wr_u8(bb, (uint8_t)nc) < 0)
			return -1;
		for (j = 0; j < ACC_MAX_CARS; j++) {
			if (!s->cars[j].used)
				continue;
			if (write_spawn_def(bb, s, j) < 0)
				return -1;
		}
	}

	/* SeasonEntity common block (104 bytes). */
	if (write_season_entity(bb, s) < 0)
		return -1;

	/*
	 * EventEntity embedded block: str_a trackName followed by
	 * 136 bytes of circuit / graphics / carSet / race / weather
	 * configuration.  The Kunos SeasonEntity vector_counts field
	 * ends with "1" so exactly one EventEntity follows.
	 */
	if (wr_str_a(bb, s->track) < 0)
		return -1;
	if (write_event_entity_rest(bb, s) < 0)
		return -1;

	/* session_mgr_state (FUN_140033890). */
	if (write_session_mgr_state(bb, s, 0, 0) < 0)
		return -1;

	/* assist_rules + leaderboard (FUN_140034a40). */
	if (write_leaderboard_section(bb, s) < 0)
		return -1;

	/* WeatherData::serialize body (FUN_14011e660, vtable[0x20]). */
	if (write_trailer_weather_data(bb, s) < 0)
		return -1;

	/* trailer_additional_state (FUN_1400330e0) — 68 bytes. */
	if (write_trailer_additional_state(bb, s) < 0)
		return -1;

	/* track_records (u8 count + N x 23-byte session records). */
	if (write_track_records(bb, s) < 0)
		return -1;

	/* Two trailer bytes (0x82, 0x83 tyre compound markers). */
	if (wr_u8(bb, 5) < 0) return -1;
	if (wr_u8(bb, 5) < 0) return -1;

	/* MultiplayerTrackRecord::writeToPacket — 19 bytes. */
	if (write_mtr(bb, s) < 0)
		return -1;

	/*
	 * MultiplayerCommunityCompetitionRatingSeries — 37 bytes:
	 * str_raw "Standard", empty str_raw, u8(1), 24 zeros.
	 */
	if (write_rating_series(bb, s) < 0)
		return -1;

	/* Final trailer: u8(3) +0x1dc, u8(0), u8(0). */
	if (wr_u8(bb, 3) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;

	(void)i;
	(void)j;
	return 0;
}

/*
 * Send a 14-byte 0x0c reject matching accServer.exe FUN_14002db30:
 *
 *   u8  0x0c
 *   u8  reason   (see enum reject_reason)
 *   u32 sub      (reason-dependent subcode)
 *   u32 detail_a (reason-dependent, e.g. received client version
 *                 for wrong-version, current n_conns for full)
 *   u32 detail_b (reason-dependent, e.g. server expected version,
 *                 max slots for full)
 *
 * Previous accd wrote u8(0x0c)+u32(7)+u8(0)+u16+u16+u16+u16 which
 * was only byte-compatible with the exe for the wrong-version
 * case by coincidence.  A bad password got code 7 on the wire
 * instead of 6, so the ACC client showed the wrong error dialog.
 */
static int
handshake_send_reject(struct Conn *c, uint8_t reason,
    uint32_t sub, uint32_t detail_a, uint32_t detail_b)
{
	struct ByteBuf bb;
	int rc;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_STATE_RECORD_0C) < 0 ||
	    wr_u8(&bb, reason) < 0 ||
	    wr_u32(&bb, sub) < 0 ||
	    wr_u32(&bb, detail_a) < 0 ||
	    wr_u32(&bb, detail_b) < 0)
		goto fail;

	rc = conn_send_framed(c, bb.data, bb.wpos);
	bb_free(&bb);
	return rc;
fail:
	bb_free(&bb);
	return -1;
}

/*
 * Send a proactive 0x2e (car system) state-sync for every
 * already-connected car to the new connection `new_conn`.
 * Matches FUN_14002dcb0 in accServer.exe which iterates the
 * server's car list and, for each car other than the joiner's,
 * emits a TCP-framed `u8 0x2e + u16 car_id + u64 last_sys_data`.
 *
 * The new joiner uses these messages to populate per-car
 * damage / fuel / tyre state before any UDP 0x1e position
 * update arrives, so cars appear with the correct state at
 * the moment of spawn.
 */
static void
handshake_send_state_sync(struct Conn *new_conn, struct Server *s)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct CarEntry *car = &s->cars[i];
		struct ByteBuf out;

		if (!car->used)
			continue;
		if (i == new_conn->car_id)
			continue;

		bb_init(&out);
		if (wr_u8(&out, SRV_CAR_SYSTEM_RELAY) == 0 &&
		    wr_u16(&out, car->car_id) == 0 &&
		    wr_u64(&out, car->last_sys_data) == 0)
			(void)conn_send_framed(new_conn, out.data,
			    out.wpos);
		bb_free(&out);
	}
}

/*
 * Send a 0x0b accept response with the welcome trailer.
 * Header: u8(0x0b) + u16(udp_port) + u8(0x12) +
 * u16(conn_id) + u32(car_id).
 */
static int
handshake_send_accept(struct Conn *c, struct Server *s)
{
	struct ByteBuf bb;
	int rc;

	/*
	 * Header layout (from FUN_14001b820 in accServer.exe):
	 *   u8  0x0b (msg id)
	 *   u16 udp_port (from server+0x10)
	 *   u8  0x12 (from server+0x7d, always 0x12)
	 *   u16 conn_id (param_4)
	 *   u32 car_id (param_3, written by FUN_140033980 body)
	 */
	bb_init(&bb);
	if (wr_u8(&bb, SRV_HANDSHAKE_RESPONSE) < 0 ||
	    wr_u16(&bb, (uint16_t)s->udp_port) < 0 ||
	    wr_u8(&bb, 0x12) < 0 ||
	    wr_u16(&bb, c->conn_id) < 0 ||
	    wr_u32(&bb, (uint32_t)s->cars[c->car_id].car_id) < 0)
		goto fail;

	if (build_welcome_trailer(&bb, s, c) < 0)
		goto fail;

	rc = conn_send_framed(c, bb.data, bb.wpos);
	bb_free(&bb);
	if (rc < 0)
		return rc;

	/* Proactive state sync for already-connected cars. */
	handshake_send_state_sync(c, s);
	return 0;
fail:
	bb_free(&bb);
	return -1;
}

int
handshake_handle(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint16_t client_version;
	char *password = NULL;
	enum reject_reason reason = REJECT_OK;
	uint32_t reject_sub = 0, reject_a = 0, reject_b = 0;

	rd_init(&r, body, len);

	if (rd_u8(&r, &msg_id) < 0 || msg_id != ACP_REQUEST_CONNECTION) {
		log_warn("handshake: bad first byte 0x%02x from fd %d",
		    msg_id, c->fd);
		return -1;
	}
	if (rd_u16(&r, &client_version) < 0) {
		log_warn("handshake: short version from fd %d", c->fd);
		return -1;
	}
	if (client_version != ACC_PROTOCOL_VERSION) {
		log_info("rejecting new connection with wrong client "
		    "version %u (server runs %u)",
		    (unsigned)client_version,
		    (unsigned)ACC_PROTOCOL_VERSION);
		reason = client_version > 0xff
		    ? REJECT_VERSION_HI : REJECT_VERSION_LO;
		reject_a = client_version;
		reject_b = ACC_PROTOCOL_VERSION;
		goto reply;
	}
	if (rd_str_a(&r, &password) < 0) {
		log_warn("handshake: short password from fd %d", c->fd);
		return -1;
	}
	if (strcmp(password, s->password) != 0) {
		log_info("rejecting connection: bad password from fd %d",
		    c->fd);
		reason = REJECT_PASSWORD;
		goto reply;
	}
	/* nconns already includes this connection (incremented in
	 * conn_new at TCP accept time), so compare with > not >=. */
	if (s->nconns > s->max_connections) {
		log_info("rejecting connection: server full");
		reason = REJECT_FULL;
		reject_a = (uint32_t)s->nconns;
		reject_b = (uint32_t)s->max_connections;
		goto reply;
	}

	/*
	 * Save the raw handshake body (after password) for echoing
	 * in the welcome trailer.  The Kunos server re-serializes
	 * the parsed fields, but echoing the raw bytes is close
	 * enough for the client to accept.
	 */
	{
		size_t echo_len = rd_remaining(&r);

		/*
		 * Guard against malicious / malformed handshake bodies:
		 * a real ACC 0x09 is ~200 B for a single driver + CarInfo.
		 * 16 KiB is far beyond any legitimate payload and well
		 * below any DoS surface.
		 */
		if (echo_len > 16384)
			echo_len = 0;
		c->hs_echo = echo_len > 0 ? malloc(echo_len) : NULL;
		if (c->hs_echo != NULL) {
			memcpy(c->hs_echo, r.p, echo_len);
			c->hs_echo_len = echo_len;
		}
	}

	/*
	 * Parse DriverInfo and CarInfo from the handshake body.
	 *
	 * The real ACC client sends a richer format than simple test
	 * clients: DriverInfo carries 5 strings with has_more()
	 * guards, a 41-byte numeric block, then steam_id; CarInfo
	 * follows with dozens of customization fields.  We detect
	 * the format by packet size (real client ~456 bytes, simple
	 * client ~150 bytes) and parse accordingly.
	 */
	{
		char *first = NULL, *last = NULL, *sname = NULL;
		char *steam = NULL, *team = NULL;
		char *skip_str = NULL;
		char steam_buf[32] = "";
		uint8_t cat = 0;
		uint16_t nat = 0;
		int32_t rnum = 0;
		uint8_t cmodel = 0, ccup = 0;
		struct CarEntry *car;

		if (len > 200) {
			/*
			 * Real client format: 5 DriverInfo strings
			 * (first, aux, last, aux, short), 41-byte
			 * numeric block, steam_id, middle bytes,
			 * then full CarInfo.
			 */
			if (rd_can_str_a(&r))
				(void)rd_str_a(&r, &first);
			if (rd_can_str_a(&r)) {
				(void)rd_str_a(&r, &skip_str);
				free(skip_str); skip_str = NULL;
			}
			if (rd_can_str_a(&r))
				(void)rd_str_a(&r, &last);
			if (rd_can_str_a(&r)) {
				(void)rd_str_a(&r, &skip_str);
				free(skip_str); skip_str = NULL;
			}
			if (rd_can_str_a(&r))
				(void)rd_str_a(&r, &sname);

			/* 41-byte numeric block. */
			if (rd_remaining(&r) >= 41) {
				(void)rd_skip(&r, 16);
				(void)rd_u8(&r, &cat);
				(void)rd_skip(&r, 24);
			}

			/* steam_id (6th string). */
			if (rd_can_str_a(&r) &&
			    rd_str_a(&r, &steam) == 0 && steam != NULL)
				snprintf(steam_buf, sizeof(steam_buf),
				    "%s", steam);

			/* Skip middle bytes, parse CarInfo. */
			(void)rd_skip(&r, 8);
			(void)rd_skip(&r, 4);		/* carModelKey */
			(void)rd_skip(&r, 4);		/* teamGuid */
			(void)rd_i32(&r, &rnum);	/* raceNumber */
			(void)rd_skip(&r, 33);		/* skin fields */
			if (rd_can_str_a(&r)) {		/* customSkinName */
				(void)rd_str_a(&r, &skip_str);
				free(skip_str); skip_str = NULL;
			}
			(void)rd_skip(&r, 1);		/* bannerKey */
			if (rd_can_str_a(&r))		/* teamName */
				(void)rd_str_a(&r, &team);
			(void)rd_u16(&r, &nat);		/* nationality */
			if (rd_can_str_a(&r)) {		/* displayName */
				(void)rd_str_a(&r, &skip_str);
				free(skip_str); skip_str = NULL;
			}
			if (rd_can_str_a(&r)) {		/* competitorName */
				(void)rd_str_a(&r, &skip_str);
				free(skip_str); skip_str = NULL;
			}
			(void)rd_skip(&r, 3);		/* nat + templateKey */
			(void)rd_u8(&r, &cmodel);	/* carModelType */
			(void)rd_u8(&r, &ccup);
		} else {
			/*
			 * Simple format (probe / test client):
			 * 3 strings, u8 cat, u16 nat, steam_id,
			 * i32 rnum, u8 model, u8 cup, str_a team.
			 */
			(void)rd_str_a(&r, &first);
			(void)rd_str_a(&r, &last);
			(void)rd_str_a(&r, &sname);
			(void)rd_u8(&r, &cat);
			(void)rd_u16(&r, &nat);
			if (rd_str_a(&r, &steam) == 0 && steam != NULL)
				snprintf(steam_buf, sizeof(steam_buf),
				    "%s", steam);
			(void)rd_i32(&r, &rnum);
			(void)rd_u8(&r, &cmodel);
			(void)rd_u8(&r, &ccup);
			(void)rd_str_a(&r, &team);
		}

		/* Ban check. */
		if (bans_contains(&s->bans, steam_buf)) {
			log_info("rejecting banned steam_id %s", steam_buf);
			reason = REJECT_BANNED;
			free(first); free(last); free(sname);
			free(steam); free(team);
			goto reply;
		}

		/*
		 * Quick-reconnect detection (FUN_140025690 in accServer.exe,
		 * logs "Removed connection due to (quick) reconnect").  If an
		 * already-authenticated peer has this steam_id, detach it
		 * from its car slot and mark the old conn for drop, then
		 * bind the new conn to that same slot so the driver's
		 * race state and grid position survive the reconnect.
		 * Done before the unsafeRejoin race-phase gate so returning
		 * drivers can always rejoin.
		 */
		{
			int reconnect_slot = -1;

			if (steam_buf[0] != '\0') {
				int j;

				/* Live-conn match: driver came back before
				 * we noticed the old socket died. */
				for (j = 0; j < ACC_MAX_CARS; j++) {
					struct Conn *old = s->conns[j];
					struct CarEntry *oc;

					if (old == NULL || old == c)
						continue;
					if (old->car_id < 0 ||
					    old->car_id >= ACC_MAX_CARS)
						continue;
					oc = &s->cars[old->car_id];
					if (oc->driver_count == 0)
						continue;
					if (strcmp(oc->drivers[0].steam_id,
					    steam_buf) != 0)
						continue;
					log_info("Removed connection due to "
					    "(quick) reconnect: conn=%u "
					    "for %s",
					    (unsigned)old->conn_id,
					    steam_buf);
					reconnect_slot = old->car_id;
					old->car_id = -1;
					old->state = CONN_DISCONNECT;
					break;
				}

				/* Zombie-slot match: old conn is gone but
				 * CarEntry still holds the driver's data
				 * (conn_drop preserves driver_count /
				 * drivers[] / race state but clears used=0
				 * so the slot can be reallocated).  We
				 * ignore used here and match purely on
				 * steam_id, then re-claim the slot so the
				 * returning driver keeps race state, grid
				 * position, penalties, and lap history
				 * across a session transition too. */
				if (reconnect_slot < 0) {
					int k;
					for (k = 0; k < ACC_MAX_CARS; k++) {
						struct CarEntry *ec =
						    &s->cars[k];
						int dj, held = 0, cc;

						if (ec->driver_count == 0)
							continue;
						for (dj = 0;
						    dj < ec->driver_count;
						    dj++) {
							if (strcmp(ec->
							    drivers[dj].
							    steam_id,
							    steam_buf) == 0)
								break;
						}
						if (dj >= ec->driver_count)
							continue;
						for (cc = 0;
						    cc < ACC_MAX_CARS;
						    cc++) {
							struct Conn *cn =
							    s->conns[cc];
							if (cn != NULL &&
							    cn != c &&
							    cn->car_id == k) {
								held = 1;
								break;
							}
						}
						if (held)
							continue;
						log_info("Recognized "
						    "reconnect (zombie "
						    "slot %d): carId %d "
						    "raceNumber #%d for %s",
						    k, k,
						    ec->race_number,
						    steam_buf);
						reconnect_slot = k;
						break;
					}
				}
			}
			if (reconnect_slot >= 0) {
				c->car_id = reconnect_slot;
				log_info("Recognized reconnect: carId %d "
				    "raceNumber #%d",
				    c->car_id,
				    s->cars[c->car_id].race_number);
				goto post_slot_assignment;
			}
		}

		/*
		 * Mid-race / late-qualy / locked-prep gate
		 * (FUN_140025690 bVar46/bVar3/bVar4 paths).  Fresh 0x09
		 * handshakes past the reconnect shortcut are rejected with
		 * 0x0c code 12 under any of:
		 *
		 *   (a) unsafeRejoin=0 and an active race is in progress
		 *       (phase FORMATION..OVERTIME, session_type==10);
		 *   (b) the qualy session is in OVERTIME — the grid is
		 *       already locking and new drivers would corrupt the
		 *       finishing order (exe's late-qualy path using
		 *       param_1[0x14180]);
		 *   (c) the operator set preparation_locked via /lockprep
		 *       and the current session is still in FORMATION or
		 *       PRE_SESSION.
		 *
		 * Reconnects skip all three via post_slot_assignment.
		 */
		if (s->session_count > 0) {
			uint8_t stype = s->sessions[s->session.session_index]
			    .session_type;
			const char *why = NULL;

			if (!s->unsafe_rejoin && stype == 10 &&
			    (s->session.phase == PHASE_FORMATION ||
			     s->session.phase == PHASE_PRE_SESSION ||
			     s->session.phase == PHASE_SESSION ||
			     s->session.phase == PHASE_OVERTIME))
				why = "unsafeRejoin=0 and race in progress";
			else if (stype == 4 &&
			    s->session.phase == PHASE_COMPLETED)
				/*
				 * "Late qualy" in the exe (FUN_140025690
				 * bVar46 path) fires when session_type==4
				 * AND server+0x14180 > 0.0.  That field is
				 * the post-session aftercare deadline,
				 * written in FUN_14002f710 right after the
				 * 0x3e results broadcast and cleared when
				 * the exe advances to the next session.
				 * Our PHASE_COMPLETED window matches that
				 * deadline exactly: results written, waiting
				 * on ts[6] aftercare before PHASE_ADVANCE.
				 */
				why = "late qualy (results broadcast, "
				    "awaiting aftercare)";
			else if (s->preparation_locked &&
			    (s->session.phase == PHASE_FORMATION ||
			     s->session.phase == PHASE_PRE_SESSION))
				why = "locked preparation phase";

			if (why != NULL) {
				log_info("Rejected driver %s, this server "
				    "does not accept connections during "
				    "%s (phase %s)", steam_buf, why,
				    session_phase_name(s->session.phase));
				reason = REJECT_BAD_SESSION;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
		}

		/*
		 * Entry list enforcement: if forceEntryList is set,
		 * look up the client's steam_id in the preloaded
		 * entries. Assign them to the matching slot, or
		 * reject if not found.
		 */
		if (s->force_entry_list) {
			int slot = -1, i;

			for (i = 0; i < ACC_MAX_CARS &&
			    i < s->max_connections; i++) {
				struct CarEntry *ec = &s->cars[i];
				int dj;

				for (dj = 0; dj < ec->driver_count; dj++) {
					if (strcmp(ec->drivers[dj].steam_id,
					    steam_buf) == 0) {
						slot = i;
						break;
					}
				}
				if (slot >= 0)
					break;
			}
			if (slot < 0) {
				log_info("rejecting %s: not in entry list",
				    steam_buf);
				reason = REJECT_BAD_CAR;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
			if (s->cars[slot].used) {
				log_info("rejecting %s: entry slot %d "
				    "already in use", steam_buf, slot);
				reason = REJECT_FULL;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
			s->cars[slot].used = 1;
			c->car_id = slot;
		} else {
			c->car_id = server_alloc_car(s);
			if (c->car_id < 0) {
				reason = REJECT_FULL;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
		}

post_slot_assignment:
		/* Re-claim the slot on the used flag in case this was a
		 * zombie reclaim (conn_drop clears .used so the slot can
		 * be reallocated; reclaim must flip it back so the rest
		 * of the server sees the driver as active again). */
		s->cars[c->car_id].used = 1;
		/* Populate the car slot with parsed data. */
		car = &s->cars[c->car_id];
		if (first != NULL)
			snprintf(car->drivers[0].first_name,
			    sizeof(car->drivers[0].first_name), "%s",
			    first);
		if (last != NULL)
			snprintf(car->drivers[0].last_name,
			    sizeof(car->drivers[0].last_name), "%s",
			    last);
		if (sname != NULL)
			snprintf(car->drivers[0].short_name,
			    sizeof(car->drivers[0].short_name), "%s",
			    sname);
		car->drivers[0].driver_category = cat;
		car->drivers[0].nationality = nat;
		snprintf(car->drivers[0].steam_id,
		    sizeof(car->drivers[0].steam_id), "%s", steam_buf);
		if (car->driver_count == 0)
			car->driver_count = 1;

		/*
		 * entrylist isServerAdmin: auto-elevate this conn to admin
		 * without requiring /admin <pw>.  Matches the exe's
		 * FUN_140018390 which sets conn->admin byte when the entry's
		 * +0x6e flag is non-zero on join.
		 */
		if (car->is_server_admin)
			c->is_admin = 1;

		/*
		 * Only override car fields from the handshake if the
		 * entry list did not pre-populate them.
		 */
		if (!s->force_entry_list) {
			car->race_number = rnum;
			car->car_model = cmodel;
			car->cup_category = ccup;
			if (team != NULL)
				snprintf(car->team_name,
				    sizeof(car->team_name), "%s", team);
		}

		/*
		 * Grid-position assignment.  entrylist.json
		 * defaultGridPosition wins when set; otherwise pick the
		 * next free slot per FUN_140021090 (server_find_grid_slot).
		 */
		if (car->default_grid_position > 0) {
			car->race.grid_position =
			    (int16_t)car->default_grid_position;
		} else {
			int g = server_find_grid_slot(s);
			if (g >= 0)
				car->race.grid_position = (int16_t)g;
		}

		free(first);
		free(last);
		free(sname);
		free(steam);
		free(team);
	}

	c->state = CONN_AUTH;
	{
		struct CarEntry *lcar = &s->cars[c->car_id];
		struct DriverInfo *ldrv = &lcar->drivers[0];
		int j, n = 0;

		log_info("handshake accepted: fd=%d conn_id=%u car_id=%d "
		    "race#=%d model=%u",
		    c->fd, (unsigned)c->conn_id, c->car_id,
		    lcar->race_number, (unsigned)lcar->car_model);
		log_debug("  driver: \"%s\" \"%s\" [%s] cat=%u steam=%s",
		    ldrv->first_name, ldrv->last_name,
		    ldrv->short_name,
		    (unsigned)ldrv->driver_category, ldrv->steam_id);
		for (j = 0; j < ACC_MAX_CARS; j++)
			if (s->cars[j].used) n++;
		lobby_notify_drivers_changed(&s->lobby, (uint8_t)n);
	}

reply:
	free(password);
	if (reason != REJECT_OK) {
		log_debug("handshake reject: reason=%d sub=%u a=%u b=%u "
		    "client_ver=0x%04x fd=%d",
		    (int)reason, (unsigned)reject_sub,
		    (unsigned)reject_a, (unsigned)reject_b,
		    (unsigned)client_version, c->fd);
		if (handshake_send_reject(c, (uint8_t)reason, reject_sub,
		    reject_a, reject_b) < 0)
			return -1;
		return -1;	/* close connection after reject */
	}
	if (handshake_send_accept(c, s) < 0)
		return -1;
	log_debug("handshake accept sent: conn=%u udp_port=%d",
	    (unsigned)c->conn_id, s->udp_port);

	/*
	 * Recompute standings now that the new car has been added
	 * so the server-tick loop notices the standings_seq bump
	 * on its next pass and broadcasts a fresh 0x36 leaderboard
	 * to every connected client.  Without this, existing peers
	 * never learn that the new player's car joined the session
	 * and their "N/N" UI stays stuck at 1/1.
	 */
	session_recompute_standings(s);
	/* Force a leaderboard rebroadcast even if positions didn't
	 * change — the car count changed which is enough for clients
	 * to update their N/N display. */
	s->session.standings_seq++;

	/*
	 * After a successful accept, fan out 0x2e new-client-
	 * joined notify to every OTHER already-connected client.
	 * This lets them add the joining car to their local entry
	 * list and display it in the lobby.  The binary also emits
	 * a paired 0x4f sub-opcode 1 message right after; we do
	 * the same.
	 */
	{
		struct ByteBuf notify;
		uint64_t timestamp_ms;
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		timestamp_ms = (uint64_t)ts.tv_sec * 1000ull +
		    (uint64_t)ts.tv_nsec / 1000000ull;

		/*
		 * Notify already-connected clients that a new car
		 * joined.  Wire is 0x2e u8 + u16 carId + u64
		 * system_data (11 bytes), same layout as the regular
		 * ACP_CAR_SYSTEM_UPDATE relay — see NOTEBOOK_B §5.6.4a.
		 * The new car's system_data is 0 until the joining
		 * client sends its first 0x2e; send that 0 rather than
		 * substituting the server wall-clock as we used to.
		 */
		bb_init(&notify);
		if (wr_u8(&notify, SRV_CAR_SYSTEM_RELAY) == 0 &&
		    wr_u16(&notify, s->cars[c->car_id].car_id) == 0 &&
		    wr_u64(&notify,
			s->cars[c->car_id].last_sys_data) == 0)
			(void)bcast_all(s, notify.data, notify.wpos,
			    c->conn_id);
		bb_free(&notify);

		/* Paired 0x4f sub-opcode 1: u8 msg_id + u16 carId +
		 * u8 sub=1 + u64 timestamp (12 bytes). */
		bb_init(&notify);
		if (wr_u8(&notify, SRV_DRIVER_STINT_RELAY) == 0 &&
		    wr_u16(&notify, s->cars[c->car_id].car_id) == 0 &&
		    wr_u8(&notify, 1) == 0 &&
		    wr_u64(&notify, timestamp_ms) == 0)
			(void)bcast_all(s, notify.data, notify.wpos,
			    c->conn_id);
		bb_free(&notify);

		/*
		 * Post-accept welcome sequence matching the real
		 * server order: 0x28 + 0x36 + 0x37.
		 *
		 * Seed the schedule timestamps now so the first 0x28
		 * below carries valid per-session records (matches
		 * Kunos: the tick loop would otherwise not fire
		 * session_start until the NEXT iteration, leaving
		 * the welcome 0x28 with all 7 slots invalid).
		 */
		if (s->session.phase == PHASE_WAITING && s->nconns > 0 &&
		    !s->session.ts_valid)
			session_start(s);
		{
			struct ByteBuf wb;

			/*
			 * 0x28 SRV_LARGE_STATE_RESPONSE.
			 * Body is FUN_140033890: session_index +
			 * 7 variable-length per-session records +
			 * 23-byte tail.  Reuse write_session_mgr_state.
			 */
			bb_init(&wb);
			if (wr_u8(&wb, SRV_LARGE_STATE_RESPONSE) == 0 &&
			    write_session_mgr_state(&wb, s,
				c->last_pong_client_ts,
				c->avg_rtt_ms) == 0)
				(void)bcast_send_one(c, wb.data, wb.wpos);
			bb_free(&wb);

			/*
			 * 0x36 initial leaderboard snapshot.  Body is
			 * `u8 0x36 + write_leaderboard_section output`,
			 * matching FUN_14002f710 in accServer.exe which
			 * prefixes 0x36 onto the same FUN_140034a40
			 * assist_rules+leaderboard block emitted inside
			 * the welcome trailer.
			 */
			{
				struct ByteBuf lb;

				bb_init(&lb);
				if (wr_u8(&lb, SRV_LEADERBOARD_BCAST) == 0 &&
				    write_leaderboard_section(&lb, s) == 0)
					(void)bcast_send_one(c, lb.data,
					    lb.wpos);
				bb_free(&lb);
			}

			/* 0x37 weather status. */
			bb_init(&wb);
			if (weather_build_broadcast(s, &wb) == 0)
				(void)bcast_send_one(c, wb.data, wb.wpos);
			bb_free(&wb);

			/* 0x4e rating summary. */
			/*
			 * 0x4e per-entry layout (kunos_wine_full_race 86-byte
			 * capture, one car, re-verified 2026-04-16):
			 *   u16 car_id
			 *   u8  0
			 *   u16 safety_rating     (×100, 0 if unset)
			 *   u16 trackmedal_rating (×100, 0 if unset)
			 *   i16 -1  (sentinel)
			 *   i16 -1  (sentinel)
			 *   str_a steam_id
			 * Previous spec had an extra u32 extra_rating before
			 * the steam_id — the capture does not contain it.
			 */
			{
				int j, nc = 0;
				int ok = 1;

				for (j = 0; j < ACC_MAX_CARS; j++)
					if (s->cars[j].used)
						nc++;
				bb_init(&wb);
				ok = wr_u8(&wb, SRV_RATING_SUMMARY) == 0;
				ok = ok && wr_u8(&wb, (uint8_t)nc) == 0;
				for (j = 0; j < ACC_MAX_CARS && ok; j++) {
					if (!s->cars[j].used)
						continue;
					ok = ok && wr_u16(&wb,
					    s->cars[j].car_id) == 0;
					ok = ok && wr_u8(&wb, 0) == 0;
					/*
					 * Welcome 0x4e per-car body is
					 * u16 car_id + u8 0 + u16 SA + u16 TR
					 * + u32 0xFFFFFFFF + u8 0 (14-byte
					 * single-car capture).  Real SA/TR
					 * values come from the local ratings
					 * ledger keyed by steam_id.
					 */
					{
						uint16_t sa = 5000, tr = 5000;
						const char *sid =
						    s->cars[j].drivers[0]
						    .steam_id;
						ratings_get(s, sid, &sa, &tr);
						ok = ok && wr_u16(&wb, sa) == 0;
						ok = ok && wr_u16(&wb, tr) == 0;
					}
					ok = ok && wr_i16(&wb, -1) == 0;
					ok = ok && wr_i16(&wb, -1) == 0;
					/*
					 * FUN_14002f710 tail of each 0x4e
					 * entry is str_a steam_id (via the
					 * generic string writer at 14004d390),
					 * not a u8 0.  The prior pad would be
					 * accepted as a zero-length str_a only
					 * by accident; real steam_ids are
					 * ~17-18 chars, and the rating HUD
					 * maps each per-entry body back to its
					 * driver by that id.
					 */
					ok = ok && wr_str_a(&wb,
					    s->cars[j].drivers[0].steam_id)
					    == 0;
				}
				if (ok)
					(void)bcast_all(s, wb.data,
					    wb.wpos, 0xFFFF);
				bb_free(&wb);
			}
		}

		log_debug("welcome sequence sent: 0x2e+0x4f bcast + "
		    "0x28+0x36+0x37+0x4e to conn=%u",
		    (unsigned)c->conn_id);
	}
	return 0;
}
