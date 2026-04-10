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
 * dispatch.c -- TCP and UDP message dispatchers.
 *
 * Dispatches framed TCP messages and UDP datagrams to the per-msg-id
 * handlers in handlers.c.  The handshake (0x09) is special-cased
 * because it runs before the connection is authenticated; every
 * other case is forwarded to handlers.c.
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/socket.h>
#include <arpa/inet.h>
#include <stddef.h>
#include <time.h>

#include "dispatch.h"
#include "handlers.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "state.h"

/* ----- one TCP message ------------------------------------------- */

static int
dispatch_one_tcp(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	uint8_t msg_id;

	if (len < 1) {
		log_warn("tcp: empty frame from fd %d", c->fd);
		return -1;
	}
	msg_id = body[0];

	log_debug("tcp rx conn=%u msg=0x%02x len=%zu",
	    (unsigned)c->conn_id, (unsigned)msg_id, len);
	if (g_debug && len > 1)
		log_hexdump("  rx", body, len);

	if (c->state == CONN_UNAUTH && msg_id != ACP_REQUEST_CONNECTION) {
		log_warn("tcp: unauthenticated msg 0x%02x from fd %d "
		    "(dropping)", (unsigned)msg_id, c->fd);
		return -1;
	}

	switch (msg_id) {
	case ACP_REQUEST_CONNECTION:	/* 0x09 */
		return handshake_handle(s, c, body, len);
	case ACP_DISCONNECT:		/* 0x10 */
		log_info("tcp 0x10: clean disconnect from conn=%u",
		    (unsigned)c->conn_id);
		return -1;
	case ACP_LAP_COMPLETED:
		return h_lap_completed(s, c, body, len);
	case ACP_SECTOR_SPLIT_BULK:
		return h_sector_split_bulk(s, c, body, len);
	case ACP_SECTOR_SPLIT_SINGLE:
		return h_sector_split_single(s, c, body, len);
	case ACP_CHAT:
		return h_chat(s, c, body, len);
	case ACP_CAR_SYSTEM_UPDATE:
		return h_car_system_update(s, c, body, len);
	case ACP_TYRE_COMPOUND_UPDATE:
		return h_tyre_compound_update(s, c, body, len);
	case ACP_CAR_LOCATION_UPDATE:
		return h_car_location_update(s, c, body, len);
	case ACP_OUT_OF_TRACK:
		return h_out_of_track(s, c, body, len);
	case ACP_REPORT_PENALTY:
		return h_report_penalty(s, c, body, len);
	case ACP_LAP_TICK:
		return h_lap_tick(s, c, body, len);
	case ACP_DAMAGE_ZONES_UPDATE:
		return h_damage_zones(s, c, body, len);
	case ACP_CAR_DIRT_UPDATE:
		return h_car_dirt(s, c, body, len);
	case ACP_UPDATE_DRIVER_SWAP_STATE:
		return h_update_driver_swap_state(s, c, body, len);
	case ACP_EXECUTE_DRIVER_SWAP:
		return h_execute_driver_swap(s, c, body, len);
	case ACP_DRIVER_SWAP_STATE_REQUEST:
		return h_driver_swap_state_request(s, c, body, len);
	case ACP_DRIVER_STINT_RESET:
		return h_driver_stint_reset(s, c, body, len);
	case ACP_ELO_UPDATE:
		return h_elo_update(s, c, body, len);
	case ACP_MANDATORY_PITSTOP_SERVED:
		return h_mandatory_pitstop_served(s, c, body, len);
	case ACP_LOAD_SETUP:
		return h_load_setup(s, c, body, len);
	case ACP_CTRL_INFO:
		return h_ctrl_info(s, c, body, len);
	default:
		log_warn("tcp: unknown msg 0x%02x from conn=%u (%zu bytes)",
		    (unsigned)msg_id, (unsigned)c->conn_id, len);
		return 0;
	}
}

int
dispatch_tcp(struct Server *s, struct Conn *c)
{
	const unsigned char *body;
	size_t len, consumed;
	int rc;

	for (;;) {
		rc = bb_take_frame(&c->rx, &body, &len, &consumed);
		if (rc == 0)
			return 0;
		if (rc < 0) {
			log_warn("tcp: framing error from fd %d", c->fd);
			return -1;
		}
		rc = dispatch_one_tcp(s, c, body, len);
		bb_consume(&c->rx, consumed);
		if (rc < 0)
			return -1;
	}
}

/* ----- UDP -------------------------------------------------------- */

/*
 * Find the connection whose peer address matches peer.  This is
 * how inbound UDP messages get associated with an authenticated
 * TCP connection.
 */
static struct Conn *
find_conn_by_peer(struct Server *s, const struct sockaddr_in *peer)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];

		if (c == NULL)
			continue;
		if (c->peer.sin_addr.s_addr == peer->sin_addr.s_addr &&
		    c->peer.sin_port == peer->sin_port)
			return c;
	}
	return NULL;
}

