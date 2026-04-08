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
 * Build a SRV_PERCAR_FAST_RATE (0x1e) broadcast body from one
 * car's runtime state.  Returns 0 on success, -1 on allocation
 * failure.  Populates *bb which the caller must bb_free().
 */
static int
build_percar_broadcast(struct ByteBuf *bb, struct CarEntry *car)
{
	struct CarRuntime *rt = &car->rt;
	int i;

	if (wr_u8(bb, SRV_PERCAR_FAST_RATE) < 0)
		return -1;
	if (wr_u16(bb, car->car_id) < 0)
		return -1;
	if (wr_u8(bb, rt->packet_seq) < 0)
		return -1;
	if (wr_u32(bb, rt->client_timestamp_ms) < 0)
		return -1;

	for (i = 0; i < 3; i++)
		if (wr_f32(bb, rt->vec_a[i]) < 0)
			return -1;
	for (i = 0; i < 3; i++)
		if (wr_f32(bb, rt->vec_b[i]) < 0)
			return -1;
	for (i = 0; i < 3; i++)
		if (wr_f32(bb, rt->vec_c[i]) < 0)
			return -1;

	for (i = 0; i < 4; i++)
		if (wr_u8(bb, rt->input_a[i]) < 0)
			return -1;

	if (wr_u8(bb, rt->scalar_32) < 0 ||
	    wr_u8(bb, rt->scalar_33) < 0 ||
	    wr_u16(bb, rt->scalar_36) < 0 ||
	    wr_u8(bb, rt->scalar_2c) < 0 ||
	    wr_u8(bb, rt->scalar_34) < 0 ||
	    wr_u8(bb, rt->scalar_35) < 0 ||
	    wr_u32(bb, rt->scalar_44) < 0)
		return -1;

	for (i = 0; i < 4; i++)
		if (wr_u8(bb, rt->input_b[i]) < 0)
			return -1;

	if (wr_u8(bb, rt->scalar_4c) < 0 ||
	    wr_i16(bb, rt->scalar_1ec) < 0)
		return -1;

	return 0;
}

void
tick_run(struct Server *s)
{
	int i;

	s->tick_count++;

	/*
	 * Per-tick per-car fan-out.  For each car that has ever
	 * received an ACP_CAR_UPDATE, rebroadcast its current
	 * runtime state to every other connected client.  The
	 * owning client is excluded so it doesn't receive its own
	 * update echoed back.
	 */
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		struct CarEntry *car = &s->cars[i];
		struct ByteBuf bb;
		uint16_t exclude = 0xFFFF;
		int j;

		if (!car->used || !car->rt.has_data)
			continue;

		/* Find the owning connection so we can exclude it. */
		for (j = 0; j < ACC_MAX_CARS; j++) {
			struct Conn *c = s->conns[j];
			if (c != NULL && c->car_id == i) {
				exclude = c->conn_id;
				break;
			}
		}

		bb_init(&bb);
		if (build_percar_broadcast(&bb, car) == 0)
			(void)bcast_all(s, bb.data, bb.wpos, exclude);
		bb_free(&bb);
	}

	/*
	 * Future phases will also fan out:
	 *   - SRV_PERCAR_SLOW_RATE (0x39) at slower cadence
	 *   - SRV_LEADERBOARD_BCAST (0x36) when standings change
	 *   - SRV_WEATHER_STATUS (0x37) on weather refresh
	 *   - SRV_RATING_SUMMARY (0x4e) when ratings change
	 *   - SRV_KEEPALIVE_14 / SRV_STATE_RECORD_0C as heartbeats
	 *   - SRV_SESSION_RESULTS (0x3e) at session end
	 *   - SRV_GRID_POSITIONS (0x3f) at race countdown
	 */
}
