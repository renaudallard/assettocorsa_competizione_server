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
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "bcast.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "results.h"
#include "session.h"
#include "state.h"
#include "tick.h"
#include "weather.h"

/*
 * Broadcast cadences, in ticks (~100 ms each):
 *   per-car fast:   every tick (10 Hz)
 *   per-car slow:   every 10 ticks (1 Hz)
 *   keepalive 0x14: every 20 ticks (2 s)
 *   weather 0x37:   every 50 ticks (5 s)
 */
#define CADENCE_PERCAR_SLOW	10
#define CADENCE_KEEPALIVE	20
#define CADENCE_WEATHER		50

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
static int
build_percar_body(struct ByteBuf *bb, struct CarEntry *car,
    struct Server *s, int32_t clock_adj)
{
	int k, ok;
	int16_t clamped;
	uint16_t rtt_hint = 0;
	uint32_t adj_ts;

	for (k = 0; k < ACC_MAX_CARS; k++) {
		struct Conn *oc = s->conns[k];
		if (oc != NULL && oc->car_id ==
		    (int)(car->car_id - ACC_CAR_ID_BASE)) {
			rtt_hint = (uint16_t)(oc->avg_rtt_ms > 65535
			    ? 65535 : oc->avg_rtt_ms);
			break;
		}
	}

	/*
	 * Per-peer timestamp adjustment matching FUN_14001a170:
	 * adjusted_ts = car_ts - peer_clock_offset.
	 *
	 * clock_offset is computed in the pong handler using
	 * game-relative time (mono_ms - session_start_ms) so
	 * the value is a small correction (~rtt/2), not a huge
	 * monotonic offset.  This lets the receiving client
	 * dead-reckon correctly across the network.
	 */
	adj_ts = (uint32_t)((int32_t)car->rt.client_timestamp_ms
	    - clock_adj);

	ok = 1;
	if (wr_u16(bb, car->car_id) < 0) return -1;
	if (wr_u8(bb, car->rt.packet_seq) < 0) return -1;
	if (wr_u32(bb, adj_ts) < 0) return -1;
	if (wr_u16(bb, rtt_hint) < 0) return -1;

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
 * Fast-rate 0x1e broadcast.  For every car that has received
 * a fresh 0x1e this tick, send a 64-byte UDP datagram to every
 * other connected peer.  Layout from FUN_14001a170.
 *
 * The timestamp at body offset 4..7 (after u8 msgid + u16
 * car_id + u8 seq) is adjusted per-peer: the exe writes
 * `car_ts - FUN_1400418b0(peer)` so the receiver sees
 * timestamps in its own timebase.  We patch the 4 bytes
 * in-place before each sendto rather than rebuilding the
 * whole packet.
 */
static void
broadcast_percar_fast(struct Server *s)
{
	int i, j;

	if (s->udp_fd < 0)
		return;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		struct CarEntry *car = &s->cars[i];
		struct ByteBuf bb;
		uint16_t exclude = 0xFFFF;

		if (!car->used || !car->rt.has_data)
			continue;

		for (j = 0; j < ACC_MAX_CARS; j++) {
			struct Conn *oc = s->conns[j];
			if (oc != NULL && oc->car_id == i) {
				exclude = oc->conn_id;
				break;
			}
		}

		for (j = 0; j < ACC_MAX_CARS; j++) {
			struct Conn *peer = s->conns[j];

			if (peer == NULL || peer->state != CONN_AUTH)
				continue;
			if (peer->conn_id == exclude)
				continue;

			bb_init(&bb);
			if (wr_u8(&bb, SRV_PERCAR_FAST_RATE) == 0 &&
			    build_percar_body(&bb, car, s,
				peer->clock_offset_ms) == 0) {
				(void)sendto(s->udp_fd, bb.data,
				    bb.wpos, 0,
				    (const struct sockaddr *)&peer->peer,
				    sizeof(peer->peer));
			}
			bb_free(&bb);
		}

		car->rt.has_data = 0;
	}
}

/*
 * Slow-rate 0x39 broadcast.  For every connected peer, emit
 * one or more UDP datagrams carrying batches of up to 8 other-
 * car records (header: u8 0x39 + u8 count + count * 63-byte
 * body).  Layout from FUN_14001a6a0 in accServer.exe.
 *
 * Matches the exe's multi-batch loop: if the peer has N > 8
 * other cars, N/8 + 1 datagrams are sent.  8 is the max count
 * per batch and is hard-coded in the inner loop of the exe.
 *
 * This runs every CADENCE_PERCAR_SLOW ticks (~1 Hz) and is an
 * unconditional recap (dirty-flag filtering is done by the
 * fast-rate path only) so newly-joined peers pick up state for
 * cars that haven't moved since the last tick.
 */
