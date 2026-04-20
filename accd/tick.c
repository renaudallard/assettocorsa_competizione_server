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
 * tick.c -- periodic server tick.
 *
 * Drives session state advancement and the periodic broadcasts
 * documented in §5.6.4a.  Phase 2 implements the per-car state
 * fan-out: every N ticks, for every car that has received an
 * ACP_CAR_UPDATE since last tick, build a SRV_PERCAR_FAST_RATE
 * (0x1e) broadcast and send it to every other connection.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "bcast.h"
#include "chat.h"
#include "ratings.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "penalty.h"
#include "prim.h"
#include "results.h"
#include "session.h"
#include "state.h"
#include "tick.h"
#include "weather.h"

/*
 * Broadcast cadences, in wall-clock milliseconds.
 *
 * The exe's main() calls CreateTimerQueueTimer(Period=3 ms), i.e. a
 * 333 Hz tick, and its cadences are all expressed as integer tick
 * counts (DAT_14014bd* in the .rdata).  Our reimpl runs one
 * poll()-driven tick per main-loop iteration — but on OpenBSD
 * `poll()` rounds short timeouts up to ~20 ms, which pins the loop
 * at ~50 Hz regardless of TICK_INTERVAL_MS.  Tick-count modulo
 * gates therefore fire at ~1/6.7 of their intended wall-clock rate.
 *
 * Fix: gate every cadence on a per-cadence `last_fired_ms` counter
 * measured against mono_ms().  Rate is now a property of the
 * cadence itself, not of the underlying tick period.
 */
#define CADENCE_SESSION_STATE_MS	1000	/* 0x28 ~1 Hz per-client */
#define CADENCE_KEEPALIVE_MS		1000	/* 0x14 ~1 Hz */
#define CADENCE_WEATHER_MS		5000	/* 0x37 every 5 s */
#define CADENCE_LEADERBOARD_MS		75000	/* 0x36 async-coalesce */
#define CADENCE_RATINGS_MS		81000	/* 0x4e debounce (.rdata) */

static uint64_t
tick_mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull +
	    (uint64_t)ts.tv_nsec / 1000000ull;
}

/*
 * Write the 63-byte per-car body used by both 0x1e and each
 * 0x39 batch element.  Layout from FUN_14001a170 / FUN_14001a6a0
 * in accServer.exe:
 *
 *   u16 car_id (+0x150 in the per-car struct)
 *   u8  seq (+0x2d)
 *   i32 adjusted_timestamp (+0x3c minus per-peer offset)
 *   u16 (+0x50, typically 0)
 *   3 * 12 bytes vec_a / vec_b / vec_c (positions / rotations)
 *   4 * u8 input_a (+0x2e..+0x31)
 *   u8 (+0x32)
 *   u8 (+0x33)
 *   u16 (+0x36)
 *   u8 (+0x2c)
 *   u8 (+0x34)
 *   u8 (+0x35)
 *   4 * u8 input_b (+0x48..+0x4b)
 *   u8 (+0x4c)
 *   i16 clamped (+0x1ec)
 */
/*
 * @clock_adj: per-peer clock offset (from pong RTT computation).
 *             Subtracted from car_ts so the receiver sees
 *             timestamps in its own timebase.  Pass 0 if unknown.
 */
int
build_percar_body(struct ByteBuf *bb, struct CarEntry *car,
    struct Server *s, int32_t clock_adj)
{
	int k, ok;
	int16_t clamped;
	uint32_t adj_ts;
	uint16_t sender_rtt_ms = 0;
	int j;

	/*
	 * Per-peer timestamp adjustment matching FUN_14001a170.
	 *
	 * clock_adj = sender_pong_ts - peer_pong_ts: the delta
	 * between the two clients' game clocks as observed from
	 * their most recent pong exchanges.  Subtracting it from
	 * car_ts converts from sender timebase to peer timebase,
	 * enabling correct dead-reckoning on the receiver.
	 *
	 * This uses client-to-client timestamps only, avoiding
	 * the server clock entirely, so it works regardless of
	 * the server's monotonic clock epoch.
	 */
	adj_ts = (uint32_t)((int32_t)car->rt.client_timestamp_ms
	    - clock_adj);

	/*
	 * FUN_14001a170 emits a u16 at conn+0x50 here — the sender
	 * connection's server-measured avg RTT.  Receivers read it
	 * to render the per-car ping column on the HUD timing tower.
	 * Capture-based analysis earlier said this was 0, but that
	 * was because the capture came from a loopback test where
	 * RTT collapses to 0 before the pong smoothing kicks in;
	 * a real client session needs the live value.
	 */
	for (j = 0; j < ACC_MAX_CARS; j++) {
		struct Conn *sender = s->conns[j];
		if (sender != NULL && sender->car_id ==
		    (int)(car - s->cars)) {
			if (sender->avg_rtt_ms > 65535)
				sender_rtt_ms = 65535;
			else
				sender_rtt_ms =
				    (uint16_t)sender->avg_rtt_ms;
			break;
		}
	}

