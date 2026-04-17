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
 * chat.c -- chat / admin command parser.
 *
 * Wire-side decoding of 0x2a ACP_CHAT happens in the dispatcher;
 * this module operates on already-decoded UTF-8 text and returns
 * 0 if the message should be broadcast as a regular 0x2b chat,
 * 1 if it was a slash command (or &swap) handled internally.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bans.h"
#include "bcast.h"
#include "chat.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "penalty.h"
#include "prim.h"
#include "session.h"
#include "state.h"

int
chat_prefix(const char *s, const char *p)
{
	size_t pl = strlen(p);

	return strncmp(s, p, pl) == 0 &&
	    (s[pl] == '\0' || s[pl] == ' ');
}

int
chat_parse_int(const char *s, int *out)
{
	char *end;
	long v;

	while (*s == ' ')
		s++;
	if (*s == '\0')
		return -1;
	v = strtol(s, &end, 10);
	if (end == s || v < 0 || v > 1000)
		return -1;
	*out = (int)v;
	return 0;
}

/*
 * Find the car_id whose race_number matches `num`.  Returns -1
 * on no match.
 */
int
chat_car_by_racenum(struct Server *s, int num)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++)
		if (s->cars[i].used && s->cars[i].race_number == num)
			return i;
	return -1;
}

/*
 * Build and broadcast a 0x2b chat message containing a system
 * notification (chat type 4 = info, 5 = system warning).
 */
void
chat_broadcast(struct Server *s, const char *text, uint8_t chat_type)
{
	struct ByteBuf out;

	if (text == NULL || text[0] == '\0')
		return;
	bb_init(&out);
	if (wr_u8(&out, SRV_CHAT_OR_STATE) == 0 &&
	    wr_str_a(&out, "Race Control") == 0 &&
	    wr_str_a(&out, text) == 0 &&
	    wr_i32(&out, 0) == 0 &&
	    wr_u8(&out, chat_type) == 0)
		(void)bcast_all(s, out.data, out.wpos, 0xFFFF);
	bb_free(&out);
}

void
chat_do_bop(struct Server *s, const char *args, int is_ballast,
    char *reply, size_t replysz)
{
	int car_num, value, car_id;
	struct CarEntry *car;
	struct ByteBuf out;
	char chat[128];

	if (chat_parse_int(args, &car_num) < 0)
		return;
	/* Skip leading spaces, then skip the first number (car_num),
	 * then skip spaces again to reach the value argument. */
	while (*args == ' ')
		args++;
	while (*args >= '0' && *args <= '9')
		args++;
	while (*args == ' ')
		args++;
	if (*args == '\0')
		return;
	{
		char *end;
		value = (int)strtol(args, &end, 10);
		if (end == args)
			return;
	}
	car_id = chat_car_by_racenum(s,car_num);
	if (car_id < 0) {
		log_warn("admin: /%s for unknown car #%d",
		    is_ballast ? "ballast" : "restrictor", car_num);
		return;
	}
	car = &s->cars[car_id];
	if (is_ballast) {
		if (value > 40) value = 40;
		if (value < 0) value = 0;
		car->ballast_kg = (uint8_t)value;
		snprintf(chat, sizeof(chat),
		    "Assigned %d kg to car #%d", value, car_num);
	} else {
		if (value > 99) value = 99;
		if (value < 0) value = 0;
		car->restrictor = (float)value / 100.0f;
		snprintf(chat, sizeof(chat),
		    "Assigned %d %% to car #%d", value, car_num);
	}

	/* Emit 0x53 MultiplayerBOPUpdate broadcast (per §5.6.4a). */
	bb_init(&out);
	if (wr_u8(&out, SRV_BOP_UPDATE) == 0 &&
	    wr_u16(&out, car->car_id) == 0 &&
	    wr_u16(&out, (uint16_t)car->ballast_kg) == 0 &&
	    wr_f32(&out, car->restrictor) == 0)
		(void)bcast_all(s, out.data, out.wpos, 0xFFFF);
	bb_free(&out);

	chat_broadcast(s, chat, 4);
	if (reply != NULL)
		snprintf(reply, replysz, "%s", chat);
	log_info("admin: %s", chat);
}

