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

	/*
	 * Repeated SessionDef sub-messages.  We don't yet have a
	 * sessions[] table on Server (added in phase 5); for now
	 * emit a single placeholder Practice session so the
	 * client has something to populate its lobby UI with.
	 */
	{
		size_t start;

		if (pb_sub_begin(bb, PB_CFG_SESSIONS, &start) < 0)
			return -1;
		if (pb_w_enum(bb, PB_SDEF_SESSION_TYPE, 0) < 0)
			return -1;
		if (pb_w_int32(bb, PB_SDEF_ROUND, 0) < 0)
			return -1;
		if (pb_w_int32(bb, PB_SDEF_DURATION_SECONDS, 600) < 0)
			return -1;
		if (pb_w_int32(bb, PB_SDEF_RACE_DAY, 0) < 0)
			return -1;
		if (pb_w_int32(bb, PB_SDEF_MINUTE_OF_DAY, 600) < 0)
			return -1;
		if (pb_w_int32(bb, PB_SDEF_TIME_MULTIPLIER, 1) < 0)
			return -1;
		if (pb_w_int32(bb, PB_SDEF_OVERTIME_DURATION_S, 120) < 0)
			return -1;
		if (pb_w_int32(bb, PB_SDEF_PRE_RACE_WAIT_TIME_S, 80) < 0)
			return -1;
		if (pb_sub_end(bb, start) < 0)
			return -1;
	}
	(void)session_type_to_pb;	/* used in phase 5 */
	return 0;
}

int
monitor_build_session_state(struct ByteBuf *bb, const struct Server *s)
{
	if (pb_w_int32(bb, PB_SS_CURRENT_SESSION_INDEX, 0) < 0)
		return -1;
	if (pb_w_int32(bb, PB_SS_WEEKEND_TIME_SECONDS,
	    (int32_t)(s->tick_count / 10)) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_IDEAL_LINE_GRIP, 0.95f) < 0)
		return -1;
	if (pb_w_int32(bb, PB_SS_AMBIENT_TEMP, 22) < 0)
		return -1;
	if (pb_w_int32(bb, PB_SS_ROAD_TEMP, 26) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_CLOUD_LEVEL, 0.1f) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_RAIN_LEVEL, 0.0f) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_TRACK_WETNESS, 0.0f) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_DRY_LINE_WETNESS, 0.0f) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_TRACK_PUDDLES, 0.0f) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_RAIN_FORECAST_10MIN, 0.0f) < 0)
		return -1;
	if (pb_w_float(bb, PB_SS_RAIN_FORECAST_30MIN, 0.0f) < 0)
		return -1;
	if (pb_w_int32(bb, PB_SS_CARS_CONNECTED, s->nconns) < 0)
		return -1;
	return 0;
}

int
monitor_build_realtime_update(struct ByteBuf *bb,
    const struct Server *s)
{
	size_t sub_start;

	if (pb_w_int32(bb, PB_RTU_SERVER_NOW,
	    (int32_t)(s->tick_count * 100)) < 0)
		return -1;
	if (pb_sub_begin(bb, PB_RTU_SESSION_STATE, &sub_start) < 0)
		return -1;
	if (monitor_build_session_state(bb, s) < 0)
		return -1;
	if (pb_sub_end(bb, sub_start) < 0)
		return -1;
	/* Repeated connections / cars are emitted as one sub each;
	 * we leave them empty for the post-handshake push and
	 * fill them in phase 5/6 when CarRaceState exists. */
	return 0;
}

int
monitor_build_leaderboard(struct ByteBuf *bb, const struct Server *s)
{
	if (pb_w_int32(bb, PB_LB_BEST_LAP, 0) < 0)
		return -1;
	if (pb_w_bool(bb, PB_LB_IS_DECLARED_WET_SESSION, 0) < 0)
		return -1;
	(void)s;
	return 0;
}

/* ----- post-handshake push sequence ------------------------------ */

static int
send_msg(struct Conn *c, uint8_t msg_id, struct ByteBuf *body)
{
	struct ByteBuf out;
	int rc;

	bb_init(&out);
	if (wr_u8(&out, msg_id) < 0) {
		bb_free(&out);
		return -1;
	}
	if (body->wpos > 0 &&
	    bb_append(&out, body->data, body->wpos) < 0) {
		bb_free(&out);
		return -1;
	}
	rc = bcast_send_one(c, out.data, out.wpos);
	bb_free(&out);
	return rc;
}

int
monitor_push_welcome_sequence(struct Server *s, struct Conn *c)
{
	struct ByteBuf body;
	struct CarEntry *car;

	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS)
		return -1;
	car = &s->cars[c->car_id];

	/* 0x04 SRV_CAR_ENTRY for the joining car. */
	bb_init(&body);
	if (monitor_build_car_entry(&body, car, c->conn_id) == 0)
		(void)send_msg(c, SRV_CAR_ENTRY, &body);
	bb_free(&body);

	/* 0x05 SRV_CONNECTION_ENTRY for the joining connection. */
	bb_init(&body);
	if (monitor_build_connection_entry(&body, s, c) == 0)
		(void)send_msg(c, SRV_CONNECTION_ENTRY, &body);
	bb_free(&body);

	/* 0x03 SRV_SESSION_STATE current snapshot. */
	bb_init(&body);
	if (monitor_build_session_state(&body, s) == 0)
		(void)send_msg(c, SRV_SESSION_STATE, &body);
	bb_free(&body);

	/* 0x07 SRV_LEADERBOARD_UPDATE current snapshot. */
	bb_init(&body);
	if (monitor_build_leaderboard(&body, s) == 0)
		(void)send_msg(c, SRV_LEADERBOARD_UPDATE, &body);
	bb_free(&body);

	log_info("welcome push sequence sent to conn=%u car=%d",
	    (unsigned)c->conn_id, c->car_id);
	return 0;
}
