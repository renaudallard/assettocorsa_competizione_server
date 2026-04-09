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
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
		struct CarEntry *car = &s->cars[c->car_id];

		if (wr_i32(bb, car->race_number) < 0)
			return -1;
		if (wr_u8(bb, car->car_model) < 0)
			return -1;
	} else {
		if (wr_i32(bb, 0) < 0)
			return -1;
		if (wr_u8(bb, 0) < 0)
			return -1;
	}

	/* Remaining sub-record padding. */
	for (i = 0; i < 5; i++)
		if (wr_u32(bb, 0) < 0)
			return -1;
	if (wr_u32(bb, 0xFF) < 0)
		return -1;

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
	log_info("handshake accepted: fd=%d conn_id=%u car_id=%d race#=%d",
	    c->fd, (unsigned)c->conn_id, c->car_id,
	    s->cars[c->car_id].race_number);

reply:
	free(password);
	if (reason != REJECT_OK) {
		if (handshake_send_reject(c, client_version) < 0)
			return -1;
		return -1;	/* close connection after reject */
	}
	if (handshake_send_accept(c, s) < 0)
		return -1;

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
		 * Push initial weather + leaderboard to the joining
		 * client, matching the real server's post-accept
		 * sequence (0x36 + 0x37).
		 */
		{
			struct ByteBuf wb;

			bb_init(&wb);
			if (weather_build_broadcast(s, &wb) == 0)
				(void)bcast_send_one(c, wb.data, wb.wpos);
			bb_free(&wb);
		}
	}
	return 0;
}
