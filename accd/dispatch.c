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
#include <string.h>
#include <time.h>

#include "bcast.h"
#include "dispatch.h"
#include "handlers.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "smpr.h"
#include "state.h"
#include "tick.h"

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

	/*
	 * SMPR (ServerMonitor) demux.  Kunos shares the gameplay
	 * tcpPort with the protobuf monitoring channel; the first
	 * body byte tells the lanes apart.  0x0a is the protobuf
	 * tag for field 1 wire-type 2 (length-delimited), which is
	 * how every ServerMonitorConnectionRequest begins (field 1
	 * is `string displayName`).  No sim opcode in msg.h equals
	 * 0x0a, so the demux is unambiguous.  After the hello the
	 * conn is push-only; we silently drop any inbound frames.
	 */
	if (msg_id == 0x0a && c->state == CONN_UNAUTH && !c->is_smpr)
		return smpr_handle_connect(s, c, body, len);
	if (c->is_smpr)
		return 0;

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
		 * Two distinct callers, distinguished by body length:
		 *
		 *   3-byte form  msg_id + u16(conn_id)
		 *     The real client sends this once a second from its
		 *     in-session UDP socket.  The server replies with a
		 *     15-byte 0x14 keepalive carrying server_ms + per-conn
		 *     ping aggregates; this also teaches the server which
		 *     UDP source port belongs to which TCP connection so
		 *     subsequent 0x1e car_updates can be relayed.
		 *
		 *   7-byte form  msg_id + u32(client_ts) + u16(server_port)
		 *     The lobby's server-browser probe (FUN_1410f2370 in
		 *     the AC2 client).  Sent by every client browsing the
		 *     server list, before any TCP connect.  The exe
		 *     unconditionally echoes the body back as a 7-byte
		 *     0x17 reply (FUN_140027f80 lines 619-635); the client
		 *     measures `local_now - client_ts` from the echoed
		 *     timestamp to populate the PING column.  Without a
		 *     reply the lobby browser shows "—".
		 */
		struct Reader kr;
		uint16_t ka_conn_id = 0;
		struct Conn *kc;

		if (len >= 7) {
			/* Lobby-probe echo — must reply regardless of
			 * whether the source has a TCP connection. */
			unsigned char echo[7];
			echo[0] = ACP_KEEPALIVE_B;	/* 0x17 */
			memcpy(echo + 1, buf + 1, 6);
			(void)sendto(s->udp_fd, echo, sizeof echo, 0,
			    (const struct sockaddr *)peer,
			    (socklen_t)sizeof(*peer));
			return;
		}

		rd_init(&kr, buf, len);
		(void)rd_skip(&kr, 1);		/* msg_id */
		(void)rd_u16(&kr, &ka_conn_id);

		kc = server_find_conn(s, ka_conn_id);
		if (kc == NULL)
			return;

		/*
		 * Bind the UDP peer to the conn's accepted IP.  c->peer
		 * starts as the TCP socket peer (set by conn_new at
		 * accept time) and stays anchored to that IP across UDP
		 * peer learning; only the port is allowed to change to
		 * tolerate NAT rebinds.  Without this gate, any UDP
		 * sender that knows or guesses a conn_id can overwrite
		 * c->peer and redirect that conn's outbound 0x14 / 0x1e
		 * traffic to itself.
		 */
		if (kc->peer.sin_addr.s_addr != peer->sin_addr.s_addr)
			return;
		/*
		 * Learn / update the UDP peer port.  Do NOT emit a 0x14
		 * here: per FUN_140041e80 in accServer.exe, 0x14 has a
		 * single emit path gated on a 1000 ms cadence per conn -
		 * matched by tick.c's broadcast_keepalive(SRV_KEEPALIVE_14).
		 * Replying to every 0x13 doubled the rate to 2 Hz/conn,
		 * confirmed by a 2-bot pcap diff against the exe (kunos
		 * 1.0 Hz/conn vs accd 2.0 Hz/conn).
		 */
		kc->peer = *peer;
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
		uint32_t now_ms, rtt;

		rd_init(&pr, buf, len);
		(void)rd_skip(&pr, 1);		/* msg_id */
		(void)rd_u16(&pr, &pong_conn);
		(void)rd_u32(&pr, &pong_srv_ts);
		(void)rd_u32(&pr, &pong_client_ts);

		pc = server_find_conn(s, pong_conn);
		if (pc == NULL)
			return;
		/*
		 * SMPR observers don't speak the sim protocol: the server
		 * never sends them a 0x14 keepalive (broadcast_keepalive
		 * filters is_smpr), so a 0x16 arriving for an SMPR slot is
		 * forged.  Ignoring it keeps an injected pong from (a)
		 * writing a sim 0x28 SRV_LARGE_STATE_RESPONSE into the
		 * observer's TCP stream via the first-pong path below and
		 * (b) polluting pc->avg_rtt_ms, which compute_server_pings
		 * folds into the server-wide ping average broadcast to
		 * real drivers in every 0x14.
		 */
		if (pc->is_smpr)
			return;

		now_ms = (uint32_t)mono_ms();
		rtt = now_ms - pong_srv_ts;
		if (rtt > 5000)
			rtt = 5000;

		if (pc->avg_rtt_ms == 0) {
			pc->avg_rtt_ms = rtt;
			log_info("pong: first sample conn=%u rtt=%u ms",
			    (unsigned)pong_conn, (unsigned)rtt);
		} else {
			/*
			 * Spike clamp: cap a single bad sample at 3x the
			 * running average so the EMA can't be dragged by
			 * an outlier (a real network glitch is fine; a
			 * 5-second hiccup shouldn't permanently inflate
			 * the leaderboard ping column).  Mirrors kunos's
			 * FUN_1400420e0 (line 40-44 in
			 * notebook-a/decomp/full/1400420e0.c):
			 *   if (avg != 0 && avg*3 < rtt) {
			 *       log("Received Ping spike ...");
			 *       rtt = avg * 3;
			 *   }
			 */
			if (pc->avg_rtt_ms * 3 < rtt) {
				uint32_t cap = pc->avg_rtt_ms * 3;

				log_kunos("Received Ping spike from connectionId %u; %u vs. avg %u ms, is capped to %u",
				    (unsigned)pong_conn,
				    (unsigned)rtt,
				    (unsigned)pc->avg_rtt_ms,
				    (unsigned)cap);
				rtt = cap;
			}
			pc->avg_rtt_ms = (pc->avg_rtt_ms * 7 + rtt) / 8;
		}
		/*
		 * Clock offset — exe's FUN_1400420e0 stores
		 *   param_1[0x2802a] = server_now - (rtt/2 + client_ts)
		 * as the fixed offset between server and client clocks.
		 * Updated on every pong.  Used only by our latency CSV
		 * dump today (no wire consumer), but tracking it here
		 * means the dump matches the exe's semantics and a future
		 * FUN_140042030-style projection helper can read it.
		 */
		pc->clock_offset_ms = (int32_t)(now_ms -
		    (pc->avg_rtt_ms / 2 + pong_client_ts));
		pc->last_udp_client_ts = pong_client_ts;
		pc->last_udp_server_ms = now_ms;
		/*
		 * Session-relative clock offset for the 0x4f force=1
		 * relay's IEEE-754 ts.  Anchor to the session-start
		 * mono_ms so the value stays bounded regardless of host
		 * uptime, matching kunos's FUN_140042030 output range.
		 * Latch on the FIRST pong, then refresh only when a
		 * lower RTT (sharper estimate) arrives — same gate as
		 * kunos's FUN_1400420e0:23.
		 */
		if (!pc->session_clock_seen || rtt < pc->best_rtt_ms) {
			uint64_t session_now =
			    mono_ms() - s->session.phase_started_ms;
			pc->session_clock_offset_ms =
			    (int64_t)session_now -
			    (int64_t)(rtt / 2) -
			    (int64_t)pong_client_ts;
			pc->best_rtt_ms = rtt;
			pc->session_clock_seen = 1;
		}

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
		if (enable_chat && src != NULL && dst != NULL &&
		    !dst->is_smpr) {
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
