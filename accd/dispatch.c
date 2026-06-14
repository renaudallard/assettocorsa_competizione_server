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
#include <stdlib.h>
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
	 * Reject a second ACP_REQUEST_CONNECTION on an already-
	 * authenticated conn.  Without this gate, an authenticated
	 * client (legitimate or hostile) can re-send 0x09 and:
	 *
	 *   * leak c->hs_echo (handshake.c unconditionally mallocs
	 *     a new echo buffer on every call, ≤ 16 KiB / re-handshake)
	 *   * re-allocate a fresh car slot via server_alloc_car,
	 *     orphaning the prior slot's `used=1` reservation until
	 *     session end (burns max_connections)
	 *   * preserve c->is_admin across the re-handshake (state
	 *     confusion -- not a privilege gain, but unexpected).
	 *
	 * The real ACC client never re-sends 0x09 after a successful
	 * handshake; the only path that does is a reconnect, which
	 * goes through conn_drop + a fresh socket.
	 */
	if (c->state == CONN_AUTH && msg_id == ACP_REQUEST_CONNECTION) {
		log_warn("tcp: duplicate 0x09 on authenticated conn=%u "
		    "(dropping)", (unsigned)c->conn_id);
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
		/* Test bot sends 0x45; real ACC clients send 0x46 (pcap
		 * 2026-05-19, paul_ricard live).  Both are the inbound dirt
		 * update; h_car_dirt stores it AND relays a 0x46 to peers,
		 * matching exe FUN_1400142f0 case 0x45 (which stores the
		 * dirt then broadcasts 0x46). */
	case SRV_CAR_DIRT_RELAY:
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

/*
 * The relay-timestamp projection offset for conn c, selected by
 * latencyStrategy, mirroring the exe projector FUN_140042030:
 *   Mode A (latency_mode != 0): the min-RTT session base
 *     session_clock_offset_ms (D_base; the field-proven path).
 *   Mode B (latency_mode == 0, the EXE DEFAULT): the slewed
 *     average-RTT offset i_fb_ms (I_fb).
 * The exe also folds a per-conn drift integrator (D_drift) into Mode A;
 * accd's Mode A omits it (a long-stint-only refinement), so Mode A is
 * D_base only.  Used by every relay-ts site (0x19/0x1b/0x3a/0x3b/0x3c)
 * and the 0x4f force=1 double.
 */
int64_t
conn_clock_offset(const struct Server *s, const struct Conn *c)
{
	if (s->latency_mode != 0)
		return c->session_clock_offset_ms;	/* Mode A */
	if (!c->i_fb_valid)
		return c->session_clock_offset_ms;	/* pre-first-pong */
	return c->i_fb_ms;				/* Mode B (default) */
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
		/*
		 * Refresh the UDP-liveness timer the 5 s reaper checks, the
		 * way the exe's 0x13 handler does (FUN_140041d90 sets conn+
		 * 0xa01e8 = now, the field FUN_14002f180's 5000 ms reaper
		 * reads).  A client that streams only keepalives (0x13/0x17)
		 * with no 0x16 pong or 0x1e physics for >5 s - e.g. paused or
		 * sitting in a menu - must stay alive like it does on the
		 * stock server.  A genuinely gone client sends no 0x13, so
		 * this never keeps a dead conn around.
		 */
		kc->last_udp_server_ms = (uint32_t)mono_ms();
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
		int new_min;

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
		/*
		 * Same IP-bind as 0x13 / 0x1e / 0x5e / 0x22: refuse a
		 * pong whose UDP source IP doesn't match the conn's
		 * accepted IP.  Without this, an attacker can guess a
		 * 16-bit conn_id and (a) poison pc->avg_rtt_ms (it
		 * folds into the server-wide ping average that every
		 * 0x14 broadcast carries), (b) drag the per-conn clock
		 * offset which the 0x4f force=1 relay derives ts from,
		 * and (c) trigger an extra first-pong 0x28 SRV_LARGE_
		 * STATE_RESPONSE.  c->peer is set at TCP accept time
		 * and re-anchored to the same IP on every 0x13 / 0x1e,
		 * so a clean comparison.
		 */
		if (pc->peer.sin_addr.s_addr != peer->sin_addr.s_addr)
			return;

		now_ms = (uint32_t)mono_ms();
		rtt = now_ms - pong_srv_ts;
		/*
		 * No 5000 ms cap: kunos's FUN_1400420e0 uses the raw
		 * (now - srv_ts) sample; only the avg*3 spike clamp below
		 * bounds an outlier, and the windowed mean dilutes it.
		 *
		 * First-pong / new-minimum-RTT gate, mirroring
		 * FUN_1400420e0:23-37.  The session-relative clock offset
		 * (the exe's D_base, 0x280c4) is latched here from the RAW
		 * rtt before the spike clamp - a new minimum is never a
		 * spike.  This is the offset every relay timestamp
		 * (0x19/0x1b/0x3a/0x3b/0x3c/0x4f) projects through, and the
		 * same condition drives the fresh 0x28 re-emit at the end.
		 * Anchor to session-start mono_ms so the value stays bounded
		 * regardless of host uptime.  (The exe also folds in a slow
		 * per-conn drift integrator, 0x280d0, that we do not track;
		 * it matters only over a long stint.)
		 */
		new_min = (!pc->session_clock_seen || rtt < pc->best_rtt_ms);
		/*
		 * A future / forged pong_srv_ts makes (now - srv_ts) wrap to
		 * a huge unsigned value whose signed form is negative.  Never
		 * latch such a sample: otherwise the first pong would pin
		 * best_rtt_ms near 4e9 forever and skew every Mode-A relay
		 * clock for the whole session.  Skipping it leaves
		 * session_clock_seen clear so the next sane pong latches,
		 * mirroring the exe's sign-bit "seen" sentinel.
		 */
		if (new_min && (int32_t)rtt >= 0) {
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
		 * Spike clamp on the windowed mean as of the previous pong,
		 * then push the (clamped) sample into the 50-slot ring and
		 * recompute the arithmetic mean over the filled slots - the
		 * exe's RTT estimator (FUN_1400420e0:39-71: 50-entry ring,
		 * avg = sum(non-negative)/count).
		 */
		if (pc->avg_rtt_ms == 0)
			log_info("pong: first sample conn=%u rtt=%u ms",
			    (unsigned)pong_conn, (unsigned)rtt);
		else if (pc->avg_rtt_ms * 3 < rtt) {
			uint32_t cap = pc->avg_rtt_ms * 3;

			log_kunos("Received Ping spike from connectionId %u; %u vs. avg %u ms, is capped to %u",
			    (unsigned)pong_conn, (unsigned)rtt,
			    (unsigned)pc->avg_rtt_ms, (unsigned)cap);
			rtt = cap;
		}
		pc->rtt_ring_idx =
		    (pc->rtt_ring_idx + 1 > RTT_RING_SLOTS - 1)
		    ? 0 : pc->rtt_ring_idx + 1;
		pc->rtt_ring[pc->rtt_ring_idx] = (int32_t)rtt;
		{
			uint64_t sum = 0;
			int cnt = 0, k;

			for (k = 0; k < RTT_RING_SLOTS; k++) {
				if (pc->rtt_ring[k] < 0)
					continue;
				sum += (uint32_t)pc->rtt_ring[k];
				cnt++;
			}
			if (cnt > 0)
				pc->avg_rtt_ms =
				    (uint32_t)(sum / (unsigned)cnt);
		}
		/*
		 * Averaged-rtt/2 clock offset - the exe's FUN_1400420e0:82
		 * stat field (param_1[0x2802a]).  Stat/CSV only, no wire
		 * consumer (the relays use session_clock_offset_ms above).
		 */
		pc->clock_offset_ms = (int32_t)(now_ms -
		    (pc->avg_rtt_ms / 2 + pong_client_ts));
		pc->last_udp_client_ts = pong_client_ts;
		pc->last_udp_server_ms = now_ms;
		/*
		 * Session-relative average-RTT offset (the exe I_avg in the
		 * session frame, recomputed every pong): the slew target for
		 * the Mode-B relay-ts offset i_fb_ms.  clock_offset_ms above
		 * is the boot-relative twin and is stat-only, so the session
		 * frame is computed separately here.  Seed i_fb on the first
		 * pong so the default Mode-B projection has a valid offset
		 * before the first slew tick.
		 */
		{
			uint64_t session_now =
			    mono_ms() - s->session.phase_started_ms;

			pc->session_avg_offset_ms = (int64_t)session_now -
			    (int64_t)(pc->avg_rtt_ms / 2) -
			    (int64_t)pong_client_ts;
			if (!pc->i_fb_valid) {
				pc->i_fb_ms = pc->session_avg_offset_ms;
				pc->i_fb_valid = 1;
			}
		}

		/*
		 * Send a fresh 0x28 with the now-correct client time base
		 * on the first pong AND on any later new-minimum-RTT pong.
		 * The exe rebuilds the 0x28 whenever the pong handler
		 * returns 1 (FUN_1400420e0:24 -> FUN_140027f80:268), which
		 * is exactly the new_min gate above; emitting only on the
		 * literal first pong left the client's timer slightly stale
		 * after a sharper RTT estimate arrived.
		 */
		if (new_min && s->session.ts_valid) {
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
		/*
		 * Same IP-bind as 0x13: refuse a peer update if the
		 * UDP source IP differs from the conn's accepted IP.
		 * Stops a forged 0x1e from hijacking the per-peer
		 * sendto destination.  Allow port changes for NAT.
		 */
		if (c != NULL &&
		    c->peer.sin_addr.s_addr != peer->sin_addr.s_addr)
			return;
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
		(void)h_udp_car_info_request(s, peer, buf, len);
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
		 *   f64 latency_ms        (IEEE-754 double, printed as int)
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
		double latency_ms = 0;
		struct Conn *src, *dst;

		rd_init(&r, buf, len);
		if (rd_u8(&r, &op) < 0 ||
		    rd_u16(&r, &source_conn) < 0 ||
		    rd_u16(&r, &target_conn) < 0 ||
		    rd_f64(&r, &latency_ms) < 0 ||
		    rd_u8(&r, &enable_chat) < 0) {
			log_warn("udp 0x5e short from %s:%u",
			    inet_ntoa(peer->sin_addr),
			    ntohs(peer->sin_port));
			return;
		}
		src = server_find_conn(s, source_conn);
		dst = server_find_conn(s, target_conn);
		log_info("0x5e latency report: %u -> %u = %d ms (chat=%u)",
		    source_conn, target_conn,
		    (int)latency_ms, (unsigned)enable_chat);
		/*
		 * Refuse the chat relay if the UDP source IP doesn't
		 * match the source conn's accepted IP -- otherwise any
		 * peer can forge a "Latency error: N ms" chat under
		 * another driver's name by picking their conn_id.
		 */
		if (src != NULL &&
		    src->peer.sin_addr.s_addr != peer->sin_addr.s_addr)
			return;
		if (enable_chat == 1 && src != NULL && dst != NULL &&
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
			    "Latency error: %d ms",
			    (int)latency_ms);
			bb_init(&out);
			/*
			 * chat_type 0 = player-chat lane (renders in the
			 * chat window attributed to the source driver),
			 * matching the exe (FUN_140027f80:505) and accd's
			 * own h_chat relay.  Type 4 would show as a
			 * sender-less SRV banner despite the named sender.
			 */
			if (wr_u8(&out, SRV_CHAT_OR_STATE) == 0 &&
			    wr_str_a(&out, from) == 0 &&
			    wr_str_a(&out, body_txt) == 0 &&
			    wr_i32(&out, 0) == 0 &&
			    wr_u8(&out, 0) == 0)
				(void)conn_send_framed(dst,
				    out.data, out.wpos);
			bb_free(&out);
		}
		return;
	}

	case ACP_ADMIN_QUERY: {		/* 0x5f */
		/*
		 * kson lobby identity handshake (exe FUN_140027f80:174-228).
		 * The caller sends our 10-char token_b -- the random
		 * per-launch token the lobby registration published (lobby.c,
		 * exe FUN_1400449c0); we reply with the 64-char token_a.  Reply
		 * ONLY on a token match: the exe gates the same way, so a UDP
		 * source that doesn't know token_b gets no reply.  This closes
		 * the previous unconditional server_name reply, which was both
		 * a UDP reflection vector and the wrong content.
		 */
		struct Reader qr;
		char *query = NULL;
		struct ByteBuf reply;

		rd_init(&qr, buf, len);
		(void)rd_skip(&qr, 1);		/* msg_id */
		if (rd_str_raw(&qr, &query) < 0 || query == NULL)
			return;
		if (strcmp(query, s->lobby.token_b) != 0) {
			log_debug("udp 0x5f: token mismatch from %s:%u",
			    inet_ntoa(peer->sin_addr), ntohs(peer->sin_port));
			free(query);
			return;
		}
		free(query);
		log_info("udp 0x5f identity query (token ok) from %s:%u",
		    inet_ntoa(peer->sin_addr), ntohs(peer->sin_port));
		/*
		 * Reply is a bare kson byte-string [u16 64][64 raw bytes]
		 * of token_a -- NO opcode prefix and NOT Format-A.  The exe
		 * (FUN_140027f80:211-219 -> writeKsonString FUN_14004d240)
		 * writes only the kson string; cf the 0x17 branch which DOES
		 * store its opcode, proving 0x5f deliberately omits it.
		 */
		bb_init(&reply);
		if (wr_str_raw(&reply, s->lobby.token_a) == 0) {
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