	ok = 1;
	if (wr_u16(bb, car->car_id) < 0) return -1;
	if (wr_u8(bb, car->rt.packet_seq) < 0) return -1;
	if (wr_u32(bb, adj_ts) < 0) return -1;
	if (wr_u16(bb, sender_rtt_ms) < 0) return -1;

	for (k = 0; k < 3 && ok; k++)
		ok = wr_f32(bb, car->rt.vec_a[k]) == 0;
	for (k = 0; k < 3 && ok; k++)
		ok = wr_f32(bb, car->rt.vec_b[k]) == 0;
	for (k = 0; k < 3 && ok; k++)
		ok = wr_f32(bb, car->rt.vec_c[k]) == 0;
	for (k = 0; k < 4 && ok; k++)
		ok = wr_u8(bb, car->rt.input_a[k]) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_32) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_33) == 0;
	if (ok) ok = wr_u16(bb, car->rt.scalar_36) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_2c) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_34) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_35) == 0;
	for (k = 0; k < 4 && ok; k++)
		ok = wr_u8(bb, car->rt.input_b[k]) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_4c) == 0;

	clamped = car->rt.scalar_1ec;
	if (ok) ok = wr_i16(bb, clamped) == 0;
	return ok ? 0 : -1;
}

/*
 * Periodic per-car fan-out.  Walks every car with rt.dirty set,
 * emits one 0x1e (or 0x39 count=1 in legacy-netcode mode) per
 * dirty car to every other authenticated peer, then clears the
 * dirty flag.  Matches FUN_14001a170 / FUN_14001a6a0 in the exe,
 * which are gated by the legacy_netcode toggle at srv+0x22 and
 * called once per scheduler tick from FUN_14002e8d0.
 *
 * Replaces the prior event-driven relay (h_udp_car_update ->
 * per-peer sendto per incoming update).  At ~18 Hz × peer_count
 * that flooded the client's UDP queue with ~520 pps per peer and
 * surfaced as position jitter + visual blinking on real clients.
 */
static void
broadcast_percar_dirty(struct Server *s)
{
	struct ByteBuf bb;
	int i, j;
	uint8_t msg_id = s->legacy_netcode
	    ? SRV_PERCAR_SLOW_RATE : SRV_PERCAR_FAST_RATE;

	if (s->udp_fd < 0)
		return;

	bb_init(&bb);
	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct CarEntry *car = &s->cars[i];
		struct Conn *sender = NULL;

		if (!car->used || !car->rt.dirty || !car->rt.has_data)
			continue;

		for (j = 0; j < ACC_MAX_CARS; j++) {
			if (s->conns[j] != NULL &&
			    s->conns[j]->car_id == i) {
				sender = s->conns[j];
				break;
			}
		}

		for (j = 0; j < ACC_MAX_CARS; j++) {
			struct Conn *peer = s->conns[j];
			int32_t delta = 0;

			if (peer == NULL || peer->state != CONN_AUTH)
				continue;
			if (peer->car_id == i)
				continue;
			/*
			 * Only apply the per-peer client-timestamp delta
			 * once both endpoints have pong'd at least once;
			 * before that last_pong_client_ts is 0 and using
			 * it would emit a huge synthetic offset (~now)
			 * into the client's smoothing filter.
			 */
			if (sender != NULL &&
			    sender->last_pong_client_ts != 0 &&
			    peer->last_pong_client_ts != 0)
				delta = (int32_t)(sender->last_pong_client_ts -
				    peer->last_pong_client_ts);

			bb_clear(&bb);
			if (wr_u8(&bb, msg_id) < 0)
				continue;
			if (s->legacy_netcode &&
			    wr_u8(&bb, 1) < 0)
				continue;
			if (build_percar_body(&bb, car, s, delta) < 0)
				continue;
			(void)sendto(s->udp_fd, bb.data, bb.wpos, 0,
			    (const struct sockaddr *)&peer->peer,
			    sizeof(peer->peer));
		}

		car->rt.dirty = 0;
	}
	bb_free(&bb);
}

/*
 * Send a 0x14 keepalive to each authenticated connection via
 * UDP.  The exe (FUN_140029b20) sends this per-peer over UDP;
 * the client replies with 0x16 pong only to UDP keepalives.
 * If sent via TCP (our old path), the client ignores it and
 * never pongs, so clock_offset_ms stays 0 and per-peer
 * timestamp adjustment is a no-op.
 *
 * Body: u8 0x14 + u32 server_ms + additional timing hints.
 * We send a minimal version with the server timestamp.
 */
