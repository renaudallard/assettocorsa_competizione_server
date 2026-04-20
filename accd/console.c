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
 * console.c -- stdin admin console.
 *
 * Provides an interactive command line on the server's terminal.
 * All commands that work via in-game /admin chat also work here;
 * the leading slash is optional.  Replies go to stdout, server
 * logs go to stderr.
 *
 * The console is poll-driven: main.c adds console_fd() to its
 * pollfd set and calls console_handle() when data arrives.
 * A partial-read line buffer handles the case where read(2)
 * returns a fragment.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "chat.h"
#include "console.h"
#include "log.h"
#include "penalty.h"
#include "session.h"
#include "state.h"

/* Set by main.c's signal handler; we set it on "quit". */
extern volatile sig_atomic_t g_stop;

#define CON_LINESZ	512

static int  con_fd = -1;
static char con_buf[CON_LINESZ];
static size_t con_have;

/* ---- helpers --------------------------------------------------- */

static void
reply(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
	fflush(stdout);
}

static void
cmd_help(void)
{
	reply("commands (leading / optional):");
	reply("  help             show this list");
	reply("  status           session phase, connections, tick");
	reply("  show cars        list car slots in use");
	reply("  show conns       list active connections");
	reply("  next             advance to next session");
	reply("  restart          restart current session");
	reply("  start            skip pre-session countdown");
	reply("  resetWeekend     reset to first session + redeliver welcome");
	reply("  kick <num>       kick car by race number");
	reply("  ban <num>        kick + persistent ban");
	reply("  dq <num>         disqualify");
	reply("  tp5 <num>        5s time penalty (tp5c = collision)");
	reply("  tp15 <num>       15s time penalty (tp15c)");
	reply("  dt <num>         drive-through (dtc)");
	reply("  sg10 <num>       10s stop-and-go (sg10c..sg30c)");
	reply("  clear <num>      clear penalties for car");
	reply("  clear_all        clear all penalties");
	reply("  ballast <n> <kg> assign ballast");
	reply("  restrictor <n> %%  assign restrictor");
	reply("  track [name]     show or change track");
	reply("  connections      list connections (also broadcasts)");
	reply("  debug            toggle debug tracing");
	reply("  quit             shut down the server");
}

static void
cmd_status(struct Server *s)
{
	reply("session %u  phase=%s  remaining=%d ms  tick=%llu  conns=%d",
	    (unsigned)s->session.session_index,
	    session_phase_name(s->session.phase),
	    s->session.time_remaining_ms,
	    (unsigned long long)s->tick_count,
	    s->nconns);
}

static void
cmd_show_cars(struct Server *s)
{
	int i, n = 0;

	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		struct CarEntry *car = &s->cars[i];
		if (!car->used)
			continue;
		reply("  car_id=%u  race#=%d  model=%u  drivers=%u  "
		    "ballast=%ukg  pos=%d  laps=%d  best=%dms",
		    (unsigned)car->car_id, car->race_number,
		    (unsigned)car->car_model,
		    (unsigned)car->driver_count,
		    (unsigned)car->ballast_kg,
		    (int)car->race.position,
		    car->race.lap_count,
		    car->race.best_lap_ms);
		n++;
	}
	reply("%d car(s) in use", n);
}

static void
cmd_show_conns(struct Server *s)
{
	int j, n = 0;

	for (j = 0; j < ACC_MAX_CARS; j++) {
		struct Conn *cn = s->conns[j];
		if (cn == NULL)
			continue;
		reply("  conn=%u  car=%d  %s:%u  admin=%d  spectator=%d",
		    (unsigned)cn->conn_id, cn->car_id,
		    inet_ntoa(cn->peer.sin_addr),
		    (unsigned)ntohs(cn->peer.sin_port),
		    cn->is_admin, cn->is_spectator);
		n++;
	}
	reply("%d connection(s)", n);
}

static void
cmd_connections(struct Server *s)
{
	int j;

	chat_broadcast(s, "Active connections:", 4);
	for (j = 0; j < ACC_MAX_CARS; j++) {
		char line[128];
		struct Conn *cn = s->conns[j];
		if (cn == NULL || cn->state != CONN_AUTH)
			continue;
		snprintf(line, sizeof(line),
		    "  conn=%u car=%d", (unsigned)cn->conn_id,
		    cn->car_id);
		chat_broadcast(s, line, 4);
	}
	cmd_show_conns(s);
}

