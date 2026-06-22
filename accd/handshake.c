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
#include "entrylist.h"
#include "handlers.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "penalty.h"
#include "prim.h"
#include "session.h"
#include "smpr.h"
#include "state.h"
#include "tick.h"
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
	if (wr_u8(bb, s->assist.disable_auto_engine_start ?
	    s->assist.disable_auto_engine_start : 2) < 0) return -1;
	if (wr_u8(bb, s->assist.disable_auto_wiper ?
	    s->assist.disable_auto_wiper : 2) < 0) return -1;
	if (wr_u8(bb, s->assist.disable_auto_lights ?
	    s->assist.disable_auto_lights : 2) < 0) return -1;

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
	if (wr_u8(bb, 2) < 0) return -1;	/* formationLapType (OnlineRules "inherit" sentinel — runtime value is in the +0x1dc tail, not here; Kunos wire shows 0x02 regardless of ServerSettings.formationLapType) */
	if (wr_u8(bb, 2) < 0) return -1;	/* shortFormationLap */
	if (wr_u16(bb, 300) < 0) return -1;	/* postRaceSeconds */
	if (wr_u8(bb, 10) < 0) return -1;	/* weatherRandomness (OnlineRules ctor constant — runtime value travels via EventConfig.weatherRandomness and the WeatherStatus isDynamic bit, not here) */
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
	    ? (float)s->session.ambient_temp : (float)ACC_DEFAULT_AMBIENT_C;
	road = s->session.track_temp > 0
	    ? (float)s->session.track_temp : ambient + 4.0f;
	rain = s->weather.current_rain > 0
	    ? s->weather.current_rain : 0.0f;

	/*
	 * CircuitInfo header (3 u8 + 4 f32 = 19 bytes).
	 *
	 * The 4 f32s are the per-track triple (formation_trigger_start,
	 * green_trigger_start, green_trigger_end) followed by a 1.0
	 * baseGrip constant.  The AC2 client uses the triple to render
	 * the formation 70 km/h zone and the leader-distance zone on the
	 * HUD; passing wrong values puts those zones at wrong norm_pos.
	 *
	 * The leading `01 20 03` is constant across every Kunos capture
	 * (brands_hatch, misano), likely a section-version / sector-count
	 * header rather than a per-track field.
	 */
	if (wr_u8(bb, 0x01) < 0) return -1;
	if (wr_u8(bb, 0x20) < 0) return -1;
	if (wr_u8(bb, 0x03) < 0) return -1;
	if (wr_f32(bb, s->formation_trigger_start) < 0) return -1;
	if (wr_f32(bb, s->green_trigger_start) < 0) return -1;
	if (wr_f32(bb, s->green_trigger_end) < 0) return -1;
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
	 * NO CarSet sub-block here.  The AC2 client EventEntity reader
	 * (FUN_1434f4390) does call vtable[0x28] on `param_1+0xb8`
	 * between GraphicsInfo and RaceRules, but any emit in this slot
	 * — even an empty `u16 0` — crashes the real client shortly
	 * after welcome receipt (confirmed at both v0.2.47 and v0.2.64
	 * attempts, symptom "Connection reset by peer" right after
	 * phase transitions).  The u16(0) reader-level theory is
	 * well-founded but something else in the client's post-welcome
	 * path is tightly coupled to zero CarSet bytes being present
	 * between GraphicsInfo and RaceRules — do not touch this until
	 * a reader trace shows the specific assertion that fires.
	 */

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
		uint16_t pit_window_wire =
		    s->pit_window_length_s > 0 &&
		    s->pit_window_length_s <= 0xfffe
		    ? (uint16_t)s->pit_window_length_s
		    : (uint16_t)0xffff;
		uint16_t max_drv_time_wire =
		    s->max_total_driving_time_s > 0 &&
		    s->max_total_driving_time_s <= 0xfffe
		    ? (uint16_t)s->max_total_driving_time_s
		    : (uint16_t)0xffff;

		/* +0x28 qualifyStandingType (0=best lap, 1=superpole). */
		if (wr_u8(bb, s->qualify_standing_type) < 0) return -1;
		/*
		 * +0x2c superpoleMaxCar — emit 0xff sentinel for "no
		 * superpole car limit", matching Kunos misano pcap (frame
		 * 46872 of kunos_misano_2players_full_session.pcapng).
		 * Pre-v0.3.3 we emitted 0xff; v0.3.3 (commit 59a9d15)
		 * switched to 0 on the theory that AC2 was rendering the
		 * "OBLIGATOIRE 255/0" widget with this field as numerator.
		 * Empirical pcap walk shows Kunos itself emits 0xff, so
		 * the widget's "255" must come from another (uninitialized)
		 * client field, and emitting 0 here pushes AC2 into "0
		 * cars allowed in superpole" — flagged INVALIDE on the
		 * race-requirements widget.  Restore the sentinel.
		 */
		if (wr_u8(bb, 0xff) < 0) return -1;
		/* +0x30 pitWindowLengthSec — u16 on wire, 0xffff unset. */
		if (wr_u16(bb, pit_window_wire) < 0) return -1;
		/* +0x34 driverStintTimeSec from eventRules.driverStintTime,
		 * truncated to u16, 0xffff unset. */
		if (wr_u16(bb, stint_wire) < 0) return -1;
		/* +0x38 isRefuellingAllowedInRace (bool). */
		if (wr_u8(bb, s->refuelling_allowed) < 0) return -1;
		/* +0x39 isRefuellingTimeFixed (bool). */
		if (wr_u8(bb, s->refuelling_time_fixed) < 0) return -1;
		/* +0x3a maxDriversCount — u8 on wire. */
		if (wr_u8(bb, s->max_drivers_count) < 0) return -1;
		/* +0x3c mandatoryPitstopCount — u8 on wire. */
		if (wr_u8(bb, mandatory_pits) < 0) return -1;
		/* +0x40 maxTotalDrivingTime — u16 on wire, 0xffff unset. */
		if (wr_u16(bb, max_drv_time_wire) < 0) return -1;
		/* +0x44..+0x46 mandatory-pit sub-flags. */
		if (wr_u8(bb, s->pit_refuelling_required) < 0) return -1;
		if (wr_u8(bb, s->pit_tyre_change_required) < 0) return -1;
		if (wr_u8(bb, s->mandatory_swap_required) < 0) return -1;
		/* Trailing tyreSetCount (u8). */
		if (wr_u8(bb, s->tyre_set_count) < 0) return -1;
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
 *     u8 dateMinute  (+0x2c)
 *     u8 raceDay - 1 (+0x30)
 *     f32 timeMultiplier (+0x34)
 *     u16 sched_field (+0x38)
 *     u32 duration_s  (+0x3c)
 *     u32 overtime_s  (+0x40)
 *     u8 0           (+0x44)
 *     u8 sessionType (+0x48)
 *     f32 dynamicTrackMultiplier (+0x4c)
 */
int
write_session_tail(struct ByteBuf *bb, const struct SessionDef *def,
    uint16_t session_overtime_s)
{
	uint16_t sched_field = def->session_type == 10 ? 80 : 3;
	uint32_t duration_s = (uint32_t)def->duration_min * 60u;

	if (wr_u8(bb, def->hour_of_day) < 0) return -1;
	if (wr_u8(bb, def->date_minute) < 0) return -1;
	if (wr_u8(bb, def->day_of_weekend > 0
	    ? (uint8_t)(def->day_of_weekend - 1) : 0) < 0) return -1;
	if (wr_f32(bb, (float)(def->time_multiplier > 0
	    ? def->time_multiplier : 1)) < 0) return -1;
	if (wr_u16(bb, sched_field) < 0) return -1;
	if (wr_u32(bb, duration_s) < 0) return -1;
	if (wr_u32(bb, session_overtime_s > 0 ? session_overtime_s : 120) < 0)
		return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, def->session_type) < 0) return -1;
	if (wr_f32(bb, def->dynamic_track_multiplier) < 0) return -1;
	return 0;
}

/*
 * Session-results header emitted inside 0x3e (SRV_SESSION_RESULTS).
 * Pcap-verified against kunos race-end emit (2026-05-11):
 *
 *   u8  hour_of_day        (e.g. P=14, R=16 from event.json)
 *   u8  dateMinute         (from event.json, default 0)
 *   u8  dayOfWeekend - 1   (P=0, Q=1, R=2 in the typical schedule)
 *   f32 timeMultiplier     (from event.json)
 *   u16 sched_field        (P=3, Q=3, R=80 -- exe FUN_140034f60
 *                           sched_field; NOT the AC2-internal enum)
 *   u32 duration_seconds   (duration_min * 60)
 *   u32 overtime_seconds   (sessionOverTimeSeconds)
 *   u8  0                  (always)
 *   u8  def->session_type  (P=0, Q=4, R=10 -- JSON-spec enum)
 *   f32 dynamicTrackMultiplier (from event.json, default 0.0)
 *
 * This is the same 23-byte slot AC2 reads via FUN_14352c640.  AC2 keeps
 * a separate enum-code field for sessions in the result widget.
 */
int
write_session_result_header(struct ByteBuf *bb,
    const struct SessionDef *def, uint16_t session_overtime_s)
{
	uint32_t duration_s = (uint32_t)def->duration_min * 60u;
	uint8_t dow_minus_one = def->day_of_weekend > 0
	    ? (uint8_t)(def->day_of_weekend - 1) : 0;

	if (wr_u8(bb, def->hour_of_day) < 0) return -1;
	if (wr_u8(bb, def->date_minute) < 0) return -1;
	if (wr_u8(bb, dow_minus_one) < 0) return -1;
	if (wr_f32(bb, (float)(def->time_multiplier > 0
	    ? def->time_multiplier : 1)) < 0) return -1;
	if (wr_u16(bb, def->session_type == 10 ? 80 : 3) < 0) return -1;
	if (wr_u32(bb, duration_s) < 0) return -1;
	if (wr_u32(bb, session_overtime_s > 0 ? session_overtime_s : 120) < 0)
		return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, def->session_type) < 0) return -1;
	if (wr_f32(bb, def->dynamic_track_multiplier) < 0) return -1;
	return 0;
}

