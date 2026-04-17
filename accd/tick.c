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
#define CADENCE_SESSION_STATE	10	/* 0x28 every ~1 s.  Single-
					 * session active replay shows
					 * Kunos at 1.01/s (911 frames /
					 * 900 s).  An earlier 81-min
					 * mixed-idle replay averaged
					 * 0.53/s only because ~half the
					 * window had no active driver. */
#define CADENCE_KEEPALIVE	10	/* 0x14 every ~1 s, matching exe */
#define CADENCE_WEATHER		50
#define CADENCE_LEADERBOARD	750	/* 0x36 every ~75 s, matches Kunos
					 * (65 msgs over 4860 s) */

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
 * Event-driven per-car relay.  Called from h_udp_car_update()
 * immediately after storing the update.  Sends a single-car
 * 0x39 batch (count=1) to every other authenticated peer,
 * matching the exe's relay behavior (0x39 only, never 0x1e
 * from server, 18 Hz event-driven not periodic).
 */
void
relay_car_update(struct Server *s, struct Conn *sender,
    struct CarEntry *car)
{
	int j;

	if (s->udp_fd < 0)
		return;
	for (j = 0; j < ACC_MAX_CARS; j++) {
		struct Conn *peer = s->conns[j];
		struct ByteBuf bb;
		int32_t delta;

		if (peer == NULL || peer->state != CONN_AUTH)
			continue;
		if (peer == sender)
			continue;

		delta = (int32_t)(sender->last_pong_client_ts -
		    peer->last_pong_client_ts);
		bb_init(&bb);
		if (wr_u8(&bb, SRV_PERCAR_SLOW_RATE) == 0 &&
		    wr_u8(&bb, 1) == 0 &&
		    build_percar_body(&bb, car, s, delta) == 0) {
			(void)sendto(s->udp_fd, bb.data, bb.wpos, 0,
			    (const struct sockaddr *)&peer->peer,
			    sizeof(peer->peer));
		}
		bb_free(&bb);
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

/* Per-car relay is fully event-driven from relay_car_update().
 * The periodic broadcast_percar_slow was removed: the Kunos
 * capture shows no periodic recap.  Event-driven relay handles
 * all car state propagation. */

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
		/*
		 * Kunos FUN_140029b20 body is u32 server_ms + three u16
		 * zeros + u8(2) + u8(4) + u8(100) + u8(100).  We had been
		 * writing avg_rtt_ms into the three u16 slots and using
		 * 0,0,100,100 for the four trailing u8s.  Neither matches
		 * the capture; switch to the constants.
		 */
		if (wr_u8(&bb, msg_id) == 0 &&
		    wr_u32(&bb, srv_ms) == 0 &&
		    wr_u16(&bb, 0) == 0 &&
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
			(uint32_t)(car->race.grid_position >= 0 ?
			    car->race.grid_position : i)) < 0 ||
		    wr_u8(&bb, 0) < 0)
			goto done;
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
 * semantic, always 0 in welcome scenarios) and +0x74 u32 time penalty
 * (we don't track cumulative penalty ms yet).
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
write_result_header(struct ByteBuf *bb, const struct CarEntry *car)
{
	const struct CarRaceState *r = &car->race;
	uint8_t position = r->position > 0 && r->position < 0xff
	    ? (uint8_t)r->position : 0;
	uint8_t drv_minus_one = (uint8_t)(car->current_driver_index - 1);

	if (wr_u8(bb, position) < 0) return -1;		/* +0x50 */
	if (wr_u8(bb, position) < 0) return -1;		/* +0x54 cup_pos */
	if (wr_u8(bb, drv_minus_one) < 0) return -1;	/* +0x58 drv-1 */
	if (wr_u32(bb, (uint32_t)r->lap_count) < 0) return -1;	/* +0x5c */
	if (wr_u16(bb, 0) < 0) return -1;		/* +0x60 unknown */
	if (wr_u32(bb, r->best_lap_ms > 0
	    ? (uint32_t)r->best_lap_ms : 0x7FFFFFFFu) < 0) return -1;
	if (wr_u32(bb, r->race_time_ms > 0
	    ? (uint32_t)r->race_time_ms : 0) < 0) return -1;	/* +0x68 */
	if (wr_u8(bb, r->formation_lap_done) < 0) return -1;	/* +0x6c */
	if (wr_u8(bb, r->disqualified) < 0) return -1;	/* +0x70 */
	if (wr_u32(bb, 0) < 0) return -1;		/* +0x74 penalty ms */
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
		(void)n;
		/*
		 * Emit one result_header per entry then the leaderboard
		 * section covering every car.  We still iterate
		 * result_count times (session_index+1) so the byte count
		 * matches the capture even while we populate per-car
		 * fields in each 23-byte block.
		 */
		for (n = 0; n < result_count && ok; n++) {
			/* Pick the first used car to populate the header;
			 * for multi-car sessions the leaderboard section
			 * that follows carries every car's record. */
			for (j = 0; j < ACC_MAX_CARS; j++)
				if (s->cars[j].used)
					break;
			ok = ok && write_result_header(&bb,
			    &s->cars[j]) == 0;
			ok = ok && write_leaderboard_section(&bb, s) == 0;
		}
		if (ok)
			(void)conn_send_framed(c, bb.data, bb.wpos);
		bb_free(&bb);
	}
	log_info("Send session results to %d clients (count=%u)",
	    s->nconns, (unsigned)result_count);
}

