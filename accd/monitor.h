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
 * monitor.h -- ServerMonitor protobuf message builders.
 *
 * The seven message types from §12B of NOTEBOOK_B.md.  These are
 * also the bodies of the sim-protocol generic-serializer ids
 * 0x01..0x07 (see §5.6.4a).  We hand-roll a minimal protobuf
 * encoder via pb.h rather than dragging in nanopb.
 *
 * Field numbers are derived from the documented schema in §12B.3.
 * They start from 1 in the order documented and assume no skips.
 * If a real client decodes any of these as malformed, the field
 * numbers are the most likely point of mismatch and the place to
 * patch first.
 *
 * Wire enums (§12B.4):
 *   ServerMonitorSessionType: Practice=0, Qualifying=1, Race=2
 *   ServerMonitorCupCategory: Overall=0, ProAm=1, Silver=2, National=3
 */

#ifndef ACCD_MONITOR_H
#define ACCD_MONITOR_H

#include "io.h"
#include "state.h"

/* ----- ServerMonitorHandshakeResult (msg 0x01) ------------------- */
#define PB_HSR_SUCCESS			1
#define PB_HSR_CONNECTION_ID		2
#define PB_HSR_ERROR_TXT		3

/* ----- ServerMonitorConnectionEntry (msg 0x05) ------------------- */
#define PB_CONN_CONNECTION_ID		1
#define PB_CONN_FIRST_NAME		2
#define PB_CONN_LAST_NAME		3
#define PB_CONN_SHORT_NAME		4
#define PB_CONN_PLAYER_ID		5
#define PB_CONN_IS_ADMIN		6
#define PB_CONN_IS_SPECTATOR		7

/* ----- ServerMonitorCarEntry (msg 0x04) -------------------------- */
#define PB_CAR_CAR_ID			1
#define PB_CAR_CAR_MODEL		2
#define PB_CAR_DRIVING_CONNECTION_ID	3
#define PB_CAR_RACE_NUMBER		4
#define PB_CAR_CUP_CATEGORY		5

/* ----- ServerMonitorSessionDef (sub-message of CFG_STATE) -------- */
#define PB_SDEF_SESSION_TYPE		1
#define PB_SDEF_ROUND			2
#define PB_SDEF_DURATION_SECONDS	3
#define PB_SDEF_RACE_DAY		4
#define PB_SDEF_MINUTE_OF_DAY		5
#define PB_SDEF_TIME_MULTIPLIER		6
#define PB_SDEF_OVERTIME_DURATION_S	7
#define PB_SDEF_PRE_RACE_WAIT_TIME_S	8

/* ----- ServerMonitorConfigurationState (msg 0x02) ---------------- */
#define PB_CFG_SERVER_NAME		1
#define PB_CFG_TRACK_NAME		2
#define PB_CFG_MAX_SLOTS		3
#define PB_CFG_TRACK_MEDALS		4
#define PB_CFG_SA_REQUIRED		5
#define PB_CFG_IS_PW_PROTECTED		6
#define PB_CFG_IS_LOCKED_ENTRY_LIST	7
#define PB_CFG_SESSIONS			8	/* repeated SessionDef */

/* ----- ServerMonitorSessionState (msg 0x03) ---------------------- */
#define PB_SS_CURRENT_SESSION_INDEX	1
#define PB_SS_WEEKEND_TIME_SECONDS	2
#define PB_SS_IDEAL_LINE_GRIP		3
#define PB_SS_AMBIENT_TEMP		4
#define PB_SS_ROAD_TEMP			5
#define PB_SS_CLOUD_LEVEL		6
#define PB_SS_RAIN_LEVEL		7
#define PB_SS_TRACK_WETNESS		8
#define PB_SS_DRY_LINE_WETNESS		9
#define PB_SS_TRACK_PUDDLES		10
#define PB_SS_RAIN_FORECAST_10MIN	11
#define PB_SS_RAIN_FORECAST_30MIN	12
#define PB_SS_CARS_CONNECTED		13

/* ----- ServerMonitorRealtimeUpdate (msg 0x06) -------------------- */
#define PB_RTU_SERVER_NOW		1
#define PB_RTU_SESSION_STATE		2	/* SessionState submessage */
#define PB_RTU_CONNECTIONS		3	/* repeated */
#define PB_RTU_CARS			4	/* repeated */

/* ----- ServerMonitorLeaderboardEntry (sub of LEADERBOARD) -------- */
#define PB_LBE_CAR_ENTRY		1	/* CarEntry submessage */
#define PB_LBE_CURRENT_STEAM_ID		2
#define PB_LBE_MISSING_MANDATORY_PITS	3
#define PB_LBE_DRIVER_TIMES		4	/* repeated int32 */
#define PB_LBE_LAST_LAP_TIME		5
#define PB_LBE_LAST_LAP_SPLITS		6	/* repeated int32 */
#define PB_LBE_BEST_LAP_TIME		7
#define PB_LBE_BEST_LAP_SPLITS		8	/* repeated int32 */
#define PB_LBE_LAP_COUNT		9
#define PB_LBE_TOTAL_TIME		10
#define PB_LBE_CURRENT_PENALTY		11
#define PB_LBE_CURRENT_PENALTY_VALUE	12
#define PB_LBE_DRIVER_NAME		13
#define PB_LBE_DRIVER_SHORT_NAME	14
#define PB_LBE_CAR_MODEL		15

/* ----- ServerMonitorLeaderboard (msg 0x07) ----------------------- */
#define PB_LB_BEST_LAP			1
#define PB_LB_BEST_SPLITS		2	/* repeated int32 */
#define PB_LB_IS_DECLARED_WET_SESSION	3
#define PB_LB_ENTRIES			4	/* repeated LeaderboardEntry */

/* ----- builders -------------------------------------------------- */

/*
 * Each builder appends a complete protobuf message body to bb
 * (without the sim-protocol msg id byte; that's added by the
 * caller).  Returns 0 on success, -1 on out-of-memory.
 */

int	monitor_build_handshake_result(struct ByteBuf *bb,
		int success, int connection_id, const char *err_txt);

int	monitor_build_connection_entry(struct ByteBuf *bb,
		const struct Server *s, const struct Conn *c);

int	monitor_build_car_entry(struct ByteBuf *bb,
		const struct CarEntry *car, int driving_connection_id);

int	monitor_build_configuration_state(struct ByteBuf *bb,
		const struct Server *s);

int	monitor_build_session_state(struct ByteBuf *bb,
		const struct Server *s);

int	monitor_build_realtime_update(struct ByteBuf *bb,
		const struct Server *s);

int	monitor_build_leaderboard(struct ByteBuf *bb,
		const struct Server *s);

/*
 * Send the post-handshake welcome push sequence (0x04 + 0x05 +
 * 0x03 + 0x07) to the joining client and a 0x04 + 0x05
 * notification of the new car/connection to every other already-
 * authenticated client.
 */
int	monitor_push_welcome_sequence(struct Server *s, struct Conn *c);

#endif /* ACCD_MONITOR_H */