#define PERCAR_SLOW_BATCH	8

static void
broadcast_percar_slow(struct Server *s)
{
	int peer_i, car_i;

	if (s->udp_fd < 0)
		return;
	for (peer_i = 0; peer_i < ACC_MAX_CARS; peer_i++) {
		struct Conn *peer = s->conns[peer_i];

		if (peer == NULL || peer->state != CONN_AUTH)
			continue;

		car_i = 0;
		while (car_i < ACC_MAX_CARS) {
			struct ByteBuf bb;
			uint8_t count;
			size_t count_off;

			bb_init(&bb);
			if (wr_u8(&bb, SRV_PERCAR_SLOW_RATE) < 0) {
				bb_free(&bb);
				break;
			}
			count_off = bb.wpos;
			if (wr_u8(&bb, 0) < 0) {
				bb_free(&bb);
				break;
			}
			count = 0;

			while (car_i < ACC_MAX_CARS &&
			    count < PERCAR_SLOW_BATCH) {
				struct CarEntry *car = &s->cars[car_i];

				car_i++;
				if (!car->used)
					continue;
				if (peer->car_id == car_i - 1)
					continue;	/* skip self */
				if (build_percar_body(&bb, car, s,
				    peer->clock_offset_ms) < 0)
					break;
				count++;
			}
			bb.data[count_off] = count;

			if (count > 0)
				(void)sendto(s->udp_fd, bb.data, bb.wpos,
				    0,
				    (const struct sockaddr *)&peer->peer,
				    sizeof(peer->peer));
			bb_free(&bb);
			if (count < PERCAR_SLOW_BATCH)
				break;	/* drained: no more full batches */
		}
	}
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
	int i;
	struct timespec ts;
	uint32_t srv_ms;
	struct ByteBuf bb;

	if (s->udp_fd < 0)
		return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	srv_ms = (uint32_t)((uint64_t)ts.tv_sec * 1000 +
	    (uint64_t)ts.tv_nsec / 1000000);

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];

		if (c == NULL || c->state != CONN_AUTH)
			continue;
		c->keepalive_sent_ms = srv_ms;

		bb_init(&bb);
		if (wr_u8(&bb, msg_id) == 0 &&
		    wr_u32(&bb, srv_ms) == 0 &&
		    wr_u16(&bb, c->conn_id) == 0 &&
		    wr_u16(&bb, 0) == 0 &&
		    wr_u16(&bb, 0) == 0 &&
		    wr_u8(&bb, 2) == 0 &&
		    wr_u8(&bb, 4) == 0 &&
		    wr_u8(&bb, 100) == 0 &&
		    wr_u8(&bb, 100) == 0) {
			(void)sendto(s->udp_fd, bb.data, bb.wpos, 0,
			    (const struct sockaddr *)&c->peer,
			    sizeof(c->peer));
		}
		bb_free(&bb);
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
	int i, n = 0;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_GRID_POSITIONS) < 0)
		goto done;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++)
		if (s->cars[i].used)
			n++;
	if (wr_u8(&bb, (uint8_t)n) < 0)
		goto done;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		struct CarEntry *car = &s->cars[i];
		if (!car->used)
			continue;
		if (wr_u16(&bb, car->car_id) < 0 ||
		    wr_u8(&bb, 0) < 0 ||
		    wr_u32(&bb,
			(uint32_t)(car->race.grid_position > 0 ?
			    car->race.grid_position : (i + 1))) < 0 ||
		    wr_u8(&bb, 0) < 0)
			goto done;
	}
	(void)bcast_all(s, bb.data, bb.wpos, 0xFFFF);
	log_info("Sending grid positions: %d cars", n);
done:
	bb_free(&bb);
}

/*
 * Build and emit the SRV_SESSION_RESULTS (0x3e) at the end of
 * a session.  Body: u8 result_count + per-car { u8 + u8 + u8 +
 * u32 + u16 + u32 + u32 + u8 + u8 + u32 } header.
 */
static void
broadcast_session_results(struct Server *s)
{
	struct ByteBuf bb;
	int i, n = 0;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_SESSION_RESULTS) < 0)
		goto done;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++)
		if (s->cars[i].used)
			n++;
	if (wr_u8(&bb, (uint8_t)n) < 0)
		goto done;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		struct CarEntry *car = &s->cars[i];
		if (!car->used)
			continue;
		if (wr_u8(&bb, (uint8_t)car->race.position) < 0 ||
		    wr_u8(&bb, car->cup_category) < 0 ||
		    wr_u8(&bb, car->driver_count) < 0 ||
		    wr_u32(&bb, (uint32_t)car->race.lap_count) < 0 ||
		    wr_u16(&bb, car->car_id) < 0 ||
		    wr_u32(&bb, (uint32_t)car->race.best_lap_ms) < 0 ||
		    wr_u32(&bb, (uint32_t)car->race.race_time_ms) < 0 ||
		    wr_u8(&bb, 0) < 0 ||
		    wr_u8(&bb, 0) < 0 ||
		    wr_u32(&bb, (uint32_t)car->race.last_lap_ms) < 0)
			goto done;
	}
	(void)bcast_all(s, bb.data, bb.wpos, 0xFFFF);
	log_info("Send session results to %d clients (%zu byte)",
	    s->nconns, bb.wpos);
