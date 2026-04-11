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
 * bcast.h -- broadcast helpers.
 *
 * The binary uses a two-tier broadcast architecture (see §5.6.4b):
 *
 *   Tier 1 -- direct relay: server receives a message, validates,
 *     and forwards the same wire body (with maybe a leading msg
 *     id byte swap) to every other connected client.  Used for
 *     0x2a chat, 0x2e car system, 0x2f tyre compound, 0x32 car
 *     location, 0x43 damage, 0x45 dirt, 0x3d out of track.
 *
 *   Tier 2 -- queued lambda with per-recipient transformation:
 *     server updates its own state, then builds a PER-RECIPIENT
 *     message with (potentially) different bytes for each client.
 *     Used for 0x19 lap completed -> 0x1b and 0x20/0x21 sector
 *     splits -> 0x3a/0x3b.
 *
 * For a clean-room reimplementation we can treat both tiers as
 * "build one message, send to N recipients" as long as the per-
 * recipient customizations (e.g. relative-to-my-best-delta) can
 * be computed in a single pass.  Phase 2 doesn't do any of that
 * yet -- it just implements the simple broadcast path.
 */

#ifndef ACCD_BCAST_H
#define ACCD_BCAST_H

#include <stddef.h>

#include "state.h"

/*
 * Send a framed TCP message to one connection.  The body must
 * start with the msg id byte.  Returns 0 on success, -1 on send
 * error.
 */
int	bcast_send_one(struct Conn *c, const void *body, size_t len);

/*
 * Send a framed TCP message to every authenticated connection
 * except the one identified by except_conn_id (use 0xFFFF for
 * "exclude nobody").  Returns the number of clients that
 * received the message.
 */
int	bcast_all(struct Server *s, const void *body, size_t len,
		uint16_t except_conn_id);

/*
 * Send a raw UDP datagram to every authenticated connection
 * except the one identified by except_conn_id.  Destination is
 * each connection's recorded UDP peer address (c->peer).  Used
 * for 0x1e / 0x39 per-car broadcasts, which the real Kunos
 * server sends over UDP, not TCP.
 */
int	bcast_all_udp(struct Server *s, const void *body, size_t len,
		uint16_t except_conn_id);

#endif /* ACCD_BCAST_H */
