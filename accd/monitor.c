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
 * monitor.c -- ServerMonitor protobuf message builders.
 *
 * Each builder produces a single protobuf message body.  These
 * are then wrapped by `monitor_push_*` helpers that prepend the
 * sim-protocol msg id byte and send the result via TCP.
 *
 * Field numbers and types match §12B of NOTEBOOK_B.md.
 */

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <string.h>

#include "bcast.h"
#include "io.h"
#include "log.h"
#include "monitor.h"
#include "msg.h"
#include "pb.h"
#include "prim.h"
#include "state.h"

/* Map handbook session-type chars to ServerMonitor enum. */
static int
session_type_to_pb(uint8_t hb)
{
	switch (hb) {
	case 0:		/* Practice */
		return 0;
	case 4:		/* Qualifying */
		return 1;
	case 10:	/* Race */
		return 2;
	default:
		return 0;
	}
}

int
monitor_build_handshake_result(struct ByteBuf *bb,
    int success, int connection_id, const char *err_txt)
{
	if (pb_w_bool(bb, PB_HSR_SUCCESS, success) < 0)
		return -1;
	if (pb_w_int32(bb, PB_HSR_CONNECTION_ID, connection_id) < 0)
		return -1;
	if (pb_w_string(bb, PB_HSR_ERROR_TXT,
	    err_txt != NULL ? err_txt : "") < 0)
		return -1;
	return 0;
}

int
monitor_build_connection_entry(struct ByteBuf *bb,
    const struct Server *s, const struct Conn *c)
{
	const struct DriverInfo *d = NULL;

	if (c == NULL)
		return -1;
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
		const struct CarEntry *car = &s->cars[c->car_id];
		uint8_t idx = car->current_driver_index;

		if (idx < car->driver_count &&
		    idx < ACC_MAX_DRIVERS_PER_CAR)
			d = &car->drivers[idx];
	}

	if (pb_w_int32(bb, PB_CONN_CONNECTION_ID, c->conn_id) < 0)
		return -1;
	if (pb_w_string(bb, PB_CONN_FIRST_NAME,
	    d != NULL ? d->first_name : "") < 0)
		return -1;
	if (pb_w_string(bb, PB_CONN_LAST_NAME,
	    d != NULL ? d->last_name : "") < 0)
		return -1;
	if (pb_w_string(bb, PB_CONN_SHORT_NAME,
	    d != NULL ? d->short_name : "") < 0)
		return -1;
	if (pb_w_string(bb, PB_CONN_PLAYER_ID,
	    d != NULL ? d->steam_id : "") < 0)
		return -1;
	if (pb_w_bool(bb, PB_CONN_IS_ADMIN, c->is_admin) < 0)
		return -1;
	if (pb_w_bool(bb, PB_CONN_IS_SPECTATOR, c->is_spectator) < 0)
		return -1;
	return 0;
}

int
monitor_build_car_entry(struct ByteBuf *bb,
    const struct CarEntry *car, int driving_connection_id)
{
	if (car == NULL)
		return -1;
	if (pb_w_int32(bb, PB_CAR_CAR_ID, car->car_id) < 0)
		return -1;
	if (pb_w_enum(bb, PB_CAR_CAR_MODEL, car->car_model) < 0)
		return -1;
	if (pb_w_int32(bb, PB_CAR_DRIVING_CONNECTION_ID,
	    driving_connection_id) < 0)
		return -1;
	if (pb_w_int32(bb, PB_CAR_RACE_NUMBER, car->race_number) < 0)
		return -1;
	if (pb_w_enum(bb, PB_CAR_CUP_CATEGORY, car->cup_category) < 0)
		return -1;
	return 0;
}

