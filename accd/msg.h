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
 * msg.h -- ACC sim protocol message id constants.
 *
 * The id space is split by direction (client->server vs
 * server->client), with substantial overlap of byte values that
 * mean different things in each direction.  See
 * notebook-b/NOTEBOOK_B.md §5.6 for the full catalog.
 *
 * Names follow the binary's own log-string identifiers (ACP_*)
 * where they exist, and are descriptive otherwise.
 */

#ifndef ACCD_MSG_H
#define ACCD_MSG_H

#include <stdint.h>

/* The protocol version every client and server must agree on. */
#define ACC_PROTOCOL_VERSION		0x100	/* build 14255706 / 1.10.2 */

/* ----- client -> server (TCP) ------------------------------------ */

#define ACP_REQUEST_CONNECTION		0x09
#define ACP_DISCONNECT			0x10	/* clean disconnect */
#define ACP_LAP_COMPLETED		0x19
#define ACP_SECTOR_SPLIT_BULK		0x20
#define ACP_SECTOR_SPLIT_SINGLE		0x21
#define ACP_CHAT			0x2a
#define ACP_CAR_SYSTEM_UPDATE		0x2e
#define ACP_TYRE_COMPOUND_UPDATE	0x2f
#define ACP_CAR_LOCATION_UPDATE		0x32
#define ACP_OUT_OF_TRACK		0x3d
#define ACP_REPORT_PENALTY		0x41	/* tentative */
#define ACP_LAP_TICK			0x42	/* tentative */
#define ACP_DAMAGE_ZONES_UPDATE		0x43
#define ACP_CAR_DIRT_UPDATE		0x46
#define ACP_UPDATE_DRIVER_SWAP_STATE	0x47
#define ACP_EXECUTE_DRIVER_SWAP		0x48
#define ACP_DRIVER_SWAP_STATE_REQUEST	0x4a
#define ACP_DRIVER_STINT_RESET		0x4f
#define ACP_ELO_UPDATE			0x51
#define ACP_MANDATORY_PITSTOP_SERVED	0x54
#define ACP_LOAD_SETUP			0x55
#define ACP_CTRL_INFO			0x5b

/* ----- client -> server (UDP, main udpPort) ---------------------- */

#define ACP_KEEPALIVE_A			0x13	/* silent */
#define ACP_PONG_PHYSICS		0x16
#define ACP_KEEPALIVE_B			0x17	/* silent */
#define ACP_CAR_UPDATE			0x1e	/* per-tick state */
#define ACP_CAR_INFO_REQUEST		0x22
#define ACP_TIME_EVENT			0x5e	/* tentative */
#define ACP_ADMIN_QUERY			0x5f

/* ----- LAN discovery (UDP 8999) ---------------------------------- */

#define ACP_LAN_DISCOVER		0x48	/* client probe */
#define ACP_LAN_RESPONSE		0xc0	/* server reply */

/* ----- server -> client ------------------------------------------ */

/*
 * Generic-serializer ids: these wrap protobuf-encoded
 * ServerMonitorProtocolMessage types 1..7.  See §12B.
 */
#define SRV_HANDSHAKE_RESULT		0x01	/* ServerMonitorHandshakeResult */
#define SRV_CONFIGURATION_STATE		0x02	/* ServerMonitorConfigurationState */
#define SRV_SESSION_STATE		0x03	/* ServerMonitorSessionState */
#define SRV_CAR_ENTRY			0x04	/* ServerMonitorCarEntry */
#define SRV_CONNECTION_ENTRY		0x05	/* ServerMonitorConnectionEntry */
#define SRV_REALTIME_UPDATE		0x06	/* ServerMonitorRealtimeUpdate */
#define SRV_LEADERBOARD_UPDATE		0x07	/* ServerMonitorLeaderboard */

#define SRV_HANDSHAKE_RESPONSE		0x0b
#define SRV_STATE_RECORD_0C		0x0c
#define SRV_KEEPALIVE_14		0x14
#define SRV_LAP_BROADCAST		0x1b
#define SRV_PERCAR_FAST_RATE		0x1e	/* periodic per-car broadcast */
#define SRV_CAR_INFO_RESPONSE		0x23	/* TCP reply to UDP 0x22 */
#define SRV_CAR_DISCONNECT_NOTIFY	0x24
#define SRV_LARGE_STATE_RESPONSE	0x28
#define SRV_CHAT_OR_STATE		0x2b
#define SRV_CAR_SYSTEM_RELAY		0x2e
#define SRV_TYRE_COMPOUND_RELAY		0x2f
#define SRV_LEADERBOARD_BCAST		0x36	/* full standings */
#define SRV_WEATHER_STATUS		0x37
#define SRV_PERCAR_SLOW_RATE		0x39	/* slow-rate sibling of 0x1e */
#define SRV_SECTOR_SPLITS_RELAY		0x3a
#define SRV_SECTOR_SPLIT_RELAY		0x3b
#define SRV_OUT_OF_TRACK_RELAY		0x3c
#define SRV_SESSION_RESULTS		0x3e
#define SRV_GRID_POSITIONS		0x3f
#define SRV_RACE_WEEKEND_RESET		0x40
#define SRV_DAMAGE_ZONES_RELAY		0x44
#define SRV_CAR_DIRT_RELAY		0x46
#define SRV_DRIVER_SWAP_STATE_BCAST	0x47
#define SRV_DRIVER_SWAP_RESULT		0x49
#define SRV_WELCOME_REDELIVERY		0x4b
#define SRV_RATING_SUMMARY		0x4e
#define SRV_DRIVER_STINT_RELAY		0x4f
#define SRV_BOP_UPDATE			0x53	/* MultiplayerBOPUpdate */
#define SRV_SETUP_DATA_RESPONSE		0x56
#define SRV_DRIVER_SWAP_NOTIFY		0x58
#define SRV_STATE_RECORD_59		0x59
#define SRV_CTRL_INFO_REQUEST		0x5b	/* 1-byte probe */
#define SRV_CONNECTIONS_LIST_ROW	0x5d	/* /connections per-row */
#define SRV_PERIODIC_UDP		0xbe

/*
 * Handshake rejection reasons (logged on the server side; sent
 * to the client as part of the 0x0b response).
 */
enum reject_reason {
	REJECT_OK = 0,
	REJECT_VERSION,
	REJECT_PASSWORD,
	REJECT_FULL,
	REJECT_BANNED,
	REJECT_KICKED,
	REJECT_BAD_CAR,
	REJECT_BAD_GRID
};

/*
 * Per-connection state machine values, matching the binary's
 * internal state byte at offset +0xa01dc.
 */
enum conn_state {
	CONN_UNAUTH = 0,	/* TCP socket open, no handshake yet */
	CONN_AUTH = 1,		/* handshake accepted */
	CONN_DISCONNECT = 3
};

#endif /* ACCD_MSG_H */