int
write_session_mgr_state(struct ByteBuf *bb, struct Server *s,
    uint32_t conn_client_ts, uint32_t conn_rtt)
{
	const struct SessionDef *def;
	int k;
	uint8_t idx;

	if (s->session_count == 0)
		return -1;
	/*
	 * Clamp session_index to the active range — session_reset can
	 * leave session_index past session_count (PHASE_RESULTS path
	 * after a weekend wraps), and writing the welcome trailer
	 * during that window would otherwise read past the in-use
	 * sessions[] entries.
	 */
	idx = (uint8_t)(s->session.session_index < s->session_count
	    ? s->session.session_index : s->session_count - 1);
	def = &s->sessions[idx];

	/* First byte: session index (NOT phase). */
	if (wr_u8(bb, idx) < 0)
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
		double now = (double)mono_ms();
		double client_adj = (double)conn_client_ts +
		    (double)(conn_rtt / 2);

		/*
		 * Per-slot validity, matching the exe's SessionManager:
		 * each slot's valid byte (SM +0x100/0x138/0x170/0x1a8/
		 * 0x1e0/0x218) starts at 0 and is set to 1 only by the
		 * boundary that owns it — FUN_14012f300 / FUN_14012f4a0
		 * flip the formation-end, green-fire, active-end and
		 * overtime-end flags on the position triggers.  The
		 * wire slot writer FUN_140035130 reads that flag and
		 * emits u8(0) alone when unset, u8(1) + f32 when set.
		 *
		 * Our ts[k] = UINT64_MAX sentinel means "boundary not
		 * yet scheduled" for race sessions (ts[2..5] stay there
		 * until formation / green crossings fire).  Emitting
		 * those as valid with a float projection of UINT64_MAX
		 * sends the client a ~1.8e19 ms deadline — so far in
		 * the future the phase-4 / phase-5 render never winds
		 * up, and the green transition never animates.  Emit
		 * them as invalid instead, matching the exe.  Slot 6
		 * (aftercare) we never stamp so it's always invalid.
		 */
		for (k = 0; k < 6; k++) {
			if (s->session.ts[k] == UINT64_MAX) {
				if (wr_u8(bb, 0) < 0) return -1;
				continue;
			}
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
	uint8_t sidx = (s->session_count > 0 &&
	    s->session.session_index < s->session_count)
	    ? s->session.session_index
	    : (s->session_count > 0 ? s->session_count - 1 : 0);
	uint8_t cur_type = s->session_count > 0
	    ? s->sessions[sidx].session_type : 0;
	/* Default emit (0x36 / welcome trailer) — current session.
	 * is_archived=0, session_idx=-1 (live state).  results_ctx=0: the
	 * exe builds the welcome trailer with the TP gate off (140033980.c
	 * passes 0) and accd does not emit a live 0x36 in the results
	 * window, so neither path applies the post-race time penalty. */
	return write_session_leaderboard_section(bb, s, cur_type, 0, -1, 0);
}

/* Resolve the per-car race-state source for the given session_idx.
 * session_idx == -1 -> live (current session, every entry).
 * session_idx >= 0  -> archive snapshot if the car participated, NULL
 *                      if it didn't (caller skips the row).
 *
 * Returning NULL — instead of falling back to the live state — keeps
 * the race-end 0x3e per-session results truthful: a driver who joined
 * the server AFTER session N ended has race_archive[N] == NULL and
 * therefore no row in session N's results.  The previous fall-through
 * to ec->race emitted that driver's CURRENT race state under session
 * N's header, mixing live and historical data in the same frame. */
static const struct CarRaceState *
race_src_for(const struct CarEntry *ec, int session_idx)
{
	if (session_idx < 0)
		return &ec->race;
	if (session_idx >= ACC_MAX_SESSIONS)
		return NULL;
	return ec->race_archive[session_idx];
}

int
write_session_leaderboard_section(struct ByteBuf *bb, struct Server *s,
    uint8_t session_type, int is_archived, int session_idx, int results_ctx)
{
	int j, d, nc = 0;
	/*
	 * cvar8 gates whether AC2 reads the per-car +0x204
	 * missingMandatoryPitstop field from the wire.  Exe derives cvar8
	 * by scanning the LeaderboardLine vector for any entry whose
	 * +0x204 field is >= 0 (FUN_140034a40:103), which only occurs for
	 * Race sessions.  Practice/Qualifying always emit cvar8=0 even when
	 * mandatory_pit_count > 0 because +0x204 is never set outside a
	 * Race.  The 0x3e race-end emit invokes us once per completed
	 * session with that session's own type so practice entries inside a
	 * race-end results frame get cvar8=0 and race entries get cvar8=1.
	 */
	int in_race = (session_type == 10);
	uint8_t cvar8 = in_race ? 1 : 0;
	/*
	 * results_ctx is the exe's FUN_140128a80 param_6 analog: true only
	 * for the 0x3e race-results broadcast, where the exe folds each
	 * car's PostRaceTime penalty into the wire finishing time and sets
	 * status bit 0x2000.  ANDed with in_race so P/Q sections inside a
	 * results frame never get a time penalty applied.
	 */
	int apply_tp = results_ctx && in_race;
	int32_t sess_best_lap = INT32_MAX;
	int32_t sess_best_sec[3] = { INT32_MAX, INT32_MAX, INT32_MAX };

	/*
	 * Session-best counters scan every slot whose race state is
	 * still in this session (session_reset wipes best_lap_ms back
	 * to 0, so stale prior-session values can't leak in).  Slots
	 * where the driver disconnected mid-session keep their timing
	 * so the fastest lap stays visible on the standings sidebar.
	 * DQ'd cars are included (the exe FUN_140128a80:421-424 has no
	 * DQ predicate on the session-best update).  Entry count (nc)
	 * considers only currently connected cars.
	 */
	for (j = 0; j < ACC_MAX_CARS; j++) {
		const struct CarRaceState *r =
		    race_src_for(&s->cars[j], session_idx);

		if (r == NULL)
			continue;	/* didn't participate in this session */

		/* Exe FUN_140128a80:421-424 has no DQ predicate on the
		 * session-best update; include DQ'd cars unconditionally. */
		if (r->best_lap_ms > 0 &&
		    r->best_lap_ms < sess_best_lap)
			sess_best_lap = r->best_lap_ms;
		for (d = 0; d < 3; d++)
			if (r->best_sectors_ms[d] > 0 &&
			    r->best_sectors_ms[d] < sess_best_sec[d])
				sess_best_sec[d] =
				    r->best_sectors_ms[d];

		/*
		 * For archived sessions race_src_for already returned non-NULL
		 * only for participants, so disconnected cars are included
		 * automatically.  For the current session, mirror the exe
		 * (FUN_140034a40): include a car that disconnected mid-session
		 * as long as it has accumulated any race data (lap or best lap).
		 * Truly empty slots (never connected this session) have both
		 * zeroed by session_reset and are skipped.
		 */
		if (session_idx < 0 && !s->cars[j].used &&
		    r->lap_count == 0 && r->best_lap_ms == 0)
			continue;
		nc++;
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
	    ? LAP_TIME_INVALID : (uint32_t)sess_best_lap) < 0) return -1;
	if (wr_u8(bb, 3) < 0) return -1;
	for (d = 0; d < 3; d++)
		if (wr_u32(bb, sess_best_sec[d] == INT32_MAX
		    ? LAP_TIME_INVALID : (uint32_t)sess_best_sec[d]) < 0)
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
				const struct CarRaceState *src =
				    race_src_for(&s->cars[j], session_idx);
				/*
				 * Mirror the counting loop: skip a disconnected
				 * car (used=0) that has no race data, but keep
				 * one that drove at least one lap or posted a
				 * best time — the same guard used for nc above.
				 */
				if (session_idx < 0 && !s->cars[j].used &&
				    (src == NULL ||
				    (src->lap_count == 0 &&
				    src->best_lap_ms == 0)))
					continue;
				if (src == NULL || src->position != pos)
					continue;
				if (write_car_leaderboard_record(bb, s,
				    &s->cars[j], cvar8, is_archived,
				    src, apply_tp) < 0)
					return -1;
				emitted++;
			}
		}
		/* Emit any stragglers whose position wasn't in [1..n]. */
		for (j = 0; j < ACC_MAX_CARS && emitted < nc; j++) {
			const struct CarRaceState *src =
			    race_src_for(&s->cars[j], session_idx);
			int16_t p;
			/*
			 * Mirror the counting loop: same disconnected-car
			 * skip condition as above and as nc's scan.
			 */
			if (session_idx < 0 && !s->cars[j].used &&
			    (src == NULL ||
			    (src->lap_count == 0 &&
			    src->best_lap_ms == 0)))
				continue;
			if (src == NULL)
				continue;
			p = src->position;
			if (p >= 1 && p <= ACC_MAX_CARS)
				continue;
			if (write_car_leaderboard_record(bb, s,
			    &s->cars[j], cvar8, is_archived, src, apply_tp) < 0)
				return -1;
			emitted++;
		}
	}

	/*
	 * Section tail (FUN_140034a40 reads param_2+0x48 then +0x78):
	 * pcap diff 2026-05-11 (race-end test) shows the first byte is
	 * the cvar8 marker — 1 in any frame where the session is a race
	 * (or mandatory_pit is configured), 0 otherwise.  The second byte
	 * stays 0 across every capture we have so far (P-only, R race-end,
	 * 4-bot Practice).  The exe seems to use this byte to signal the
	 * "race-context active" flag to the AC2 leaderboard widget.
	 */
	if (wr_u8(bb, cvar8) < 0) return -1;
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
    const struct Server *s, const struct CarEntry *ec, uint8_t cvar8,
    int is_archived, const struct CarRaceState *race_src, int apply_results_tp)
{
	const struct CarRaceState *race = race_src != NULL
	    ? race_src : &ec->race;
	const struct PenaltyQueue *pq = &race->pen;
	/*
	 * Results-phase post-race time penalty fold (exe FUN_140128a80:
	 * 460-467).  In the 0x3e race-results context the exe adds the car's
	 * PostRaceTime (wire 0x0e) penalty milliseconds onto the +0x1f0
	 * finishing-time field and sets status-word bit 0x2000.  apply_
	 * results_tp is the caller's param_6 analog (set only for that
	 * context, never the welcome trailer or live 0x36).  penalty_total_ms
	 * mirrors the value accd's standings sort already uses (session.c) so
	 * the displayed finishing time stays consistent with the order.
	 */
	uint32_t tp_ms = apply_results_tp ? penalty_total_ms(pq) : 0;
	/*
	 * Derive in_race from cvar8 — the leaderboard caller passed it
	 * down with the per-session decision (race-bit = 1 for race
	 * entries, 0 for P/Q entries even within a 0x3e race-end frame).
	 * mandatory_pit also drives cvar8=1 but doesn't change pq_emit
	 * surfacing behaviour; treat cvar8=1 as in_race here.
	 */
	int in_race = cvar8 != 0;
	int pi, d;

	if (wr_u16(bb, ec->car_id) < 0) return -1;
	if (wr_u16(bb, (uint16_t)ec->race_number) < 0) return -1;
	/*
	 * FUN_140034210 reads u8 at car+0x58 (car_model) then u8 at
	 * car+0x5c (cup_category) from the runtime Car struct; the
	 * client's reader FUN_14352ae00 stores them back at +0x58 and
	 * +0x5c, where car_model drives per-car name and model display
	 * in the pre-race garage and HUD timing tower.  A previous
	 * attempt to emit (cup_category, current_driver_index) here
	 * made every PRO driver (cup=0) render as car_model=0 =
	 * Porsche 991 GT3 R — the "always Porsche in garage" regression.
	 */
	if (wr_u8(bb, ec->car_model) < 0) return -1;
	{
		/*
		 * Kunos derives cup_category from the driver's category
		 * (conn+0xa017c) at handshake time, not from the wire
		 * carInfo cup byte:
		 *   driver_category 0 (Bronze)   -> cup 2 (Am)
		 *   driver_category 1 (Silver)   -> cup 3 (ProAm)
		 *   driver_category 2 (Gold)     -> cup 0 (Pro)
		 *   driver_category 3 (Platinum) -> cup 0 (Pro)
		 *   else                         -> cup 4
		 * Verified at FUN_140025690:496-505 in accServer.exe.
		 * The ec->cup_category field is left set by handshake
		 * parsing but the wire-emit value is the derived form.
		 */
		/*
		 * current_driver_index is clamped to [0, ACC_MAX_DRIVERS_PER_CAR)
		 * at the entrylist source (entrylist.c) and on every swap path,
		 * but clamp locally too so this drivers[] deref can never read
		 * past the 4-element array even if a future caller seeds an
		 * out-of-range index.
		 */
		uint8_t di = ec->current_driver_index < ACC_MAX_DRIVERS_PER_CAR
		    ? ec->current_driver_index : 0;
		uint8_t dc = ec->drivers[di].driver_category;
		uint8_t cup;
		switch (dc) {
		case 0:	cup = 2; break;
		case 1:	cup = 3; break;
		case 2: case 3:	cup = 0; break;
		default:	cup = 4; break;
		}
		if (wr_u8(bb, cup) < 0) return -1;
	}
	/*
	 * Per-car status / lap-states word (exe LeaderboardLine +0x1d0).
	 * Emit the last-known client lap-states word verbatim, matching the
	 * stock server: the exe copies the inbound word (src+0x54) straight
	 * into the leaderboard line (140128a80.c:441 / 140127d80.c:304) and
	 * the 0x36 builder FUN_140034210:72 writes it with no mask.  The
	 * cut/penalty/out/in bits ride through unchanged.  FCY (word 0x20)
	 * and SafetyCar (0x10) are client-decoded bits that already ride
	 * through this verbatim echo from the inbound word, so they need no
	 * server synthesis.  The one bit the exe edits server-side is word
	 * 0x2000, set only in the results phase for cars carrying a
	 * PostRaceTime penalty (140128a80.c:460); the AC2 client never decodes
	 * it (decoder FUN_140f8e8d0 stops at 0x800), so it is byte-parity only
	 * with no client-visible effect.  accd sets it below when a results-
	 * phase time penalty applies (tp_ms > 0), mirroring the exe.
	 *
	 * Emit the completed-lap flags word for the 0x36 status, matching
	 * exe FUN_140128a80:441: LL+0x1d0 is set from the last completed
	 * lap's history entry car_field, not from the in-progress car_field.
	 * HasCut 0x01 in this word tells AC2 to display the cut lap time
	 * in last_lap but inhibit the timing-tower new-best commit.
	 * The in-progress car_field is carried by the 0x3a/0x3c relays.
	 */
	{
		uint16_t status = race->completed_lap_flags;
		if (tp_ms > 0)
			status |= 0x2000;	/* PostRaceTime marker */
		if (wr_u16(bb, status) < 0)
			return -1;
	}

	{
		/*
		 * Find the first unserved + non-pending entry to populate
		 * the active-penalty prefix.  Pending entries are client-
		 * reported via 0x41 and feed the per-car tail bytes only,
		 * not the active_pen prefix (kunos's car+0xc8/+0xcc are
		 * populated by a different path).
		 */
		int active = -1;
		for (pi = 0; pi < pq->count; pi++) {
			if (pq->slots[pi].served)
				continue;
			if (pq->slots[pi].pending)
				continue;
			if (pq->slots[pi].admin)
				continue;
			if (pq->slots[pi].race_end_tp != 0)
				continue;	/* race-end converted */
			active = pi;
			break;
		}
		if (active >= 0) {
			float remaining =
			    (float)pq->slots[active].laps_remaining;
			uint16_t wire = penalty_wire_value(
			    pq->slots[active].kind,
			    pq->slots[active].reason);
			/*
			 * Match FUN_140034210:72's present-gate: when the
			 * active entry has no renderable wire code AND no
			 * remaining, emit a single present=0 byte.  Emitting
			 * present=1 + u16 + f32 for a wire-0 entry adds 6
			 * phantom bytes and shifts the rest of the 0x36 record
			 * (cvar8, pq count/codes), so the client mis-parses.
			 */
			if (wire != 0 || remaining != 0.0f) {
				if (wr_u8(bb, 1) < 0) return -1;
				if (wr_u16(bb, wire) < 0) return -1;
				if (wr_f32(bb, remaining) < 0) return -1;
			} else {
				if (wr_u8(bb, 0) < 0) return -1;
			}
		} else {
			if (wr_u8(bb, 0) < 0) return -1;
		}
	}

	if (cvar8) {
		/*
		 * AC2 stores this byte at LeaderboardLine +0x204 named
		 * "missingMandatoryPitstop" (per JSON writer at
		 * 1434ec4b0.c).  Emit max(0, mandatory_pit_count -
		 * mandatory_pit_served) per car — for sessions WITHOUT
		 * mandatory pits this collapses to 0 (widget shows
		 * "OBLIGATOIRE 0/0" and the client hides it); for
		 * sessions WITH mandatory pits the widget shows the live
		 * remaining count, decreasing each time the client sends
		 * 0x54 ACP_MANDATORY_PITSTOP_SERVED (which increments
		 * race->mandatory_pit_served).
		 *
		 * Pre-v0.3.7 this byte was race->formation_mid_passed —
		 * a formation-lap progress flag, not a missing-pit count.
		 * v0.3.7 hard-coded the byte to 0 to suppress the
		 * "OBLIGATOIRE 255/0 INVALIDE" widget at race start (when
		 * the AC2 client's +0x204 ctor default 0xff was rendering
		 * as 255).  This now uses the live count for parity with
		 * eventRules.json that requires mandatory pit stops.
		 */
		uint8_t remaining = 0;
		if (s->mandatory_pit_count > race->mandatory_pit_served)
			remaining = (uint8_t)(s->mandatory_pit_count
			    - race->mandatory_pit_served);
		if (wr_u8(bb, remaining) < 0) return -1;
	}

	{
		/*
		 * pq array: in Practice / Qualifying, exclude pending
		 * (client-0x41) entries — they feed only the per-car tail
		 * bytes, matching kunos's 4-bot Practice pcap where
		 * pq_emit stays 0 throughout the 52-penalty burst.
		 *
		 * In Race, surface every non-served entry in pq_emit
		 * regardless of pending — kunos's 1-min Race pcap shows
		 * pq_emit=1 mid-race once a 0x41 lands in PHASE_SESSION.
		 * The active_pen prefix still respects `pending` so the
		 * client-reported entry doesn't get treated as server-
		 * confirmed.
		 */
		uint8_t pq_emit = 0;
		int emitted_slots = 0;
		for (pi = 0; pi < pq->count; pi++) {
			if (pq->slots[pi].served)
				continue;
			if (pq->slots[pi].admin)
				continue;
			/*
			 * Hide pending entries outside a race emit (Kunos's
			 * Practice / Qualifying pq_emit stays 0 across a
			 * pending-only burst), AND in any archived-session
			 * emit (e.g. P section of a race-end 0x3e where the
			 * pending DT was reported in R, not P).  A pending
			 * PostRaceTime entry is also hidden in race: the exe
			 * accumulates the TP counter without pushing a Penalty
			 * below the 256 s threshold, so it never reaches the
			 * +0x208 array (it rides the per-car tail only).
			 */
			if (pq->slots[pi].pending &&
			    (!in_race || is_archived ||
			     pq->slots[pi].kind == PEN_TP5 ||
			     pq->slots[pi].kind == PEN_TP15))
				continue;
			pq_emit++;
		}
		/*
		 * Race-mode sentinel: kunos emits pq_emit count >= 1 in
		 * any race-session leaderboard record, even before the
		 * first penalty.  When the queue is empty (or fully
		 * served/admin-only), the +0x208 array still carries one
		 * zero entry.  Pcap-verified: 0x36[1] in race-end test
		 * is 228 B with count=1 wire=0; matching practice 0x36
		 * is 4 B shorter (no sentinel).
		 */
		if (pq_emit == 0 && in_race)
			pq_emit = 1;
		if (wr_u8(bb, pq_emit) < 0) return -1;
		for (pi = 0; pi < pq->count; pi++) {
			int32_t wire;
			if (pq->slots[pi].served)
				continue;
			if (pq->slots[pi].admin)
				continue;
			if (pq->slots[pi].pending &&
			    (!in_race || is_archived ||
			     pq->slots[pi].kind == PEN_TP5 ||
			     pq->slots[pi].kind == PEN_TP15))
				continue;
			/*
			 * Pcap-verified (run_race_end.sh, 2026-05-11):
			 * kunos's pq slot emits wire=0 for any entry that
			 * is either pending (client-0x41 not yet server-
			 * confirmed) or race-end-converted (DT/SG that
			 * FUN_140127440 has moved to the post-race time
			 * penalty list).  The original DT wire still shows
			 * up in the per-car tail (b0, b1) below — only the
			 * i32 array slot is zeroed.
			 */
			wire = (pq->slots[pi].pending ||
			    pq->slots[pi].race_end_tp != 0) ? 0 :
			    (int32_t)penalty_wire_value(
				pq->slots[pi].kind,
				pq->slots[pi].reason);
			if (wr_i32(bb, wire) < 0) return -1;
			emitted_slots++;
		}
		if (emitted_slots == 0 && in_race) {
			/* Empty-queue sentinel — see comment above. */
			if (wr_i32(bb, 0) < 0) return -1;
		}
	}

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
	 * them from the runtime Car struct, but the AC2 client stores
	 * them at different semantic fields on its side.  Real-client
	 * testing (2026-04-21) pinned the third u32 as last_lap_ms on
	 * the client — emitting car_system there (commit d9a0c87) left
	 * the HUD's last-lap / predicted-lap timers blank because the
	 * client got 0 for last_lap and computed no delta.
	 *
	 *   +0x180  u16   current-driver-index (which driver of a
	 *                 multi-driver entry the client shows as active;
	 *                 FUN_14000a7c0 copies it from the runtime car)
	 *   +0x1d4  u32   best-lap-ms
	 *   +0x1b0  u32   last-lap-ms (client semantic — drives HUD
	 *                 last-lap + predicted-lap delta)
	 *   +0x1f4  u16   lap count
	 *   +0x1f0  u32   race-time-ms
	 *   +0x1f8  u8    ELO, clamped
	 */
	if (wr_u16(bb, ec->current_driver_index) < 0) return -1;
	if (wr_u32(bb, race->best_lap_ms > 0
	    ? (uint32_t)race->best_lap_ms : LAP_TIME_INVALID) < 0) return -1;
	if (wr_u32(bb, race->last_lap_ms > 0
	    ? (uint32_t)race->last_lap_ms : LAP_TIME_INVALID) < 0) return -1;
	if (wr_u16(bb, (uint16_t)race->lap_count) < 0) return -1;
	{
		/*
		 * Exe (FUN_140128a80:444-455) always emits 0x7fffffff for
		 * this field in live sessions; only the post-race results
		 * path (0x3e) uses the actual elapsed time.  In live context
		 * apply_results_tp is 0, so emit the sentinel.
		 */
		uint32_t rt = LAP_TIME_INVALID;
		if (apply_results_tp && race->race_time_ms > 0) {
			rt = (uint32_t)race->race_time_ms + tp_ms;
			if (rt == 0)
				rt = LAP_TIME_INVALID;
		}
		if (wr_u32(bb, rt) < 0)
			return -1;
	}
	if (wr_u8(bb, ec->last_elo < 0xff
	    ? (uint8_t)ec->last_elo : 0xff) < 0) return -1;

	{
		int si;
		int l1_n = 0;
		int l2_n;
		uint8_t wide_flag = 0;
		int32_t l2_buf[ACC_LAP_HISTORY];

		/*
		 * l1 carries the sector splits for the last completed lap.
		 * FUN_140034210 reads them from LeaderboardLine +0x1b8, which
		 * FUN_140128a80 copies from the car's lap-history entry at
		 * lap-end.  Use last_lap_splits_ms[] (snapshot taken just
		 * before sector_ms[] is reset at lap completion), NOT
		 * sector_ms[] (the current in-progress lap's splits).
		 * Pcap-verified 2026-06-22: kunos emits l1_n=0 mid-lap-2
		 * because the formation lap had no recorded sector splits;
		 * accd was emitting l1_n=2 from sector_ms[] by mistake.
		 */
		for (si = 0; si < 3; si++)
			if (race->last_lap_splits_ms[si] > 0)
				l1_n = si + 1;

		/*
		 * l2 carries the per-car lap history.
		 * FUN_140034210 emits the vector size directly (end-begin)>>2.
		 * The exe pre-allocates 3 INT32_MAX sentinels at
		 * LeaderboardLine ctor time (pcap-verified 2026-06-22: kunos
		 * emits l2_n=3 before any lap is completed on misano, and
		 * l2_n=3 after the first lap).  The minimum-1 introduced in
		 * 608be23 was wrong; that commit's matrix compared accd at
		 * 1-lap state (stadion, 1s formation lap) vs kunos at 0-lap
		 * state on misano (4161m, formation lap never completed in 30s).
		 */
		{
			int nh = race->lap_history_count < ACC_LAP_HISTORY
			    ? race->lap_history_count : ACC_LAP_HISTORY;
			int start = race->lap_history_count
			    <= ACC_LAP_HISTORY ? 0
			    : race->lap_history_count % ACC_LAP_HISTORY;
			int k;

			l2_n = nh < 3 ? 3 : nh;
			for (k = 0; k < nh; k++) {
				int idx = (start + k) % ACC_LAP_HISTORY;
				l2_buf[k] = race->lap_history_ms[idx];
			}
			for (; k < l2_n; k++)
				l2_buf[k] = LAP_TIME_INVALID;
		}

		/*
		 * FUN_140034210 scans both sector lists; if ANY value
		 * >= LAP_WIDE_PIVOT (= 65536 ms = 65.536 s) it switches
		 * BOTH lists to u32 encoding.  Otherwise each value is
		 * written as u16 capped at 0xffff.  The sentinel
		 * LAP_TIME_INVALID for empty laps forces wide mode
		 * naturally, so narrow mode only kicks in when every
		 * sector is a real sub-65 s split.
		 */
#define LAP_WIDE_PIVOT	0x10000u
		for (si = 0; si < l1_n; si++)
			if ((uint32_t)race->last_lap_splits_ms[si] >= LAP_WIDE_PIVOT)
				wide_flag = 1;
		for (si = 0; si < l2_n; si++)
			if ((uint32_t)l2_buf[si] >= LAP_WIDE_PIVOT)
				wide_flag = 1;

		if (wr_u8(bb, wide_flag) < 0) return -1;
		if (wr_u8(bb, (uint8_t)l1_n) < 0) return -1;
		for (si = 0; si < l1_n; si++) {
			uint32_t v = (uint32_t)race->last_lap_splits_ms[si];
			if (wide_flag) {
				if (wr_u32(bb, v) < 0) return -1;
			} else {
				if (wr_u16(bb,
				    v >= LAP_WIDE_PIVOT ? 0xffffu
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
				    v >= LAP_WIDE_PIVOT ? 0xffffu
				    : (uint16_t)v) < 0) return -1;
			}
		}
	}

	/*
	 * Final two bytes: FUN_140034210 reads car+0x200 then car+0x201.
	 * Pcap diff against kunos accServer (2026-05-09) showed these go
	 * from 00 00 (no active penalty) to (wire_code, value) once a
	 * client 0x41 lands in the queue.  Kunos populates them at
	 * session boundary via FUN_140128a80 then FUN_14012ab30/aca0
	 * then FUN_1400f03b0 (the (kind,cat) wire-code dispatcher); the
	 * dispatcher's u16 output carries the wire code in byte 0 and
	 * the per-penalty u8 (severity / laps) in byte 1.
	 *
	 * For accd we collapse this to "head unserved penalty" data,
	 * matching kunos for the common single-penalty case.
	 */
	{
		/*
		 * Kunos's tail tracks DQ-priority: once a car has any DQ
		 * report, only DQ events update the tail (non-DQ reports
		 * leave the tail unchanged at the prior DQ).  Before the
		 * first DQ, the tail follows the latest report.  4-bot
		 * pcap diff (2026-05-11) confirmed: bot1's tail evolves
		 * wire 1 → 5 → 11 → 19 → ... on each new (cat) DQ, never
		 * touched by intermediate DT/SG/TP/RBL reports.
		 *
		 * Implementation: scan from back twice — first for a DQ
		 * entry, fall back to the latest non-DQ if none found.
		 */
		uint8_t b0 = 0, b1 = 0;
		int pi;

		for (pi = pq->count - 1; pi >= 0; pi--) {
			if (pq->slots[pi].served)
				continue;
			if (pq->slots[pi].kind != PEN_DQ)
				continue;
			/*
			 * DQ entries are always visible in the tail, even
			 * when our model has them marked pending=1 (e.g. a
			 * server-side DQ from TP-accumulation overflow or
			 * ladder force=1 that h_report_penalty's loop has
			 * lumped under pending alongside the client-reported
			 * entries).  The pending filter only applies to the
			 * non-DQ scan below.
			 */
			b0 = (uint8_t)penalty_wire_value(
			    pq->slots[pi].kind, pq->slots[pi].reason);
			/*
			 * Tail b1 is the entry's laps_remaining verbatim.
			 * Kunos's `list at param_1+0x30` stores the original
			 * 0x41 value when an entry is fresh and resets it to
			 * 0 on an overwrite — accd mirrors that by setting
			 * laps_remaining at materialise time (per-entry
			 * value for direct reports; counter/100 for the
			 * TP-accumulation auto-DQ; explicit 0-reset in the
			 * penalty_enqueue dedup path).  The earlier
			 * count==1 clamp was an over-approximation that
			 * forced b1=0 in every multi-entry case, which broke
			 * the TP-then-admin-DQ scenario (kunos `13 03`,
			 * accd `13 00`).
			 */
			b1 = (uint8_t)pq->slots[pi].laps_remaining;
			break;
		}
		if (b0 == 0) {
			for (pi = pq->count - 1; pi >= 0; pi--) {
				if (pq->slots[pi].served)
					continue;
				/*
				 * Admin chat-issued penalties DO surface in the
				 * per-car tail.  Kunos pcap (1-min race, admin
				 * /dt 911, 2026-06-20) shows b0/b1 = 0f 03
				 * (wire 15 = RaceControl DT, value 3) the moment
				 * the /dt lands, persisting to the race-end 0x3e.
				 * The tail is built from the PenaltySheet
				 * (+0x58/+0x59) that FUN_140125f50 populates, not
				 * from a separately pushed Penalty object, so it
				 * is not empty for fresh admin entries.  Only the
				 * active_pen prefix and pq_emit list stay at 0
				 * for admin entries (kunos leaves those
				 * untouched); those skips remain above.
				 */
				/*
				 * Archived-session emit only: hide pending
				 * (client-0x41) entries.  In the current
				 * session the tail follows the latest report
				 * (cat17 test in P: pending DT shows wire 33),
				 * but an older session's archived state must
				 * not surface a penalty that didn't exist
				 * yet (race-end 0x3e[1] P section).
				 *
				 * Race-end-converted entries STAY visible in
				 * the current-session tail of 0x3e results —
				 * kunos pcap shows the original DT wire +
				 * value 3 in the post-conversion 0x3e tail.
				 */
				if (pq->slots[pi].pending && is_archived)
					continue;
				b0 = (uint8_t)penalty_wire_value(
				    pq->slots[pi].kind,
				    pq->slots[pi].reason);
				b1 = (uint8_t)pq->slots[pi].laps_remaining;
				break;
			}
		}
		if (wr_u8(bb, b0) < 0) return -1;
		if (wr_u8(bb, b1) < 0) return -1;
	}
	return 0;
}