void
tick_run(struct Server *s)
{
	uint32_t *last_standings_seq = &s->session.last_standings_seq;
	uint8_t *last_phase = &s->session.last_phase;

	s->tick_count++;

	/* Drive the session phase machine. */
	session_tick(s);

	/* Per-car relay is event-driven from h_udp_car_update(),
	 * not periodic.  The 1 Hz recap below is kept for newly-
	 * joined peers to pick up state for idle cars. */

	/* Per-car relay is fully event-driven from h_udp_car_update.
	 * No periodic recap: Kunos capture shows none. */

	/* Keepalive 0x14 every ~1s (matching exe capture). */
	if ((s->tick_count % CADENCE_KEEPALIVE) == 0)
		broadcast_keepalive(s, SRV_KEEPALIVE_14);

	/*
	 * Leaderboard rebroadcast on standings change.
	 * Capture shows 0x4e rating summary is only sent on
	 * connection events (3 total over 20 min), not on
	 * every standings change.  Decouple from leaderboard.
	 */
	if (s->session.standings_seq != *last_standings_seq ||
	    (s->tick_count % CADENCE_LEADERBOARD) == 0) {
		*last_standings_seq = s->session.standings_seq;
		broadcast_leaderboard(s);
	}

	/*
	 * 0x28 session state per-connection broadcast.
	 *
	 * The exe sends 0x28 to every client every tick (~1s)
	 * as a continuous heartbeat (confirmed by 902-message
	 * Kunos capture).  Each message is built per-connection
	 * with a per-client time base from FUN_1400418b0.
	 */
	if ((s->tick_count % CADENCE_SESSION_STATE) == 0 &&
	    s->nconns > 0) {
		int i;

		for (i = 0; i < ACC_MAX_CARS; i++) {
			struct Conn *c = s->conns[i];
			struct ByteBuf bb;

			if (c == NULL || c->state != CONN_AUTH)
				continue;
			bb_init(&bb);
			if (wr_u8(&bb, SRV_LARGE_STATE_RESPONSE) == 0 &&
			    write_session_mgr_state(&bb, s,
				c->last_pong_client_ts,
				c->avg_rtt_ms) == 0)
				(void)conn_send_framed(c,
				    bb.data, bb.wpos);
			bb_free(&bb);
		}
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

