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

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

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
	if (conn_send_framed(c, body, len) < 0) {
		log_debug("bcast: send failed to conn=%u fd=%d, "
		    "marking disconnect", (unsigned)c->conn_id, c->fd);
		c->state = CONN_DISCONNECT;
		return -1;
	}
	return 0;
}

/*
 * Build the 2- or 6-byte length prefix into hdr[], return its length.
 */
static size_t
build_tcp_hdr(unsigned char hdr[6], size_t len)
{
	if (len < 0xFFFF) {
		hdr[0] = (unsigned char)(len & 0xff);
		hdr[1] = (unsigned char)((len >> 8) & 0xff);
		return 2;
	}
	hdr[0] = 0xff;
	hdr[1] = 0xff;
	hdr[2] = (unsigned char)(len & 0xff);
	hdr[3] = (unsigned char)((len >> 8) & 0xff);
	hdr[4] = (unsigned char)((len >> 16) & 0xff);
	hdr[5] = (unsigned char)((len >> 24) & 0xff);
	return 6;
}

int
conn_send_framed(struct Conn *c, const void *body, size_t len)
{
	unsigned char hdr[6];
	size_t hdrlen, total, queued_before;
	ssize_t n;

	if (c == NULL || c->fd < 0)
		return -1;
	hdrlen = build_tcp_hdr(hdr, len);
	total = hdrlen + len;
	queued_before = c->tx.wpos - c->tx.rpos;

	/*
	 * Fast path: queue is empty, try to push the entire message
	 * straight to the kernel.  On EAGAIN or short write, capture
	 * the remainder in c->tx for drain-on-POLLOUT.
	 */
	if (queued_before == 0) {
		size_t sent = 0;
		while (sent < total) {
			size_t off = sent;
			const void *p;
			size_t rem;
			if (off < hdrlen) {
				p = hdr + off;
				rem = hdrlen - off;
			} else {
				p = (const unsigned char *)body +
				    (off - hdrlen);
				rem = total - off;
			}
			n = write(c->fd, p, rem);
			if (n > 0) {
				sent += (size_t)n;
				continue;
			}
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0 && (errno == EAGAIN ||
			    errno == EWOULDBLOCK))
				break;
			return -1;
		}
		if (sent >= total)
			return 0;
		/* Partial: queue whatever's left. */
		if (sent < hdrlen) {
			if (bb_append(&c->tx, hdr + sent,
			    hdrlen - sent) < 0)
				return -1;
			if (bb_append(&c->tx, body, len) < 0)
				return -1;
		} else {
			size_t body_sent = sent - hdrlen;
			if (bb_append(&c->tx,
			    (const unsigned char *)body + body_sent,
			    len - body_sent) < 0)
				return -1;
		}
		return 0;
	}

	/* Queue not empty — preserve order, append the whole frame. */
	if (bb_append(&c->tx, hdr, hdrlen) < 0)
		return -1;
	if (bb_append(&c->tx, body, len) < 0)
		return -1;
	return 0;
}

int
conn_drain_tx(struct Conn *c)
{
	ssize_t n;
	size_t have;

	if (c == NULL || c->fd < 0)
		return -1;
	for (;;) {
		have = c->tx.wpos - c->tx.rpos;
		if (have == 0) {
			bb_clear(&c->tx);
			return 0;
		}
		n = write(c->fd, c->tx.data + c->tx.rpos, have);
		if (n > 0) {
			c->tx.rpos += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 1;
		return -1;
	}
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

int
bcast_all_udp(struct Server *s, const void *body, size_t len,
    uint16_t except_conn_id)
{
	int i, sent;

	if (s->udp_fd < 0)
		return 0;
	sent = 0;
	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];

		if (c == NULL)
			continue;
		if (c->state != CONN_AUTH)
			continue;
		if (c->conn_id == except_conn_id)
			continue;
		if (sendto(s->udp_fd, body, len, 0,
		    (const struct sockaddr *)&c->peer,
		    sizeof(c->peer)) < 0)
			continue;
		sent++;
	}
	return sent;
}
