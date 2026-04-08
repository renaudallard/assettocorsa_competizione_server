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

#include "bcast.h"
#include "chat.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "penalty.h"
#include "prim.h"
#include "session.h"
#include "state.h"

static int
prefix(const char *s, const char *p)
{
	size_t pl = strlen(p);

	return strncmp(s, p, pl) == 0 &&
	    (s[pl] == '\0' || s[pl] == ' ');
}

static int
parse_int_arg(const char *s, int *out)
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
static int
car_by_race_number(struct Server *s, int num)
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
static void
broadcast_system_chat(struct Server *s, const char *text, uint8_t chat_type)
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

static void
ballast_or_restrictor(struct Server *s, const char *args, int is_ballast)
{
	int car_num, value, car_id;
	struct CarEntry *car;
	struct ByteBuf out;
	char chat[128];

	if (parse_int_arg(args, &car_num) < 0)
		return;
	while (*args == ' ')
		args++;
	while (*args && (*args < '0' || *args > '9'))
		args++;
	if (*args == '\0')
		return;
	{
		char *end;
		value = (int)strtol(args, &end, 10);
		if (end == args)
			return;
	}
	car_id = car_by_race_number(s, car_num);
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

	broadcast_system_chat(s, chat, 4);
	log_info("admin: %s", chat);
}

static void
issue_penalty(struct Server *s, const char *cmd, const char *args,
    int collision)
{
	int car_num, car_id, kind;
	char chat[128];

	if (parse_int_arg(args, &car_num) < 0)
		return;
	car_id = car_by_race_number(s, car_num);
	if (car_id < 0) {
		log_warn("admin: /%s for unknown car #%d", cmd, car_num);
		return;
	}
	kind = penalty_kind_from_string(cmd);
	if (kind == PEN_NONE)
		return;
	if (penalty_enqueue(s, car_id, (uint8_t)kind, collision) < 0)
		return;
	penalty_format_chat(chat, sizeof(chat),
	    (uint8_t)kind, collision, car_num);
	broadcast_system_chat(s, chat, 4);
	log_info("admin: %s", chat);
}

static void
do_kick_or_ban(struct Server *s, const char *args, int permanent)
{
	int car_num, car_id;
	struct Conn *target = NULL;
	int j;
	char chat[128];

	if (parse_int_arg(args, &car_num) < 0)
		return;
	car_id = car_by_race_number(s, car_num);
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
	broadcast_system_chat(s, chat, 5);

	/*
	 * Send the kick / ban notification directly to the target
	 * via 0x2b chat type 5 (the binary's "system warning"
	 * variant) before closing.  We mark the connection as
	 * disconnect-pending so the main loop drops it on the
	 * next iteration.
	 */
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
	log_info("admin: %s", chat);
}