int
monitor_build_configuration_state(struct ByteBuf *bb,
    const struct Server *s)
{
	int has_pw = s->password[0] != '\0';

	if (pb_w_string(bb, PB_CFG_SERVER_NAME, s->server_name) < 0)
		return -1;
	if (pb_w_string(bb, PB_CFG_TRACK_NAME, s->track) < 0)
		return -1;
	if (pb_w_int32(bb, PB_CFG_MAX_SLOTS, s->max_connections) < 0)
		return -1;
	if (pb_w_int32(bb, PB_CFG_TRACK_MEDALS, 0) < 0)
		return -1;
	if (pb_w_int32(bb, PB_CFG_SA_REQUIRED, 0) < 0)
		return -1;
	if (pb_w_bool(bb, PB_CFG_IS_PW_PROTECTED, has_pw) < 0)
		return -1;
	if (pb_w_bool(bb, PB_CFG_IS_LOCKED_ENTRY_LIST, 0) < 0)
		return -1;

	/* Repeated SessionDef sub-messages from the configured sessions. */
	{
		int i;

		for (i = 0; i < s->session_count; i++) {
			const struct SessionDef *def = &s->sessions[i];
			size_t start;

			if (pb_sub_begin(bb, PB_CFG_SESSIONS, &start) < 0)
				return -1;
			if (pb_w_enum(bb, PB_SDEF_SESSION_TYPE,
			    session_type_to_pb(def->session_type)) < 0)
				return -1;
			if (pb_w_int32(bb, PB_SDEF_ROUND, i) < 0)
				return -1;
			if (pb_w_int32(bb, PB_SDEF_DURATION_SECONDS,
			    (int32_t)def->duration_min * 60) < 0)
				return -1;
			if (pb_w_int32(bb, PB_SDEF_RACE_DAY,
			    def->day_of_weekend) < 0)
				return -1;
			if (pb_w_int32(bb, PB_SDEF_MINUTE_OF_DAY,
			    (int32_t)def->hour_of_day * 60) < 0)
				return -1;
			if (pb_w_int32(bb, PB_SDEF_TIME_MULTIPLIER,
			    def->time_multiplier > 0 ?
			    def->time_multiplier : 1) < 0)
				return -1;
			if (pb_w_int32(bb, PB_SDEF_OVERTIME_DURATION_S,
			    120) < 0)
				return -1;
			if (pb_w_int32(bb, PB_SDEF_PRE_RACE_WAIT_TIME_S,
			    80) < 0)
				return -1;
			if (pb_sub_end(bb, start) < 0)
				return -1;
		}
	}
	return 0;
}

int
monitor_build_session_state(struct ByteBuf *bb, const struct Server *s)
{
	if (pb_w_int32(bb, PB_SS_CURRENT_SESSION_INDEX,
	    s->session.session_index) < 0)
		return -1;
	if (pb_w_int32(bb, PB_SS_WEEKEND_TIME_SECONDS,
	    (int32_t)s->session.weekend_time_s) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_IDEAL_LINE_GRIP, 0.95f) < 0)
		return -1;
	/*
	 * ambientTemp / roadTemp are fixed32 (float) on the wire: the exe
	 * parser FUN_14003f510 requires tags 0x25 / 0x2d, not the varint
	 * pb_w_int32 emits, so a kunos-schema reader dropped both temps.
	 */
	if (pb_w_float(bb, PB_SS_AMBIENT_TEMP,
	    (float)(s->session.ambient_temp > 0 ? s->session.ambient_temp : ACC_DEFAULT_AMBIENT_C)) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_ROAD_TEMP,
	    (float)(s->session.track_temp > 0 ? s->session.track_temp : 26)) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_CLOUD_LEVEL, s->weather.clouds) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_RAIN_LEVEL, s->weather.current_rain) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_TRACK_WETNESS, s->weather.track_wetness) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_DRY_LINE_WETNESS,
	    s->weather.dry_line_wetness) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_TRACK_PUDDLES, s->weather.puddles) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_RAIN_FORECAST_10MIN,
	    s->weather.current_rain) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_RAIN_FORECAST_30MIN,
	    s->weather.current_rain) < 0)
		return -1;
	/*
	 * Cars connected = drivers in cars, not raw connections:
	 * nconns also counts carless spectators and SMPR monitors,
	 * which would disagree with the CarEntry / ConnectionEntry
	 * records emitted alongside in the same update.
	 */
	if (pb_w_int32(bb, PB_SS_CARS_CONNECTED,
	    server_used_car_count(s)) < 0)
		return -1;
	return 0;
}

/* Find the conn_id driving slot `car_idx`, or -1 if no live driver. */
static int
driving_conn_for_car(const struct Server *s, int car_idx)
{
	int j;
	for (j = 0; j < ACC_MAX_CARS; j++)
		if (s->conns[j] != NULL && s->conns[j]->car_id == car_idx)
			return (int)s->conns[j]->conn_id;
	return -1;
}

