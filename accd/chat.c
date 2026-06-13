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
#include "handlers.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "penalty.h"
#include "prim.h"
#include "session.h"
#include "state.h"
#include "weather.h"

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
	    wr_str_a(&out, RC_SENDER) == 0 &&
	    wr_str_a(&out, text) == 0 &&
	    wr_i32(&out, 0) == 0 &&
	    wr_u8(&out, chat_type) == 0)
		(void)bcast_all(s, out.data, out.wpos, BCAST_EXCEPT_NONE);
	bb_free(&out);
}

/*
 * Unicast a 0x2b Race-Control reply to a single connection, the
 * requesting admin.  The exe (FUN_140021680) sends diagnostic and
 * config command replies only to the issuing admin's socket: it
 * leaves the broadcast flag clear so the reply goes through the
 * unicast path (FUN_14004cc50), and only /start, /restart, /next and
 * /clear_all (plus the kick/ban announcement) fan out to everyone.
 * Broadcasting diagnostics such as /connections would expose every
 * driver's conn id, car, admin and spectator state to all players.
 */
static void
chat_reply(struct Conn *c, const char *text, uint8_t chat_type)
{
	struct ByteBuf out;

	if (c == NULL || text == NULL || text[0] == '\0')
		return;
	bb_init(&out);
	if (wr_u8(&out, SRV_CHAT_OR_STATE) == 0 &&
	    wr_str_a(&out, RC_SENDER) == 0 &&
	    wr_str_a(&out, text) == 0 &&
	    wr_i32(&out, 0) == 0 &&
	    wr_u8(&out, chat_type) == 0)
		(void)bcast_send_one(c, out.data, out.wpos);
	bb_free(&out);
}

/*
 * Same wire as the player-relay path in handlers.c::h_chat (0x2b
 * with chat_type=0) but the sender label is operator-supplied so
 * the message renders in the in-game chat panel under a fake
 * driver name (e.g. "SERVER", "ADMIN").  Useful for the `say`
 * console command -- Race-Control broadcasts (chat_broadcast
 * above) land in the dedicated overlay lane which most operators
 * expect for system notifications, but operator chatter to
 * drivers needs the regular chat panel.
 */