static void
cmd_dq(struct Server *s, const char *args)
{
	int car_num, car_id;
	char msg[128];

	if (chat_parse_int(args, &car_num) < 0) {
		reply("usage: dq <race_number>");
		return;
	}
	car_id = chat_car_by_racenum(s, car_num);
	if (car_id < 0) {
		reply("unknown car #%d", car_num);
		return;
	}
	penalty_enqueue(s, car_id, EXE_DQ, 19, 3, 1, 0,
	    REASON_RACE_CONTROL);
	snprintf(msg, sizeof(msg),
	    "Car #%d was disqualified by Race Control", car_num);
	chat_broadcast(s, msg, 4);
	reply("%s", msg);
}

static void
cmd_clear(struct Server *s, const char *args)
{
	int car_num, car_id;
	char msg[128];

	if (chat_parse_int(args, &car_num) < 0) {
		reply("usage: clear <race_number>");
		return;
	}
	car_id = chat_car_by_racenum(s, car_num);
	if (car_id < 0) {
		reply("unknown car #%d", car_num);
		return;
	}
	penalty_clear(s, car_id);
	snprintf(msg, sizeof(msg),
	    "Pending penalties for #%d cleared by Race Control",
	    car_num);
	chat_broadcast(s, msg, 4);
	reply("%s", msg);
}

static void
cmd_cleartp(struct Server *s, const char *args)
{
	int car_num, car_id;
	char msg[128];

	if (chat_parse_int(args, &car_num) < 0) {
		reply("usage: cleartp <race_number>");
		return;
	}
	car_id = chat_car_by_racenum(s, car_num);
	if (car_id < 0) {
		reply("unknown car #%d", car_num);
		return;
	}
	penalty_clear(s, car_id);
	snprintf(msg, sizeof(msg),
	    "Pending post race time penalties for #%d "
	    "cleared by Race Control", car_num);
	chat_broadcast(s, msg, 4);
	reply("%s", msg);
}

static void
cmd_with_reply(char *buf)
{
	if (buf[0] != '\0')
		reply("%s", buf);
}

/* ---- dispatch -------------------------------------------------- */