int
monitor_build_realtime_update(struct ByteBuf *bb,
    const struct Server *s)
{
	size_t sub_start;
	int i;

	/*
	 * serverNow is a fixed64 double (ms) on the wire: the exe parser
	 * FUN_14003f040 requires tag 0x09 (8-byte) for field 1, not a
	 * varint.  Do the *1000 in 64-bit so it cannot wrap.
	 */
	if (pb_w_double(bb, PB_RTU_SERVER_NOW,
	    (double)((int64_t)s->session.weekend_time_s * 1000)) < 0)
		return -1;
	if (pb_sub_begin(bb, PB_RTU_SESSION_STATE, &sub_start) < 0)
		return -1;
	if (monitor_build_session_state(bb, s) < 0)
		return -1;
	if (pb_sub_end(bb, sub_start) < 0)
		return -1;

	/* Repeated PB_RTU_CONNECTIONS — one ConnectionEntry submessage per
	 * authenticated, non-SMPR conn currently attached. */
	for (i = 0; i < ACC_MAX_CARS; i++) {
		const struct Conn *o = s->conns[i];
		if (o == NULL || o->state != CONN_AUTH || o->is_smpr)
			continue;
		if (pb_sub_begin(bb, PB_RTU_CONNECTIONS, &sub_start) < 0)
			return -1;
		if (monitor_build_connection_entry(bb, s, o) < 0)
			return -1;
		if (pb_sub_end(bb, sub_start) < 0)
			return -1;
	}

	/* Repeated PB_RTU_CARS — one CarEntry per used slot. */
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		const struct CarEntry *car = &s->cars[i];
		int driving_conn;
		if (!car->used)
			continue;
		driving_conn = driving_conn_for_car(s, i);
		if (pb_sub_begin(bb, PB_RTU_CARS, &sub_start) < 0)
			return -1;
		if (monitor_build_car_entry(bb, car, driving_conn) < 0)
			return -1;
		if (pb_sub_end(bb, sub_start) < 0)
			return -1;
	}
	return 0;
}

