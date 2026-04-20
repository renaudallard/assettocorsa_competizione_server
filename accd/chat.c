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

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "bans.h"
#include "bcast.h"
#include "chat.h"
#include "entrylist.h"
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

	/*
	 * Emit 0x53 MultiplayerBOPUpdate broadcast.  FUN_14011d7d0
	 * writes (u16 car_id, u16 restrictor_pct, u32 ballast_kg) —
	 * in that order, with restrictor before ballast and ballast
	 * as a u32, not a float.  We had the last two fields flipped
	 * and typed wrong; the client was reading our ballast bytes
	 * as restrictor (and vice versa) and rendering ballast as a
	 * float — every admin BoP edit showed a nonsense value.
	 */
	bb_init(&out);
	if (wr_u8(&out, SRV_BOP_UPDATE) == 0 &&
	    wr_u16(&out, car->car_id) == 0 &&
	    wr_u16(&out,
		(uint16_t)(car->restrictor * 100.0f + 0.5f)) == 0 &&
	    wr_u32(&out, (uint32_t)car->ballast_kg) == 0)
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

/*
 * Copy cfg/<name>.json to cfg/current/<name>.txt, matching the
 * snapshot accServer.exe writes from FUN_14002aca0 on weekend reset.
 * Missing source files are skipped silently.  Returns 0 on success,
 * -1 only if we could not create the destination directory.
 */
static int
snapshot_cfg_current(const struct Server *s)
{
	static const char *names[] = {
		"configuration", "event", "settings",
		"entrylist", "eventRules", NULL
	};
	char cur_dir[320];
	int i;

	snprintf(cur_dir, sizeof(cur_dir), "%s/current", s->cfg_dir);
	if (mkdir(cur_dir, 0755) < 0 && errno != EEXIST) {
		log_warn("snapshot_cfg_current: mkdir %s: %s",
		    cur_dir, strerror(errno));
		return -1;
	}
	for (i = 0; names[i] != NULL; i++) {
		char src[448], dst[448], buf[8192];
		FILE *fp_src, *fp_dst;
		size_t n;

		snprintf(src, sizeof(src), "%s/%s.json",
		    s->cfg_dir, names[i]);
		snprintf(dst, sizeof(dst), "%s/%s.txt",
		    cur_dir, names[i]);
		fp_src = fopen(src, "rb");
		if (fp_src == NULL)
			continue;
		fp_dst = fopen(dst, "wb");
		if (fp_dst == NULL) {
			fclose(fp_src);
			log_warn("snapshot_cfg_current: open %s: %s",
			    dst, strerror(errno));
			continue;
		}
		{
			int copy_ok = 1;
			while ((n = fread(buf, 1, sizeof(buf), fp_src)) > 0) {
				if (fwrite(buf, 1, n, fp_dst) != n) {
					log_warn("snapshot_cfg_current: "
					    "short write to %s: %s",
					    dst, strerror(errno));
					copy_ok = 0;
					break;
				}
			}
			fclose(fp_src);
			if (fclose(fp_dst) != 0 && copy_ok)
				log_warn("snapshot_cfg_current: close %s: %s",
				    dst, strerror(errno));
		}
	}
	log_info("snapshot_cfg_current: wrote files under cfg/current/");
	return 0;
}

/*
 * Common post-reset broadcast: cfg snapshot, 0x40 weekend-reset with
 * weather body, and 0x4b welcome-trailer redelivery to every peer.
 * Shared by /track (new event) and /resetWeekend (same event).
 */
void
chat_weekend_reset_broadcast(struct Server *s)
{
	int j;

	(void)snapshot_cfg_current(s);

	/*
	 * 0x40 weekend-reset broadcast.  FUN_14002c740 line 242 writes msg_id
	 * 0x40, then appends the WeatherData serialize body (vtable slot 0x20
	 * on the WeatherData object — same call our write_trailer_weather_data
	 * mirrors), then fans out to every peer via FUN_14004cc50.
	 */
	{
		struct ByteBuf wb;
		bb_init(&wb);
		if (wr_u8(&wb, SRV_RACE_WEEKEND_RESET) == 0 &&
		    write_trailer_weather_data(&wb, s) == 0)
			(void)bcast_all(s, wb.data, wb.wpos, 0xFFFF);
		bb_free(&wb);
	}

	for (j = 0; j < ACC_MAX_CARS; j++) {
		struct Conn *cn = s->conns[j];
		struct ByteBuf bb;

		if (cn == NULL || cn->state != CONN_AUTH)
			continue;

		bb_init(&bb);
		if (wr_u8(&bb, SRV_WELCOME_REDELIVERY) == 0 &&
		    build_welcome_trailer(&bb, s, cn) == 0)
			(void)conn_send_framed(cn, bb.data, bb.wpos);
		bb_free(&bb);
	}
}