void
chat_broadcast_as(struct Server *s, const char *sender, const char *text)
{
	struct ByteBuf out;

	if (text == NULL || text[0] == '\0' ||
	    sender == NULL || sender[0] == '\0')
		return;
	bb_init(&out);
	if (wr_u8(&out, SRV_CHAT_OR_STATE) == 0 &&
	    wr_str_a(&out, sender) == 0 &&
	    wr_str_a(&out, text) == 0 &&
	    wr_i32(&out, 0) == 0 &&
	    wr_u8(&out, 0) == 0)
		(void)bcast_all(s, out.data, out.wpos, BCAST_EXCEPT_NONE);
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
		/* The exe clamps ballast to -40..40 kg (signed) at
		 * FUN_14001dae0:452-459 (value >= 41 -> 40, < -40 -> -40). */
		if (value > 40) value = 40;
		if (value < -40) value = -40;
		car->ballast_kg = (int8_t)value;
		snprintf(chat, sizeof(chat),
		    "Assigned %d kg to car #%d", value, car_num);
	} else {
		/* Handbook V: restrictor 0..20 %. */
		if (value > 20) value = 20;
		if (value < 0) value = 0;
		car->restrictor = (float)value / 100.0f;
		snprintf(chat, sizeof(chat),
		    "Assigned %d %% to car #%d", value, car_num);
	}

	/*
	 * Emit 0x53 MultiplayerBOPUpdate to the affected car's own
	 * connection (the exe unicasts it via a single socket send in
	 * FUN_14001dae0, not a broadcast; other clients pick up the new
	 * BoP from the welcome spawnDef on their next join).  Wire body =
	 * u16 car_id, u16 ballast (signed kg), u32 restrictor as an
	 * IEEE-754 float fraction (0.0-0.2 = 0-20 %).  Verified against the
	 * exe serializer FUN_14011d7d0 and client reader FUN_1434f4ba0
	 * (matching offsets +0x28/+0x2c/+0x30) and a kunos pcap probe: the
	 * admin handler FUN_14001dae0 writes ballast to CarEntry+0x1fc (wire
	 * field 2) and the restrictor float to CarEntry+0x200 (wire field 3).
	 * A prior fix had the two fields swapped and mis-typed (restrictor*100
	 * as u16, ballast as u32), so the client read ballast as restrictor
	 * and vice versa.  car->restrictor already holds the fraction, so it
	 * goes out verbatim as a float.
	 */
	bb_init(&out);
	if (wr_u8(&out, SRV_BOP_UPDATE) == 0 &&
	    wr_u16(&out, car->car_id) == 0 &&
	    wr_u16(&out, (uint16_t)car->ballast_kg) == 0 &&
	    wr_f32(&out, car->restrictor) == 0) {
		int j;
		for (j = 0; j < ACC_MAX_CARS; j++) {
			struct Conn *cc = s->conns[j];
			if (cc != NULL && cc->state == CONN_AUTH &&
			    cc->car_id == car_id) {
				bcast_send_one(cc, out.data, out.wpos);
				break;
			}
		}
	}
	bb_free(&out);

	/*
	 * The exe unicasts the "Assigned N kg/% to car #M" confirmation to
	 * the issuing admin only (FUN_140021680: the /ballast //restrictor
	 * reply is routed to the admin's own socket, never broadcast).  We
	 * hand the banner back through `reply`; the in-game caller
	 * (chat_process) unicasts it to the issuing conn and the local
	 * console prints it.  The affected car still gets the 0x53 above.
	 */
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
		struct PenaltyQueue *pq = &s->cars[car_id].race.pen;
		int pre = pq->count;
		int pi;
		if (kind == PEN_TP5)
			val = 5;
		else if (kind == PEN_TP15)
			val = 15;
		if (penalty_enqueue(s, car_id, exe, 8, val, 0,
		    collision, REASON_RACE_CONTROL) < 0)
			return;
		/*
		 * Admin chat-issued penalties are tracked server-side for
		 * race-end conversion + per-session results but kunos does
		 * NOT surface them in the 0x36 active_pen prefix or
		 * pq_emit list (pcap of admin /dt/sg/tp scenarios shows
		 * the per-car record stays at active_pen=0 / pq_emit=0
		 * even though the broadcast chat reports the penalty).
		 * Mark every slot freshly enqueued by this call as `admin`
		 * so write_car_leaderboard_record can skip them.
		 * Use >= so the post-eviction pre==count case is handled
		 * too: when the queue is at ACC_MAX_PENALTIES and the new
		 * push evicts one, pre lands equal to count and a plain >
		 * would leave the freshly-enqueued slot unmarked.
		 */
		if (pre >= pq->count)
			pre = pq->count > 0 ? pq->count - 1 : 0;
		if (pre < 0)
			pre = 0;
		for (pi = pre; pi < pq->count; pi++)
			pq->slots[pi].admin = 1;
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
		    wr_str_a(&out, RC_SENDER) == 0 &&
		    wr_str_a(&out, reason) == 0 &&
		    wr_i32(&out, 0) == 0 &&
		    wr_u8(&out, 5) == 0)
			(void)bcast_send_one(target, out.data, out.wpos);
		bb_free(&out);
	}
	target->state = CONN_DISCONNECT;
	if (car_id >= 0 && car_id < ACC_MAX_CARS) {
		/*
		 * Pick the active driver's steam_id (not slot 0).  In a
		 * multi-driver entrylist entry the current stint may be
		 * drivers[1] / drivers[2]; targeting drivers[0] punishes
		 * the wrong account.  /ban writes to the persistent list
		 * + disk; /kick writes to the ephemeral kicks list
		 * cleared on weekend wrap.
		 */
		struct CarEntry *car = &s->cars[car_id];
		uint8_t di = car->current_driver_index;
		const char *sid;

		if (di >= ACC_MAX_DRIVERS_PER_CAR ||
		    di >= car->driver_count)
			di = 0;
		sid = car->drivers[di].steam_id;

		if (permanent) {
			if (bans_add(&s->bans, sid) == 0) {
				bans_save(&s->bans, s->cfg_dir);
				log_debug("admin: banned steam_id %s", sid);
			}
		} else {
			if (bans_add(&s->kicks, sid) == 0)
				log_debug("admin: kicked steam_id %s", sid);
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
 * Emit one 0x40 weekend-reset broadcast carrying the current weather
 * snapshot.  FUN_14002c740 writes msg_id 0x40, then appends the
 * WeatherData serialize body (vtable slot 0x20 on the WeatherData
 * object, the same call our write_trailer_weather_data mirrors), then
 * fans out to every peer via FUN_14004cc50.
 */
static void
weekend_emit_weather_reset(struct Server *s)
{
	struct ByteBuf wb;

	bb_init(&wb);
	if (wr_u8(&wb, SRV_RACE_WEEKEND_RESET) == 0 &&
	    write_trailer_weather_data(&wb, s) == 0)
		(void)bcast_all(s, wb.data, wb.wpos, BCAST_EXCEPT_NONE);
	bb_free(&wb);
}

/*
 * Common post-reset broadcast, a port of the two-phase weekend reset
 * FUN_14002c740 (reached from FUN_14002aca0).  Shared by /track (new
 * event) and /resetWeekend (same event); the caller has already run
 * session_reset, leaving session.weekend_time_s at the first session.
 *
 * The exe re-draws the weekend weather, broadcasts a 0x40 with the
 * forecast evaluated at "friday night" (weekend time 0), validates it
 * against cfg/weatherRules.json (re-drawing and re-broadcasting until
 * the rules pass or abortSimulationsAfterMs elapses), then advances the
 * clock to the first session and broadcasts a second 0x40.  Finally it
 * snapshots cfg/current and redelivers the welcome trailer (0x4b) to
 * every peer.  With no weatherRules.json the loop runs exactly once,
 * giving the friday-night + first-session pair the stock server sends.
 */
void
chat_weekend_reset_broadcast(struct Server *s)
{
	uint32_t first_session_time = s->session.weekend_time_s;
	uint64_t start_ms = mono_ms();
	int attempt = 0;
	int j;

	log_info("weather: resetting weekend to friday night");

	/* Phase 1: re-draw + friday-night 0x40, with rule retry. */
	for (;;) {
		attempt++;
		weather_redraw(s);
		s->session.weekend_time_s = 0;
		(void)weather_step(s);
		weekend_emit_weather_reset(s);

		if (weather_validate(s))
			break;
		if ((int64_t)(mono_ms() - start_ms) >
		    (int64_t)s->weather_rules.abort_after_ms) {
			log_info("weather: rules unmet after %d draws, "
			    "keeping the last forecast", attempt);
			break;
		}
	}

	/* Phase 2: advance to the first session + final 0x40. */
	s->session.weekend_time_s = first_session_time;
	(void)weather_step(s);
	weekend_emit_weather_reset(s);

	(void)snapshot_cfg_current(s);

	for (j = 0; j < ACC_MAX_CARS; j++) {
		struct Conn *cn = s->conns[j];
		struct ByteBuf bb;

		if (cn == NULL || cn->state != CONN_AUTH || cn->is_smpr)
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
	char norm[512];

	if (text == NULL || *text == '\0')
		return 1;

	/*
	 * Accept § (U+00A7) as an alias for the & command prefix, matching
	 * the exe gate at FUN_140021680:156.  rd_str_a UTF-8-encodes § as
	 * 0xC2 0xA7; rewrite a leading § to '&' into a local copy so the &
	 * matchers below fire.  h_chat keeps the original text for relay and
	 * logging, so a plain § chat message is still relayed verbatim.
	 */
	if ((unsigned char)text[0] == 0xC2 && (unsigned char)text[1] == 0xA7) {
		snprintf(norm, sizeof(norm), "&%s", text + 2);
		text = norm;
	}

	log_info("CHAT conn=%u: %s", (unsigned)c->conn_id, text);

	/* /admin elevation. */
	if (chat_prefix(text, "/admin")) {
		const char *arg = text + 6;
		uint64_t now_ms;

		while (*arg == ' ')
			arg++;
		if (*arg == '\0') {
			log_info("admin: missing password");
			chat_reply(c, "wrong amount of parameters; "
			    "please use /admin pw", 4);
			return 1;
		}
		/*
		 * Per-source-IP rate limit: one attempt per second per
		 * remote IP.  Without keying on the IP an attacker can
		 * drop the conn, reconnect (calloc gives a fresh Conn
		 * with last_admin_attempt_ms=0), and try again -- the
		 * per-conn variant from commit 5d4f1be is bypassable
		 * once Conn is recycled.  The Server-level admin_retry
		 * table outlives any individual conn so the cooldown
		 * stays in force across reconnects.
		 */
		now_ms = mono_ms();
		{
			uint32_t ip = c->peer.sin_addr.s_addr;
			size_t n = sizeof(s->admin_retry) /
			    sizeof(s->admin_retry[0]);
			size_t i, slot = n;
			uint64_t oldest = UINT64_MAX;
			size_t oldest_idx = 0;

			for (i = 0; i < n; i++) {
				if (s->admin_retry[i].ip == ip) {
					slot = i;
					break;
				}
				if (s->admin_retry[i].last_ms < oldest) {
					oldest = s->admin_retry[i].last_ms;
					oldest_idx = i;
				}
			}
			if (slot == n)
				slot = oldest_idx;
			if (s->admin_retry[slot].ip == ip &&
			    s->admin_retry[slot].last_ms != 0 &&
			    now_ms - s->admin_retry[slot].last_ms < 1000) {
				log_info("admin: ip throttled (conn=%u)",
				    (unsigned)c->conn_id);
				return 1;
			}
			s->admin_retry[slot].ip = ip;
			s->admin_retry[slot].last_ms = now_ms;
		}
		c->last_admin_attempt_ms = now_ms;
		if (strcmp(arg, s->admin_password) == 0) {
			struct ByteBuf out;
			c->is_admin = 1;
			/*
			 * Unicast the elevation reply only to the requesting
			 * conn, matching the exe's FUN_140021680 admin path
			 * (calls FUN_14004cc50 unicast).  chat_broadcast
			 * here would announce "You are now server admin" to
			 * every connected client, exposing the elevation.
			 */
			bb_init(&out);
			if (wr_u8(&out, SRV_CHAT_OR_STATE) == 0 &&
			    wr_str_a(&out, RC_SENDER) == 0 &&
			    wr_str_a(&out, "You are now server admin") == 0 &&
			    wr_i32(&out, 0) == 0 &&
			    wr_u8(&out, 4) == 0)
				(void)bcast_send_one(c, out.data, out.wpos);
			bb_free(&out);
			log_info("admin: conn=%u elevated to admin",
			    (unsigned)c->conn_id);
		} else {
			log_info("admin: wrong password from conn=%u",
			    (unsigned)c->conn_id);
			chat_reply(c, "Wrong password", 4);
		}
		return 1;
	}

	/* &swap <index> (driver swap, non-admin). */
	if (chat_prefix(text, "&swap")) {
		const char *arg = text + 5;
		int target;
		struct CarEntry *car;
		uint8_t cur_type;

		if (c->car_id < 0) {
			log_info("swap: conn=%u has no car",
			    (unsigned)c->conn_id);
			return 1;
		}
		car = &s->cars[c->car_id];
		/*
		 * Match exe FUN_140027990:88-94 gating: only allow &swap
		 * during Practice (session_type=0) or Qualifying (=4),
		 * and only while the car is in the pit lane (race
		 * sessions use the conventional pit-stop swap path
		 * driven by 0x48 / mandatory pitstop, not the chat
		 * command).  The exe's pit gate reads byte +0x153 of
		 * the carEntry which corresponds to our `on_track`
		 * field (= 2 when in pit lane).  We mirror with
		 * race->in_pit which is the same physical state.
		 */
		cur_type = session_cur_type(s);
		if (cur_type != 0 && cur_type != 4) {
			log_info("swap: conn=%u rejected — session_type=%u "
			    "(only P/Q allowed)",
			    (unsigned)c->conn_id, (unsigned)cur_type);
			return 1;
		}
		if (!car->race.in_pit) {
			log_info("swap: conn=%u rejected — car not in pit lane",
			    (unsigned)c->conn_id);
			return 1;
		}
		/*
		 * The &swap argument is 1-based on the stock server (its
		 * reject says "use 1, 2, 3"; exe FUN_140027990:99 does
		 * local_c0 - 1), so decrement to the 0-based internal driver
		 * index.  --target is only evaluated once chat_parse_int has
		 * succeeded, so a non-numeric arg still takes the invalid path.
		 */
		if (chat_parse_int(arg, &target) < 0 ||
		    --target < 0 || target >= car->driver_count) {
			log_info("swap: invalid target from conn=%u "
			    "(use 1, 2, 3 dependent on your team)",
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

		/*
		 * Broadcast the updated swap state via the shared helper
		 * instead of open-coding 0x47 inline.  Two reasons:
		 *   * broadcast_swap_state honors do_driver_swap_broadcast
		 *     (settings.json knob); the open-coded version
		 *     bypassed it.
		 *   * For multi-driver team_entry_id groups the caller
		 *     loops over team mates -- not a concern here since
		 *     &swap targets a driver in the SAME car, but using
		 *     the shared path keeps the wire shape canonical.
		 */
		broadcast_swap_state(s, car);

		/*
		 * Acknowledge the handover request back to the sender
		 * with SRV_DRIVER_HANDOVER_REQ (0x59).  Matches
		 * FUN_140027990 in accServer.exe (the &swap chat
		 * handler); that function in turn calls FUN_140020380
		 * to enumerate team-mate slots and emits one 0x59
		 * unicast per teammate.  accd's &swap targets a driver
		 * in the SAME car (single CarEntry, multi-driver), so
		 * the team-mate iteration collapses to a single 0x59
		 * back to the requester; the 4-byte body carries the
		 * source car_id and the 0-based driver index (the 1-based
		 * &swap argument minus one) of the driver who will take
		 * over.  Clients use this to
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

	/*
	 * &delta <default|error|diff>: netcar latency display mode, matching
	 * the exe (FUN_140021680).  Driver-open and conn-local; accd stores
	 * the mode but has no netcar latency display that consumes it, so the
	 * effect is the confirmation reply only.
	 */
	if (chat_prefix(text, "&delta")) {
		const char *arg = text + 6;

		while (*arg == ' ')
			arg++;
		if (strcmp(arg, "default") == 0) {
			c->netcar_delta_mode = 0;
			chat_reply(c, "Showing regular laptime delta for "
			    "netcars", 4);
		} else if (strcmp(arg, "error") == 0) {
			c->netcar_delta_mode = 1;
			chat_reply(c, "Showing (corrected) latency errors "
			    "for netcars", 4);
		} else if (strcmp(arg, "diff") == 0) {
			c->netcar_delta_mode = 2;
			chat_reply(c, "Showing difference between legacy "
			    "and logstep latency", 4);
		} else {
			chat_reply(c, "please set the mode to use: default, "
			    "error or diff", 4);
		}
		return 1;
	}

	/*
	 * &formation: per-car formation state dump (diagnostic), matching the
	 * exe (FUN_14001be10).  Driver-open.  Reports each car's start state:
	 * lead (race leader), Track (on the racing surface) or !Track.
	 */
	if (chat_prefix(text, "&formation")) {
		char line[256];
		size_t off;
		int i, wrote;

		wrote = snprintf(line, sizeof(line), "Starts:");
		off = (wrote > 0) ? (size_t)wrote : 0;
		for (i = 0; i < ACC_MAX_CARS && off < sizeof(line) - 1; i++) {
			struct CarEntry *car = &s->cars[i];
			const char *st;

			if (!car->used)
				continue;
			if (car->race.position == 1)
				st = "lead";
			else if (car->race.on_track)
				st = "Track";
			else
				st = "!Track";
			wrote = snprintf(line + off, sizeof(line) - off,
			    " #%d%s:%s", ACC_CAR_ID_BASE + i,
			    (i == c->car_id) ? "(you)" : "", st);
			if (wrote < 0)
				break;
			off += (size_t)wrote;
			if (off >= sizeof(line)) {
				off = sizeof(line) - 1;
				break;
			}
		}
		chat_reply(c, line, 4);
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
			chat_reply(c, s->log_conditions
			    ? "conditions are printed now"
			    : "conditions stopped printing", 4);
		} else if (strcmp(arg, "bandwidth") == 0) {
			s->log_bandwidth = !s->log_bandwidth;
			chat_reply(c, s->log_bandwidth
			    ? "bandwidth stats are printed now"
			    : "bandwidth stats stopped printing", 4);
		} else if (strcmp(arg, "qos") == 0) {
			s->log_qos = !s->log_qos;
			chat_reply(c, s->log_qos
			    ? "netcode stats are printed now"
			    : "netcode stats stopped printing", 4);
		} else if (*arg == '\0') {
			chat_reply(c, "missing parameter", 4);
		} else {
			chat_reply(c, "unknown debug request", 4);
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
		chat_reply(c, msg, 4);
	} else if (chat_prefix(text, "/broadcast") ||
	           chat_prefix(text, "/say") ||
	           chat_prefix(text, "/announce")) {
		/*
		 * Admin-only system broadcast.  Pushes the operator's
		 * text to every client as a 0x2b "Race Control" message
		 * (the same wire used internally for penalty / BoP /
		 * weekend-reset notifications).  Useful for race-start
		 * countdowns, pit-window reminders, or manual incident
		 * announcements when the operator isn't in-car.
		 *
		 * Length cap: wr_str_a truncates at 255 UTF-8 codepoints
		 * (ksstr u8 length prefix), so longer payloads simply
		 * get clipped rather than failing the broadcast.
		 */
		const char *arg = text;
		while (*arg != '\0' && *arg != ' ')
			arg++;
		while (*arg == ' ')
			arg++;
		if (*arg == '\0') {
			chat_broadcast(s, "usage: /broadcast <message>", 4);
		} else {
			log_info("admin: /broadcast %s", arg);
			chat_broadcast(s, arg, 4);
		}
	} else if (chat_prefix(text, "/go") ||
	    chat_prefix(text, "/start")) {
		/*
		 * /go (our legacy name) and /start (the exe's name):
		 * cut the pre-session wait and start the active
		 * session now.  Matches FUN_14012f290 in accServer.exe,
		 * which advances the session manager's next-start gate.
		 * For us: if we're in WAITING or FORMATION, collapse
		 * ts[0]..ts[1] so the next tick transitions us into
		 * PRE_SESSION immediately.  Later boundaries are not
		 * shifted — the active session still gets its full
		 * duration from ts[2] onwards.
		 */
		log_info("admin: /start");
		chat_broadcast(s, "Session started by administrator", 4);
		if (s->session.ts_valid) {
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
				struct PenaltyQueue *pq =
				    &s->cars[car_id].race.pen;
				int pre = pq->count;
				int pi;
				/*
		 * category here is the AC2 cat enum (0..17) used to index
		 * pen_cat_severity[18] in penalty_enqueue's dedup gate;
		 * passing the wire-value 19 silently bypasses that gate
		 * (19 >= sizeof(pen_cat_severity)) so repeated /dq spam
		 * piled duplicate entries into the 8-slot queue, evicting
		 * legitimate DT/SG history.  Use 8 (Trolling/RaceControl
		 * — kunos's penalty.c:249 fallback for non-forced DQ).
		 */
		penalty_enqueue(s, car_id, EXE_DQ, 8, 3,
				    1, 0, REASON_RACE_CONTROL);
				/*
				 * Mark the freshly enqueued DQ slot(s) as
				 * admin so 0x36's active_pen / pq_emit skip
				 * them — kunos doesn't surface admin-issued
				 * DQ in the per-car prefix.  See
				 * chat_do_penalty for the same pattern.
				 */
				/* See handlers.c h_report_penalty -- same
				 * `pre == count` post-eviction case. */
				if (pre >= pq->count)
					pre = pq->count > 0 ? pq->count - 1 : 0;
				if (pre < 0)
					pre = 0;
				for (pi = pre; pi < pq->count; pi++)
					pq->slots[pi].admin = 1;
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

				penalty_clear_tp(s, car_id);
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
	} else if (chat_prefix(text, "/sg10")) {
		/*
		 * SG is always the collision variant, matching the exe admin
		 * command (FUN_14001dae0: bare /sg10,/sg20,/sg30 unconditionally
		 * append "causing a collision").  There is no non-collision
		 * /sgNN or accd-only /sgNNc token; the collision PEN_SG*C kinds
		 * still serve client 0x41 reports and the penalty ladder.
		 */
		chat_do_penalty(s, "sg10c", text + 5, 1, NULL, 0);
	} else if (chat_prefix(text, "/sg20")) {
		chat_do_penalty(s, "sg20c", text + 5, 1, NULL, 0);
	} else if (chat_prefix(text, "/sg30")) {
		chat_do_penalty(s, "sg30c", text + 5, 1, NULL, 0);
	} else if (chat_prefix(text, "/ballast")) {
		char rb[128] = "";
		chat_do_bop(s, text + 8, 1, rb, sizeof(rb));
		chat_reply(c, rb, 4);	/* unicast to the issuing admin */
	} else if (chat_prefix(text, "/restrictor")) {
		char rb[128] = "";
		chat_do_bop(s, text + 11, 0, rb, sizeof(rb));
		chat_reply(c, rb, 4);
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
			chat_reply(c,
			    "Entry list cannot be saved on public servers",
			    4);
		} else {
			if (entrylist_save(s, s->cfg_dir) == 0) {
				chat_reply(c,
				    "Saved entry list to cfg/entrylist.json",
				    4);
			} else {
				chat_reply(c,
				    "Failed to save entry list", 4);
			}
		}
		log_info("admin: /manual entrylist");
	} else if (chat_prefix(text, "/manual start")) {
		chat_reply(c,
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

			if (cn == NULL || cn->state != CONN_AUTH ||
			    cn->is_smpr)
				continue;
			if (conn_send_framed(cn, &probe, 1) == 0)
				sent++;
		}
		{
			char line[64];
			snprintf(line, sizeof(line),
			    "Requesting controllers for %d clients", sent);
			chat_reply(c, line, 4);
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
				    "Requested controller info for car #%d",
				    car_num);
				chat_reply(c, line, 4);
				log_info("admin: /controller %d -> probe",
				    car_num);
			} else {
				char line[64];
				snprintf(line, sizeof(line),
				    "Couldn't locate connection for car #%d",
				    car_num);
				chat_reply(c, line, 4);
			}
		}
	} else if (chat_prefix(text, "/connections")) {
		int j;
		chat_reply(c, "Active connections:", 4);
		for (j = 0; j < ACC_MAX_CARS; j++) {
			char line[128];
			struct Conn *cn = s->conns[j];
			if (cn == NULL || cn->state != CONN_AUTH)
				continue;
			snprintf(line, sizeof(line),
			    "  conn=%u car=%d%s%s",
			    (unsigned)cn->conn_id, cn->car_id,
			    cn->is_admin ? " [admin]" : "",
			    cn->is_spectator ? " [spectator]" : "");
			chat_reply(c, line, 4);
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
				chat_reply(c, line, 4);
				log_info("admin: /hellban %d (conn=%u)",
				    car_num, (unsigned)cn->conn_id);
			} else {
				char line[80];
				snprintf(line, sizeof(line),
				    "Couldn't locate connection for car #%d",
				    car_num);
				chat_reply(c, line, 4);
			}
		}
	} else if (chat_prefix(text, "/latencymode")) {
		int mode;
		char line[96];

		if (chat_parse_int(text + 12, &mode) < 0) {
			chat_reply(c, "wrong parameters, please use "
			    "'latencymode n' (with n between 0 and 1)", 4);
		} else if (mode >= 2) {
			snprintf(line, sizeof(line),
			    "unknown latency mode %d", mode);
			chat_reply(c, line, 4);
		} else {
			s->latency_mode = (uint8_t)mode;
			snprintf(line, sizeof(line), "Latency mode: %d", mode);
			chat_reply(c, line, 4);
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
		chat_reply(c, s->legacy_netcode
		    ? "Server now uses legacy netcode"
		    : "Server is now in regular mode", 4);
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