done:
	bb_free(&bb);
}

void
tick_run(struct Server *s)
{
	uint32_t *last_standings_seq = &s->session.last_standings_seq;
	uint8_t *last_phase = &s->session.last_phase;

	s->tick_count++;

	/* Drive the session phase machine. */
	session_tick(s);

	/* Fast-rate per-car broadcast (every tick, UDP). */
	broadcast_percar_fast(s);

	/* Slow-rate per-car batched broadcast (1 Hz, UDP). */
	if ((s->tick_count % CADENCE_PERCAR_SLOW) == 0)
		broadcast_percar_slow(s);

	/* Keepalive heartbeat: only when no weather broadcasts
	 * are flowing (the real server uses 0x37 as heartbeat). */
	if ((s->tick_count % CADENCE_KEEPALIVE) == 0 &&
	    (s->tick_count % CADENCE_WEATHER) != 0)
		broadcast_keepalive(s, SRV_KEEPALIVE_14);

	/*
	 * Leaderboard rebroadcast on standings change.
	 */
	if (s->session.standings_seq != *last_standings_seq) {
		*last_standings_seq = s->session.standings_seq;
		broadcast_leaderboard(s);
		/*
		 * 0x4e periodic per-connection rating summary
		 * broadcast: per-row record with conn id, two
		 * i16 ratings * 10, sentinel u32, format-A name.
		 */
		{
			struct ByteBuf bb;
			int j, n = 0;
			int ok = 1;

			for (j = 0; j < ACC_MAX_CARS; j++)
				if (s->conns[j] != NULL &&
				    s->conns[j]->state == CONN_AUTH)
					n++;
			bb_init(&bb);
			ok = wr_u8(&bb, SRV_RATING_SUMMARY) == 0;
			ok = ok && wr_u8(&bb, (uint8_t)n) == 0;
			for (j = 0; j < ACC_MAX_CARS && ok; j++) {
				struct Conn *cn = s->conns[j];
				if (cn == NULL ||
				    cn->state != CONN_AUTH)
					continue;
				ok = ok && wr_u16(&bb, cn->conn_id) == 0;
				ok = ok && wr_u8(&bb, 0) == 0;
				ok = ok && wr_i16(&bb, 0) == 0;
				ok = ok && wr_i16(&bb, 0) == 0;
				ok = ok && wr_u32(&bb, 0xffffffff) == 0;
				ok = ok && wr_str_a(&bb, "") == 0;
			}
			if (ok)
				(void)bcast_all(s, bb.data, bb.wpos,
				    0xFFFF);
			bb_free(&bb);
		}
	}

	/*
	 * One-shot actions on phase transitions.
	 * The exe re-broadcasts 0x28 whenever the computed phase
	 * changes (condition 3 in FUN_14002f710 server_tick_tail).
	 */
	if (s->session.phase != *last_phase) {
		{
			struct ByteBuf bb;

			bb_init(&bb);
			if (wr_u8(&bb, SRV_LARGE_STATE_RESPONSE) == 0 &&
			    write_session_mgr_state(&bb, s) == 0)
				(void)bcast_all(s, bb.data, bb.wpos,
				    0xFFFF);
			bb_free(&bb);
		}
		if (s->session.phase == PHASE_FORMATION)
			broadcast_grid(s);
		if (s->session.phase == PHASE_COMPLETED) {
			broadcast_session_results(s);
			if (!s->session.results_written) {
				(void)results_write(s);
				s->session.results_written = 1;
			}
		}
		*last_phase = s->session.phase;
	}

	/*
	 * Weather: step the simulator and broadcast 0x37 every
	 * cadence.  The broadcast carries weekend_time_s which
	 * drives the client's in-game clock, so it must be sent
	 * unconditionally (matching the Kunos 5-second cadence).
	 */
	if ((s->tick_count % CADENCE_WEATHER) == 0) {
		struct ByteBuf bb;

		(void)weather_step(s);
		bb_init(&bb);
		if (weather_build_broadcast(s, &bb) == 0)
			(void)bcast_all(s, bb.data, bb.wpos, 0xFFFF);
		bb_free(&bb);
	}
}

