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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "bcast.h"
#include "bans.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "state.h"
#include "weather.h"

int
build_welcome_trailer(struct ByteBuf *bb, struct Server *s, struct Conn *c)
{
	int i;

	/*
	 * Welcome trailer strings use u16-byte-length-prefixed raw
	 * UTF-8 (not Format-A), as observed from the real server.
	 */
	if (wr_str_raw(bb, s->server_name) < 0)
		return -1;
	if (wr_str_raw(bb, s->track) < 0)
		return -1;

	/* Separator + assigned car_id. */
	if (wr_u8(bb, 1) < 0)
		return -1;
	if (wr_u16(bb, (uint16_t)c->car_id) < 0)
		return -1;

	/* Session flags. */
	if (wr_u8(bb, 1) < 0)
		return -1;
	if (wr_u8(bb, 1) < 0)
		return -1;

	/* preRaceWaitingTimeSeconds, sessionOverTimeSeconds. */
	if (wr_u32(bb, 52) < 0)
		return -1;
	if (wr_u32(bb, 50) < 0)
		return -1;

	/* Echoed car info: raceNumber (i32), carModel (u8). */
	{
		struct CarEntry *car = NULL;
		struct DriverInfo *drv = NULL;
		int j;

		if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
			car = &s->cars[c->car_id];
			if (car->driver_count > 0)
				drv = &car->drivers[0];
		}

		if (wr_i32(bb, car ? car->race_number : 0) < 0)
			return -1;
		if (wr_u8(bb, car ? car->car_model : 0) < 0)
			return -1;

		/* Per-track season entity (pit positions, sector
		 * markers). Zero-filled; the client loads its own
		 * track data from game files. */
		for (i = 0; i < 1052; i++)
			if (wr_u8(bb, 0) < 0)
				return -1;

		/* Separator + echoed DriverInfo (Format-A). */
		if (wr_u8(bb, 1) < 0)
			return -1;
		if (wr_str_a(bb, drv ? drv->first_name : "") < 0)
			return -1;
		if (wr_str_a(bb, drv ? drv->last_name : "") < 0)
			return -1;
		if (wr_str_a(bb, drv ? drv->short_name : "") < 0)
			return -1;
		if (wr_u8(bb, drv ? drv->driver_category : 0) < 0)
			return -1;
		if (wr_u16(bb, drv ? drv->nationality : 0) < 0)
			return -1;

		/* Padding + steam_id as Format-A. */
		for (i = 0; i < 10; i++)
			if (wr_u8(bb, 0) < 0)
				return -1;
		if (wr_str_a(bb, drv ? drv->steam_id : "") < 0)
			return -1;

		/* Padding to reach assist rules block. */
		for (i = 0; i < 16; i++)
			if (wr_u8(bb, 0) < 0)
				return -1;

		/* AssistRules: separator + 8x u8 + padding + f32(1.0)
		 * + 6x u8 assist flags. */
		if (wr_u8(bb, 1) < 0)
			return -1;
		for (i = 0; i < 8; i++)
			if (wr_u8(bb, 2) < 0)
				return -1;
		if (wr_u32(bb, 0) < 0)
			return -1;
		if (wr_f32(bb, 1.0f) < 0)
			return -1;
		for (i = 0; i < 6; i++)
			if (wr_u8(bb, 2) < 0)
				return -1;

		/* Server config fields. */
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 5) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 5) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 4) < 0) return -1;
		if (wr_u16(bb, 0) < 0) return -1;
		if (wr_f32(bb, 0.8f) < 0) return -1;
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_f32(bb, 1.0f) < 0) return -1;
		if (wr_f32(bb, 0.5f) < 0) return -1;

		/* More config: formation lap, driver swap, etc. */
		for (i = 0; i < 8; i++)
			if (wr_u8(bb, 1) < 0)
				return -1;
		if (wr_u8(bb, 2) < 0) return -1;
		if (wr_u16(bb, 0) < 0) return -1;
		if (wr_u8(bb, 2) < 0) return -1;
		if (wr_u8(bb, 100) < 0) return -1;
		if (wr_u8(bb, 100) < 0) return -1;
		if (wr_u8(bb, 15) < 0) return -1;
		if (wr_u32(bb, 0) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 2) < 0) return -1;
		if (wr_u8(bb, 2) < 0) return -1;
		if (wr_u16(bb, 300) < 0) return -1;
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_u8(bb, 10) < 0) return -1;
		if (wr_u8(bb, 3) < 0) return -1;
		for (i = 0; i < 8; i++)
			if (wr_u8(bb, 2) < 0)
				return -1;
		if (wr_u32(bb, 100) < 0) return -1;
		if (wr_u32(bb, 3000) < 0) return -1;
		if (wr_u32(bb, 15) < 0) return -1;
		if (wr_u32(bb, 3) < 0) return -1;
		if (wr_u32(bb, 0) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;

		/* Track name as Format-A. */
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_str_a(bb, s->track) < 0)
			return -1;

		/* Weather inline snapshot (separator + f32 fields). */
		if (wr_u8(bb, 1) < 0) return -1;
		{
			float g2 = s->session.grip_level > 0
			    ? s->session.grip_level : 1.0f;

			if (wr_u8(bb, 0x20) < 0) return -1;
			if (wr_u8(bb, 3) < 0) return -1;
			if (wr_f32(bb, g2) < 0) return -1;
			if (wr_f32(bb, s->weather.clouds * 0.1f) < 0)
				return -1;
			if (wr_f32(bb, s->weather.current_rain * 0.1f) < 0)
				return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 5) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 5) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 4) < 0) return -1;
			if (wr_u16(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0xFF) < 0) return -1;
			if (wr_u8(bb, 1) < 0) return -1;
			if (wr_u32(bb, 0xFFFFFFFF) < 0) return -1;
			if (wr_u8(bb, 0xFF) < 0) return -1;
			if (wr_u8(bb, 1) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 1) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u16(bb, 0xFFFF) < 0) return -1;
			if (wr_u32(bb, 0) < 0) return -1;
			if (wr_u8(bb, 1) < 0) return -1;
		}

		/* Session definitions (per-session records). */
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_u8(bb, 0x32) < 0) return -1;
		if (wr_u8(bb, 3) < 0) return -1;
		for (j = 0; j < 3; j++) {
			float a_t = s->session.ambient_temp > 0
			    ? (float)s->session.ambient_temp : 22.0f;

			if (wr_f32(bb, a_t) < 0) return -1;
			if (wr_f32(bb, a_t + 8.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.1f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, a_t) < 0) return -1;
			if (wr_f32(bb, -1.0f) < 0) return -1;
			if (wr_f32(bb, 5.0f) < 0) return -1;
			if (wr_f32(bb, 15.0f) < 0) return -1;
			if (wr_f32(bb, -1.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.4f) < 0) return -1;
			if (wr_f32(bb, 0.3f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
		}

		/* Session schedule entries. */
		for (j = 0; j < s->session_count && j < 3; j++) {
			const struct SessionDef *def = &s->sessions[j];

			if (wr_u8(bb, def->hour_of_day) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, def->time_multiplier) < 0) return -1;
			if (wr_u16(bb, 0) < 0) return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
			if (wr_u16(bb, 3) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u16(bb, (uint16_t)(def->duration_min * 60)) < 0)
				return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u16(bb, 120) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
		}

		/* Leaderboard inline + car entries. */
		if (wr_u32(bb, 0x7FFFFFFF) < 0) return -1;
		if (wr_u8(bb, 3) < 0) return -1;
		if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;
		if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;
		if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;

		{
			int nc = 0;
			for (j = 0; j < ACC_MAX_CARS; j++)
				if (s->cars[j].used) nc++;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, (uint8_t)nc) < 0) return -1;
			for (j = 0; j < ACC_MAX_CARS; j++) {
				struct CarEntry *ec = &s->cars[j];
				struct DriverInfo *ed;

				if (!ec->used) continue;
				ed = &ec->drivers[0];
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_u16(bb, ec->car_id) < 0) return -1;
				if (wr_u16(bb, (uint16_t)ec->race_number) < 0)
					return -1;
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_u16(bb, ec->driver_count) < 0) return -1;
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_u8(bb, 1) < 0) return -1;
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_str_a(bb, ed->first_name) < 0) return -1;
				if (wr_str_a(bb, ed->short_name) < 0) return -1;
				if (wr_u16(bb, 0) < 0) return -1;
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_u32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_u32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_u32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_u8(bb, 1) < 0) return -1;
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_u8(bb, 3) < 0) return -1;
				if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_u32(bb, 0) < 0) return -1;
				if (wr_u32(bb, 0) < 0) return -1;
			}
		}

		/* Per-car realtime data placeholder. */
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_u32(bb, 0) < 0) return -1;
		{
			float g = s->session.grip_level > 0
			    ? s->session.grip_level : 1.0f;

			if (wr_f32(bb, g) < 0) return -1;
			if (wr_f32(bb, s->weather.clouds) < 0) return -1;
			if (wr_f32(bb, s->weather.current_rain) < 0) return -1;
			if (wr_u32(bb, 0) < 0) return -1;
			if (wr_u8(bb, 5) < 0) return -1;
			if (wr_u32(bb, 0) < 0) return -1;
			if (wr_f32(bb, 0.4f) < 0) return -1;
			if (wr_f32(bb, 0.3f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_u8(bb, 5) < 0) return -1;
			for (i = 0; i < 20; i++)
				if (wr_u8(bb, 0) < 0) return -1;
		}

		/* Inline weather snapshot. */
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (weather_build_broadcast(s, bb) < 0)
			return -1;

		/* Session schedule recap. */
		for (j = 0; j < s->session_count && j < 3; j++) {
			const struct SessionDef *def = &s->sessions[j];

			if (wr_u8(bb, def->hour_of_day) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, def->time_multiplier) < 0) return -1;
			if (wr_u16(bb, 0) < 0) return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
			if (wr_u16(bb, 3) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u16(bb, (uint16_t)(def->duration_min * 60)) < 0)
				return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u16(bb, 120) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
		}

		/* Tyre compound + tail. */
		if (wr_u8(bb, 5) < 0) return -1;
		if (wr_u8(bb, 5) < 0) return -1;
		for (i = 0; i < 6; i++)
			if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 0xFF) < 0) return -1;
		/* 0x40 variant for tyre compound. */
		if (wr_u8(bb, 0x40) < 0) return -1;
		if (wr_u32(bb, 0xFFFFFFFF) < 0) return -1;
		if (wr_u32(bb, 0) < 0) return -1;
		if (wr_u32(bb, 0) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		/* Compound name "Standard". */
		if (wr_u8(bb, 8) < 0) return -1;
		if (bb_append(bb, "Standard", 8) < 0) return -1;
		/* Tail padding. */
		for (i = 0; i < 24; i++)
			if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 3) < 0) return -1;
		if (wr_u16(bb, 0) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
	}

	return 0;

	return 0;
}