static void
broadcast_keepalive(struct Server *s, uint8_t msg_id)
{
	/*
	 * Kunos FUN_140029b20 / FUN_1400336d0 body:
	 *   u8  msg_id (0x14)
	 *   u32 server_ms
	 *   u16 conn_id of THIS recipient     (exe: param_3, per-peer)
	 *   u16 conn stats slot (+0x10)        (0 — we don't track)
	 *   u16 conn stats slot (+0x12)        (0)
	 *   u8  2 / u8 4 / u8 100 / u8 100     (fixed timing hints)
	 * Total 15 bytes.  The conn_id is the per-recipient echo — the
	 * client filters on `conn_id == my_conn_id` before emitting
	 * a 0x16 pong, so a hardcoded 0 kept every client's ping HUD
	 * at `-- ms` and left avg_rtt_ms stuck at 0 on the server.
	 */
	unsigned char pkt[15];
	int i;
	struct timespec ts;
	uint32_t srv_ms;

	if (s->udp_fd < 0)
		return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	srv_ms = (uint32_t)((uint64_t)ts.tv_sec * 1000 +
	    (uint64_t)ts.tv_nsec / 1000000);

	pkt[0]  = msg_id;
	pkt[1]  = (unsigned char)(srv_ms & 0xff);
	pkt[2]  = (unsigned char)((srv_ms >> 8) & 0xff);
	pkt[3]  = (unsigned char)((srv_ms >> 16) & 0xff);
	pkt[4]  = (unsigned char)((srv_ms >> 24) & 0xff);
	pkt[7]  = 0;
	pkt[8]  = 0;
	pkt[9]  = 0;
	pkt[10] = 0;
	pkt[11] = 2;
	pkt[12] = 4;
	pkt[13] = 100;
	pkt[14] = 100;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];

		if (c == NULL || c->state != CONN_AUTH)
			continue;
		pkt[5] = (unsigned char)(c->conn_id & 0xff);
		pkt[6] = (unsigned char)((c->conn_id >> 8) & 0xff);
		c->keepalive_sent_ms = srv_ms;
		(void)sendto(s->udp_fd, pkt, sizeof(pkt), 0,
		    (const struct sockaddr *)&c->peer,
		    sizeof(c->peer));
	}
}

/*
 * Build and emit the SRV_LEADERBOARD_BCAST (0x36) when the
 * standings have changed.  Matches FUN_14002f710 in accServer.exe:
 * the body is `u8 0x36 + FUN_140034a40 output` (the same leaderboard
 * section embedded in the welcome trailer), so we reuse the shared
 * write_leaderboard_section helper from handshake.c instead of
 * hand-rolling a simplified record.
 */
static void
broadcast_leaderboard(struct Server *s)
{
	struct ByteBuf bb;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_LEADERBOARD_BCAST) < 0)
		goto done;
	if (write_leaderboard_section(&bb, s) < 0)
		goto done;
	(void)bcast_all(s, bb.data, bb.wpos, 0xFFFF);
	log_info("Updated leaderboard for %d clients", s->nconns);
done:
	bb_free(&bb);
}

/*
 * Build and emit the SRV_GRID_POSITIONS (0x3f) at the start of
 * the RACE phase.  Body: u8 grid_count + per-car { u16 carId +
 * u8 flag_a + u32 grid_position + u8 flag_b }.
 */
static void
broadcast_grid(struct Server *s)
{
	struct ByteBuf bb;
	int i, g, n = 0, emitted = 0;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_GRID_POSITIONS) < 0)
		goto done;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++)
		if (s->cars[i].used)
			n++;
	if (wr_u8(&bb, (uint8_t)n) < 0)
		goto done;
	/*
	 * Emit in grid order — FUN_140032400 in the exe assigns grid
	 * slots and walks them in ascending order when building the
	 * 0x3f payload, and the client infers each car's starting
	 * position from the record sequence.  Cars without a valid
	 * grid slot trail in car_id order.
	 */
	for (g = 0; g <= ACC_MAX_CARS && emitted < n; g++) {
		for (i = 0; i < ACC_MAX_CARS && i < s->max_connections;
		    i++) {
			struct CarEntry *car = &s->cars[i];
			if (!car->used)
				continue;
			if (car->race.grid_position != g)
				continue;
			if (wr_u16(&bb, car->car_id) < 0 ||
			    wr_u8(&bb, 0) < 0 ||
			    wr_u32(&bb, (uint32_t)g) < 0 ||
			    wr_u8(&bb, 0) < 0)
				goto done;
			emitted++;
		}
	}
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections &&
	    emitted < n; i++) {
		struct CarEntry *car = &s->cars[i];
		if (!car->used)
			continue;
		if (car->race.grid_position >= 0 &&
		    car->race.grid_position <= ACC_MAX_CARS)
			continue;
		if (wr_u16(&bb, car->car_id) < 0 ||
		    wr_u8(&bb, 0) < 0 ||
		    wr_u32(&bb, (uint32_t)i) < 0 ||
		    wr_u8(&bb, 0) < 0)
			goto done;
		emitted++;
	}
	(void)bcast_all(s, bb.data, bb.wpos, 0xFFFF);
	log_info("Sending grid positions: %d cars", n);
