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
 * smpr.h -- ServerMonitor protocol (SMPR) connection handler.
 *
 * SMPR is the kunos protobuf side-channel that lets external
 * hosting / monitoring tools (accweb, accservermanager,
 * emperorservers) attach to a running server and stream
 * configuration / session / leaderboard updates.
 *
 * The protocol shares the gameplay TCP port (`tcpPort`).  The
 * dispatcher demultiplexes at the first frame: a sim handshake
 * (opcode 0x09) routes to the regular handshake handler, while a
 * protobuf ServerMonitorConnectionRequest (first byte 0x0a = tag
 * for field 1, wire type 2 length-delimited) routes here.
 *
 * After the handshake, the server pushes 0x01..0x07 messages
 * matching the ServerMonitor protocol (§12B of NOTEBOOK_B.md):
 *
 *   0x01 REGISTRATION_RESULT     - reply to the hello
 *   0x02 SERVER_CONFIGURATION    - track / session list / max slots
 *   0x03 SESSION_STATE           - phase / weather / temps / cars
 *   0x04 CAR_ENTRY               - one per connected car
 *   0x05 CONNECTION_ENTRY        - one per connected sim driver
 *   0x06 REALTIME_UPDATE         - periodic per-client (default 250 ms)
 *   0x07 LEADERBOARD_UPDATE      - fan-out after every sim 0x36
 *
 * The encoder side already exists in monitor.{c,h} + pb.{c,h};
 * this module wires it to actual TCP fan-out.
 */
#ifndef ACCD_SMPR_H
#define ACCD_SMPR_H

#include <stddef.h>

struct Conn;
struct Server;

/*
 * Handle a ServerMonitorConnectionRequest just received over TCP.
 * Decodes the protobuf body, marks the Conn as SMPR (so gameplay
 * handlers ignore it), and pushes the REGISTRATION_RESULT plus the
 * initial state burst (0x02 config, 0x03 session, 0x04 cars, 0x05
 * connections).  Returns 0 on success; on decode failure, sends a
 * REGISTRATION_RESULT(success=false, errorTxt="bad request") and
 * leaves the Conn flagged so the dispatcher can close it.
 */
int	smpr_handle_connect(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/*
 * Per-tick: push REALTIME_UPDATE (0x06) to every SMPR conn whose
 * smpr_rt_interval_ms has elapsed since the last push.
 */
void	smpr_tick_realtime(struct Server *s);

/*
 * Fan-out hook: push LEADERBOARD_UPDATE (0x07) to every SMPR conn
 * immediately after a sim-side 0x36 fires.  Wired right after the
 * broadcast_leaderboard_if_changed call in tick.c.
 */
void	smpr_broadcast_leaderboard(struct Server *s);
void	smpr_broadcast_session_state(struct Server *s);

/*
 * Delta notifies — fan out CONNECTION_ENTRY / CAR_ENTRY to every
 * SMPR conn when a sim conn joins or leaves.  Called from
 * handshake.c on success and state.c on conn_drop.
 */
void	smpr_notify_conn_changed(struct Server *s, struct Conn *changed);
void	smpr_notify_car_changed(struct Server *s, int car_id);

#endif /* ACCD_SMPR_H */
