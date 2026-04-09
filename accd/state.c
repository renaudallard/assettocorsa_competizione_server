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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bcast.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "state.h"

void
server_init(struct Server *s)
{
	memset(s, 0, sizeof(*s));
	s->tcp_fd = -1;
	s->udp_fd = -1;
	s->lan_fd = -1;
	for (int i = 0; i < ACC_MAX_CARS; i++)
		s->cars[i].car_id = (uint16_t)i;
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
	if (c->state == CONN_AUTH && c->car_id >= 0) {
		struct ByteBuf bb;
		struct DriverInfo *drv;

		/* 0x4e rating summary to the disconnecting client. */
		drv = &s->cars[c->car_id].drivers[0];
		bb_init(&bb);
		if (wr_u8(&bb, SRV_RATING_SUMMARY) == 0 &&
		    wr_u8(&bb, 1) == 0 &&
		    wr_u16(&bb, c->conn_id) == 0 &&
		    wr_u8(&bb, 0) == 0 &&
		    wr_i16(&bb, 0) == 0 &&
		    wr_i16(&bb, 0) == 0 &&
		    wr_u32(&bb, 0xFFFFFFFF) == 0 &&
		    wr_str_a(&bb, drv->steam_id) == 0)
			(void)tcp_send_framed(c->fd, bb.data, bb.wpos);
		bb_free(&bb);

		/* 0x24 disconnect notify to all other clients. */
		bb_init(&bb);
		if (wr_u8(&bb, SRV_CAR_DISCONNECT_NOTIFY) == 0 &&
		    wr_u16(&bb, (uint16_t)c->car_id) == 0)
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
	if (c->conn_id < ACC_MAX_CARS && s->conns[c->conn_id] == c) {
		s->conns[c->conn_id] = NULL;
		s->nconns--;
	}
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
		s->cars[c->car_id].used = 0;
		c->car_id = -1;
	}
	free(c);
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
			s->cars[i].car_id = (uint16_t)i;
			return i;
		}
	}
	return -1;
}