/*
 * 0x4e SRV_RATING_SUMMARY body (multi-car).  Walked by the welcome
 * trailer fan-out and by the periodic broadcast in tick_run; both
 * sites previously inlined this exact build, leading to two-site
 * drift hazard the moment the wire shape changes.  Body per-car
 * matches FUN_14002f710's tail layout: u16 car_id, u8 current_driver_index, i16 SA,
 * i16 SA (repeated, exe reads same conn offset twice), i16 -1,
 * i16 -1, str_a steam_id.  Ratings are stored x100 internally but
 * the 0x4e wire scale is x10 (the AC2 client divides each rating
 * slot by 10 at 143526030), so the SA slots are emitted as the
 * stored value /10.
 */
int
build_rating_summary(struct ByteBuf *bb, const struct Server *s)
{
	int j, nc = 0;

	/*
	 * Exe (FUN_14002f710:958-1019) counts and iterates the CONN vector,
	 * not the car vector.  A disconnected driver's conn is removed from
	 * the vector immediately; their car slot stays used until the zombie
	 * reclaim window.  Iterating used-cars sent one stale entry per
	 * disconnected driver.  Mirror the exe by counting AUTH'd non-SMPR
	 * conns that own a car slot.
	 */
	for (j = 0; j < ACC_MAX_CARS; j++) {
		const struct Conn *c = s->conns[j];

		if (c == NULL || c->state != CONN_AUTH || c->is_smpr ||
		    c->car_id < 0)
			continue;
		nc++;
	}
	if (wr_u8(bb, SRV_RATING_SUMMARY) < 0) return -1;
	if (wr_u8(bb, (uint8_t)nc) < 0) return -1;
	for (j = 0; j < ACC_MAX_CARS; j++) {
		const struct Conn *c = s->conns[j];
		const struct CarEntry *car;
		uint16_t sa = 5000, tr = 5000;
		const char *sid;
		uint8_t di;

		if (c == NULL || c->state != CONN_AUTH || c->is_smpr ||
		    c->car_id < 0)
			continue;
		car = &s->cars[c->car_id];
		/*
		 * Emit the CURRENT driver's rating, matching the driver
		 * ratings_on_lap evolves (handlers.c).  Using drivers[0]
		 * would show a stale co-driver's rating after a swap.
		 */
		di = car->current_driver_index < car->driver_count
		    ? car->current_driver_index : 0;
		sid = car->drivers[di].steam_id;
		ratings_get(s, sid, &sa, &tr);
		if (wr_u16(bb, car->car_id) < 0) return -1;
		if (wr_u8(bb, (uint8_t)di) < 0) return -1;
		if (wr_u16(bb, (uint16_t)(sa / 10)) < 0) return -1;
		if (wr_u16(bb, (uint16_t)(sa / 10)) < 0) return -1;
		if (wr_i16(bb, -1) < 0) return -1;
		if (wr_i16(bb, -1) < 0) return -1;
		/*
		 * FUN_14002f710 tail of each entry is str_a steam_id
		 * (via the generic string writer at 14004d390), not a
		 * u8 0 pad — real steam_ids are ~17-18 chars.
		 */
		if (wr_str_a(bb, sid) < 0) return -1;
	}
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
	const struct WeatherStatus *w = &s->weather;
	float ambient = w->ambient_mean != 0.0f ? w->ambient_mean
	    : (s->session.ambient_temp > 0
	    ? (float)s->session.ambient_temp
	    : (float)ACC_DEFAULT_AMBIENT_C);
	int i;

	/*
	 * Top-level WeatherData wire: 12 u32 + i16 nSine + N x u32 +
	 * i16 nCosine + M x u32.  Pcap-verified across both misano
	 * welcome frames (frame 7 + frame 142, identical Fourier
	 * state, both 76 B with 5 sine + 1 cosine coefficients).
	 * Matches FUN_14011e660 static decomp byte-for-byte.
	 */
	if (wr_u32(bb, w->is_dynamic ? 1u : 0u) < 0) return -1;	/* +0x28 isDynamic */
	if (wr_f32(bb, ambient) < 0) return -1;		/* +0x30 ambientTemperatureMean */
	if (wr_f32(bb, w->wind_speed_base) < 0) return -1;	/* +0x34 windSpeed */
	if (wr_f32(bb, w->wind_speed_mean) < 0) return -1;	/* +0x38 windSpeedMean */
	if (wr_f32(bb, w->wind_speed_dev) < 0) return -1;	/* +0x3c windSpeedDeviation */
	if (wr_f32(bb, w->wind_direction_base) < 0) return -1;	/* +0x40 windDirection */
	if (wr_f32(bb, w->wind_direction_change) < 0) return -1;	/* +0x44 windDirectionChange */
	if (wr_u32(bb, (uint32_t)w->wind_harmonic) < 0) return -1;	/* +0x48 windHarmonic */
	if (wr_u32(bb, (uint32_t)w->n_harmonics) < 0) return -1;	/* +0x4c nHarmonics */
	if (wr_f32(bb, w->weather_base_mean) < 0) return -1;	/* +0x50 weatherBaseMean */
	if (wr_f32(bb, w->weather_base_dev) < 0) return -1;	/* +0x54 weatherBaseDeviation */
	if (wr_f32(bb, w->variability_dev) < 0) return -1;	/* +0x58 variabilityDeviation */

	if (wr_i16(bb, (int16_t)w->n_sine) < 0) return -1;
	for (i = 0; i < (int)w->n_sine && i < ACCD_WX_MAX_SINE; i++)
		if (wr_f32(bb, w->sine_coeffs[i]) < 0) return -1;
	if (wr_i16(bb, (int16_t)w->n_cosine) < 0) return -1;
	for (i = 0; i < (int)w->n_cosine && i < ACCD_WX_MAX_COSINE; i++)
		if (wr_f32(bb, w->cosine_coeffs[i]) < 0) return -1;
	return 0;
}

