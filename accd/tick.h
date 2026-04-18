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
 * tick.h -- main server tick.
 *
 * Called periodically (target 333 Hz / 3 ms) from the main loop,
 * matching Kunos's CreateTimerQueueTimer(Period=3) schedule.
 * Drives session phase advancement and the periodic broadcasts:
 *   0x36 leaderboard, 0x37 weather, 0x3e session results,
 *   0x3f grid positions, 0x4e ratings, plus the per-tick keepalive
 *   beats and the per-car state fan-out.
 */

#ifndef ACCD_TICK_H
#define ACCD_TICK_H

#include "state.h"

void	tick_run(struct Server *s);

/*
 * Build a 63-byte per-car body used by 0x39 relay.
 * clock_adj = sender_pong_ts - peer_pong_ts for per-peer
 * timestamp adjustment.
 */
int	build_percar_body(struct ByteBuf *bb, struct CarEntry *car,
		struct Server *s, int32_t clock_adj);

#endif /* ACCD_TICK_H */
