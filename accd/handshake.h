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
 * handshake.h -- ACP_REQUEST_CONNECTION (0x09) parser and
 * 0x0b handshake response builder.
 */

#ifndef ACCD_HANDSHAKE_H
#define ACCD_HANDSHAKE_H

#include <stddef.h>

#include "state.h"

/*
 * Parse and act on an ACP_REQUEST_CONNECTION message body
 * (msg id byte already consumed).  Validates the version,
 * password, server-full state, and entry list.  Calls
 * handshake_send_response() with the appropriate accept or
 * reject outcome and updates the connection state.
 *
 * Returns 0 on success (whether accepted or rejected — both
 * are "successfully handled"), -1 on a fatal protocol error
 * that should drop the connection.
 */
int	handshake_handle(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/*
 * Build the welcome trailer body (carIndex + trackName + eventId +
 * session list + entry list + per-car records).  Used by both the
 * 0x0b handshake response and the 0x4b welcome redelivery.
 */
int	build_welcome_trailer(struct ByteBuf *bb, struct Server *s,
		struct Conn *c);

/*
 * Emit one spawnDef record for the car at `car_slot`.  Used by
 * the welcome trailer spawnDef loop and by the 0x23 CarInfoResponse
 * reply.  Returns -1 if the car is not connected or lacks a valid
 * handshake echo.
 */
int	write_spawn_def(struct ByteBuf *bb, struct Server *s, int car_slot);

/*
 * Emit the assist_rules + leaderboard section from FUN_140034a40
 * in accServer.exe.  Used by the welcome trailer (0x0b body) and
 * by the standalone 0x36 leaderboard broadcast (prefixed with
 * u8 0x36).  Byte-exact to the exe: u32 + u8 + 3 u32 + u8 + u16
 * car_count + per-car FUN_140034210 entry + 2 u8 tail.
 */
int	write_leaderboard_section(struct ByteBuf *bb, struct Server *s);

#endif /* ACCD_HANDSHAKE_H */
