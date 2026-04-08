/*
 * tick.c -- periodic server tick.
 *
 * Stub for phase 1.  Increments a tick counter, will eventually
 * drive the session state machine and the periodic broadcasts
 * documented in §5.6.4a.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>

#include "log.h"
#include "msg.h"
#include "tick.h"

void
tick_run(struct Server *s)
{
	s->tick_count++;

	/*
	 * Phase 1: nothing to do.  Future phases will fan out:
	 *   - Per-car state via SRV_PERCAR_FAST_RATE / SLOW_RATE
	 *     to every other connected client.
	 *   - SRV_LEADERBOARD_BCAST when the standings change.
	 *   - SRV_WEATHER_STATUS at the configured cadence.
	 *   - SRV_RATING_SUMMARY when ratings change.
	 *   - SRV_KEEPALIVE_14 / SRV_STATE_RECORD_0C as keepalives.
	 *   - SRV_SESSION_RESULTS at session end.
	 *   - SRV_GRID_POSITIONS at race countdown.
	 */
	(void)s;
}
