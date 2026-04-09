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
 * bcast.c -- broadcast helpers.
 */

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>

#include "bcast.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "state.h"

int
bcast_send_one(struct Conn *c, const void *body, size_t len)
{
	if (c == NULL || c->fd < 0)
		return -1;
	return tcp_send_framed(c->fd, body, len);
}

int
bcast_all(struct Server *s, const void *body, size_t len,
    uint16_t except_conn_id)
{
	int i, sent;

	sent = 0;
	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];

		if (c == NULL)
			continue;
		if (c->state != CONN_AUTH)
			continue;
		if (c->conn_id == except_conn_id)
			continue;
		if (bcast_send_one(c, body, len) == 0)
			sent++;
	}
	return sent;
}