void
dispatch_udp(struct Server *s, const struct sockaddr_in *peer,
    const unsigned char *buf, size_t len)
{
	uint8_t msg_id;
	struct Conn *c;

	if (len < 1) {
		log_warn("udp: empty datagram from %s:%u",
		    inet_ntoa(peer->sin_addr), ntohs(peer->sin_port));
		return;
	}
	msg_id = buf[0];

	/* Skip keepalive noise in debug output. */
	if (msg_id != ACP_KEEPALIVE_A && msg_id != ACP_KEEPALIVE_B) {
		log_debug("udp rx msg=0x%02x len=%zu from %s:%u",
		    (unsigned)msg_id, len,
		    inet_ntoa(peer->sin_addr), ntohs(peer->sin_port));
		if (g_debug && len > 1)
			log_hexdump("  rx", buf, len);
	}

	switch (msg_id) {
	case ACP_KEEPALIVE_A:		/* 0x13 */
	case ACP_KEEPALIVE_B: {		/* 0x17 */
		/*
		 * The real client sends 0x13 + u16(conn_id).  The
		 * server replies with a 0x14 keepalive carrying the
		 * server timestamp and per-car timing hints.  This
		 * also serves as the UDP peer-address association:
		 * the first keepalive from a client teaches the
		 * server which UDP source port belongs to which
		 * TCP connection.
		 */
		struct Reader kr;
		uint16_t ka_conn_id = 0;
		struct ByteBuf reply;
		struct Conn *kc;
		struct timespec kts;
		uint32_t srv_ms;

		rd_init(&kr, buf, len);
		(void)rd_skip(&kr, 1);		/* msg_id */
		(void)rd_u16(&kr, &ka_conn_id);

		kc = server_find_conn(s, ka_conn_id);
		if (kc == NULL)
			return;

		/* Learn / update the UDP peer address. */
		kc->peer = *peer;

		clock_gettime(CLOCK_MONOTONIC, &kts);
		srv_ms = (uint32_t)((uint64_t)kts.tv_sec * 1000 +
		    (uint64_t)kts.tv_nsec / 1000000);

		bb_init(&reply);
		if (wr_u8(&reply, SRV_KEEPALIVE_14) == 0 &&
		    wr_u32(&reply, srv_ms) == 0 &&
		    wr_u16(&reply, 0) == 0 &&
		    wr_u16(&reply, 0) == 0 &&
		    wr_u16(&reply, 0) == 0 &&
		    wr_u8(&reply, 2) == 0 &&
		    wr_u8(&reply, 4) == 0 &&
		    wr_u8(&reply, 100) == 0 &&
		    wr_u8(&reply, 100) == 0) {
			(void)sendto(s->udp_fd, reply.data, reply.wpos, 0,
			    (const struct sockaddr *)peer,
			    (socklen_t)sizeof(*peer));
		}
		bb_free(&reply);
		return;
	}

	case ACP_PONG_PHYSICS:		/* 0x16 */
		/* Client echoes the server timestamp from 0x14. */
		return;

	case ACP_CAR_UPDATE: {		/* 0x1e */
		/*
		 * Match by source_conn_id from the packet (bytes
		 * 1-2) instead of peer address, so multiple clients
		 * behind the same NAT can coexist.  Also update the
		 * peer address for sendto replies.
		 */
		uint16_t src_conn = 0;

		if (len >= 3)
			src_conn = (uint16_t)(buf[1] | (buf[2] << 8));
		c = server_find_conn(s, src_conn);
		if (c != NULL)
			c->peer = *peer;
		(void)h_udp_car_update(s, c, buf, len);
		return;
	}

	case ACP_CAR_INFO_REQUEST:	/* 0x22 */
		(void)h_udp_car_info_request(s, buf, len);
		return;

	case ACP_TIME_EVENT:		/* 0x5e */
		log_info("udp 0x5e time event from %s:%u (%zu bytes) — TODO",
		    inet_ntoa(peer->sin_addr), ntohs(peer->sin_port), len);
		return;

	case ACP_ADMIN_QUERY: {		/* 0x5f */
		/*
		 * Admin / server-identity query.  Client sends a
		 * Format-B string (the identifier it expects); if it
		 * matches our configured identifier we reply with a
		 * Format-A server name.  For phase 2 we just reply
		 * unconditionally with the server name since we don't
		 * yet carry a separate query identifier.
		 */
		struct ByteBuf reply;

		log_info("udp 0x5f admin query from %s:%u",
		    inet_ntoa(peer->sin_addr), ntohs(peer->sin_port));
		bb_init(&reply);
		if (wr_u8(&reply, ACP_ADMIN_QUERY) == 0 &&
		    wr_str_a(&reply, s->server_name) == 0) {
			(void)sendto(s->udp_fd, reply.data, reply.wpos, 0,
			    (const struct sockaddr *)peer,
			    (socklen_t)sizeof(*peer));
		}
		bb_free(&reply);
		return;
	}

	default:
		log_warn("Received unknown UDP paket %u from %s:%u",
		    (unsigned)msg_id, inet_ntoa(peer->sin_addr),
		    ntohs(peer->sin_port));
		return;
	}
}
