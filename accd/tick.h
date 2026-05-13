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
 * Emit 0x36 ACP_LEADERBOARD_BCAST to every connected client.  Thin
 * wrapper over broadcast_leaderboard_if_changed; the cache compare
 * happens inside so back-to-back callers don't fan out duplicate
 * payloads to peers that already received the latest state.
 */
void	broadcast_leaderboard(struct Server *s);

/*
 * Build the leaderboard payload and broadcast only if its bytes
 * differ from the cached last-emit.  Mirrors kunos's
 * FUN_14002f710 deep-compare emit trigger.  Returns 1 on emit, 0
 * when the cache already matched.
 */
int	broadcast_leaderboard_if_changed(struct Server *s);

/*
 * Mark the leaderboard dirty so the next tick drains the pending
 * flag and runs broadcast_leaderboard_if_changed.  Called from state
 * mutations kunos emits 0x36 for (penalty enqueue, peer leave,
 * session phase change).  Lap completes and sector splits MUST NOT
 * call this — kunos doesn't emit on those.  Cheap (one byte flip).
 */
void	leaderboard_request_emit(struct Server *s);

/*
 * Build the leaderboard payload and broadcast unconditionally,
 * bypassing the deep-compare gate.  Used at kunos-documented
 * mandatory-emit moments (post-handshake fan-out, weekend wrap,
 * phase-boundary force) where kunos's pcap shows a 0x36 even when
 * the bytes happen to be identical to the prior emit.  Cache is
 * updated after the fan-out so the next gated check has the latest
 * bytes.  Returns 1 on emit (always), 0 on build failure.
 */
int	broadcast_leaderboard_force(struct Server *s);

/*
 * Build a 63-byte per-car body used by 0x39 relay.
 * clock_adj = sender_pong_ts - peer_pong_ts for per-peer
 * timestamp adjustment.
 */
int	build_percar_body(struct ByteBuf *bb, struct CarEntry *car,
		struct Server *s, int32_t clock_adj);

/*
 * Compute server-wide aggregate pings across active conns.  Used
 * by both the periodic 0x14 keepalive broadcast and the 0x13
 * keepalive reply path in dispatch.c.
 */
void	compute_server_pings(const struct Server *s,
		uint16_t *avg_out, uint16_t *max_out);

/*
 * Pack a 15-byte 0x14 keepalive body into pkt.  Same shape for the
 * periodic broadcast and the per-conn reply on 0x13 — body carries
 * (msg_id, srv_ms, conn_rtt, avg_ping, max_ping, 2/4/100/100).
 */
void	build_keepalive_pkt(unsigned char pkt[15], uint8_t msg_id,
		uint32_t srv_ms, uint16_t conn_rtt,
		uint16_t avg_ping, uint16_t max_ping);

#endif /* ACCD_TICK_H */