done:
	bb_free(&bb);
}

/*
 * Build and emit SRV_SESSION_RESULTS (0x3e) at end of session.
 *
 * Wire format per accServer.exe FUN_1400197b0 + FUN_1400351f0:
 *   u8 0x3e
 *   u8 result_count
 *   result_count × (23-byte result_header + per-car leaderboard_section)
 *
 * The 23-byte result_header is emitted from a per-car results struct
 * at stride 0x150 in FUN_1400351f0.  Our struct CarRaceState carries
 * enough to populate all fields except the +0x60 u16 (unknown
 * semantic, always 0 in welcome scenarios); the +0x74 u32 time
 * penalty is computed via penalty_total_ms().
 *
 * We used to emit write_session_tail (23 bytes) here — same byte
 * count matched the 81-min capture sizes (235/468/706 for 1/2/3
 * completed sessions) but the field semantics were session metadata,
 * not per-car race results, so any client rendering position / lap
 * count / best lap from these bytes saw garbage.
 *
 * result_count is session_index+1 when called at end-of-session,
 * matching the observed cumulative behavior.
 */
static int
write_result_header(struct ByteBuf *bb, const struct CarEntry *car,
    const struct CarRaceState *r)
{
	uint8_t position = r->position > 0 && r->position < 0xff
	    ? (uint8_t)r->position : 0;
	/*
	 * FUN_1400351f0 writes `cVar3 + -1` where cVar3 is the byte at
	 * entry+0x58.  The exe stores that byte 1-based (driver #1,
	 * #2, ...) so the minus-one transforms it to the 0-based wire
	 * index the client expects.  Our current_driver_index is
	 * already 0-based, so emit it directly — subtracting here
	 * wrapped to 0xff for the first driver and broke the result
	 * screen.
	 */
	uint8_t drv_idx = car->current_driver_index;
	uint32_t penalty_ms = penalty_total_ms(&r->pen);
	/*
	 * +0x74 penalty_ms = sum of time penalties the client should
	 * add to race_time_ms for final standings.  See
	 * penalty_total_ms() in penalty.c for the kind-to-seconds
	 * conversion (admin TP5/TP15 are unconditional; unserved
	 * DT/SG at race end convert to 30/40/50/60 s per handbook
	 * V.1.8.11 / FUN_140127440 decomp).
	 */

	if (wr_u8(bb, position) < 0) return -1;		/* +0x50 */
	if (wr_u8(bb, position) < 0) return -1;		/* +0x54 cup_pos */
	if (wr_u8(bb, drv_idx) < 0) return -1;		/* +0x58 0-based drv */
	if (wr_u32(bb, (uint32_t)r->lap_count) < 0) return -1;	/* +0x5c */
	if (wr_u16(bb, 0) < 0) return -1;		/* +0x60 unknown */
	if (wr_u32(bb, r->best_lap_ms > 0
	    ? (uint32_t)r->best_lap_ms : 0x7FFFFFFFu) < 0) return -1;	/* +0x64 */
	if (wr_u32(bb, r->race_time_ms > 0
	    ? (uint32_t)r->race_time_ms : 0) < 0) return -1;	/* +0x68 */
	if (wr_u8(bb, r->formation_lap_done) < 0) return -1;	/* +0x6c */
	if (wr_u8(bb, r->disqualified) < 0) return -1;	/* +0x70 */
	if (wr_u32(bb, penalty_ms) < 0) return -1;	/* +0x74 penalty ms */
	return 0;
}