static int
build_leaderboard_entry(struct ByteBuf *bb, const struct Server *s,
    int car_idx)
{
	const struct CarEntry *car = &s->cars[car_idx];
	const struct CarRaceState *race = &car->race;
	const struct DriverInfo *d = NULL;
	int driving_conn = driving_conn_for_car(s, car_idx);
	size_t sub_start;
	int k;
	uint8_t idx;

	idx = car->current_driver_index;
	if (idx < car->driver_count && idx < ACC_MAX_DRIVERS_PER_CAR)
		d = &car->drivers[idx];

	if (pb_sub_begin(bb, PB_LBE_CAR_ENTRY, &sub_start) < 0) return -1;
	if (monitor_build_car_entry(bb, car, driving_conn) < 0) return -1;
	if (pb_sub_end(bb, sub_start) < 0) return -1;

	if (d != NULL && d->steam_id[0] != '\0')
		if (pb_w_string(bb, PB_LBE_CURRENT_STEAM_ID, d->steam_id) < 0)
			return -1;

	/* missing-mandatory-pits gauge: cfg requirement - served. */
	if (s->mandatory_pit_count > 0) {
		int32_t miss = (int32_t)s->mandatory_pit_count -
		    (int32_t)race->mandatory_pit_served;
		if (miss < 0) miss = 0;
		if (pb_w_int32(bb, PB_LBE_MISSING_MANDATORY_PITS, miss) < 0)
			return -1;
	}

	/* PB_LBE_DRIVER_TIMES (repeated fixed32) — accumulated stint ms
	 * per driver index, in order.  The exe parser FUN_14003e770
	 * accepts packed (0x22) or single fixed32 (0x25), not varint. */
	for (k = 0; k < car->driver_count && k < ACC_MAX_DRIVERS_PER_CAR; k++)
		if (pb_w_fixed32(bb, PB_LBE_DRIVER_TIMES,
		    (uint32_t)race->driver_stint_ms[k]) < 0)
			return -1;

	if (pb_w_int32(bb, PB_LBE_LAST_LAP_TIME, race->last_lap_ms) < 0)
		return -1;

	/* PB_LBE_LAST_LAP_SPLITS (repeated) — splits of the just-completed
	 * lap.  Pick from lap_splits_ms[last_idx] where last_idx wraps
	 * around the ring. */
	if (race->lap_history_count > 0) {
		uint32_t last_idx = (race->lap_history_count - 1)
		    % ACC_LAP_HISTORY;
		for (k = 0; k < 3; k++)
			if (pb_w_int32(bb, PB_LBE_LAST_LAP_SPLITS,
			    race->lap_splits_ms[last_idx][k]) < 0)
				return -1;
	}

	if (pb_w_int32(bb, PB_LBE_BEST_LAP_TIME, race->best_lap_ms) < 0)
		return -1;

	for (k = 0; k < 3; k++)
		if (pb_w_int32(bb, PB_LBE_BEST_LAP_SPLITS,
		    race->best_sectors_ms[k]) < 0)
			return -1;

	if (pb_w_int32(bb, PB_LBE_LAP_COUNT, race->lap_count) < 0)
		return -1;
	if (pb_w_int32(bb, PB_LBE_TOTAL_TIME, race->race_time_ms) < 0)
		return -1;

	/*
	 * First active penalty.  Mirror the 0x36 head-selection
	 * (handshake.c): skip served, client-reported pending, admin,
	 * and race-end-converted entries so the monitor reports the
	 * same active penalty the client-visible leaderboard does,
	 * not whatever happens to sit at slot 0.
	 */
	{
		int ap = -1, pi;

		for (pi = 0; pi < race->pen.count; pi++) {
			if (race->pen.slots[pi].served)
				continue;
			if (race->pen.slots[pi].pending)
				continue;
			if (race->pen.slots[pi].admin)
				continue;
			if (race->pen.slots[pi].race_end_tp != 0)
				continue;
			ap = pi;
			break;
		}
		if (ap >= 0) {
			if (pb_w_enum(bb, PB_LBE_CURRENT_PENALTY,
			    race->pen.slots[ap].kind) < 0)
				return -1;
			if (pb_w_int32(bb, PB_LBE_CURRENT_PENALTY_VALUE,
			    race->pen.slots[ap].laps_remaining) < 0)
				return -1;
		}
	}

	if (d != NULL) {
		char full[ACC_MAX_NAME_LEN * 2 + 2];

		snprintf(full, sizeof full, "%s %s",
		    d->first_name, d->last_name);
		if (pb_w_string(bb, PB_LBE_DRIVER_NAME, full) < 0)
			return -1;
		if (pb_w_string(bb, PB_LBE_DRIVER_SHORT_NAME,
		    d->short_name) < 0)
			return -1;
	}

	if (pb_w_enum(bb, PB_LBE_CAR_MODEL, car->car_model) < 0)
		return -1;
	return 0;
}

int
monitor_build_leaderboard(struct ByteBuf *bb, const struct Server *s)
{
	int32_t session_best_lap = 0;
	int32_t session_best_splits[3] = {0, 0, 0};
	int i, k;
	size_t sub_start;

	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		const struct CarRaceState *race = &s->cars[i].race;
		if (!s->cars[i].used)
			continue;
		if (race->best_lap_ms > 0 && (session_best_lap == 0 ||
		    race->best_lap_ms < session_best_lap))
			session_best_lap = race->best_lap_ms;
		for (k = 0; k < 3; k++)
			if (race->best_sectors_ms[k] > 0 &&
			    (session_best_splits[k] == 0 ||
			    race->best_sectors_ms[k] < session_best_splits[k]))
				session_best_splits[k] = race->best_sectors_ms[k];
	}

	if (pb_w_int32(bb, PB_LB_BEST_LAP, session_best_lap) < 0)
		return -1;
	for (k = 0; k < 3; k++)
		if (pb_w_int32(bb, PB_LB_BEST_SPLITS,
		    session_best_splits[k]) < 0)
			return -1;
	if (pb_w_bool(bb, PB_LB_IS_DECLARED_WET_SESSION, 0) < 0)
		return -1;

	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		if (!s->cars[i].used)
			continue;
		if (pb_sub_begin(bb, PB_LB_ENTRIES, &sub_start) < 0)
			return -1;
		if (build_leaderboard_entry(bb, s, i) < 0)
			return -1;
		if (pb_sub_end(bb, sub_start) < 0)
			return -1;
	}
	return 0;
}

/* The monitor_push_welcome_sequence function was removed: the
 * game client uses the sim-protocol welcome (0x0b + 0x37), not
 * the protobuf monitor messages (0x03-0x07).  The protobuf
 * builders above remain available for future ServerMonitor
 * broadcast protocol support on a separate UDP port. */
