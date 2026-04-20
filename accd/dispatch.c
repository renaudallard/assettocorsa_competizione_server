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
#include <stdio.h>
#include <time.h>

#include "bcast.h"
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

	/*
	 * Shadow-ban: the /hellban admin command flips c->hellbanned so
	 * the server silently drops every inbound message from this
	 * connection (chat, lap, sector, car update, etc).  The client
	 * keeps seeing outbound broadcasts from others so their UI
	 * appears normal, but their own actions never propagate.
	 * Handshake + clean disconnect still pass through so the socket
	 * isn't wedged.
	 */
	if (c->hellbanned && msg_id != ACP_REQUEST_CONNECTION &&
	    msg_id != ACP_DISCONNECT) {
		log_debug("tcp: hellban drop msg 0x%02x from conn=%u",
		    (unsigned)msg_id, (unsigned)c->conn_id);
		return 0;
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
		/*
		 * INVARIANT: body aliases c->rx.data.  No handler
		 * may append to or recv() into c->rx during dispatch,
		 * as bb_reserve could realloc the backing buffer and
		 * invalidate body.  All current handlers only read
		 * from body via rd_init (which copies the pointer)
		 * and write to separate local ByteBufs.
		 */
		rc = dispatch_one_tcp(s, c, body, len);
		bb_consume(&c->rx, consumed);
		if (rc < 0)
			return -1;
	}
}

/* ----- UDP -------------------------------------------------------- */

/*
 * Find the connection whose peer address matches peer.  Kept
 * for potential future use; UDP car updates now match by the
 * source_conn_id field in the packet body for NAT support.
 */