static void
broadcast_session_results(struct Server *s)
{
	int i, n;
	uint8_t result_count;

	if (s->session_count == 0)
		return;
	result_count = (uint8_t)(s->session.session_index + 1);
	if (result_count > s->session_count)
		result_count = s->session_count;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];
		struct ByteBuf bb;
		int ok = 1;
		int j, used = 0;

		if (c == NULL || c->state != CONN_AUTH)
			continue;
		for (j = 0; j < ACC_MAX_CARS; j++)
			if (s->cars[j].used)
				used++;
		if (used == 0)
			continue;
		bb_init(&bb);
		ok = ok && wr_u8(&bb, SRV_SESSION_RESULTS) == 0;
		ok = ok && wr_u8(&bb, result_count) == 0;
		/*
		 * FUN_1400351f0 walks a per-session-results vector and
		 * emits one (header + leaderboard) pair per completed
		 * session, each describing THAT session's leading car.
		 * We used to pick the first used car for every iteration,
		 * so the end-of-session screen for sessions 1+ showed the
		 * wrong driver and times.  Resolve the session leader from
		 * the archive slot (or live state for the current
		 * session).
		 */
		for (n = 0; n < result_count && ok; n++) {
			int leader = -1;
			const struct CarRaceState *src = NULL;
			int cur = s->session.session_index;

			for (j = 0; j < ACC_MAX_CARS; j++) {
				if (!s->cars[j].used)
					continue;
				if (n == cur) {
					if (s->cars[j].race.position == 1) {
						leader = j;
						src = &s->cars[j].race;
						break;
					}
				} else if (n < cur &&
				    n < ACC_MAX_SESSIONS &&
				    s->cars[j].race_archive[n] != NULL &&
				    s->cars[j].race_archive[n]->position
					== 1) {
					leader = j;
					src = s->cars[j].race_archive[n];
					break;
				}
			}
			if (leader < 0) {
				/* Fallback: first used car, archived state
				 * if available, else live. */
				for (j = 0; j < ACC_MAX_CARS; j++)
					if (s->cars[j].used) {
						leader = j;
						break;
					}
				if (leader < 0) {
					ok = 0;
					break;
				}
				if (n < cur && n < ACC_MAX_SESSIONS &&
				    s->cars[leader].race_archive[n] != NULL)
					src = s->cars[leader]
					    .race_archive[n];
				else
					src = &s->cars[leader].race;
			}
			ok = ok && write_result_header(&bb,
			    &s->cars[leader], src) == 0;
			ok = ok && write_leaderboard_section(&bb, s) == 0;
		}
		if (ok)
			(void)conn_send_framed(c, bb.data, bb.wpos);
		bb_free(&bb);
	}
	log_info("Send session results to %d clients (count=%u)",
	    s->nconns, (unsigned)result_count);
}

/*
 * Optional 0xbe periodic telemetry push to 127.0.0.1:<stats_udp_port>.
 *
 * Mirrors FUN_14002e8d0 + FUN_140034c70 in accServer.exe: a 1 Hz UDP
 * datagram carrying a snapshot of the server state (weekend time, phase,
 * session manager, weather, connection and car lists).  The exe sends
 * this only when a stats port is configured (short at +0x112 in its
 * server struct); we replicate the gating via s->stats_udp_port.
 *
 * Byte layout matches the exe structurally but the two opaque internal
 * serializers (FUN_140033890 session_mgr_state, FUN_1400330e0 additional
 * state) are substituted with our canonical writers from handshake.c /
 * weather.c, which cover the same field surface as the exe's 0x28 /
 * 0x37 payloads.  Since 0xbe is localhost-only telemetry with no known
 * external consumer, exact parity with the exe is not required — any
 * monitoring tool can match our format by reading the fields in order.
 *
 * Intentionally reuses s->udp_fd (the main game UDP socket).  Kunos
 * uses a dedicated stats socket at offset +0x78 but a shared loopback
 * send is equivalent in practice.
 */
static void
broadcast_stats_udp(struct Server *s)
{
	struct ByteBuf bb, wb;
	struct sockaddr_in dst;
	int i, ok, n_conn, n_car;

	if (s->stats_udp_port <= 0 || s->stats_udp_port > 65535)
		return;
	if (s->udp_fd < 0)
		return;

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons((uint16_t)s->stats_udp_port);
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);	/* 127.0.0.1 */

	bb_init(&bb);
	bb_init(&wb);
	ok = wr_u8(&bb, SRV_PERIODIC_UDP) == 0;
	ok = ok && wr_f32(&bb, (float)s->session.weekend_time_s) == 0;
	ok = ok && wr_u8(&bb, s->session.phase) == 0;
	ok = ok && write_session_mgr_state(&bb, s, 0, 0) == 0;

	/*
	 * Weather block: reuse weather_build_broadcast (which writes the
	 * 0x37 opcode + 17 floats) into a scratch buffer and append
	 * everything after the leading opcode byte.
	 */
	if (ok && weather_build_broadcast(s, &wb) == 0 && wb.wpos > 1) {
		if (bb_append(&bb, wb.data + 1, wb.wpos - 1) < 0)
			ok = 0;
	}

	/* Connection list. */
	n_conn = 0;
	for (i = 0; i < ACC_MAX_CARS; i++)
		if (s->conns[i] != NULL &&
		    s->conns[i]->state == CONN_AUTH)
			n_conn++;
	ok = ok && wr_u8(&bb, (uint8_t)(n_conn > 255 ? 255 : n_conn)) == 0;
	for (i = 0; i < ACC_MAX_CARS && ok; i++) {
		struct Conn *c = s->conns[i];
		struct CarEntry *car;
		const char *name = "";

		if (c == NULL || c->state != CONN_AUTH)
			continue;
		car = (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) ?
		    &s->cars[c->car_id] : NULL;
		if (car != NULL && car->driver_count > 0)
			name = car->drivers[car->current_driver_index
			    < car->driver_count
			    ? car->current_driver_index : 0].last_name;
		ok = ok && wr_u16(&bb, car ? car->car_id : 0) == 0;
		ok = ok && wr_u16(&bb, car ? (uint16_t)car->race_number : 0)
		    == 0;
		ok = ok && wr_u8(&bb,
		    car ? car->current_driver_index : 0) == 0;
		ok = ok && wr_str_b(&bb, name) == 0;
		ok = ok && wr_u16(&bb, (uint16_t)c->avg_rtt_ms) == 0;
	}

	/* Car list. */
	n_car = 0;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++)
		if (s->cars[i].used)
			n_car++;
	ok = ok && wr_u8(&bb, (uint8_t)(n_car > 255 ? 255 : n_car)) == 0;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections && ok; i++) {
		if (!s->cars[i].used)
			continue;
		ok = ok && wr_u16(&bb, s->cars[i].car_id) == 0;
		ok = ok && wr_u16(&bb,
		    (uint16_t)s->cars[i].race.position) == 0;
	}

	if (ok)
		(void)sendto(s->udp_fd, bb.data, bb.wpos, 0,
		    (const struct sockaddr *)&dst, sizeof(dst));
	bb_free(&bb);
	bb_free(&wb);
}