static void
console_dispatch(struct Server *s, const char *line)
{
	const char *p = line;
	char rbuf[128];

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\0')
		return;
	/* Strip optional leading /. */
	if (*p == '/')
		p++;

	rbuf[0] = '\0';

	if (chat_prefix(p, "help") || *p == '?') {
		cmd_help();
	} else if (chat_prefix(p, "quit") ||
	    chat_prefix(p, "shutdown")) {
		reply("shutting down");
		g_stop = 1;
	} else if (chat_prefix(p, "status")) {
		cmd_status(s);
	} else if (chat_prefix(p, "show cars")) {
		cmd_show_cars(s);
	} else if (chat_prefix(p, "show conns")) {
		cmd_show_conns(s);
	} else if (chat_prefix(p, "next")) {
		session_advance(s);
		chat_broadcast(s, "Forwarding to next session", 4);
		reply("forwarding to next session");
	} else if (chat_prefix(p, "restart")) {
		session_reset(s, s->session.session_index);
		chat_broadcast(s,
		    "Session restarted by administrator", 4);
		reply("session restarted");
	} else if (chat_prefix(p, "start") || chat_prefix(p, "go")) {
		chat_broadcast(s, "Session started by administrator", 4);
		if (s->session.ts_valid) {
			s->session.ts[0] = 0;
			s->session.ts[1] = 0;
		}
		reply("session started");
	} else if (chat_prefix(p, "resetWeekend") ||
	    chat_prefix(p, "resetweekend")) {
		session_reset(s, 0);
		chat_broadcast(s,
		    "Race weekend reset by administrator", 4);
		chat_weekend_reset_broadcast(s);
		reply("race weekend reset");
	} else if (chat_prefix(p, "kick")) {
		chat_do_kick(s, p + 4, 0, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "ban")) {
		chat_do_kick(s, p + 3, 1, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "dq")) {
		cmd_dq(s, p + 2);
	} else if (chat_prefix(p, "clear_all")) {
		penalty_clear_all(s);
		chat_broadcast(s,
		    "All pending penalties cleared by Race Control", 4);
		reply("all penalties cleared");
	} else if (chat_prefix(p, "cleartp")) {
		cmd_cleartp(s, p + 7);
	} else if (chat_prefix(p, "clear")) {
		cmd_clear(s, p + 5);
	} else if (chat_prefix(p, "tp5c")) {
		chat_do_penalty(s, "tp5c", p + 4, 1, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "tp5")) {
		chat_do_penalty(s, "tp5", p + 3, 0, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "tp15c")) {
		chat_do_penalty(s, "tp15c", p + 5, 1, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "tp15")) {
		chat_do_penalty(s, "tp15", p + 4, 0, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "dtc")) {
		chat_do_penalty(s, "dtc", p + 3, 1, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "dt")) {
		chat_do_penalty(s, "dt", p + 2, 0, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "sg10c")) {
		chat_do_penalty(s, "sg10c", p + 5, 1, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "sg10")) {
		chat_do_penalty(s, "sg10", p + 4, 0, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "sg20c")) {
		chat_do_penalty(s, "sg20c", p + 5, 1, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "sg20")) {
		chat_do_penalty(s, "sg20", p + 4, 0, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "sg30c")) {
		chat_do_penalty(s, "sg30c", p + 5, 1, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "sg30")) {
		chat_do_penalty(s, "sg30", p + 4, 0, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "ballast")) {
		chat_do_bop(s, p + 7, 1, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "restrictor")) {
		chat_do_bop(s, p + 10, 0, rbuf, sizeof(rbuf));
		cmd_with_reply(rbuf);
	} else if (chat_prefix(p, "tracks")) {
		int ti, n = chat_track_count();
		reply("available tracks (%d):", n);
		for (ti = 0; ti < n; ti++)
			reply("  %s", chat_track_name(ti));
	} else if (chat_prefix(p, "track")) {
		const char *targ = p + 5;
		while (*targ == ' ') targ++;
		if (*targ == '\0') {
			int ti, n = chat_track_count();
			reply("current track: %s", s->track);
			reply("available tracks (%d):", n);
			for (ti = 0; ti < n; ti++)
				reply("  %s", chat_track_name(ti));
		} else {
			chat_do_track(s, p + 5, rbuf, sizeof(rbuf));
			cmd_with_reply(rbuf);
		}
	} else if (chat_prefix(p, "connections")) {
		cmd_connections(s);
	} else if (chat_prefix(p, "debug")) {
		g_debug = !g_debug;
		reply("debug tracing %s", g_debug ? "enabled" : "disabled");
	} else if (chat_prefix(p, "legacy")) {
		chat_broadcast(s, "Server now uses legacy netcode", 4);
		reply("legacy netcode enabled");
	} else if (chat_prefix(p, "regular")) {
		chat_broadcast(s, "Server is now in regular mode", 4);
		reply("regular mode enabled");
	} else if (chat_prefix(p, "admin")) {
		reply("console is already admin");
	} else {
		reply("unknown command: %s (type 'help')", p);
	}
}

/* ---- public API ------------------------------------------------ */

void
console_init(void)
{
	if (!isatty(STDIN_FILENO)) {
		log_info("stdin is not a tty, admin console disabled");
		con_fd = -1;
		return;
	}
	con_fd = STDIN_FILENO;
	con_have = 0;
	log_info("admin console enabled (type 'help' for commands)");
}

int
console_fd(void)
{
	return con_fd;
}

void
console_handle(struct Server *s)
{
	ssize_t n;

	if (con_fd < 0)
		return;

	n = read(con_fd, con_buf + con_have,
	    sizeof(con_buf) - con_have - 1);
	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		log_info("console: stdin closed");
		con_fd = -1;
		return;
	}
	con_have += (size_t)n;

	for (;;) {
		char *nl;
		char line[CON_LINESZ];
		size_t llen;

		nl = memchr(con_buf, '\n', con_have);
		if (nl == NULL) {
			if (con_have >= sizeof(con_buf) - 1) {
				log_warn("console: line too long, "
				    "discarding");
				con_have = 0;
			}
			break;
		}
		llen = (size_t)(nl - con_buf);
		if (llen >= sizeof(line))
			llen = sizeof(line) - 1;
		memcpy(line, con_buf, llen);
		line[llen] = '\0';
		/* Strip trailing \r. */
		if (llen > 0 && line[llen - 1] == '\r')
			line[llen - 1] = '\0';

		/* Shift remainder past the newline. */
		con_have -= (size_t)(nl - con_buf) + 1;
		if (con_have > 0)
			memmove(con_buf, nl + 1, con_have);

		console_dispatch(s, line);
	}
}

void
console_close(void)
{
	con_fd = -1;
	con_have = 0;
}