void
chat_do_track(struct Server *s, const char *args,
    char *reply, size_t replysz)
{
	const char *name;
	char msg[128];

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

	snprintf(s->track, sizeof(s->track), "%s", name);
	session_reset(s, 0);
	chat_weekend_reset_broadcast(s);

	snprintf(msg, sizeof(msg), "Event changed to %s", s->track);
	chat_broadcast(s, msg, 4);
	log_info("admin: %s", msg);

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

	/*
	 * /report is the only slash command any driver may issue —
	 * accServer.exe gates every other slash command on is_admin
	 * but lets "/report" through unauthenticated.  Admins may also
	 * use it.  Append to cfg/reports.txt for later review.
	 */
	if (chat_prefix(text, "/report")) {
		const char *arg = text + 7;
		char path[320];
		FILE *fp;
		time_t now = time(NULL);
		struct tm tm_buf;
		char ts[32];

		while (*arg == ' ')
			arg++;
		(void)localtime_r(&now, &tm_buf);
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
		snprintf(path, sizeof(path), "%s/reports.txt", s->cfg_dir);
		fp = fopen(path, "a");
		if (fp != NULL) {
			fprintf(fp, "%s conn=%u car=%d %s: %s\n",
			    ts, (unsigned)c->conn_id, c->car_id,
			    c->is_admin ? "admin" : "driver", arg);
			fclose(fp);
		}
		log_info("report %s conn=%u car=%d: %s",
		    c->is_admin ? "(admin)" : "(driver)",
		    (unsigned)c->conn_id, c->car_id, arg);
		return 1;
	}

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
	} else if (chat_prefix(text, "/debug")) {
		/*
		 * /debug <sub>: toggle a server-side log-verbosity flag.
		 * accServer.exe recognizes 'conditions' (+0x116),
		 * 'bandwidth' (+0x114), and 'qos' (+0x117) — server-local
		 * verbosity only, no wire impact.  Each toggle replies
		 * '<name> stats are printed now' or '... stopped printing'.
		 */
		const char *arg = text + 6;
		while (*arg == ' ')
			arg++;
		if (strcmp(arg, "conditions") == 0) {
			s->log_conditions = !s->log_conditions;
			chat_broadcast(s, s->log_conditions
			    ? "conditions are printed now"
			    : "conditions stopped printing", 4);
		} else if (strcmp(arg, "bandwidth") == 0) {
			s->log_bandwidth = !s->log_bandwidth;
			chat_broadcast(s, s->log_bandwidth
			    ? "bandwidth stats are printed now"
			    : "bandwidth stats stopped printing", 4);
		} else if (strcmp(arg, "qos") == 0) {
			s->log_qos = !s->log_qos;
			chat_broadcast(s, s->log_qos
			    ? "netcode stats are printed now"
			    : "netcode stats stopped printing", 4);
		} else if (*arg == '\0') {
			chat_broadcast(s, "missing parameter", 4);
		} else {
			chat_broadcast(s, "unknown debug request", 4);
		}
		log_info("admin: /debug %s", arg);
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
	} else if (chat_prefix(text, "/resetWeekend") ||
	           chat_prefix(text, "/resetweekend")) {
		log_info("admin: /resetWeekend");
		chat_broadcast(s,
		    "Race weekend reset by administrator", 4);
		session_reset(s, 0);
		chat_weekend_reset_broadcast(s);
	} else if (chat_prefix(text, "/kick")) {
		chat_do_kick(s, text + 5, 0, NULL, 0);
	} else if (chat_prefix(text, "/ban")) {
		chat_do_kick(s, text + 4, 1, NULL, 0);
	} else if (chat_prefix(text, "/dq")) {
		if (chat_parse_int(text + 3, &car_num) == 0) {
			int car_id = chat_car_by_racenum(s,car_num);
			if (car_id >= 0) {
				penalty_enqueue(s, car_id, EXE_DQ, 19, 3,
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
		/*
		 * accServer.exe rejects this on "public servers"
		 * (register_to_lobby = 1) via FUN_140025170.  Match that
		 * posture: a public lobby server should not overwrite
		 * its curated entrylist.json from a live session.
		 */
		if (s->register_to_lobby) {
			chat_broadcast(s,
			    "Entry list cannot be saved on public servers",
			    4);
		} else {
			if (entrylist_save(s, s->cfg_dir) == 0) {
				chat_broadcast(s,
				    "Saved entry list to cfg/entrylist.json",
				    4);
			} else {
				chat_broadcast(s,
				    "Failed to save entry list", 4);
			}
		}
		log_info("admin: /manual entrylist");
	} else if (chat_prefix(text, "/manual start")) {
		chat_broadcast(s,
		    "This cmd was replaced by the formationLapType setting",
		    4);
	} else if (chat_prefix(text, "/controllers")) {
		/*
		 * Send a 1-byte 0x5b probe to every authenticated
		 * connection.  accServer.exe iterates its car list and
		 * calls FUN_14004cc50 with a single-byte 0x5b payload
		 * for each one that has a live connection; the client
		 * replies with ACP_CTRL_INFO carrying its assist / car
		 * config, which h_ctrl_info then forwards as 0x2b chat
		 * to the requesting admin.
		 */
		int j, sent = 0;
		for (j = 0; j < ACC_MAX_CARS; j++) {
			struct Conn *cn = s->conns[j];
			uint8_t probe = SRV_CTRL_INFO_REQUEST;

			if (cn == NULL || cn->state != CONN_AUTH)
				continue;
			if (conn_send_framed(cn, &probe, 1) == 0)
				sent++;
		}
		{
			char line[64];
			snprintf(line, sizeof(line),
			    "Requesting controllers for %d clients", sent);
			chat_broadcast(s, line, 4);
			log_info("admin: /controllers -> %d probes", sent);
		}
	} else if (chat_prefix(text, "/controller")) {
		if (chat_parse_int(text + 11, &car_num) == 0) {
			int car_id = chat_car_by_racenum(s, car_num);
			struct Conn *cn = NULL;
			int j;

			if (car_id >= 0)
				for (j = 0; j < ACC_MAX_CARS; j++)
					if (s->conns[j] != NULL &&
					    s->conns[j]->car_id == car_id) {
						cn = s->conns[j];
						break;
					}
			if (cn != NULL) {
				uint8_t probe = SRV_CTRL_INFO_REQUEST;
				char line[64];

				(void)conn_send_framed(cn, &probe, 1);
				snprintf(line, sizeof(line),
				    "Requesting controller for car #%d",
				    car_num);
				chat_broadcast(s, line, 4);
				log_info("admin: /controller %d -> probe",
				    car_num);
			} else {
				char line[64];
				snprintf(line, sizeof(line),
				    "Couldn't locate connection for car #%d",
				    car_num);
				chat_broadcast(s, line, 4);
			}
		}
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
		if (chat_parse_int(text + 8, &car_num) == 0) {
			int car_id = chat_car_by_racenum(s, car_num);
			struct Conn *cn = NULL;
			int j;

			if (car_id >= 0)
				for (j = 0; j < ACC_MAX_CARS; j++)
					if (s->conns[j] != NULL &&
					    s->conns[j]->car_id == car_id) {
						cn = s->conns[j];
						break;
					}
			if (cn != NULL) {
				char line[80];

				cn->hellbanned = 1;
				snprintf(line, sizeof(line),
				    "Car #%d has been hellbanned", car_num);
				chat_broadcast(s, line, 4);
				log_info("admin: /hellban %d (conn=%u)",
				    car_num, (unsigned)cn->conn_id);
			} else {
				char line[80];
				snprintf(line, sizeof(line),
				    "Couldn't locate connection for car #%d",
				    car_num);
				chat_broadcast(s, line, 4);
			}
		}
	} else if (chat_prefix(text, "/latencymode")) {
		int mode;
		char line[96];

		if (chat_parse_int(text + 12, &mode) < 0) {
			chat_broadcast(s, "wrong parameters, please use "
			    "'latencymode n' (with n between 0 and 1)", 4);
		} else if (mode >= 2) {
			snprintf(line, sizeof(line),
			    "unknown latency mode %d", mode);
			chat_broadcast(s, line, 4);
		} else {
			s->latency_mode = (uint8_t)mode;
			snprintf(line, sizeof(line), "Latency mode: %d", mode);
			chat_broadcast(s, line, 4);
			log_info("admin: /latencymode %d", mode);
		}
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
	} else if (chat_prefix(text, "/lockprep")) {
		s->preparation_locked = 1;
		chat_broadcast(s, "Preparation phase is now LOCKED — no "
		    "new drivers until unlock", 4);
		log_info("admin: /lockprep");
	} else if (chat_prefix(text, "/unlockprep")) {
		s->preparation_locked = 0;
		chat_broadcast(s, "Preparation phase is now OPEN", 4);
		log_info("admin: /unlockprep");
	} else {
		log_info("admin: unknown command: %s", text);
	}
	return 1;
}