/*
 * trailer_additional_state (FUN_1400330e0): per-session weather record
 * embedded in the welcome trailer.  The exe serializer FUN_140033510
 * writes a fixed 7 u32 head, dispatches one vtable body, then a 1 f32
 * tail.  Every branch of FUN_1400330e0 (static and dynamic weather alike)
 * builds the body with ksRacing::WeatherStatus::vftable = 9 floats, so
 * the record is always 68 B (17 f32) regardless of weatherRandomness.
 * The AC2 client reader FUN_14352cb30 is symmetric (7 u32 + WeatherStatus
 * body + 1 f32).  The 84 B WeatherData body (FUN_14011e660) belongs only
 * to the separate TopLevel WeatherData section, never to TrackConditions.
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
	float dry = dyn ? tanhf(tanhf(s->weather.dry_line_wetness) * 0.9f)
	    : s->weather.dry_line_wetness;

	ambient = s->session.ambient_temp > 0
	    ? (float)s->session.ambient_temp : (float)ACC_DEFAULT_AMBIENT_C;
	road = s->session.track_temp > 0
	    ? (float)s->session.track_temp : ambient + 4.0f;
	/* prefer evolved dynamic temps so late joiners see current conditions
	 * (mirrors weather_build_broadcast which uses ambient_current/road_current) */
	if (dyn && s->weather.ambient_current != 0.0f)
		ambient = s->weather.ambient_current;
	if (dyn && s->weather.road_current != 0.0f)
		road = s->weather.road_current;

	if (weather_write_track_conditions_head(bb, &s->grip) < 0)
		return -1;

	if (wr_f32(bb, ambient) < 0) return -1;
	if (wr_f32(bb, road) < 0) return -1;
	if (wr_f32(bb, s->weather.wind_speed) < 0) return -1;
	if (wr_f32(bb, s->weather.wind_direction) < 0) return -1;
	if (wr_f32(bb, clouds) < 0) return -1;
	if (wr_f32(bb, rain) < 0) return -1;
	if (wr_f32(bb, dry) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;

	if (wr_f32(bb, fmodf((float)s->session.weekend_time_s, 86400.0f)) < 0)
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
		if (wr_u8(bb, def->date_minute) < 0) return -1;
		if (wr_u8(bb, (uint8_t)(def->day_of_weekend > 0
		    ? def->day_of_weekend - 1 : 0)) < 0) return -1;
		if (wr_f32(bb, (float)(def->time_multiplier > 0
		    ? def->time_multiplier : 1)) < 0) return -1;
		if (wr_u16(bb, sched_field) < 0) return -1;
		if (wr_u32(bb, duration_s) < 0) return -1;
		if (wr_u32(bb, s->session_overtime_s > 0
	    ? s->session_overtime_s : 120) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, def->session_type) < 0) return -1;
		if (wr_f32(bb, def->dynamic_track_multiplier) < 0) return -1;
	}
	return 0;
}