void
chat_do_penalty(struct Server *s, const char *cmd, const char *args,
    int collision, char *reply, size_t replysz)
{
	int car_num, car_id, kind;
	char chat[128];

	if (chat_parse_int(args, &car_num) < 0)
		return;
	car_id = chat_car_by_racenum(s, car_num);
	if (car_id < 0) {
		log_warn("admin: /%s for unknown car #%d", cmd, car_num);
		return;
	}
	kind = penalty_kind_from_string(cmd);
	if (kind == PEN_NONE)
		return;
	{
		uint8_t exe = penalty_exe_kind_of((uint8_t)kind);
		int32_t val = 3;	/* admin /dt, /sg* default */
		if (kind == PEN_TP5)
			val = 5;
		else if (kind == PEN_TP15)
			val = 15;
		if (penalty_enqueue(s, car_id, exe, 8, val, 0,
		    collision, REASON_RACE_CONTROL) < 0)
			return;
	}
	penalty_format_chat(chat, sizeof(chat),
	    (uint8_t)kind, REASON_RACE_CONTROL, collision, car_num);
	chat_broadcast(s, chat, 4);
	if (reply != NULL)
		snprintf(reply, replysz, "%s", chat);
	log_info("admin: %s", chat);
}

void
chat_do_kick(struct Server *s, const char *args, int permanent,
    char *reply, size_t replysz)
{
	int car_num, car_id;
	struct Conn *target = NULL;
	int j;
	char chat[128];

	if (chat_parse_int(args, &car_num) < 0)
		return;
	car_id = chat_car_by_racenum(s, car_num);
	if (car_id < 0) {
		log_warn("admin: /%s for unknown car #%d",
		    permanent ? "ban" : "kick", car_num);
		return;
	}
	for (j = 0; j < ACC_MAX_CARS; j++) {
		if (s->conns[j] != NULL && s->conns[j]->car_id == car_id) {
			target = s->conns[j];
			break;
		}
	}
	if (target == NULL) {
		log_warn("admin: car #%d has no active connection", car_num);
		return;
	}
	snprintf(chat, sizeof(chat),
	    permanent ? "Car #%d has been banned from the server"
	              : "Car #%d has been kicked from the server",
	    car_num);
	chat_broadcast(s, chat, 5);

	{
		struct ByteBuf out;
		const char *reason = permanent
		    ? "You have been banned from the server"
		    : "You have been kicked from the server";

		bb_init(&out);
		if (wr_u8(&out, SRV_CHAT_OR_STATE) == 0 &&
		    wr_str_a(&out, "Race Control") == 0 &&
		    wr_str_a(&out, reason) == 0 &&
		    wr_i32(&out, 0) == 0 &&
		    wr_u8(&out, 5) == 0)
			(void)bcast_send_one(target, out.data, out.wpos);
		bb_free(&out);
	}
	target->state = CONN_DISCONNECT;
	if (permanent && car_id >= 0 && car_id < ACC_MAX_CARS) {
		const char *sid = s->cars[car_id].drivers[0].steam_id;

		if (bans_add(&s->bans, sid) == 0) {
			bans_save(&s->bans, s->cfg_dir);
			log_info("admin: banned steam_id %s", sid);
		}
	}
	if (reply != NULL)
		snprintf(reply, replysz, "%s", chat);
	log_info("admin: %s", chat);
}

static const char *track_list[] = {
	"monza", "misano", "paul_ricard", "silverstone", "spa",
	"nurburgring", "hungaroring", "zandvoort", "brands_hatch",
	"zolder", "barcelona", "mount_panorama_2019", "laguna_seca",
	"suzuka", "kyalami", "oulton_park", "snetterton", "donington",
	"imola", "watkins_glen", "cota", "indianapolis", "valencia",
	"nurburgring_24h", "red_bull_ring",
	NULL
};

int
chat_track_count(void)
{
	int n = 0;
	while (track_list[n] != NULL)
		n++;
	return n;
}

const char *
chat_track_name(int index)
{
	if (index < 0 || index >= chat_track_count())
		return NULL;
	return track_list[index];
}

