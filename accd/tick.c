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
#include "state.h"
#include "tick.h"

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

void
tick_run(struct Server *s)
{
	s->tick_count++;

	/* Fast-rate per-car broadcast (every tick). */
	broadcast_percar(s, SRV_PERCAR_FAST_RATE, 0);

	/* Slow-rate per-car broadcast (lower cadence). */
	if ((s->tick_count % CADENCE_PERCAR_SLOW) == 0)
		broadcast_percar(s, SRV_PERCAR_SLOW_RATE, 1);

	/* Keepalive heartbeat: SRV_KEEPALIVE_14 is a single-byte
	 * message.  The binary also emits SRV_STATE_RECORD_0C and
	 * SRV_KEEPALIVE_36/37/3E from the server tick tail; we
	 * emit the simple keepalive here as the minimum viable
	 * heartbeat. */
	if ((s->tick_count % CADENCE_KEEPALIVE) == 0)
		broadcast_keepalive(s, SRV_KEEPALIVE_14);

	/*
	 * Future phases will also fan out:
	 *   - SRV_LEADERBOARD_BCAST (0x36) when standings change
	 *   - SRV_WEATHER_STATUS (0x37) on weather refresh
	 *   - SRV_RATING_SUMMARY (0x4e) when ratings change
	 *   - SRV_SESSION_RESULTS (0x3e) at session end
	 *   - SRV_GRID_POSITIONS (0x3f) at race countdown
	 */
	(void)CADENCE_WEATHER;
}