/*
 * MultiplayerTrackRecord::writeToPacket (FUN_14011da70) — 19 bytes.
 * No direct decomp; the field purpose is a "best lap record" for the
 * circuit (signed sentinels when none recorded).  Bytes 6..11 carry
 * the 0xfff0ffffffff sentinel verified against kunos_welcome7.bin and
 * welcome2.bin (both kunos captures confirm byte 7 = 0xf0).
 */
static int
write_mtr(struct ByteBuf *bb, struct Server *s)
{
	(void)s;
	if (wr_u32(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0xff) < 0) return -1;
	if (wr_u8(bb, 0xf0) < 0) return -1;
	if (wr_u32(bb, 0xffffffff) < 0) return -1;
	if (wr_u32(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	return 0;
}

/*
 * MultiplayerCommunityCompetitionRatingSeries.  FUN_14011d9a0:
 *   str "Standard"        (writeKsonString = u16 len + N bytes)
 *   str ""                (empty second category)
 *   u32 count             (RatingLine vector length)
 *   count × RatingLine    (vtable[0x20] per entry)
 *
 * A real ACC Misano capture (notebook-a/captures/kunos_misano_*.
 * pcapng) shows Kunos emits **count=1** followed by a 22-byte
 * all-zero RatingLine entry: `3 × kson_string(empty) + 4 × u32(0)`.
 * Emitting count=0 with no entries lets the client's welcome
 * parser fall off the end of the buffer; same error signature as
 * the old `u8 1 + 24 zeros` bug but with the count truly zero the
 * trailer is simply ~22 bytes short of what the client expects.
 */
static int
write_rating_series(struct ByteBuf *bb, struct Server *s)
{
	int k;

	(void)s;
	if (wr_str_raw(bb, "Standard") < 0) return -1;
	if (wr_str_raw(bb, "") < 0) return -1;
	if (wr_u32(bb, 1) < 0) return -1;	/* vector count */
	/*
	 * Empty RatingLine — 21 bytes per FUN_140118e10 (the exe's
	 * reader): 3 × kson_string, u8, u32, 3 × u16, u32.  With all
	 * fields zero that is 6 + 1 + 4 + 6 + 4 = 21 bytes, matching
	 * the Kunos welcome trailer byte-for-byte.
	 */
	for (k = 0; k < 3; k++)
		if (wr_u16(bb, 0) < 0) return -1;	/* 3 empty strs */
	if (wr_u8(bb, 0) < 0) return -1;
	if (wr_u32(bb, 0) < 0) return -1;
	for (k = 0; k < 3; k++)
		if (wr_u16(bb, 0) < 0) return -1;
	if (wr_u32(bb, 0) < 0) return -1;
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

	/*
	 * Byte@+2: sequential 1-based index among the active (used) cars
	 * ordered by slot.  FUN_140032c90 reads car+0x2 (a per-car counter
	 * set when the car is added) and adds 1.  With non-contiguous slots
	 * (a slot was freed by a mid-session disconnect) the exe's index is
	 * smaller than slot+1; e.g. slots 0 and 2 active yields b2=1 and b2=2
	 * where slot+1 would give 1 and 3.
	 */
	{
		int k, seq = 0;
		for (k = 0; k <= car_slot; k++)
			if (s->cars[k].used) seq++;
		slot1 = (uint8_t)seq;
	}
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

	/* spawnDef tail: active driver index, u64 carSystem (+0x1b0), the
	 * car-location byte (+0x153), the tyre compound (+0x152), 5 dirt,
	 * 5 damage, then the per-car BoP: u16 ballast + u32 restrictor-as-
	 * float.  The exe welcome serializer FUN_140032c90 emits
	 * CarEntry+0x1fc (ballast, signed) and +0x200 (restrictor float) -
	 * the same BoP the 0x53 SRV_BOP_UPDATE carries.  This is NOT the
	 * 0x51 elo: the exe keeps that at +0x1f8 and never puts it in the
	 * welcome record (a prior accd version mislabeled the slot 'elo'
	 * and emitted last_elo + 0, so a late joiner saw existing cars'
	 * BoP wrong).  Dirt, damage and the tyre compound carry the latest
	 * 0x46 / 0x43 / 0x2f values so the newcomer renders the car with
	 * the same weathering and tyres everyone else already sees. */
	if (wr_u8(bb, ec->current_driver_index) < 0) return -1;
	if (wr_u64(bb, ec->last_sys_data) < 0) return -1;
	if (wr_u8(bb, ec->race.car_location) < 0) return -1;
	if (wr_u8(bb, ec->race.current_tyres) < 0) return -1;
	for (k = 0; k < 5; k++)
		if (wr_u8(bb, ec->race.car_dirt[k]) < 0) return -1;
	for (k = 0; k < 5; k++)
		if (wr_u8(bb, ec->race.damage[k]) < 0) return -1;
	if (wr_u16(bb, (uint16_t)ec->ballast_kg) < 0) return -1;
	if (wr_f32(bb, ec->restrictor) < 0) return -1;
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
	 *   u64 last_sys_data, u8 car_location, u8 tyre_compound,
	 *   5x u8 dirt, 5x u8 damage,
	 *   u16 ballast, f32 restrictor.
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

	/*
	 * Final trailer: formationLapType +0x1dc, u8(0), u8(0).
	 *
	 * The client stores this byte at player_ctx+0x630 and also sets
	 * player_ctx+0x631 = 1 when the value is 4 or 5 (AC2 client
	 * welcome_parser FUN_14352a150).  The server-side exe dispatch
	 * at FUN_14002f710:277 selects the silent (1 s) vs verbose
	 * (3-5.5 s) green-fire path on the same byte via
	 * `((type - 3) & 0xfd) == 0 ? silent : verbose`.  Values 3/5
	 * = silent, everything else = verbose with red-lights window.
	 */
	if (wr_u8(bb, s->formation_lap_type) < 0) return -1;
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
 * Send a proactive 0x4f sub=1 (stint started) to `new_conn` for every
 * car whose driver stint is currently active (exe FUN_14002dcb0 sweep,
 * only during PHASE_SESSION / PHASE_OVERTIME of a race session).
 * Wire: u8 SRV_DRIVER_STINT_RELAY + u16 car_id + u8(1) + 8-byte double.
 * The timestamp is the server-session-relative ms at which the stint
 * started, matching the value the original 0x4f relay would have
 * delivered to peers already present when the stint began.
 */
static void
handshake_send_stint_sync(struct Conn *new_conn, struct Server *s)
{
	int i;

	if (!session_is_race(s))
		return;
	if (s->session.phase != PHASE_SESSION &&
	    s->session.phase != PHASE_OVERTIME)
		return;
	if (!s->session.green_fired)
		return;
	/*
	 * The exe seeds the 0x4f stint-start frame only when stint
	 * enforcement is active: FUN_14002dcb0 builds it inside
	 * `if (param_4 != 0)` where param_4 = (driverStintTimeSec > 0)
	 * (caller FUN_140025690:1365).  With no stint limit it sends only
	 * the 0x2e state sync.  driver_stint_time_s tracks the same
	 * post-fallback value, so gate on it.
	 */
	if (s->driver_stint_time_s == 0)
		return;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct CarEntry *car = &s->cars[i];
		struct ByteBuf out;
		double ts_d;
		uint8_t ts_bytes[8];

		if (!car->used)
			continue;
		if (i == new_conn->car_id)
			continue;
		if (car->race.stint_start_ms == 0)
			continue;

		ts_d = (double)(car->race.stint_start_ms -
		    s->session.phase_started_ms);
		memcpy(ts_bytes, &ts_d, sizeof(ts_bytes));

		bb_init(&out);
		if (wr_u8(&out, SRV_DRIVER_STINT_RELAY) == 0 &&
		    wr_u16(&out, car->car_id) == 0 &&
		    wr_u8(&out, 1) == 0 &&
		    bb_append(&out, ts_bytes, sizeof(ts_bytes)) == 0)
			(void)conn_send_framed(new_conn, out.data, out.wpos);
		bb_free(&out);
	}
}

/*
 * Return 1 if car model byte is permitted in the configured carGroup.
 * Mirrors FUN_140025690:1062-1135 (exe DAT_140143e30=GT3, e38=GT4,
 * e50=GT2, e48=Cup, e40=ST; verified from exe binary wchar_t data).
 * TCX and GTC are NOT enforced by the exe at the handshake layer;
 * they only affect the lobby wire byte (FUN_140116480).  Unknown
 * groups fall through and admit all cars, matching exe LAB_14002749d.
 */
static int
car_in_group(const char *group, uint8_t model)
{
	if (strcmp(group, "FreeForAll") == 0)
		return 1;
	if (strcmp(group, "GT3") == 0)
		return (uint8_t)(model - 0x32) > 0x0b &&
		    (model > 0x1a ||
		    ((0x4040200U >> (model & 0x1f)) & 1) == 0);
	if (strcmp(group, "GT4") == 0)
		return (uint8_t)(model - 0x32) <= 0x0b;
	if (strcmp(group, "GT2") == 0)
		return (uint8_t)(model - 0x1a) <= 0x3c &&
		    ((0x1fc0000000000009ULL >>
		    ((uint64_t)(model - 0x1a) & 0x3f)) & 1) == 1;
	if (strcmp(group, "Cup") == 0)
		return model == 9 || model == 0x1a;
	if (strcmp(group, "ST") == 0)
		return model == 0x12;
	return 1;
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
	    wr_u32(&bb, (c->car_id >= 0 && c->car_id < ACC_MAX_CARS)
		? (uint32_t)s->cars[c->car_id].car_id
		: 0xffffffffu) < 0)
		goto fail;

	if (build_welcome_trailer(&bb, s, c) < 0)
		goto fail;

	rc = conn_send_framed(c, bb.data, bb.wpos);
	/* Remember the welcome trailer body size so the post-handshake
	 * `Sent handshake response for car N connection M with N bytes`
	 * banner can report it (matches kunos's log output verbatim). */
	c->welcome_bytes = (uint32_t)bb.wpos;
	bb_free(&bb);
	if (rc < 0)
		return rc;

	/* Proactive state sync for already-connected cars. */
	handshake_send_state_sync(c, s);
	handshake_send_stint_sync(c, s);

	/*
	 * Mid-race joiner: start this driver's stint now, mirroring the exe
	 * which begins stint timing at take-control/handshake
	 * (FUN_140025690:1015).  Gated on an active race (green fired) so it
	 * only counts toward the ExceededDriverStintLimit DQ; cars already
	 * present when green fires are started by
	 * session_advance_race_triggers.  Idempotent via the
	 * stint_start_ms!=0 guard.
	 */
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS &&
	    session_is_race(s) && s->session.green_fired)
		stint_start_tracking(s, c->car_id);
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
	int is_reconnect = 0;
	uint8_t wire_medals = 0, wire_sa = 0, wire_rc = 0, wire_cp = 0;

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
	if (strcmp(password, s->password) == 0) {
		/* Regular driver password. */
		c->is_spectator = 0;
	} else if (s->spectator_password[0] != '\0' &&
	    strcmp(password, s->spectator_password) == 0) {
		/*
		 * Spectator password: flag the connection so downstream
		 * code (e.g. monitor.c's PB_CONN_IS_SPECTATOR emit) can
		 * treat it distinctly.  Same accept path as a driver —
		 * Kunos doesn't enforce separate slot limits.
		 */
		c->is_spectator = 1;
		log_info("handshake: spectator join from fd %d", c->fd);
	} else {
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
		/*
		 * sub=1 marks a spectator-full reject (connection-list
		 * capacity), matching the exe FUN_14002db30(9, 1, ...);
		 * drivers keep sub=0.  a/b are the connection count and
		 * max_connections.
		 */
		reject_sub = c->is_spectator ? 1 : 0;
		reject_a = (uint32_t)s->nconns;
		reject_b = (uint32_t)s->max_connections;
		goto reply;
	}

	/*
	 * Carless observer (exe FUN_140033980): a spectatorPassword
	 * connection takes no car slot.  Still parse driver info so
	 * the steam_id is available for the ban/kick check below —
	 * exe FUN_140025690:281-338 walks the ban/kick lists before
	 * reaching the spectator path at line 1269.  The spectator
	 * exit is placed after those checks, before car allocation.
	 */

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

			/*
			 * 41-byte numeric block.  The first 4 bytes are the
			 * client-declared ratings the join gate trusts
			 * (FUN_140025690:462): trackMedals, SA, RC,
			 * competition; driver_category is at offset 16.
			 */
			if (rd_remaining(&r) >= 41) {
				(void)rd_u8(&r, &wire_medals);
				(void)rd_u8(&r, &wire_sa);
				(void)rd_u8(&r, &wire_rc);
				(void)rd_u8(&r, &wire_cp);
				(void)rd_skip(&r, 12);
				(void)rd_u8(&r, &cat);
				(void)rd_skip(&r, 24);
			}

			/* steam_id (6th string). */
			if (rd_can_str_a(&r) &&
			    rd_str_a(&r, &steam) == 0 && steam != NULL)
				snprintf(steam_buf, sizeof(steam_buf),
				    "%s", steam);

			/*
			 * Skip middle bytes, parse CarInfo.  Wire offsets
			 * pinned against the AC2 client's JSON serializer
			 * (FUN_1434e9a70):
			 *
			 *   "raceNumber"        ← struct +0x30 = body +8
			 *   "raceNumberPadding" ← struct +0x54 = body +35
			 *
			 * The "raceNumber" field is the visible race number
			 * the HUD draws on the car door / leaderboard
			 * column.  An earlier shift to body+35 read the
			 * "padding" slot instead, which is -1 / 0xFFFFFFFF
			 * when the driver hasn't configured a number,
			 * making every connecting car render as #65535.
			 */
			(void)rd_skip(&r, 8);		/* DriverInfo / CarInfo separator */
			(void)rd_skip(&r, 8);		/* CarInfo +0..+7 (2 × u32 ids) */
			(void)rd_i32(&r, &rnum);	/* raceNumber at body +8 (struct +0x30) */
			(void)rd_skip(&r, 33);		/* CarInfo +12..+44 (skin / cup / etc) */
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

		/*
		 * Eager same-SteamID dedup, mirroring the exe
		 * (FUN_140025690:267-279): the instant a fresh handshake is
		 * parsed, before the ban/kick/rating/slot gates, close any
		 * existing connection (driver OR spectator) with a matching
		 * SteamID.  accd's quick-reconnect below only covers live
		 * driver slots after the gates, so a stale spectator or
		 * rejected-rejoin socket would otherwise linger until the
		 * passive UDP reaper.  Mark the old conn dead only; the driver
		 * slot reclaim stays with the quick-reconnect (car_id untouched).
		 */
		if (steam_buf[0] != '\0') {
			int dj;
			for (dj = 0; dj < ACC_MAX_CARS; dj++) {
				struct Conn *o = s->conns[dj];
				if (o == NULL || o == c)
					continue;
				if (strcmp(o->steam_id, steam_buf) != 0)
					continue;
				log_info("closing old connection %u for "
				    "SteamId %s during new handshake",
				    (unsigned)o->conn_id, steam_buf);
				o->state = CONN_DISCONNECT;
			}
			snprintf(c->steam_id, sizeof(c->steam_id), "%s",
			    steam_buf);
		}

		/* Ban check. */
		if (bans_contains(&s->bans, steam_buf)) {
			log_debug("rejecting banned steam_id %s", steam_buf);
			reason = REJECT_BANNED;
			free(first); free(last); free(sname);
			free(steam); free(team);
			goto reply;
		}
		/* Kick check (ephemeral, cleared on weekend wrap). */
		if (bans_contains(&s->kicks, steam_buf)) {
			log_debug("rejecting kicked steam_id %s", steam_buf);
			reason = REJECT_KICKED;
			free(first); free(last); free(sname);
			free(steam); free(team);
			goto reply;
		}

		/* Spectator exit: ban/kick passed, skip car allocation. */
		if (c->is_spectator) {
			c->car_id = -1;
			c->state = CONN_AUTH;
			free(first); free(last); free(sname);
			free(steam); free(team);
			log_kunos("Sending initData with -1 carIndex, meaning "
			    "connectionId %u enters as spectator",
			    (unsigned)c->conn_id);
			goto reply;
		}

		/*
		 * Join rating / competition gate (FUN_140025690).  The
		 * trackMedals / SA / RC / competition values are the client-
		 * declared ratings read off the wire numeric block above; the
		 * original server trusts them like the steam_id.
		 *
		 * A CP server (isCPServer) accepts connections only during
		 * Free Practice (session_type 0) and gates on the competition-
		 * rating window [competitionRatingMin, competitionRatingMax];
		 * passing that, and every normal server, then gates on the
		 * medals / SA / RC floors.  ACC_RATING_REQUIRED skips an unset
		 * (0xff) floor; reject_b carries the floor, reject_a the wire-
		 * declared rating (FUN_14002db30).
		 */
		if (s->is_cp_server) {
			if (session_cur_type(s) != 0) {
				log_info("rejecting %s: CP server accepts "
				    "connections only in Free Practice",
				    steam_buf);
				reason = REJECT_BAD_SESSION;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
			if ((int)(int8_t)wire_cp < s->competition_rating_min) {
				log_info("rejecting %s: competition %u < "
				    "min %d", steam_buf, (unsigned)wire_cp,
				    s->competition_rating_min);
				reason = REJECT_CP_RATING;
				reject_sub = 3;
				reject_a = wire_cp;
				reject_b = (uint32_t)s->competition_rating_min;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
			if ((int)(int8_t)wire_cp > s->competition_rating_max) {
				log_info("rejecting %s: competition %u > "
				    "max %d", steam_buf, (unsigned)wire_cp,
				    s->competition_rating_max);
				reason = REJECT_CP_RATING;
				reject_sub = 4;
				reject_a = wire_cp;
				reject_b = (uint32_t)s->competition_rating_max;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
		}
		if (ACC_RATING_REQUIRED(s->track_medals_required) &&
		    wire_medals < s->track_medals_required) {
			log_info("rejecting %s: track medals %u < required %u",
			    steam_buf, (unsigned)wire_medals,
			    (unsigned)s->track_medals_required);
			reason = REJECT_CP_RATING;
			reject_sub = 0;
			reject_a = wire_medals;
			reject_b = s->track_medals_required;
			free(first); free(last); free(sname);
			free(steam); free(team);
			goto reply;
		}
		if (ACC_RATING_REQUIRED(s->safety_rating_required) &&
		    (int)(int8_t)wire_sa < (int)s->safety_rating_required) {
			log_info("rejecting %s: SA %u < required %u",
			    steam_buf, (unsigned)wire_sa,
			    (unsigned)s->safety_rating_required);
			reason = REJECT_CP_RATING;
			reject_sub = 1;
			reject_a = wire_sa;
			reject_b = s->safety_rating_required;
			free(first); free(last); free(sname);
			free(steam); free(team);
			goto reply;
		}
		if (ACC_RATING_REQUIRED(s->racecraft_rating_required) &&
		    (int)(int8_t)wire_rc < (int)s->racecraft_rating_required) {
			log_info("rejecting %s: RC %u < required %u",
			    steam_buf, (unsigned)wire_rc,
			    (unsigned)s->racecraft_rating_required);
			reason = REJECT_CP_RATING;
			reject_sub = 2;
			reject_a = wire_rc;
			reject_b = s->racecraft_rating_required;
			free(first); free(last); free(sname);
			free(steam); free(team);
			goto reply;
		}

		/*
		 * drivers[] index matched by the forceEntryList steam_id
		 * search.  0 for non-entrylist joins; set below for multi-
		 * driver team entries so the connecting co-driver's fields
		 * land in the correct drivers[] slot.  Declared here so
		 * the reconnect goto does not jump over its initializer.
		 */
		int matched_dj = 0;

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
					if (strcmp(old->steam_id,
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
				/*
				 * forceEntryList carModel gate: if the entry
				 * pins a specific carModel, the joiners wire
				 * cmodel must match.  Apply here BEFORE the
				 * reconnect goto so the slot reservation
				 * doesnt silently accept a wrong car.  Kunos
				 * wire (pcap-verified): a=current, b=required.
				 */
				if (s->force_entry_list &&
				    s->cars[reconnect_slot].forced_car_model
				    != 0xff &&
				    s->cars[reconnect_slot].forced_car_model
				    != cmodel) {
					log_info("rejecting %s: wrong "
					    "carModel %u (entry expects %u)",
					    steam_buf, (unsigned)cmodel,
					    (unsigned)s->cars[reconnect_slot]
					        .forced_car_model);
					reason = REJECT_BAD_CAR;
					reject_sub = 0;
					reject_a = cmodel;
					reject_b = s->cars[reconnect_slot]
					    .forced_car_model;
					/*
					 * The live-reconnect match above
					 * already detached the prior conn from
					 * this slot (car_id=-1), so conn_drop
					 * will not clear cars[].used.  Release
					 * it here so a rejected reconnect does
					 * not orphan the slot (used=1 with no
					 * owner); the driver's preserved data
					 * still allows a later correct-car
					 * reclaim via the zombie path.
					 */
					s->cars[reconnect_slot].used = 0;
					free(first); free(last); free(sname);
					free(steam); free(team);
					goto reply;
				}
				c->car_id = reconnect_slot;
				is_reconnect = 1;
				log_info("Recognized reconnect: carId %d "
				    "carModel %u raceNumber #%d",
				    c->car_id, (unsigned)cmodel,
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
			uint8_t sidx = s->session.session_index < s->session_count
			    ? s->session.session_index : 0;
			uint8_t stype = s->sessions[sidx].session_type;
			const char *why = NULL;

			if (!s->unsafe_rejoin && stype == 10 &&
			    s->session.phase >= PHASE_FORMATION &&
			    s->session.phase <= PHASE_OVERTIME)
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
			else if (s->preparation_locked && stype == 10 &&
			    (s->session.phase == PHASE_FORMATION ||
			    s->session.phase == PHASE_PRE_SESSION))
				why = "locked preparation phase";

			if (why != NULL) {
				log_info("Rejected driver %s, this server "
				    "does not accept connections during "
				    "%s (phase %s)", steam_buf, why,
				    session_phase_name(s->session.phase));
				reason = REJECT_BAD_SESSION;
				/*
				 * Mid-race / late-qualy / locked-prep emits
				 * sub=1 (exe FUN_140025690:835 ->
				 * FUN_14002db30(0xc, 1, ...)).  The CP-server-
				 * not-Free-Practice BAD_SESSION above keeps
				 * sub=0, matching FUN_14002db30(0xc, 0, ...).
				 */
				reject_sub = 1;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
		}

		/*
		 * carGroup model enforcement (exe FUN_140025690:1062-1135).
		 * "FreeForAll" (and TCX/GTC) admit all cars; GT3/GT4/GT2/
		 * Cup/ST restrict to specific model ranges.  Reconnects
		 * already branched via post_slot_assignment and skip this.
		 * Reject reason 1 mirrors FUN_14002db30(1,0,0,0,...).
		 */
		if (!car_in_group(s->car_group, cmodel)) {
			log_info("rejecting %s: car model %u not in "
			    "group %s", steam_buf, (unsigned)cmodel,
			    s->car_group);
			reason = REJECT_BAD_GROUP;
			free(first); free(last); free(sname);
			free(steam); free(team);
			goto reply;
		}

		/*
		 * Entry list enforcement: if forceEntryList is set,
		 * look up the client's steam_id in the preloaded
		 * entries. Assign them to the matching slot, or
		 * reject if not found.
		 */
		if (s->force_entry_list) {
			int slot = -1, i;

			/*
			 * Match steam_id across every slot's drivers[].
			 * Two-pass: first try to find a matching slot
			 * that's also unused (this is the post-team-entry-
			 * expansion case, where a multi-driver entry has
			 * duplicated drivers[] into N slots and one of
			 * them is free for the connecting teammate).
			 * Falls back to any matching slot so the existing
			 * REJECT_FULL path fires for the "all members of
			 * the team already connected" case.
			 */
			for (i = 0; i < ACC_MAX_CARS; i++) {
				struct CarEntry *ec = &s->cars[i];
				int dj;

				if (ec->used)
					continue;
				for (dj = 0; dj < ec->driver_count; dj++) {
					if (strcmp(ec->drivers[dj].steam_id,
					    steam_buf) == 0) {
						matched_dj = dj;
						slot = i;
						break;
					}
				}
				if (slot >= 0)
					break;
			}
			if (slot < 0) {
				for (i = 0; i < ACC_MAX_CARS; i++) {
					struct CarEntry *ec = &s->cars[i];
					int dj;

					for (dj = 0; dj < ec->driver_count;
					    dj++) {
						if (strcmp(ec->drivers[dj]
						    .steam_id, steam_buf)
						    == 0) {
							matched_dj = dj;
							slot = i;
							break;
						}
					}
					if (slot >= 0)
						break;
				}
			}
			if (slot < 0) {
				log_info("rejecting %s: not in entry list",
				    steam_buf);
				/*
				 * Kunos's exe emits REJECT_FULL (=9) for
				 * "steam_id not in forceEntryList".  Pcap
				 * (run_reject_codes.sh bad_car path with a
				 * 1-entry list whose steam_id != bot's)
				 * confirmed 0c 09 ...  REJECT_BAD_CAR (=11)
				 * is reserved for the "wrong carModel" path
				 * (second kunos rdata string "Player has
				 * entry list item with forced car model X,
				 * but chose Y and is rejected"), not "not in
				 * list".
				 */
				reason = REJECT_FULL;
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
			/*
			 * Forced-car-model check: entrylist may pin a specific
			 * carModel for this entry.  Wire cmodel must match.
			 * Kunos emits REJECT_BAD_CAR (=11) with sub=0 +
			 * a=current (wire) + b=required (forced) per the
			 * rdata "Player has entry list item with forced car
			 * model %d, but chose %d and is rejected".
			 */
			if (s->cars[slot].forced_car_model != 0xff &&
			    s->cars[slot].forced_car_model != cmodel) {
				log_info("rejecting %s: wrong carModel "
				    "%u (entry expects %u)",
				    steam_buf, (unsigned)cmodel,
				    (unsigned)s->cars[slot].forced_car_model);
				reason = REJECT_BAD_CAR;
				reject_sub = 0;
				reject_a = cmodel;
				reject_b = s->cars[slot].forced_car_model;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
			s->cars[slot].used = 1;
			c->car_id = slot;
		} else {
			/*
			 * Enforce the runtime maxCarSlots cap on a fresh
			 * (non-entrylist) joiner: the exe rejects with reason 9
			 * once the live car count reaches the rating-clamped slot
			 * count (FUN_140025690:656-658 -> FUN_1400214b0).  accd's
			 * server_alloc_car bounds only by max_connections, so a
			 * rating-restricted public server admitted more cars than
			 * it advertised in the lobby.  (The exe also clamps to the
			 * track pit count; accd has no per-track pit-box table, so
			 * that secondary clamp is not applied.)
			 */
			int used = 0, ci;
			for (ci = 0; ci < ACC_MAX_CARS; ci++)
				if (s->cars[ci].used)
					used++;
			if (used >= s->max_car_slots) {
				reason = REJECT_FULL;
				/*
				 * Wire parity with exe FUN_140025690:645:
				 * sub=0, a=current car count, b=max_car_slots.
				 */
				reject_sub = 0;
				reject_a = (uint32_t)used;
				reject_b = (uint32_t)s->max_car_slots;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
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
		/*
		 * Fresh occupant (different steam_id than any zombie):
		 * drop the previous driver's race state and archived
		 * snapshots so we don't surface their lap history in the
		 * 0x56 garage reply or seed the new joiner with their
		 * leaderboard row / grid position.  Reconnects (is_reconnect)
		 * intentionally keep the slot's prior state.
		 */
		if (!is_reconnect) {
			struct CarEntry *fc = &s->cars[c->car_id];
			int ai;

			for (ai = 0; ai < ACC_MAX_SESSIONS; ai++) {
				if (fc->race_archive[ai] != NULL) {
					free(fc->race_archive[ai]);
					fc->race_archive[ai] = NULL;
				}
			}
			memset(&fc->race, 0, sizeof(fc->race));
			fc->race.position = (int16_t)(c->car_id + 1);
			fc->race.grid_position = -1;
		}
		/* Populate the car slot with parsed data. */
		car = &s->cars[c->car_id];
		if (first != NULL)
			snprintf(car->drivers[matched_dj].first_name,
			    sizeof(car->drivers[matched_dj].first_name), "%s",
			    first);
		if (last != NULL)
			snprintf(car->drivers[matched_dj].last_name,
			    sizeof(car->drivers[matched_dj].last_name), "%s",
			    last);
		if (sname != NULL)
			snprintf(car->drivers[matched_dj].short_name,
			    sizeof(car->drivers[matched_dj].short_name), "%s",
			    sname);
		car->drivers[matched_dj].driver_category = cat;
		car->drivers[matched_dj].nationality = nat;
		snprintf(car->drivers[matched_dj].steam_id,
		    sizeof(car->drivers[matched_dj].steam_id), "%s", steam_buf);
		if (car->driver_count == 0)
			car->driver_count = 1;
		car->current_driver_index = (uint8_t)matched_dj;

		/*
		 * entrylist isServerAdmin: auto-elevate this conn to admin
		 * if the operator hasn't set an admin password.  Matches the
		 * exe's FUN_140018390 which always elevates on the +0x6e
		 * flag.  We diverge: when adminPassword is configured the
		 * password challenge is the authoritative gate, so an
		 * entrylist match without /admin <pw> shouldn't grant
		 * privileges — the steam_id is client-supplied and not
		 * Steam-ticket-verified, and an attacker who knows an
		 * admin steam_id would otherwise bypass the password.
		 * Open servers (empty adminPassword) keep the auto-elevation.
		 */
		if (car->is_server_admin && s->admin_password[0] == '\0')
			c->is_admin = 1;

		/*
		 * Only override car fields from the handshake if the
		 * entry list did not pre-populate them, and only on a
		 * fresh slot.  A reconnect must keep the existing
		 * car_entry's race_number / model / cup so the driver
		 * doesn't reset their on-grid identity across the drop.
		 *
		 * Race-number uniqueness mirrors accServer.exe
		 * FUN_140025690: try requested, requested+1, ..., +9,
		 * then 1..999, else 999.  See server_alloc_race_number
		 * in state.c.
		 */
		if (!s->force_entry_list && !is_reconnect) {
			car->race_number = server_alloc_race_number(s,
			    c->car_id, (int)rnum);
			car->car_model = cmodel;
			/*
			 * Derive cup_category from driver_category (cat),
			 * matching FUN_140025690:496-505.  The raw wire
			 * byte (ccup) is ignored — the exe never stores it.
			 */
			switch (cat) {
			case 0:  car->cup_category = 2; break;
			case 1:  car->cup_category = 3; break;
			case 2:  case 3:  car->cup_category = 0; break;
			default: car->cup_category = 4; break;
			}
			if (team != NULL)
				snprintf(car->team_name,
				    sizeof(car->team_name), "%s", team);
		} else if (car->forced_car_model == 0xff) {
			/*
			 * forceEntryList=1 + forcedCarModel=-1 means the
			 * operator lets each driver pick their own car
			 * within the entry slot.  Pick up the wire model
			 * so the leaderboard reports the driver's actual
			 * selection instead of the CarEntry default 0
			 * (Porsche 991 GT3 R).  Done for both fresh joins
			 * AND zombie/reconnect slot reclaims because kunos
			 * logs "carModel %d" on both paths (FUN_140025690
			 * "Creating new car connection" / "Recognized
			 * reconnect") -- the wire byte is authoritative.
			 */
			car->car_model = cmodel;
		}
		/*
		 * Grid-position assignment.  Only assigned when the slot
		 * has no grid position yet — a reclaim (zombie reconnect)
		 * or a race session that inherited its grid from a prior
		 * qualy archive already has grid_position set, and we
		 * must not overwrite it or the driver ends up on the
		 * wrong row after reconnect.
		 * When unset: entrylist.json defaultGridPosition wins,
		 * otherwise pick the next free slot per FUN_140021090
		 * (server_find_grid_slot).
		 */
		if (car->race.grid_position < 0) {
			if (car->default_grid_position >= 0) {
				car->race.grid_position =
				    (int16_t)car->default_grid_position;
			} else {
				int g = server_find_grid_slot(s);
				if (g >= 0)
					car->race.grid_position = (int16_t)g;
			}
		}
		/* accweb regex: ^\s*Car (\d+) Pos (\d+)$  -- grid slot.
		 * Emit unconditionally on successful handshake; the
		 * grid slot is meaningful even when 0 (pole = unused
		 * in 0-indexed find_grid_slot return). */
		if (car->race.grid_position >= 0)
			log_kunos("  Car %d Pos %d",
			    ACC_CAR_ID_BASE + c->car_id,
			    (int)car->race.grid_position);

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
		/*
		 * Kunos-format stdout banners for log scrapers.  Output
		 * matches accServer.exe's actual runtime lines verbatim,
		 * sampled from a live kunos capture on 2026-05-14:
		 *
		 *   New connection request: id 26 BotTPAccum Driver S76561199000000911 on car model 35
		 *   Creating new car connection: carId 1014, carModel 35, raceNumber #911
		 *   Sent handshake response for car 1014 connection 26 with 1121 bytes
		 *
		 * Kunos's printf string is `id %d %s %s %s on car model %d`
		 * with 3 separate string args -- firstName, lastName,
		 * steamId (in that order).  Emit the three fields as
		 * distinct args so a downstream parser that walks
		 * whitespace-separated tokens gets the same shape.
		 */
		log_kunos("New connection request: id %u %s %s %s on car model %u",
		    (unsigned)c->conn_id,
		    ldrv->first_name, ldrv->last_name, ldrv->steam_id,
		    (unsigned)lcar->car_model);
		log_kunos("Creating new car connection: carId %d, "
		    "carModel %u, raceNumber #%d",
		    ACC_CAR_ID_BASE + c->car_id,
		    (unsigned)lcar->car_model, lcar->race_number);
		/*
		 * "Sent handshake response ... with N bytes" is emitted
		 * AFTER handshake_send_accept runs (further down in
		 * handshake_handle, past the reply: label) so the
		 * N-bytes value reflects the actual welcome trailer
		 * size stored on c->welcome_bytes.
		 */
		for (j = 0; j < ACC_MAX_CARS; j++)
			if (s->cars[j].used) n++;
		lobby_notify_drivers_changed(&s->lobby, (uint8_t)n);
		log_kunos("%d client(s) online", n);
		/* Fan out CONNECTION_ENTRY + CAR_ENTRY to any attached
		 * SMPR monitors so external dashboards see the new
		 * driver / car immediately, matching kunos's per-conn
		 * push from the handshake handler. */
		smpr_notify_conn_changed(s, c);
		smpr_notify_car_changed(s, c->car_id);
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
	 * Kunos-format `Sent handshake response for car %d connection
	 * %d with %d bytes` banner.  Emitted here (not inside the
	 * earlier banner block) because handshake_send_accept is what
	 * fills in c->welcome_bytes.
	 */
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS)
		log_kunos("Sent handshake response for car %d connection %u with %u bytes",
		    ACC_CAR_ID_BASE + c->car_id,
		    (unsigned)c->conn_id,
		    (unsigned)c->welcome_bytes);

	/*
	 * Recompute standings now that the new car has been added.
	 * The welcome path below embeds the leaderboard section in the
	 * 0x0b reply; the tick-loop deep-compare in broadcast_leader-
	 * board_if_changed will fan a follow-up 0x36 to every conn on
	 * the next tick since the new car shifts the cached payload.
	 */
	session_recompute_standings(s);

	/*
	 * No join notify is broadcast to existing peers here.  The exe's
	 * post-accept loop (FUN_140025690) sends 0x04 SRV_CAR_ENTRY /
	 * 0x05 SRV_CONNECTION_ENTRY about the joiner ONLY to monitor/SMPR
	 * connections (the param_1[6]/[7] list), never to sim clients, and
	 * the AC2 game client has no TCP dispatch case for 0x04/0x05 (they
	 * hit its "Unknown TCP paket" default).  Game peers learn the new
	 * car from the next 0x36 leaderboard alone: the client's 0x36 parser
	 * resizes its car array to the wire line count and fills every slot,
	 * so a car appears purely by being listed.  accd routes 0x04/0x05 to
	 * monitors through its own SMPR path (smpr_notify_conn_changed).  The
	 * joiner itself is caught up on existing cars by handshake_send_state_
	 * sync (one 0x2e per car).  Verified: conn-lifecycle parity audit
	 * 2026-06-01 + the 2026-05-10 2-bot pcap.
	 *
	 */
	{
		(void)c;

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
		if (s->session.phase == PHASE_WAITING &&
		    server_used_car_count(s) > 0 &&
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
			 *
			 * Send the buffer to the joiner, then fan it out
			 * to every other connected peer so they learn that
			 * the new car appeared.  Kunos pcap (2026-05-10
			 * 2-bot test) shows the real server emits exactly
			 * this pattern: one 0x36 to the new joiner + one
			 * 0x36 to each existing peer with the updated car
			 * list.
			 */
			/*
			 * Post-handshake 0x36 fan-out — kunos emits to every
			 * connected peer (including the joiner) so the new
			 * car appears in everyone's leaderboard immediately.
			 * Force the emit (bypass deep-compare) because the
			 * cache may be coincidentally byte-identical when the
			 * joiner's car shape happens to match a freshly-reset
			 * row.  The cache is updated inside the force call so
			 * the next tick's gated drain won't re-emit.
			 */
			(void)broadcast_leaderboard_force(s);

			/*
			 * Team-entry 0x47 fan-out: when the joiner is a
			 * member of a multi-car team group, kunos emits a
			 * 0x47 SRV_DRIVER_SWAP_STATE_BCAST for every car
			 * in the group at the join boundary so existing
			 * peers see the team's current swap_state shape.
			 * Standalone single-driver entries (team_entry_id
			 * == -1) skip this fan-out — kunos doesn't emit
			 * 0x47 at join for them either.
			 */
			if (c->car_id >= 0 &&
			    s->cars[c->car_id].team_entry_id >= 0) {
				int8_t group = s->cars[c->car_id].team_entry_id;
				int g;
				for (g = 0; g < ACC_MAX_CARS; g++) {
					if (s->cars[g].team_entry_id == group &&
					    s->cars[g].used)
						broadcast_swap_state(s,
						    &s->cars[g]);
				}
			}

			/*
			 * No standalone 0x37 weather here.  The welcome
			 * trailer's TopLevel WeatherData + TrackConditions
			 * blocks already carry the joiner's weather state.
			 * Kunos pcap (2026-05-10 2-bot test) shows kunos
			 * emits its first 0x37 only on the 5 s cadence; a
			 * welcome-time 0x37 was a redundant extra frame.
			 */

			/*
			 * No 0x4e rating summary here.  The welcome trailer's
			 * RatingSeries block already carries every active
			 * driver's rating to the joiner; the standalone 0x4e
			 * message is only used for periodic refresh, gated on
			 * the 81000 ms cadence in FUN_14002f710.  A 2-bot pcap
			 * of accServer.exe under wine showed exactly one 0x4e
			 * frame per stream (the first periodic tick), with no
			 * handshake-time emit.
			 *
			 * Mark ratings dirty so the next tick.c periodic gate
			 * (CADENCE_RATINGS_MS) fires once the new joiner's
			 * record exists in the rating table.  Reset the
			 * debounce anchor to 0 so the refresh fires on the next
			 * tick instead of waiting out the remainder of the 81 s
			 * window, mirroring the exe (FUN_140025690 sets the
			 * emit-time anchor to 0 on a join).  A literal 0 still
			 * holds off until uptime passes 81 s, matching the exe.
			 */
			if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS &&
			    s->cars[c->car_id].driver_count > 0)
				ratings_seed_from_client(s,
				    c->steam_id,
				    wire_sa);
			s->ratings_dirty = 1;
			s->ratings_last_emit_ms = 0;
		}

		log_debug("welcome sequence sent: 0x2e+0x4f to new_conn, "
		    "0x28 to new_conn, 0x36 bcast to all, conn=%u",
		    (unsigned)c->conn_id);
	}
	return 0;
}
