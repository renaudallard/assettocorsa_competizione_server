/*
 * chat.c -- chat / admin command parser.
 *
 * Wire-side decoding of 0x2a ACP_CHAT happens in the dispatcher;
 * this module operates on already-decoded UTF-8 text.  The reply
 * builders here log their action and produce a one-line text
 * reply that the caller broadcasts as 0x2b.
 *
 * Phase 1 has only the dispatch skeleton.  Each command is logged
 * but only a few have working implementations.  The rest will
 * be wired up as their downstream state effects are needed.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "chat.h"
#include "log.h"
#include "msg.h"
#include "state.h"

static int
prefix(const char *s, const char *p)
{
	size_t pl = strlen(p);

	return strncmp(s, p, pl) == 0 &&
	    (s[pl] == '\0' || s[pl] == ' ');
}

/*
 * Parse a positive integer following a command keyword.  Returns
 * 0 on success and writes *out, -1 on missing or malformed input.
 */
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

void
chat_process(struct Server *s, struct Conn *c, const char *text)
{
	int car_num;

	if (text == NULL || *text == '\0')
		return;

	log_info("CHAT conn=%u: %s", (unsigned)c->conn_id, text);

	/*
	 * /admin <pw> -- elevate the issuer to admin if the
	 * password matches.  Special-cased because every other
	 * /command requires admin already.
	 */
	if (prefix(text, "/admin")) {
		const char *arg = text + 6;

		while (*arg == ' ')
			arg++;
		if (*arg == '\0') {
			log_info("admin: missing password");
			return;
		}
		if (strcmp(arg, s->admin_password) == 0) {
			c->is_admin = 1;
			log_info("admin: conn=%u elevated to admin",
			    (unsigned)c->conn_id);
		} else {
			log_info("admin: wrong password from conn=%u",
			    (unsigned)c->conn_id);
		}
		return;
	}

	/*
	 * Non-admin commands.
	 */
	if (prefix(text, "&swap")) {
		log_info("swap: conn=%u requested driver swap",
		    (unsigned)c->conn_id);
		/* TODO: validate session phase, locate driver, etc. */
		return;
	}

	/*
	 * Everything else from here on is admin-only.
	 */
	if (text[0] != '/') {
		log_info("chat broadcast (TODO): conn=%u: %s",
		    (unsigned)c->conn_id, text);
		return;
	}
	if (!c->is_admin) {
		log_info("admin command rejected (not admin) from conn=%u",
		    (unsigned)c->conn_id);
		return;
	}

	if (prefix(text, "/next")) {
		log_info("admin: /next — advance session (TODO)");
	} else if (prefix(text, "/restart")) {
		log_info("admin: /restart — restart session (TODO)");
	} else if (prefix(text, "/kick")) {
		if (parse_int_arg(text + 5, &car_num) == 0)
			log_info("admin: /kick %d (TODO)", car_num);
	} else if (prefix(text, "/ban")) {
		if (parse_int_arg(text + 4, &car_num) == 0)
			log_info("admin: /ban %d (TODO)", car_num);
	} else if (prefix(text, "/dq")) {
		if (parse_int_arg(text + 3, &car_num) == 0)
			log_info("admin: /dq %d (TODO)", car_num);
	} else if (prefix(text, "/clear_all")) {
		log_info("admin: /clear_all (TODO)");
	} else if (prefix(text, "/clear")) {
		if (parse_int_arg(text + 6, &car_num) == 0)
			log_info("admin: /clear %d (TODO)", car_num);
	} else if (prefix(text, "/cleartp")) {
		if (parse_int_arg(text + 8, &car_num) == 0)
			log_info("admin: /cleartp %d (TODO)", car_num);
	} else if (prefix(text, "/tp5c")) {
		if (parse_int_arg(text + 5, &car_num) == 0)
			log_info("admin: /tp5c %d (TODO)", car_num);
	} else if (prefix(text, "/tp5")) {
		if (parse_int_arg(text + 4, &car_num) == 0)
			log_info("admin: /tp5 %d (TODO)", car_num);
	} else if (prefix(text, "/tp15c")) {
		if (parse_int_arg(text + 6, &car_num) == 0)
			log_info("admin: /tp15c %d (TODO)", car_num);
	} else if (prefix(text, "/tp15")) {
		if (parse_int_arg(text + 5, &car_num) == 0)
			log_info("admin: /tp15 %d (TODO)", car_num);
	} else if (prefix(text, "/dtc")) {
		if (parse_int_arg(text + 4, &car_num) == 0)
			log_info("admin: /dtc %d (TODO)", car_num);
	} else if (prefix(text, "/dt")) {
		if (parse_int_arg(text + 3, &car_num) == 0)
			log_info("admin: /dt %d (TODO)", car_num);
	} else if (prefix(text, "/sg10c")) {
		if (parse_int_arg(text + 6, &car_num) == 0)
			log_info("admin: /sg10c %d (TODO)", car_num);
	} else if (prefix(text, "/sg10")) {
		if (parse_int_arg(text + 5, &car_num) == 0)
			log_info("admin: /sg10 %d (TODO)", car_num);
	} else if (prefix(text, "/sg20c")) {
		if (parse_int_arg(text + 6, &car_num) == 0)
			log_info("admin: /sg20c %d (TODO)", car_num);
	} else if (prefix(text, "/sg20")) {
		if (parse_int_arg(text + 5, &car_num) == 0)
			log_info("admin: /sg20 %d (TODO)", car_num);
	} else if (prefix(text, "/sg30c")) {
		if (parse_int_arg(text + 6, &car_num) == 0)
			log_info("admin: /sg30c %d (TODO)", car_num);
	} else if (prefix(text, "/sg30")) {
		if (parse_int_arg(text + 5, &car_num) == 0)
			log_info("admin: /sg30 %d (TODO)", car_num);
	} else if (prefix(text, "/ballast")) {
		log_info("admin: /ballast (TODO)");
	} else if (prefix(text, "/restrictor")) {
		log_info("admin: /restrictor (TODO)");
	} else if (prefix(text, "/track")) {
		log_info("admin: /track (TODO)");
	} else if (prefix(text, "/manual entrylist")) {
		log_info("admin: /manual entrylist (TODO)");
	} else if (prefix(text, "/controllers")) {
		log_info("admin: /controllers (TODO)");
	} else if (prefix(text, "/controller")) {
		if (parse_int_arg(text + 11, &car_num) == 0)
			log_info("admin: /controller %d (TODO)", car_num);
	} else if (prefix(text, "/connections")) {
		log_info("admin: /connections (TODO)");
	} else if (prefix(text, "/hellban")) {
		log_info("admin: /hellban (TODO)");
	} else if (prefix(text, "/report")) {
		log_info("admin: /report (TODO)");
	} else if (prefix(text, "/latencymode")) {
		log_info("admin: /latencymode (TODO)");
	} else if (prefix(text, "/legacy")) {
		log_info("admin: /legacy netcode (TODO)");
	} else if (prefix(text, "/regular")) {
		log_info("admin: /regular netcode (TODO)");
	} else if (prefix(text, "/debug")) {
		log_info("admin: /debug (TODO)");
	} else {
		log_info("admin: unknown command: %s", text);
	}
}
