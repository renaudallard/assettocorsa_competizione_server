/*
 * Copyright (c) 2025-2026 Renaud Allard
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

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
