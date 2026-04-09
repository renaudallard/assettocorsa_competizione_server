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

#include "bcast.h"
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

static void
broadcast_percar(struct Server *s, uint8_t msg_id, int extra_context_byte)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		struct CarEntry *car = &s->cars[i];
		struct ByteBuf bb;
		uint16_t exclude = 0xFFFF;
		int j;

		if (!car->used || !car->rt.has_data)
			continue;

		for (j = 0; j < ACC_MAX_CARS; j++) {
			struct Conn *c = s->conns[j];
			if (c != NULL && c->car_id == i) {
				exclude = c->conn_id;
				break;
			}
		}

		bb_init(&bb);
		if (wr_u8(&bb, msg_id) == 0) {
			if (extra_context_byte) {
				/* 0x39 slow-rate has one extra context
				 * byte right after the msg id. */
				(void)wr_u8(&bb, 0);
			}
			/* Rest of the per-car body (skip the msg id
			 * that build_percar_broadcast would also
			 * write). */
			if (wr_u16(&bb, car->car_id) == 0 &&
			    wr_u8(&bb, car->rt.packet_seq) == 0 &&
			    wr_u32(&bb, car->rt.client_timestamp_ms) == 0) {
				int k;
				int ok = 1;

				for (k = 0; k < 3 && ok; k++)
					ok = wr_f32(&bb, car->rt.vec_a[k]) == 0;
				for (k = 0; k < 3 && ok; k++)
					ok = wr_f32(&bb, car->rt.vec_b[k]) == 0;
				for (k = 0; k < 3 && ok; k++)
					ok = wr_f32(&bb, car->rt.vec_c[k]) == 0;
				for (k = 0; k < 4 && ok; k++)
					ok = wr_u8(&bb, car->rt.input_a[k]) == 0;
				if (ok) ok = wr_u8(&bb, car->rt.scalar_32) == 0;
				if (ok) ok = wr_u8(&bb, car->rt.scalar_33) == 0;
				if (ok) ok = wr_u16(&bb, car->rt.scalar_36) == 0;
				if (ok) ok = wr_u8(&bb, car->rt.scalar_2c) == 0;
				if (ok) ok = wr_u8(&bb, car->rt.scalar_34) == 0;
				if (ok) ok = wr_u8(&bb, car->rt.scalar_35) == 0;
				if (ok) ok = wr_u32(&bb, car->rt.scalar_44) == 0;
				for (k = 0; k < 4 && ok; k++)
					ok = wr_u8(&bb, car->rt.input_b[k]) == 0;
				if (ok) ok = wr_u8(&bb, car->rt.scalar_4c) == 0;
				if (ok) ok = wr_i16(&bb, car->rt.scalar_1ec) == 0;
				if (ok)
					(void)bcast_all(s, bb.data,
					    bb.wpos, exclude);
			}
		}
		bb_free(&bb);
	}
}

static void
broadcast_keepalive(struct Server *s, uint8_t msg_id)
{
	unsigned char buf[1] = { msg_id };

	(void)bcast_all(s, buf, sizeof(buf), 0xFFFF);
}

/*
 * Build and emit the SRV_LEADERBOARD_BCAST (0x36) when the
 * standings have changed.  Body: u32 session_meta + per-car
 * minimal records (car_id + position + lap_count + last_lap +
 * best_lap).  This is a simplified version of the binary's
 * leaderboard record; full SRV_LEADERBOARD_UPDATE (0x07)
 * protobuf carries the rich version.
 */
static void
broadcast_leaderboard(struct Server *s)
{
	struct ByteBuf bb;
	int i, n = 0;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_LEADERBOARD_BCAST) < 0 ||
	    wr_u32(&bb, s->session.standings_seq) < 0)
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
		    wr_u16(&bb, (uint16_t)car->race.position) < 0 ||
		    wr_i32(&bb, car->race.lap_count) < 0 ||
		    wr_i32(&bb, car->race.last_lap_ms) < 0 ||
		    wr_i32(&bb, car->race.best_lap_ms) < 0)
			goto done;
	}
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
	uint16_t *last_standings_seq = &s->session.last_standings_seq;
	uint8_t *last_phase = &s->session.last_phase;

	s->tick_count++;

	/* Drive the session phase machine. */
	session_tick(s);

	/* Fast-rate per-car broadcast (every tick). */
	broadcast_percar(s, SRV_PERCAR_FAST_RATE, 0);

	/* Slow-rate per-car broadcast (lower cadence). */
	if ((s->tick_count % CADENCE_PERCAR_SLOW) == 0)
		broadcast_percar(s, SRV_PERCAR_SLOW_RATE, 1);

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
	 * One-shot grid broadcast at PRE_RACE entry.
	 */
	if (s->session.phase != *last_phase) {
		if (s->session.phase == PHASE_PRE_RACE)
			broadcast_grid(s);
		if (s->session.phase == PHASE_POST_SESSION) {
			broadcast_session_results(s);
			if (!s->session.results_written) {
				(void)results_write(s);
				s->session.results_written = 1;
			}
		}
		*last_phase = s->session.phase;
	}

	/*
	 * Weather: step the deterministic simulator and broadcast
	 * 0x37 when something changed enough to be worth pushing.
	 */
	if ((s->tick_count % CADENCE_WEATHER) == 0) {
		if (weather_step(s)) {
			struct ByteBuf bb;

			bb_init(&bb);
			if (weather_build_broadcast(s, &bb) == 0)
				(void)bcast_all(s, bb.data, bb.wpos, 0xFFFF);
			bb_free(&bb);
		}
	}
}