__attribute__((unused))
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

	/*
	 * Skip debug noise from very high-rate traffic: keepalives
	 * (0x13 / 0x17 / 0x16 pong at 1 Hz × N clients), car updates
	 * (0x1e at 18 Hz × N clients), and car-info requests (0x22
	 * bursts while the garage opens).  Every debug line is one
	 * snprintf + one write() to the log file, so on a busy server
	 * the hexdump alone turns into tens of MB/s of log traffic
	 * — enough that a blocking write() on a slow filesystem
	 * stalls the poll loop and shows up as in-game lag.  Rare /
	 * diagnostic packets still get the full hexdump.
	 */
	switch (msg_id) {
	case ACP_KEEPALIVE_A:
	case ACP_KEEPALIVE_B:
	case ACP_PONG_PHYSICS:
	case ACP_CAR_UPDATE:
	case ACP_CAR_INFO_REQUEST:
		break;
	default:
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

		/* Record when this keepalive was sent so the pong
		 * handler can compute round-trip time. */
		kc->keepalive_sent_ms = srv_ms;

		bb_init(&reply);
		if (wr_u8(&reply, SRV_KEEPALIVE_14) == 0 &&
		    wr_u32(&reply, srv_ms) == 0 &&
		    wr_u16(&reply, kc->conn_id) == 0 &&
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

	case ACP_PONG_PHYSICS: {	/* 0x16 */
		/*
		 * Client echoes the server timestamp from 0x14 plus
		 * its own client-side timestamp.  Compute RTT and
		 * clock offset per FUN_1400420e0 in accServer.exe.
		 *
		 * Wire: u8 0x16 + u16 conn_id + u32 srv_ts_echo +
		 *        u32 client_ts.
		 *
		 * RTT = server_now - srv_ts_echo.
		 * clock_offset = game_ms - (avg_rtt/2 + client_ts).
		 *
		 * The exe uses a game-relative timer (starts near 0)
		 * so the offset is a small correction (~rtt/2).  We
		 * derive game_ms from mono_ms - session_start_ms.
		 * The offset is subtracted from car timestamps in
		 * the per-peer 0x1e broadcast so the receiver sees
		 * timestamps in its own timebase.
		 */
		struct Reader pr;
		uint16_t pong_conn = 0;
		uint32_t pong_srv_ts = 0, pong_client_ts = 0;
		struct Conn *pc;
		struct timespec pts;
		uint32_t now_ms, rtt;

		rd_init(&pr, buf, len);
		(void)rd_skip(&pr, 1);		/* msg_id */
		(void)rd_u16(&pr, &pong_conn);
		(void)rd_u32(&pr, &pong_srv_ts);
		(void)rd_u32(&pr, &pong_client_ts);

		pc = server_find_conn(s, pong_conn);
		if (pc == NULL)
			return;

		clock_gettime(CLOCK_MONOTONIC, &pts);
		now_ms = (uint32_t)((uint64_t)pts.tv_sec * 1000 +
		    (uint64_t)pts.tv_nsec / 1000000);
		rtt = now_ms - pong_srv_ts;
		if (rtt > 5000)
			rtt = 5000;

		if (pc->avg_rtt_ms == 0)
			pc->avg_rtt_ms = rtt;
		else
			pc->avg_rtt_ms = (pc->avg_rtt_ms * 7 + rtt) / 8;

		/*
		 * On the FIRST pong, send a fresh 0x28 with the
		 * now-correct client time base.  The welcome
		 * sequence 0x28 had client_ts=0 (no pong yet),
		 * giving the client a ~1min timer offset from
		 * menu/loading time.
		 */
		if (pc->last_pong_client_ts == 0 &&
		    s->session.ts_valid) {
			struct ByteBuf bb;

			bb_init(&bb);
			if (wr_u8(&bb, SRV_LARGE_STATE_RESPONSE) == 0 &&
			    write_session_mgr_state(&bb, s,
				pong_client_ts, rtt) == 0)
				(void)conn_send_framed(pc,
				    bb.data, bb.wpos);
			bb_free(&bb);
		}
		pc->last_pong_client_ts = pong_client_ts;
		return;
	}

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
		if (c != NULL && c->hellbanned) {
			/*
			 * Shadow-banned: don't process or relay the car
			 * state so other clients see this car frozen.
			 */
			return;
		}
		(void)h_udp_car_update(s, c, buf, len);
		return;
	}

	case ACP_CAR_INFO_REQUEST:	/* 0x22 */
		(void)h_udp_car_info_request(s, buf, len);
		return;

	case ACP_TIME_EVENT: {		/* 0x5e */
		/*
		 * Client-reported latency check between two peers.  Body
		 * per FUN_1400250e0 (case 0x5e in the UDP dispatch inside
		 * FUN_140027f80):
		 *
		 *   u8  0x5e
		 *   u16 source_conn_id
		 *   u16 target_conn_id
		 *   u64 latency_raw_ms
		 *   u8  forward_as_chat   (1 = send 0x2b to target too)
		 *
		 * Server looks up both conns.  If forward_as_chat is set
		 * and both conns exist, logs
		 *   "CLIENT_TIME_CHECK_CHAT (carId) driver: Latency error: N ms"
		 * and sends the same message as 0x2b chat to the target.
		 */
		struct Reader r;
		uint8_t op, enable_chat = 0;
		uint16_t source_conn = 0, target_conn = 0;
		uint64_t latency_raw = 0;
		struct Conn *src, *dst;

		rd_init(&r, buf, len);
		if (rd_u8(&r, &op) < 0 ||
		    rd_u16(&r, &source_conn) < 0 ||
		    rd_u16(&r, &target_conn) < 0 ||
		    rd_u64(&r, &latency_raw) < 0 ||
		    rd_u8(&r, &enable_chat) < 0) {
			log_warn("udp 0x5e short from %s:%u",
			    inet_ntoa(peer->sin_addr),
			    ntohs(peer->sin_port));
			return;
		}
		src = server_find_conn(s, source_conn);
		dst = server_find_conn(s, target_conn);
		log_info("0x5e latency report: %u -> %u = %u ms (chat=%u)",
		    source_conn, target_conn,
		    (unsigned)latency_raw, (unsigned)enable_chat);
		if (enable_chat && src != NULL && dst != NULL) {
			char body_txt[96];
			const char *from = "?";
			struct ByteBuf out;

			if (src->car_id >= 0 &&
			    src->car_id < ACC_MAX_CARS) {
				struct CarEntry *car = &s->cars[src->car_id];
				if (car->driver_count > 0)
					from = car->drivers[0].last_name;
			}
			snprintf(body_txt, sizeof(body_txt),
			    "Latency error: %u ms",
			    (unsigned)latency_raw);
			bb_init(&out);
			if (wr_u8(&out, SRV_CHAT_OR_STATE) == 0 &&
			    wr_str_a(&out, from) == 0 &&
			    wr_str_a(&out, body_txt) == 0 &&
			    wr_i32(&out, 0) == 0 &&
			    wr_u8(&out, 4) == 0)
				(void)conn_send_framed(dst,
				    out.data, out.wpos);
			bb_free(&out);
		}
		return;
	}

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
		/*
		 * Observed noise from real ACC clients:
		 *   msg_id 195 (0xc3) with ~1200-byte body and bytes
		 *   `c3 00 00 00 01 08 ...` → QUIC v1 Initial packet
		 *   (RFC 9000: long-header form, fixed bit, Initial
		 *   type, pn_len=4; version `00 00 00 01` = QUIC v1;
		 *   DCID length 8; then AES-GCM-encrypted payload).
		 *   Likely a misdirected telemetry / background QUIC
		 *   probe from the client; neither accServer.exe's UDP
		 *   dispatcher nor SMPR handler compares against 0xc3,
		 *   so stock Kunos drops these too.  Client retries a
		 *   handful of times then gives up — the bursts are
		 *   the retry storm, not a sustained stream.
		 *
		 * We log WARN deliberately (useful operator signal if
		 * a *new* unknown id shows up) and drop without reply.
		 */
		log_warn("Received unknown UDP paket %u from %s:%u",
		    (unsigned)msg_id, inet_ntoa(peer->sin_addr),
		    ntohs(peer->sin_port));
		return;
	}
}