/*
 * Send a 14-byte 0x0c reject response matching the real server
 * format: u8(0x0c) + u32(server_ver=7) + u8(0) +
 * u16(client_ver_echo) + u16(0) + u16(ACC_PROTOCOL_VERSION) +
 * u16(0).
 */
static int
handshake_send_reject(struct Conn *c, uint16_t client_version)
{
	struct ByteBuf bb;
	int rc;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_STATE_RECORD_0C) < 0 ||
	    wr_u32(&bb, 7) < 0 ||
	    wr_u8(&bb, 0) < 0 ||
	    wr_u16(&bb, client_version) < 0 ||
	    wr_u16(&bb, 0) < 0 ||
	    wr_u16(&bb, ACC_PROTOCOL_VERSION) < 0 ||
	    wr_u16(&bb, 0) < 0)
		goto fail;

	rc = tcp_send_framed(c->fd, bb.data, bb.wpos);
	bb_free(&bb);
	return rc;
fail:
	bb_free(&bb);
	return -1;
}

/*
 * Send a 0x0b accept response with the welcome trailer.
 * Header: u8(0x0b) + u16(udp_port) + u8(0x12) +
 * u16(nconns) + u16(conn_id) + u16(0).
 */
static int
handshake_send_accept(struct Conn *c, struct Server *s)
{
	struct ByteBuf bb;
	int rc;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_HANDSHAKE_RESPONSE) < 0 ||
	    wr_u16(&bb, (uint16_t)s->udp_port) < 0 ||
	    wr_u8(&bb, 0x12) < 0 ||
	    wr_u16(&bb, (uint16_t)s->nconns) < 0 ||
	    wr_u16(&bb, c->conn_id) < 0 ||
	    wr_u16(&bb, 0) < 0)
		goto fail;

	if (build_welcome_trailer(&bb, s, c) < 0)
		goto fail;

	rc = tcp_send_framed(c->fd, bb.data, bb.wpos);
	bb_free(&bb);
	return rc;
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
		reason = REJECT_VERSION;
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
		goto reply;
	}

	c->car_id = server_alloc_car(s);
	if (c->car_id < 0) {
		reason = REJECT_FULL;
		goto reply;
	}

	/*
	 * Parse DriverInfo: first_name, last_name, short_name,
	 * category(u8), nationality(u16), steam_id.
	 * Then CarInfo: race_number(i32), car_model(u8),
	 * cup_category(u8), team_name, ...
	 * Fields after team_name are best-effort; if the body
	 * is short we just use defaults.
	 */
	{
		struct CarEntry *car = &s->cars[c->car_id];
		char *first = NULL, *last = NULL, *sname = NULL;
		char *steam = NULL, *team = NULL;
		uint8_t cat;
		uint16_t nat;
		int32_t rnum;
		uint8_t cmodel, ccup;

		if (rd_str_a(&r, &first) == 0)
			snprintf(car->drivers[0].first_name,
			    sizeof(car->drivers[0].first_name), "%s",
			    first);
		if (rd_str_a(&r, &last) == 0)
			snprintf(car->drivers[0].last_name,
			    sizeof(car->drivers[0].last_name), "%s",
			    last);
		if (rd_str_a(&r, &sname) == 0)
			snprintf(car->drivers[0].short_name,
			    sizeof(car->drivers[0].short_name), "%s",
			    sname);
		if (rd_u8(&r, &cat) == 0)
			car->drivers[0].driver_category = cat;
		if (rd_u16(&r, &nat) == 0)
			car->drivers[0].nationality = nat;
		if (rd_str_a(&r, &steam) == 0)
			snprintf(car->drivers[0].steam_id,
			    sizeof(car->drivers[0].steam_id), "%s",
			    steam);
		car->driver_count = 1;

		/* Check ban list after parsing steam_id. */
		if (bans_contains(&s->bans, car->drivers[0].steam_id)) {
			log_info("rejecting banned steam_id %s",
			    car->drivers[0].steam_id);
			car->used = 0;
			c->car_id = -1;
			reason = REJECT_BANNED;
			free(first); free(last); free(sname);
			free(steam); free(team);
			goto reply;
		}

		if (rd_i32(&r, &rnum) == 0)
			car->race_number = rnum;
		if (rd_u8(&r, &cmodel) == 0)
			car->car_model = cmodel;
		if (rd_u8(&r, &ccup) == 0)
			car->cup_category = ccup;
		if (rd_str_a(&r, &team) == 0)
			snprintf(car->team_name,
			    sizeof(car->team_name), "%s", team);

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

		log_info("handshake accepted: fd=%d conn_id=%u car_id=%d "
		    "race#=%d model=%u",
		    c->fd, (unsigned)c->conn_id, c->car_id,
		    lcar->race_number, (unsigned)lcar->car_model);
		log_debug("  driver: \"%s\" \"%s\" [%s] cat=%u steam=%s",
		    ldrv->first_name, ldrv->last_name,
		    ldrv->short_name,
		    (unsigned)ldrv->driver_category, ldrv->steam_id);
	}