void
tick_run(struct Server *s)
{
	uint32_t *last_standings_seq = &s->session.last_standings_seq;
	uint8_t *last_phase = &s->session.last_phase;
	/*
	 * Wall-clock cadence state.  Initialized to 0 so every gate
	 * fires on the first tick after startup (matches the prior
	 * tick-modulo behavior that always fires at tick 0).
	 */
	static uint64_t last_keepalive_ms = 0;
	static uint64_t last_leaderboard_ms = 0;
	static uint64_t last_session_state_ms = 0;
	static uint64_t last_weather_ms = 0;
	/*
	 * Tick-rate probe.  Every 60 s of wall-clock, log the observed
	 * tick rate so we can confirm the main-loop busy-wait is
	 * hitting the intended 333 Hz.  Costs one log line per minute.
	 */
	static uint64_t tickprobe_start_ms = 0;
	static uint32_t tickprobe_start_count = 0;
	uint64_t now_ms = tick_mono_ms();

	s->tick_count++;
	if (tickprobe_start_ms == 0) {
		tickprobe_start_ms = now_ms;
		tickprobe_start_count = s->tick_count;
	} else if (now_ms - tickprobe_start_ms >= 60000) {
		uint32_t dt_ticks = s->tick_count - tickprobe_start_count;
		uint64_t dt_ms = now_ms - tickprobe_start_ms;
		log_info("tick rate: %u ticks in %llu ms = %.1f Hz",
		    (unsigned)dt_ticks,
		    (unsigned long long)dt_ms,
		    (double)dt_ticks * 1000.0 / (double)dt_ms);
		tickprobe_start_ms = now_ms;
		tickprobe_start_count = s->tick_count;
	}

	/* Drive the session phase machine. */
	session_tick(s);

	/*
	 * Race green-flag position gate (FUN_14012f4a0).  While a race
	 * session is in its formation lap, find the current leader and
	 * feed its normalized track position into the trigger check.
	 * When green fires, broadcast the "Race start initialized"
	 * system chat — the exe emits the exact same 0x2b.
	 */
	if (s->session.phase == PHASE_PRE_SESSION &&
	    s->session_count > 0 &&
	    s->sessions[s->session.session_index].session_type == 10 &&
	    !s->session.green_fired) {
		int i, leader = -1;

		for (i = 0; i < ACC_MAX_CARS; i++) {
			if (!s->cars[i].used || !s->cars[i].rt.has_data)
				continue;
			if (s->cars[i].race.position == 1) {
				leader = i;
				break;
			}
		}
		if (leader >= 0) {
			float pos;

			memcpy(&pos, &s->cars[leader].rt.scalar_44,
			    sizeof(pos));
			if (pos >= 0.0f && pos <= 1.0f &&
			    session_advance_race_triggers(s, pos))
				chat_broadcast(s,
				    "Race start initialized", 4);
		}
	}

	/*
	 * Per-car fan-out: send 0x1e (or 0x39 in legacy-netcode mode)
	 * for every car marked dirty since the last sweep.  Matches
	 * FUN_14001a170 / FUN_14001a6a0 being called once per scheduler
	 * tick from FUN_14002e8d0.
	 */
	broadcast_percar_dirty(s);

	/*
	 * Keepalive 0x14 + 0xbe localhost telemetry + optional latency
	 * CSV row, all sharing the 1 s wall-clock cadence.  See
	 * CADENCE_KEEPALIVE_MS — driven off now_ms so the cadence is
	 * honest regardless of how many tick iterations the OS schedules
	 * per second.
	 */
	if (now_ms - last_keepalive_ms >= CADENCE_KEEPALIVE_MS) {
		broadcast_keepalive(s, SRV_KEEPALIVE_14);
		broadcast_stats_udp(s);

		if (s->write_latency_dumps &&
		    s->latency_dump_fp != NULL && s->nconns > 0) {
			FILE *fp = (FILE *)s->latency_dump_fp;
			int i;

			for (i = 0; i < ACC_MAX_CARS; i++) {
				struct Conn *c = s->conns[i];
				const char *sid = "";

				if (c == NULL || c->state != CONN_AUTH)
					continue;
				if (c->car_id >= 0 &&
				    c->car_id < ACC_MAX_CARS &&
				    s->cars[c->car_id].driver_count > 0)
					sid = s->cars[c->car_id]
					    .drivers[0].steam_id;
				fprintf(fp, "%llu,%u,%s,%u,%d\n",
				    (unsigned long long)now_ms,
				    (unsigned)c->conn_id, sid,
				    (unsigned)c->avg_rtt_ms,
				    (int)c->clock_offset_ms);
			}
			fflush(fp);
		}
		last_keepalive_ms = now_ms;
	}

	/*
	 * Leaderboard rebroadcast.  useAsyncLeaderboard (settings.json,
	 * default 1) coalesces fan-out to the CADENCE_LEADERBOARD slot
	 * only — lap completions bump standings_seq but don't trigger a
	 * fresh 0x36 mid-tick.  Sync mode (=0) broadcasts on every
	 * standings change for minimum latency at the cost of extra
	 * fan-out CPU under heavy lap-completion traffic.
	 */
	{
		int changed = s->session.standings_seq !=
		    *last_standings_seq;
		int cadence = now_ms - last_leaderboard_ms >=
		    CADENCE_LEADERBOARD_MS;
		int fire = cadence ||
		    (changed && !s->use_async_leaderboard);

		if (fire) {
			*last_standings_seq = s->session.standings_seq;
			broadcast_leaderboard(s);
			last_leaderboard_ms = now_ms;
		}
	}

	/*
	 * 0x28 session state per-connection broadcast.
	 *
	 * The exe sends 0x28 to every client every tick (~1s)
	 * as a continuous heartbeat (confirmed by 902-message
	 * Kunos capture).  Each message is built per-connection
	 * with a per-client time base from FUN_1400418b0.
	 */
	if (now_ms - last_session_state_ms >= CADENCE_SESSION_STATE_MS &&
	    s->nconns > 0) {
		struct ByteBuf bb;
		int i;

		/*
		 * Per-peer body (each conn gets its own clock-base
		 * projected timestamps), but the scratch buffer itself
		 * is reused across all peers to avoid one malloc+free
		 * per conn per second.
		 */
		bb_init(&bb);
		for (i = 0; i < ACC_MAX_CARS; i++) {
			struct Conn *c = s->conns[i];
			uint32_t client_ts_est;

			if (c == NULL || c->state != CONN_AUTH)
				continue;
			/*
			 * Extrapolate the client's clock forward from the
			 * last pong snapshot so the f32 delta in the 0x28
			 * body is accurate at emit time rather than lagging
			 * by up to one pong interval (~1 s).  Matches exe
			 * FUN_1400418b0 which sums the clock-offset double
			 * with a drift/time-since-pong term rebuilt from
			 * every incoming UDP packet.
			 */
			if (c->last_pong_server_ms != 0)
				client_ts_est = c->last_pong_client_ts +
				    ((uint32_t)now_ms - c->last_pong_server_ms);
			else
				client_ts_est = c->last_pong_client_ts;
			bb_clear(&bb);
			if (wr_u8(&bb, SRV_LARGE_STATE_RESPONSE) == 0 &&
			    write_session_mgr_state(&bb, s,
				client_ts_est,
				c->avg_rtt_ms) == 0)
				(void)conn_send_framed(c,
				    bb.data, bb.wpos);
		}
		bb_free(&bb);
		last_session_state_ms = now_ms;
	}

	/*
	 * One-shot actions on phase transitions.
	 */
	if (s->session.phase != *last_phase) {
		/*
		 * 0x3f grid positions fire once per race, at the
		 * PRE_SESSION (countdown) transition.  Per
		 * FUN_14002f710 the exe gates the emit on
		 * `(iVar11 != 0) && (phase == 0x04)`, i.e. grid
		 * results ready and phase == PRE_SESSION.  We had been
		 * firing at FORMATION which is one level too early.
		 */
		if (s->session.phase == PHASE_PRE_SESSION &&
		    s->session_count > 0 &&
		    s->sessions[s->session.session_index]
			.session_type == 10)
			broadcast_grid(s);
		if (s->session.phase == PHASE_COMPLETED) {
			/*
			 * Flush + check driver-stint violations before
			 * results serialize so any ExceededDriver
			 * StintLimit DQ shows up in the result record.
			 */
			stint_check_violations(s);
			/*
			 * Stint checks may have DQ'd cars; re-sort so
			 * 0x3e session results broadcast + results.json
			 * emit positions with DQ'd cars at the bottom.
			 */
			session_recompute_standings(s);
			/*
			 * Update Trust rating based on race outcome.
			 * Runs only once per session-complete (before
			 * archive) so we don't inflate TR on repeated
			 * ticks.  Uses the session results written below.
			 */
			if (!s->session.results_written &&
			    s->session_count > 0 &&
			    s->sessions[s->session.session_index]
				.session_type == 10) {
				int leader_laps = 0, i;
				for (i = 0; i < ACC_MAX_CARS; i++) {
					int lc = s->cars[i].race.lap_count;
					/*
					 * Include disconnected drivers in
					 * the leader scan — a driver who was
					 * leading and disconnected still set
					 * the benchmark lap count.
					 */
					if (s->cars[i].driver_count > 0 &&
					    lc > leader_laps)
						leader_laps = lc;
				}
				for (i = 0; i < ACC_MAX_CARS; i++) {
					struct CarEntry *car = &s->cars[i];
					int pct;
					if (car->driver_count == 0)
						continue;
					pct = leader_laps > 0
					    ? (car->race.lap_count * 100)
					      / leader_laps : 0;
					ratings_on_race_end(s,
					    car->drivers[0].steam_id, pct,
					    car->race.disqualified);
				}
			}
			broadcast_session_results(s);
			if (!s->session.results_written) {
				(void)results_write(s);
				s->session.results_written = 1;
			}
			/*
			 * Snapshot per-car race state so future 0x56
			 * garage requests for this session's laps can
			 * serve them after we've moved on.
			 */
			session_archive_snapshot(s);
			/* Persist the local rating ledger at session end. */
			ratings_save(s);
		}
		*last_phase = s->session.phase;
	}

	/*
	 * Periodic 0x4e rating summary.  The exe gates this on
	 * DAT_14014bd58 = 81000 ms (verified in .rdata) — a deliberate
	 * 81 s debounce that keeps the rating fan-out out of the
	 * per-second broadcast cost.  Previously debounced at 10 s
	 * which was 8× too fast.
	 */
	if (ratings_is_dirty(s) &&
	    now_ms - s->ratings_last_emit_ms >= CADENCE_RATINGS_MS) {
		struct ByteBuf wb;
		int j, nc = 0, ok = 1;
		for (j = 0; j < ACC_MAX_CARS; j++)
			if (s->cars[j].used)
				nc++;
		bb_init(&wb);
		ok = wr_u8(&wb, SRV_RATING_SUMMARY) == 0;
		ok = ok && wr_u8(&wb, (uint8_t)nc) == 0;
		for (j = 0; j < ACC_MAX_CARS && ok; j++) {
			uint16_t sa = 5000, tr = 5000;
			if (!s->cars[j].used)
				continue;
			ratings_get(s,
			    s->cars[j].drivers[0].steam_id, &sa, &tr);
			ok = ok && wr_u16(&wb, s->cars[j].car_id) == 0;
			ok = ok && wr_u8(&wb, 0) == 0;
			ok = ok && wr_u16(&wb, sa) == 0;
			ok = ok && wr_u16(&wb, tr) == 0;
			ok = ok && wr_i16(&wb, -1) == 0;
			ok = ok && wr_i16(&wb, -1) == 0;
			/*
			 * Same tail as the welcome and disconnect 0x4e
			 * paths: str_a steam_id, not a u8 0 pad.  See
			 * FUN_14002f710 tail for the reference write.
			 */
			ok = ok && wr_str_a(&wb,
			    s->cars[j].drivers[0].steam_id) == 0;
		}
		if (ok)
			(void)bcast_all(s, wb.data, wb.wpos, 0xFFFF);
		bb_free(&wb);
		ratings_clear_dirty(s);
		s->ratings_last_emit_ms = now_ms;
	}

	/*
	 * Weather: step the simulator and broadcast 0x37 every
	 * cadence.  The broadcast carries weekend_time_s which
	 * drives the client's in-game clock, so it must be sent
	 * unconditionally (matching the Kunos 5-second cadence).
	 */
	if (now_ms - last_weather_ms >= CADENCE_WEATHER_MS) {
		struct ByteBuf bb;

		(void)weather_step(s);
		bb_init(&bb);
		if (weather_build_broadcast(s, &bb) == 0)
			(void)bcast_all(s, bb.data, bb.wpos, 0xFFFF);
		bb_free(&bb);
		last_weather_ms = now_ms;
	}
}