int
chat_process(struct Server *s, struct Conn *c, const char *text)
{
	int car_num;

	if (text == NULL || *text == '\0')
		return 1;

	log_info("CHAT conn=%u: %s", (unsigned)c->conn_id, text);

	/* /admin elevation. */
	if (prefix(text, "/admin")) {
		const char *arg = text + 6;

		while (*arg == ' ')
			arg++;
		if (*arg == '\0') {
			log_info("admin: missing password");
			return 1;
		}
		if (strcmp(arg, s->admin_password) == 0) {
			c->is_admin = 1;
			broadcast_system_chat(s, "You are now server admin", 4);
			log_info("admin: conn=%u elevated to admin",
			    (unsigned)c->conn_id);
		} else {
			log_info("admin: wrong password from conn=%u",
			    (unsigned)c->conn_id);
		}
		return 1;
	}

	/* &swap (driver swap, non-admin). */
	if (prefix(text, "&swap")) {
		log_info("swap: conn=%u requested driver swap (TODO)",
		    (unsigned)c->conn_id);
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
	if (prefix(text, "/next")) {
		log_info("admin: /next");
		broadcast_system_chat(s, "Forwarding to next session", 4);
		session_advance(s);
	} else if (prefix(text, "/restart")) {
		log_info("admin: /restart");
		broadcast_system_chat(s, "Session restarted by administrator", 4);
		session_reset(s, s->session.session_index);
	} else if (prefix(text, "/kick")) {
		do_kick_or_ban(s, text + 5, 0);
	} else if (prefix(text, "/ban")) {
		do_kick_or_ban(s, text + 4, 1);
	} else if (prefix(text, "/dq")) {
		if (parse_int_arg(text + 3, &car_num) == 0) {
			int car_id = car_by_race_number(s, car_num);
			if (car_id >= 0) {
				penalty_enqueue(s, car_id, PEN_DQ, 0);
				char chat[128];
				snprintf(chat, sizeof(chat),
				    "Car #%d was disqualified by Race Control",
				    car_num);
				broadcast_system_chat(s, chat, 4);
			}
		}
	} else if (prefix(text, "/clear_all")) {
		penalty_clear_all(s);
		broadcast_system_chat(s,
		    "All pending penalties cleared by Race Control", 4);
	} else if (prefix(text, "/clear")) {
		if (parse_int_arg(text + 6, &car_num) == 0) {
			int car_id = car_by_race_number(s, car_num);
			char chat[128];

			penalty_clear(s, car_id);
			snprintf(chat, sizeof(chat),
			    "Pending penalties for #%d cleared by Race Control",
			    car_num);
			broadcast_system_chat(s, chat, 4);
		}
	} else if (prefix(text, "/cleartp")) {
		if (parse_int_arg(text + 8, &car_num) == 0) {
			int car_id = car_by_race_number(s, car_num);
			char chat[128];

			penalty_clear(s, car_id);
			snprintf(chat, sizeof(chat),
			    "Pending post race time penalties for #%d "
			    "cleared by Race Control", car_num);
			broadcast_system_chat(s, chat, 4);
		}
	} else if (prefix(text, "/tp5c")) {
		issue_penalty(s, "tp5c", text + 5, 1);
	} else if (prefix(text, "/tp5")) {
		issue_penalty(s, "tp5", text + 4, 0);
	} else if (prefix(text, "/tp15c")) {
		issue_penalty(s, "tp15c", text + 6, 1);
	} else if (prefix(text, "/tp15")) {
		issue_penalty(s, "tp15", text + 5, 0);
	} else if (prefix(text, "/dtc")) {
		issue_penalty(s, "dtc", text + 4, 1);
	} else if (prefix(text, "/dt")) {
		issue_penalty(s, "dt", text + 3, 0);
	} else if (prefix(text, "/sg10c")) {
		issue_penalty(s, "sg10c", text + 6, 1);
	} else if (prefix(text, "/sg10")) {
		issue_penalty(s, "sg10", text + 5, 0);
	} else if (prefix(text, "/sg20c")) {
		issue_penalty(s, "sg20c", text + 6, 1);
	} else if (prefix(text, "/sg20")) {
		issue_penalty(s, "sg20", text + 5, 0);
	} else if (prefix(text, "/sg30c")) {
		issue_penalty(s, "sg30c", text + 6, 1);
	} else if (prefix(text, "/sg30")) {
		issue_penalty(s, "sg30", text + 5, 0);
	} else if (prefix(text, "/ballast")) {
		ballast_or_restrictor(s, text + 8, 1);
	} else if (prefix(text, "/restrictor")) {
		ballast_or_restrictor(s, text + 11, 0);
	} else if (prefix(text, "/track")) {
		log_info("admin: /track (TODO needs entrylist reload)");
	} else if (prefix(text, "/manual entrylist")) {
		log_info("admin: /manual entrylist (TODO)");
	} else if (prefix(text, "/controllers")) {
		log_info("admin: /controllers (TODO needs 0x5b request)");
	} else if (prefix(text, "/controller")) {
		if (parse_int_arg(text + 11, &car_num) == 0)
			log_info("admin: /controller %d (TODO)", car_num);
	} else if (prefix(text, "/connections")) {
		int j;
		broadcast_system_chat(s, "Active connections:", 4);
		for (j = 0; j < ACC_MAX_CARS; j++) {
			char line[128];
			struct Conn *cn = s->conns[j];
			if (cn == NULL || cn->state != CONN_AUTH)
				continue;
			snprintf(line, sizeof(line),
			    "  conn=%u car=%d", (unsigned)cn->conn_id,
			    cn->car_id);
			broadcast_system_chat(s, line, 4);
		}
	} else if (prefix(text, "/hellban")) {
		log_info("admin: /hellban (TODO)");
	} else if (prefix(text, "/report")) {
		log_info("admin: /report (TODO)");
	} else if (prefix(text, "/latencymode")) {
		log_info("admin: /latencymode (TODO)");
	} else if (prefix(text, "/legacy")) {
		log_info("admin: /legacy netcode");
		broadcast_system_chat(s, "Server now uses legacy netcode", 4);
	} else if (prefix(text, "/regular")) {
		log_info("admin: /regular netcode");
		broadcast_system_chat(s, "Server is now in regular mode", 4);
	} else if (prefix(text, "/debug")) {
		log_info("admin: /debug (toggle)");
	} else {
		log_info("admin: unknown command: %s", text);
	}
	return 1;
}
