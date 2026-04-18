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
 * state.c -- per-connection and server state lifecycle.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bcast.h"
#include "ratings.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "session.h"
#include "state.h"

void
server_init(struct Server *s)
{
	memset(s, 0, sizeof(*s));
	s->tcp_fd = -1;
	s->udp_fd = -1;
	s->lan_fd = -1;
	s->allow_auto_dq = 1;
	s->use_async_leaderboard = 0;
	s->unsafe_rejoin = 1;
	s->legacy_netcode = 1;
	s->formation_trigger_start = 0.80f;
	s->green_trigger_start = 0.89f;
	s->green_trigger_end = 0.96f;
	s->formation_lap_type = 3;
	s->short_formation_lap = 0;
	s->write_latency_dumps = 0;
	s->do_driver_swap_broadcast = 1;
	s->config_version = 0;
	lobby_init(&s->lobby);
	for (int i = 0; i < ACC_MAX_CARS; i++)
		s->cars[i].car_id = (uint16_t)(ACC_CAR_ID_BASE + i);
}

void
server_free(struct Server *s)
{
	for (int i = 0; i < ACC_MAX_CARS; i++) {
		if (s->conns[i] != NULL) {
			conn_drop(s, s->conns[i]);
			s->conns[i] = NULL;
		}
	}
	if (s->latency_dump_fp != NULL) {
		fclose((FILE *)s->latency_dump_fp);
		s->latency_dump_fp = NULL;
	}
}

struct Conn *
conn_new(struct Server *s, int fd, const struct sockaddr_in *peer)
{
	int slot;
	struct Conn *c;

	for (slot = 0; slot < s->max_connections && slot < ACC_MAX_CARS; slot++) {
		if (s->conns[slot] == NULL)
			break;
	}
	if (slot >= s->max_connections || slot >= ACC_MAX_CARS)
		return NULL;

	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->fd = fd;
	c->peer = *peer;
	c->state = CONN_UNAUTH;
	c->conn_id = (uint16_t)slot;
	c->car_id = -1;
	c->hs_echo = NULL;
	c->hs_echo_len = 0;
	bb_init(&c->rx);
	bb_init(&c->tx);

	s->conns[slot] = c;
	s->nconns++;
	return c;
}

void
conn_drop(struct Server *s, struct Conn *c)
{
	if (c == NULL)
		return;

	/*
	 * If the connection was authenticated, send a 0x4e rating
	 * summary to the disconnecting client, then emit a 0x24
	 * disconnect notify to every remaining connected client.
	 */
	if (c->state == CONN_AUTH && c->car_id >= 0 &&
	    c->car_id < ACC_MAX_CARS) {
		struct ByteBuf bb;
		struct DriverInfo *drv;

		/*
		 * 0x4e rating summary to the disconnecting client.
		 * See handshake.c welcome path for the per-entry
		 * field layout (must match byte-for-byte).
		 */
		drv = &s->cars[c->car_id].drivers[0];
		{
			uint16_t sa = 5000, tr = 5000;
			ratings_get(s, drv->steam_id, &sa, &tr);
			bb_init(&bb);
			if (wr_u8(&bb, SRV_RATING_SUMMARY) == 0 &&
			    wr_u8(&bb, 1) == 0 &&
			    wr_u16(&bb, s->cars[c->car_id].car_id) == 0 &&
			    wr_u8(&bb, 0) == 0 &&
			    wr_u16(&bb, sa) == 0 &&
			    wr_u16(&bb, tr) == 0 &&
			    wr_i16(&bb, -1) == 0 &&
			    wr_i16(&bb, -1) == 0 &&
			    wr_u32(&bb, 0) == 0 &&
			    wr_str_a(&bb, drv->steam_id) == 0)
				(void)conn_send_framed(c, bb.data, bb.wpos);
			bb_free(&bb);
		}

		/* 0x24 disconnect notify to all other clients. */
		bb_init(&bb);
		if (wr_u8(&bb, SRV_CAR_DISCONNECT_NOTIFY) == 0 &&
		    wr_u16(&bb, s->cars[c->car_id].car_id) == 0)
			(void)bcast_all(s, bb.data, bb.wpos, c->conn_id);
		bb_free(&bb);
		log_info("Sent car %d disco to %d clients",
		    c->car_id, s->nconns - 1);
	}

	log_debug("conn_drop: conn=%u fd=%d car=%d state=%d",
	    (unsigned)c->conn_id, c->fd, c->car_id, (int)c->state);
	if (c->fd >= 0)
		close(c->fd);
	bb_free(&c->rx);
	bb_free(&c->tx);
	free(c->hs_echo);
	if (c->conn_id < ACC_MAX_CARS && s->conns[c->conn_id] == c) {
		s->conns[c->conn_id] = NULL;
		s->nconns--;
	}
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
		/*
		 * Mark the car slot as unused so it can be
		 * reallocated, but preserve the race state
		 * (lap times, position) so the car stays in
		 * the leaderboard if they had valid laps.
		 * The Kunos server keeps disconnected cars
		 * in the standings.
		 */
		s->cars[c->car_id].used = 0;
		c->car_id = -1;
		session_recompute_standings(s);
		s->session.standings_seq++;
	}
	free(c);
	{
		int j, n = 0;
		for (j = 0; j < ACC_MAX_CARS; j++)
			if (s->cars[j].used)
				n++;
		lobby_notify_drivers_changed(&s->lobby, (uint8_t)n);
	}
}

struct Conn *
server_find_conn(struct Server *s, uint16_t conn_id)
{
	if (conn_id >= ACC_MAX_CARS)
		return NULL;
	return s->conns[conn_id];
}

int
server_alloc_car(struct Server *s)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		if (!s->cars[i].used) {
			s->cars[i].used = 1;
			s->cars[i].car_id = (uint16_t)(ACC_CAR_ID_BASE + i);
			return i;
		}
	}
	return -1;
}

/*
 * Active grid-position assignment per FUN_140021090.  Scan every
 * used car, find the maximum assigned grid number, and return
 * max+1 if it still fits under max_connections.  Otherwise walk
 * downward looking for an unoccupied slot; return -1 if the grid
 * is full.
 *
 * Entrylist.json may pre-assign a grid position via
 * `defaultGridPosition`.  If so the caller should honor that
 * instead of calling this helper.
 */
int
server_find_grid_slot(struct Server *s)
{
	int i, max_pits, used_count, max_assigned;
	uint8_t occupied[ACC_MAX_CARS];

	max_pits = s->max_connections > 0 &&
	    s->max_connections <= ACC_MAX_CARS
	    ? s->max_connections : ACC_MAX_CARS;
	max_assigned = -1;
	used_count = 0;
	for (i = 0; i < ACC_MAX_CARS; i++)
		occupied[i] = 0;
	for (i = 0; i < ACC_MAX_CARS; i++) {
		int g;
		if (!s->cars[i].used)
			continue;
		used_count++;
		g = s->cars[i].race.grid_position;
		if (g >= 0 && g < ACC_MAX_CARS) {
			occupied[g] = 1;
			if (g > max_assigned)
				max_assigned = g;
		}
	}
	if (max_assigned + 1 < max_pits)
		return max_assigned + 1;
	/* Fall back: walk down from max_pits-1 looking for unoccupied. */
	for (i = max_pits - 1; i >= 0; i--)
		if (!occupied[i])
			return i;
	(void)used_count;
	return -1;
}