reply:
	free(password);
	if (reason != REJECT_OK) {
		log_debug("handshake reject: reason=%d client_ver=0x%04x "
		    "fd=%d", (int)reason, (unsigned)client_version,
		    c->fd);
		if (handshake_send_reject(c, client_version) < 0)
			return -1;
		return -1;	/* close connection after reject */
	}
	if (handshake_send_accept(c, s) < 0)
		return -1;
	log_debug("handshake accept sent: conn=%u udp_port=%d",
	    (unsigned)c->conn_id, s->udp_port);

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

		bb_init(&notify);
		if (wr_u8(&notify, SRV_CAR_SYSTEM_RELAY) == 0 &&
		    wr_u16(&notify, (uint16_t)c->car_id) == 0 &&
		    wr_u64(&notify, timestamp_ms) == 0)
			(void)bcast_all(s, notify.data, notify.wpos,
			    c->conn_id);
		bb_free(&notify);

		/* Paired 0x4f sub-opcode 1: u8 msg_id + u16 carId +
		 * u8 sub=1 + u64 timestamp (12 bytes). */
		bb_init(&notify);
		if (wr_u8(&notify, SRV_DRIVER_STINT_RELAY) == 0 &&
		    wr_u16(&notify, (uint16_t)c->car_id) == 0 &&
		    wr_u8(&notify, 1) == 0 &&
		    wr_u64(&notify, timestamp_ms) == 0)
			(void)bcast_all(s, notify.data, notify.wpos,
			    c->conn_id);
		bb_free(&notify);

		/*
		 * Post-accept welcome sequence matching the real
		 * server: 0x28 + 0x36 + 0x37 + 0x4e.
		 */
		{
			struct ByteBuf wb;

			/* 0x36 initial leaderboard snapshot. */
		{
			struct ByteBuf lb;
			int j, nc = 0;

			bb_init(&lb);
			if (wr_u8(&lb, SRV_LEADERBOARD_BCAST) == 0 &&
			    wr_u32(&lb, s->session.standings_seq) == 0) {
				/* Best session splits (INT32_MAX = none). */
				(void)wr_u8(&lb, 3);
				(void)wr_i32(&lb, 0x7FFFFFFF);
				(void)wr_i32(&lb, 0x7FFFFFFF);
				(void)wr_i32(&lb, 0x7FFFFFFF);
				(void)wr_u8(&lb, 0);

				for (j = 0; j < ACC_MAX_CARS &&
				    j < s->max_connections; j++)
					if (s->cars[j].used) nc++;
				(void)wr_u8(&lb, (uint8_t)nc);

				for (j = 0; j < ACC_MAX_CARS &&
				    j < s->max_connections; j++) {
					struct CarEntry *ec = &s->cars[j];

					if (!ec->used) continue;
					(void)wr_u16(&lb, ec->car_id);
					(void)wr_u16(&lb,
					    (uint16_t)ec->race_number);
					(void)wr_u16(&lb, 0);
					(void)wr_u16(&lb, ec->driver_count);
					(void)wr_u8(&lb, 0);
					(void)wr_u8(&lb, 1);
					(void)wr_u8(&lb, 0);
					(void)wr_u8(&lb, 0);
					(void)wr_str_a(&lb,
					    ec->drivers[0].first_name);
					(void)wr_str_a(&lb,
					    ec->drivers[0].short_name);
					(void)wr_u16(&lb, 0);
					(void)wr_u8(&lb, 0);
					(void)wr_i32(&lb, 0x7FFFFFFF);
					(void)wr_i32(&lb, 0x7FFFFFFF);
					(void)wr_u8(&lb, 0);
					(void)wr_i32(&lb, 0x7FFFFFFF);
					(void)wr_u8(&lb, 1);
					(void)wr_u8(&lb, 0);
					(void)wr_u8(&lb, 3);
					(void)wr_i32(&lb, 0x7FFFFFFF);
					(void)wr_i32(&lb, 0x7FFFFFFF);
					(void)wr_i32(&lb, 0x7FFFFFFF);
					(void)wr_u32(&lb, 0);
					(void)wr_u32(&lb, 0);
				}
				(void)bcast_send_one(c, lb.data, lb.wpos);
			}
			bb_free(&lb);
		}

		/* 0x28 SRV_LARGE_STATE_RESPONSE: session
			 * timing + assist rule snapshot. */
			bb_init(&wb);
			if (wr_u8(&wb, SRV_LARGE_STATE_RESPONSE) == 0 &&
			    wr_u8(&wb, 0) == 0 &&
			    wr_u8(&wb, 1) == 0) {
				int k;
				float grip = s->session.grip_level > 0
				    ? s->session.grip_level : 1.0f;

				/* 3 copies of session time as f32. */
				for (k = 0; k < 3; k++)
					(void)wr_f32(&wb,
					    (float)s->session.weekend_time_s);
				/* 3 copies of end time. */
				for (k = 0; k < 3; k++)
					(void)wr_f32(&wb,
					    (float)(s->sessions[0].duration_min
					    * 60));
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 6);
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 1);
				(void)wr_f32(&wb, grip);
				(void)wr_u16(&wb, 3);
				(void)wr_u8(&wb, 0);
				(void)wr_u16(&wb, 600);
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 0);
				(void)wr_u16(&wb, 120);
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 0);
				(void)wr_f32(&wb, grip);
				(void)bcast_send_one(c, wb.data, wb.wpos);
			}
			bb_free(&wb);

			/* 0x37 weather status. */
			bb_init(&wb);
			if (weather_build_broadcast(s, &wb) == 0)
				(void)bcast_send_one(c, wb.data, wb.wpos);
			bb_free(&wb);

			/* 0x4e rating summary for this connection. */
			bb_init(&wb);
			if (wr_u8(&wb, SRV_RATING_SUMMARY) == 0 &&
			    wr_u8(&wb, 1) == 0 &&
			    wr_u16(&wb, c->conn_id) == 0 &&
			    wr_u8(&wb, 0) == 0 &&
			    wr_i16(&wb, 0) == 0 &&
			    wr_i16(&wb, 0) == 0 &&
			    wr_u32(&wb, 0xFFFFFFFF) == 0 &&
			    wr_str_a(&wb, "") == 0)
				(void)bcast_send_one(c, wb.data, wb.wpos);
			bb_free(&wb);
		}

		log_debug("welcome sequence sent: 0x2e+0x4f bcast + "
		    "0x36+0x28+0x37+0x4e to conn=%u",
		    (unsigned)c->conn_id);
	}
	return 0;
}