void
chat_do_track(struct Server *s, const char *args,
    char *reply, size_t replysz)
{
	const char *name;
	char msg[128];
	int j;

	while (*args == ' ')
		args++;
	name = args;
	if (*name == '\0') {
		if (reply != NULL)
			snprintf(reply, replysz,
			    "current track: %s (type 'tracks' for list)",
			    s->track);
		return;
	}

	/* Update the track name. */
	snprintf(s->track, sizeof(s->track), "%s", name);
	session_reset(s, 0);

	snprintf(msg, sizeof(msg), "Event changed to %s", s->track);
	chat_broadcast(s, msg, 4);
	log_info("admin: %s", msg);

	/*
	 * Send 0x4b welcome trailer redelivery + monitor welcome
	 * sequence to every connected client so they load the new
	 * track.
	 */
	for (j = 0; j < ACC_MAX_CARS; j++) {
		struct Conn *cn = s->conns[j];
		struct ByteBuf bb;

		if (cn == NULL || cn->state != CONN_AUTH)
			continue;

		/* 0x4b with the same body as the 0x0b welcome trailer. */
		bb_init(&bb);
		if (wr_u8(&bb, SRV_WELCOME_REDELIVERY) == 0 &&
		    build_welcome_trailer(&bb, s, cn) == 0)
			(void)conn_send_framed(cn, bb.data, bb.wpos);
		bb_free(&bb);
	}

	if (reply != NULL)
		snprintf(reply, replysz, "%s", msg);
}

