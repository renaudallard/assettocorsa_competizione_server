/*
 * tick.h -- main server tick.
 *
 * Called periodically (target ~10 Hz) from the main loop.
 * Drives session phase advancement and the periodic broadcasts:
 *   0x36 leaderboard, 0x37 weather, 0x3e session results,
 *   0x3f grid positions, 0x4e ratings, plus the per-tick keepalive
 *   beats and the per-car state fan-out.
 */

#ifndef ACCD_TICK_H
#define ACCD_TICK_H

#include "state.h"

void	tick_run(struct Server *s);

#endif /* ACCD_TICK_H */