int
chat_process(struct Server *s, struct Conn *c, const char *text)
{
	int car_num;

	if (text == NULL || *text == '\0')
		return 1;

	log_info("CHAT conn=%u: %s", (unsigned)c->conn_id, text);

	/* /admin elevation. */
	if (chat_prefix(text, "/admin")) {
		const char *arg = text + 6;

		while (*arg == ' ')
			arg++;
		if (*arg == '\0') {
			log_info("admin: missing password");
			return 1;
		}
		if (strcmp(arg, s->admin_password) == 0) {
			c->is_admin = 1;
			chat_broadcast(s,"You are now server admin", 4);
			log_info("admin: conn=%u elevated to admin",
			    (unsigned)c->conn_id);
		} else {
			log_info("admin: wrong password from conn=%u",
			    (unsigned)c->conn_id);
		}
		return 1;
	}

	/* &swap <index> (driver swap, non-admin). */
	if (chat_prefix(text, "&swap")) {
		const char *arg = text + 5;
		int target;
		struct CarEntry *car;

		if (c->car_id < 0) {
			log_info("swap: conn=%u has no car",
			    (unsigned)c->conn_id);
			return 1;
		}
		car = &s->cars[c->car_id];
		if (chat_parse_int(arg, &target) < 0 ||
		    target < 0 || target >= car->driver_count) {
			log_info("swap: invalid target from conn=%u",
			    (unsigned)c->conn_id);
			return 1;
		}
		if ((uint8_t)target == car->current_driver_index) {
			log_info("swap: target %d is already active",
			    target);
			return 1;
		}
		car->swap_state[target] = 1;	/* REQUESTED */
		log_info("swap: conn=%u requested driver %d on car %u",
		    (unsigned)c->conn_id, target,
		    (unsigned)car->car_id);

		/* Broadcast updated swap state to all clients. */
		{
			struct ByteBuf bb;
			int i;

			bb_init(&bb);
			if (wr_u8(&bb, SRV_DRIVER_SWAP_STATE_BCAST) == 0 &&
			    wr_u16(&bb, car->car_id) == 0 &&
			    wr_u8(&bb, car->driver_count) == 0) {
				for (i = 0; i < car->driver_count; i++)
					(void)wr_u8(&bb, car->swap_state[i]);
				(void)bcast_all(s, bb.data, bb.wpos,
				    0xFFFF);
			}
			bb_free(&bb);
		}

		/*
		 * Acknowledge the handover request back to the sender
		 * with SRV_DRIVER_HANDOVER_REQ (0x59).  Matches
		 * FUN_140027990 in accServer.exe: 4-byte body carrying
		 * the source car_id and the (driver_index - 1) slot of
		 * the driver who will take over.  Clients use this to
		 * display the "handover pending" UI until the matching
		 * 0x48 ACP_EXECUTE_DRIVER_SWAP is received.
		 */
		{
			struct ByteBuf bb;

			bb_init(&bb);
			if (wr_u8(&bb, SRV_DRIVER_HANDOVER_REQ) == 0 &&
			    wr_u16(&bb, car->car_id) == 0 &&
			    wr_u8(&bb, (uint8_t)target) == 0)
				(void)conn_send_framed(c, bb.data, bb.wpos);
			bb_free(&bb);
		}
		return 1;
	}

	/* Regular chat broadcast (no slash). */
	if (text[0] != '/')
		return 0;

	if (!c->is_admin) {
		log_info("admin command rejected (not admin) from conn=%u",
		    (unsigned)c->conn_id);
		return 1;
	}

	/* Order matters: longer prefixes first to avoid /tp5 vs /tp5c. */
	if (chat_prefix(text, "/next")) {
		log_info("admin: /next");
		chat_broadcast(s,"Forwarding to next session", 4);
		session_advance(s);
	} else if (chat_prefix(text, "/wt")) {
		/*
		 * /wt: dump current weather snapshot.  accServer.exe
		 * header is "Standard weather:" (or "Snowflake weather:"
		 * when the dynamic-mode flag at +0x315 is set), followed
		 * by rain / cloud / wetness / dry-line fields scaled to
		 * integer percent via DAT_14014bd74 (f32 100.0).
		 */
		char msg[160];
		const char *head = s->weather.randomness > 0
		    ? "Snowflake weather:" : "Standard weather:";
		snprintf(msg, sizeof(msg),
		    "%s rain=%d clouds=%d wet=%d dry=%d "
		    "wind=%d/%d amb=%d road=%d",
		    head,
		    (int)(s->weather.current_rain * 100.0f),
		    (int)(s->weather.clouds * 100.0f),
		    (int)(s->weather.track_wetness * 100.0f),
		    (int)(s->weather.dry_line_wetness * 100.0f),
		    (int)s->weather.wind_speed,
		    (int)s->weather.wind_direction,
		    (int)s->session.ambient_temp,
		    (int)s->session.track_temp);
		log_info("admin: /wt");
		chat_broadcast(s, msg, 4);
	} else if (chat_prefix(text, "/go")) {
		/*
		 * /go: cut the pre-session wait and start the active
		 * session now.  Matches FUN_14012f290 in accServer.exe,
		 * which advances the session manager's next-start gate.
		 * For us: if we're in WAITING or FORMATION, collapse
		 * ts[0]..ts[1] so the next tick transitions us into
		 * PRE_SESSION immediately.  Later boundaries are not
		 * shifted — the active session still gets its full
		 * duration from ts[2] onwards.
		 */
		log_info("admin: /go");
		chat_broadcast(s, "Session started by administrator", 4);
		if (s->session.ts_valid) {
			uint64_t now = s->session.phase_started_ms;
			(void)now;
			s->session.ts[0] = 0;
			s->session.ts[1] = 0;
		}
	} else if (chat_prefix(text, "/restart")) {
		log_info("admin: /restart");
		chat_broadcast(s,"Session restarted by administrator", 4);
		session_reset(s, s->session.session_index);
	} else if (chat_prefix(text, "/kick")) {
		chat_do_kick(s, text + 5, 0, NULL, 0);
	} else if (chat_prefix(text, "/ban")) {
		chat_do_kick(s, text + 4, 1, NULL, 0);
	} else if (chat_prefix(text, "/dq")) {
		if (chat_parse_int(text + 3, &car_num) == 0) {
			int car_id = chat_car_by_racenum(s,car_num);
			if (car_id >= 0) {
				penalty_enqueue(s, car_id, EXE_DQ, 8, 3,
				    1, 0, REASON_RACE_CONTROL);
				char chat[128];
				snprintf(chat, sizeof(chat),
				    "Car #%d was disqualified by Race Control",
				    car_num);
				chat_broadcast(s,chat, 4);
			}
		}
	} else if (chat_prefix(text, "/clear_all")) {
		penalty_clear_all(s);
		chat_broadcast(s,
		    "All pending penalties cleared by Race Control", 4);
	} else if (chat_prefix(text, "/clear")) {
		if (chat_parse_int(text + 6, &car_num) == 0) {
			int car_id = chat_car_by_racenum(s,car_num);

			if (car_id < 0) {
				log_warn("admin: /clear for unknown car #%d",
				    car_num);
			} else {
				char chat[128];

				penalty_clear(s, car_id);
				snprintf(chat, sizeof(chat),
				    "Pending penalties for #%d cleared "
				    "by Race Control", car_num);
				chat_broadcast(s,chat, 4);
			}
		}
	} else if (chat_prefix(text, "/cleartp")) {
		if (chat_parse_int(text + 8, &car_num) == 0) {
			int car_id = chat_car_by_racenum(s,car_num);

			if (car_id < 0) {
				log_warn("admin: /cleartp for unknown car #%d",
				    car_num);
			} else {
				char chat[128];

				penalty_clear(s, car_id);
				snprintf(chat, sizeof(chat),
				    "Pending post race time penalties for #%d "
				    "cleared by Race Control", car_num);
				chat_broadcast(s,chat, 4);
			}
		}
	} else if (chat_prefix(text, "/tp5c")) {
		chat_do_penalty(s, "tp5c", text + 5, 1, NULL, 0);
	} else if (chat_prefix(text, "/tp5")) {
		chat_do_penalty(s, "tp5", text + 4, 0, NULL, 0);
	} else if (chat_prefix(text, "/tp15c")) {
		chat_do_penalty(s, "tp15c", text + 6, 1, NULL, 0);
	} else if (chat_prefix(text, "/tp15")) {
		chat_do_penalty(s, "tp15", text + 5, 0, NULL, 0);
	} else if (chat_prefix(text, "/dtc")) {
		chat_do_penalty(s, "dtc", text + 4, 1, NULL, 0);
	} else if (chat_prefix(text, "/dt")) {
		chat_do_penalty(s, "dt", text + 3, 0, NULL, 0);
	} else if (chat_prefix(text, "/sg10c")) {
		chat_do_penalty(s, "sg10c", text + 6, 1, NULL, 0);
	} else if (chat_prefix(text, "/sg10")) {
		chat_do_penalty(s, "sg10", text + 5, 0, NULL, 0);
	} else if (chat_prefix(text, "/sg20c")) {
		chat_do_penalty(s, "sg20c", text + 6, 1, NULL, 0);
	} else if (chat_prefix(text, "/sg20")) {
		chat_do_penalty(s, "sg20", text + 5, 0, NULL, 0);
	} else if (chat_prefix(text, "/sg30c")) {
		chat_do_penalty(s, "sg30c", text + 6, 1, NULL, 0);
	} else if (chat_prefix(text, "/sg30")) {
		chat_do_penalty(s, "sg30", text + 5, 0, NULL, 0);
	} else if (chat_prefix(text, "/ballast")) {
		chat_do_bop(s, text + 8, 1, NULL, 0);
	} else if (chat_prefix(text, "/restrictor")) {
		chat_do_bop(s, text + 11, 0, NULL, 0);
	} else if (chat_prefix(text, "/track")) {
		chat_do_track(s, text + 6, NULL, 0);
	} else if (chat_prefix(text, "/manual entrylist")) {
		log_info("admin: /manual entrylist (TODO)");
	} else if (chat_prefix(text, "/controllers")) {
		log_info("admin: /controllers (TODO needs 0x5b request)");
	} else if (chat_prefix(text, "/controller")) {
		if (chat_parse_int(text + 11, &car_num) == 0)
			log_info("admin: /controller %d (TODO)", car_num);
	} else if (chat_prefix(text, "/connections")) {
		int j;
		chat_broadcast(s,"Active connections:", 4);
		for (j = 0; j < ACC_MAX_CARS; j++) {
			char line[128];
			struct Conn *cn = s->conns[j];
			if (cn == NULL || cn->state != CONN_AUTH)
				continue;
			snprintf(line, sizeof(line),
			    "  conn=%u car=%d", (unsigned)cn->conn_id,
			    cn->car_id);
			chat_broadcast(s,line, 4);
		}
	} else if (chat_prefix(text, "/hellban")) {
		log_info("admin: /hellban (TODO)");
	} else if (chat_prefix(text, "/report")) {
		log_info("admin: /report (TODO)");
	} else if (chat_prefix(text, "/latencymode")) {
		log_info("admin: /latencymode (TODO)");
	} else if (chat_prefix(text, "/mp") ||
	           chat_prefix(text, "/legacy") ||
	           chat_prefix(text, "/regular")) {
		/*
		 * Single toggle in accServer.exe at server struct +0x22,
		 * reached via the 2-char "/mp" command.  Our earlier
		 * /legacy and /regular split was wrong — map both onto
		 * the same flip for backward compatibility.
		 */
		s->legacy_netcode = !s->legacy_netcode;
		log_info("admin: /mp -> legacy_netcode=%d",
		    (int)s->legacy_netcode);
		chat_broadcast(s, s->legacy_netcode
		    ? "Server now uses legacy netcode"
		    : "Server is now in regular mode", 4);
	} else if (chat_prefix(text, "/debug")) {
		log_info("admin: /debug (toggle)");
	} else {
		log_info("admin: unknown command: %s", text);
	}
	return 1;
}
