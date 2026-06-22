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
 * config.c -- configuration file readers.
 *
 * The default ACC server ships its configs as UTF-16 LE JSON.
 * We read each file, convert to UTF-8 with iconv, then run a
 * minimal extractor that pulls flat top-level int and string
 * values out of the result.  No nested objects or arrays in
 * phase 1 — the entry list / event rules are read by separate
 * helpers when we get to phase 3.
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "entrylist.h"
#include "json.h"
#include "log.h"
#include "state.h"
#include "weather.h"

#define CFG_MAX_SIZE	(1u << 20)

static char *
read_file(const char *path, size_t *outlen)
{
	int fd;
	struct stat st;
	char *buf;
	ssize_t n;
	size_t off = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) < 0) {
		close(fd);
		return NULL;
	}
	if (st.st_size <= 0 || st.st_size > (off_t)CFG_MAX_SIZE) {
		close(fd);
		errno = EFBIG;
		return NULL;
	}
	buf = malloc((size_t)st.st_size);
	if (buf == NULL) {
		close(fd);
		return NULL;
	}
	while (off < (size_t)st.st_size) {
		n = read(fd, buf + off, (size_t)st.st_size - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			close(fd);
			return NULL;
		}
		if (n == 0)
			break;
		off += (size_t)n;
	}
	close(fd);
	*outlen = off;
	return buf;
}

/*
 * Decode a raw config file into a NUL-terminated UTF-8 string.
 *
 * Kunos's accServer.exe writes its configs as UTF-16 LE with a BOM
 * (ff fe).  We accept that encoding for compatibility with the
 * stock tooling, but we also accept plain UTF-8 (with or without a
 * BOM) so the files can be edited by hand in any ordinary text
 * editor on Linux / OpenBSD.  Detection is by BOM sniffing; in the
 * absence of a UTF-16 LE BOM the bytes are returned verbatim.
 */
static char *
decode_cfg_bytes(const char *in, size_t inlen)
{
	iconv_t cd;
	char *out, *outp;
	const char *inp;
	size_t outbufsz, inrem, outrem;

	/* Optional UTF-8 BOM -- strip it. */
	if (inlen >= 3 &&
	    (unsigned char)in[0] == 0xef &&
	    (unsigned char)in[1] == 0xbb &&
	    (unsigned char)in[2] == 0xbf) {
		in += 3;
		inlen -= 3;
	}

	/* No UTF-16 LE BOM? Treat as UTF-8 / ASCII and return a copy. */
	if (inlen < 2 ||
	    (unsigned char)in[0] != 0xff ||
	    (unsigned char)in[1] != 0xfe) {
		out = malloc(inlen + 1);
		if (out == NULL)
			return NULL;
		if (inlen > 0)
			memcpy(out, in, inlen);
		out[inlen] = '\0';
		return out;
	}

	/* UTF-16 LE with BOM -- skip the BOM, transcode via iconv. */
	in += 2;
	inlen -= 2;

	cd = iconv_open("UTF-8", "UTF-16LE");
	if (cd == (iconv_t)-1)
		return NULL;
	outbufsz = inlen * 2 + 1;
	out = malloc(outbufsz);
	if (out == NULL) {
		iconv_close(cd);
		return NULL;
	}
	inp = in;
	inrem = inlen;
	outp = out;
	outrem = outbufsz - 1;
	if (iconv(cd, (char **)&inp, &inrem, &outp, &outrem) == (size_t)-1) {
		free(out);
		iconv_close(cd);
		return NULL;
	}
	*outp = '\0';
	iconv_close(cd);
	return out;
}

static void
copy_str(char *dst, size_t dstsz, const char *src)
{
	if (src == NULL || dstsz == 0)
		return;
	strncpy(dst, src, dstsz - 1);
	dst[dstsz - 1] = '\0';
}

static char *
load_one(const char *cfg_dir, const char *name)
{
	char path[512];
	char *raw, *utf8;
	size_t rawlen;

	snprintf(path, sizeof(path), "%s/%s", cfg_dir, name);
	raw = read_file(path, &rawlen);
	if (raw == NULL) {
		log_warn("config: cannot read %s: %s", path, strerror(errno));
		return NULL;
	}
	utf8 = decode_cfg_bytes(raw, rawlen);
	free(raw);
	if (utf8 == NULL)
		log_warn("config: decode failed for %s", path);
	return utf8;
}

/*
 * Parse a JSON file under cfg_dir and return the root node.
 * Caller must free with json_free().  Returns NULL on error.
 */
static struct json_node *
load_json(const char *cfg_dir, const char *name)
{
	char *utf8 = load_one(cfg_dir, name);
	struct json_node *root;
	char err[256] = "";
	size_t len;

	if (utf8 == NULL)
		return NULL;
	len = strlen(utf8);
	root = json_parse(utf8, len, err, sizeof(err));
	if (root == NULL)
		log_warn("config: parse %s: %s", name, err);
	free(utf8);
	return root;
}

int
config_load(struct Server *s, const char *cfg_dir)
{
	struct json_node *configuration, *settings, *event;

	/* defaults */
	s->tcp_port = 9232;
	s->udp_port = 9231;
	s->max_connections = ACC_MAX_CARS;
	s->max_car_slots = 10;
	s->lan_discovery = 1;
	s->password[0] = '\0';
	s->admin_password[0] = '\0';
	s->spectator_password[0] = '\0';
	snprintf(s->server_name, sizeof(s->server_name), "accd");
	snprintf(s->track, sizeof(s->track), "monza");

	configuration = load_json(cfg_dir, "configuration.json");
	if (configuration == NULL)
		return -1;
	s->tcp_port = json_obj_get_int(configuration, "tcpPort", s->tcp_port);
	s->udp_port = json_obj_get_int(configuration, "udpPort", s->udp_port);
	/*
	 * Deprecated maxClients (exe FUN_1401030e0:101): read it first as the
	 * connection cap, log a deprecation notice, then let maxConnections
	 * override.  A legacy config that only sets maxClients then keeps the
	 * operator value instead of falling back to the default.
	 */
	{
		int mc = json_obj_get_int(configuration, "maxClients", -1);
		if (mc >= 0) {
			log_warn("maxClients=%d is deprecated; please move to "
			    "maxConnections", mc);
			s->max_connections = mc;
		}
	}
	s->max_connections = json_obj_get_int(configuration,
	    "maxConnections", s->max_connections);
	s->lan_discovery = json_obj_get_int(configuration,
	    "lanDiscovery", s->lan_discovery);
	s->stats_udp_port = json_obj_get_int(configuration,
	    "serverDiagnosticsUdpPort", 0);
	s->configuration_version = (uint32_t)json_obj_get_int(
	    configuration, "configVersion", 0);
	/*
	 * registerToLobby lives in configuration.json (exe FUN_1401030e0:165);
	 * the settings.json reader FUN_140106300 never looks at it.  Reading it
	 * from settings.json ignored every stock public config and silently
	 * forced accd private, flipping the maxCarSlots clamp, preRaceWaiting
	 * floor, forceEntryList / allowAutoDQ forcing and formationLapType remap.
	 */
	s->register_to_lobby = json_obj_get_int(configuration,
	    "registerToLobby", s->register_to_lobby);
	json_free(configuration);

	settings = load_json(cfg_dir, "settings.json");
	if (settings != NULL) {
		copy_str(s->server_name, sizeof(s->server_name),
		    json_obj_get_str(settings, "serverName"));
		copy_str(s->password, sizeof(s->password),
		    json_obj_get_str(settings, "password"));
		copy_str(s->admin_password, sizeof(s->admin_password),
		    json_obj_get_str(settings, "adminPassword"));
		copy_str(s->spectator_password,
		    sizeof(s->spectator_password),
		    json_obj_get_str(settings, "spectatorPassword"));
		copy_str(s->car_group, sizeof(s->car_group),
		    json_obj_get_str(settings, "carGroup"));
		if (s->car_group[0] == '\0')
			snprintf(s->car_group, sizeof(s->car_group),
			    "FreeForAll");
		s->ignore_premature_disconnects = json_obj_get_int(
		    settings, "ignorePrematureDisconnects",
		    s->ignore_premature_disconnects);
		s->dump_leaderboards = json_obj_get_int(settings,
		    "dumpLeaderboards", s->dump_leaderboards);
		s->dump_entry_list = json_obj_get_int(settings,
		    "dumpEntryList", s->dump_entry_list);
		s->allow_auto_dq = json_obj_get_int(settings,
		    "allowAutoDQ", s->allow_auto_dq);
		s->use_async_leaderboard = (uint8_t)json_obj_get_int(
		    settings, "useAsyncLeaderboard",
		    s->use_async_leaderboard);
		s->unsafe_rejoin = (uint8_t)json_obj_get_int(settings,
		    "unsafeRejoin", s->unsafe_rejoin);
		{
			int flt = json_obj_get_int(settings,
			    "formationLapType", s->formation_lap_type);
			if (flt == 2) {
				log_warn("wrong formationLapType %d, "
				    "defaulting to 3", flt);
				flt = 3;
			}
			s->formation_lap_type = (uint8_t)flt;
		}
		/*
		 * Remaining settings.json keys read by the exe
		 * (FUN_140106300).  Most are informational; the two
		 * that drive accd behaviour are isPrepPhaseLocked
		 * (mirrors our /lockprep admin state) and latencyStrategy
		 * (init value for the /latencymode toggle).
		 */
		s->preparation_locked = (uint8_t)json_obj_get_int(settings,
		    "isPrepPhaseLocked", s->preparation_locked);
		s->short_formation_lap = (uint8_t)json_obj_get_int(settings,
		    "shortFormationLap", s->short_formation_lap);
		s->write_latency_dumps = (uint8_t)json_obj_get_int(settings,
		    "writeLatencyFileDumps", s->write_latency_dumps);
		s->do_driver_swap_broadcast = (uint8_t)json_obj_get_int(
		    settings, "doDriverSwapBroadcast",
		    s->do_driver_swap_broadcast);
		s->latency_mode = (uint8_t)json_obj_get_int(settings,
		    "latencyStrategy", s->latency_mode);
		s->config_version = (uint32_t)json_obj_get_int(settings,
		    "configVersion", (int)s->config_version);
		log_info("settings.json: configVersion=%u "
		    "shortFormationLap=%u latencyStrategy=%u "
		    "writeLatencyFileDumps=%u doDriverSwapBroadcast=%u "
		    "isPrepPhaseLocked=%u",
		    (unsigned)s->config_version,
		    (unsigned)s->short_formation_lap,
		    (unsigned)s->latency_mode,
		    (unsigned)s->write_latency_dumps,
		    (unsigned)s->do_driver_swap_broadcast,
		    (unsigned)s->preparation_locked);
		s->max_car_slots = json_obj_get_int(settings,
		    "maxCarSlots", 10);
		{
			int dflt_monitors = s->max_connections / 4;
			if (dflt_monitors < 2)
				dflt_monitors = 2;
			s->max_monitors = json_obj_get_int(settings,
			    "maxMonitors", dflt_monitors);
			if (s->max_monitors < 0)
				s->max_monitors = 0;
			if (s->max_monitors > s->max_connections)
				s->max_monitors = s->max_connections;
			s->max_monitors_per_ip = json_obj_get_int(settings,
			    "maxMonitorsPerIp", 2);
			if (s->max_monitors_per_ip < 1)
				s->max_monitors_per_ip = 1;
		}
		/*
		 * Kunos clamps maxCarSlots based on rating requirements
		 * per FUN_1400214b0:55-66:
		 *
		 *   slots = min(30, 10 + min(3, max(0, TM)) +
		 *                       max(0, SA) * 0.25)
		 *
		 * i.e. base 10, +1 per track medal (capped at +3), and
		 * +0.25 per SA point.  Reaching 30 needs TM>=3 AND
		 * SA>=68 (kunos's own log line says "3 TM + 70 SA").
		 * Without rating reqs the cap is 10; we replicate that
		 * lower bound so the locally advertised count matches
		 * the lobby's clamp.
		 */
		/*
		 * Defaults are -1 ("unset" wire sentinel 0xff via the
		 * uint8_t cast), matching the manpage example and the
		 * kunos SDK.  A previous default of 0 told the ACC
		 * server browser "ranked-only with rating >= 0", which
		 * blocked unrated players from joining even on a
		 * password-less server (issue #1, reporter
		 * thomasbourimech, 2026-05-24).
		 */
		s->track_medals_required = (uint8_t)json_obj_get_int(
		    settings, "trackMedalsRequirement", -1);
		s->safety_rating_required = (uint8_t)json_obj_get_int(
		    settings, "safetyRatingRequirement", -1);
		/*
		 * Intentional divergence: the stock exe reader FUN_140106300
		 * matches the key "racecraftRatingRequirement " with a trailing
		 * space (length 0x1b), while its own writer FUN_1401122f0 emits
		 * the key without one (length 0x1a), so kunos silently ignores
		 * this setting.  accd reads the correct key and honours it, so
		 * an operator who sets racecraftRatingRequirement gets the
		 * enforcement they asked for.
		 */
		s->racecraft_rating_required = (uint8_t)json_obj_get_int(
		    settings, "racecraftRatingRequirement", -1);
		/*
		 * FUN_140106300 line 666 rejects an out-of-range
		 * trackMedalsRequirement (valid 0..3) and defaults it to "no
		 * requirement".  Without this an operator typo like 5 is
		 * advertised verbatim and, because it makes
		 * ACC_RATING_REQUIRED() true, also suppresses the maxCarSlots
		 * clamp below.  accd's "no requirement" sentinel is
		 * ACC_RATING_UNSET (the open-server value) rather than the
		 * exe's 0, so reset to that.
		 */
		if (s->track_medals_required != ACC_RATING_UNSET &&
		    s->track_medals_required > 3) {
			log_warn("trackMedalsRequirement %u out of range "
			    "(0..3), ignoring",
			    (unsigned)s->track_medals_required);
			s->track_medals_required = ACC_RATING_UNSET;
		}
		/*
		 * isCPServer + the competition-rating window (FUN_140106300
		 * + FUN_140025690): on a CP server the join gate restricts
		 * connections to Free Practice and gates on the wire-declared
		 * competition rating against [min, max].  Default min 0 / max
		 * INT32_MAX = no window when unset.
		 */
		s->is_cp_server = json_obj_get_int(settings,
		    "isCPServer", 0) ? 1 : 0;
		/*
		 * isCPInvServer (exe FUN_140106300 +0xe3): an invitational CP
		 * server suppresses the same public-MP forcing/clamping gates
		 * as isCPServer.  FUN_1400214b0 and FUN_140023700 gate on BOTH
		 * +0x202 == 0 AND +0x203 == 0.
		 */
		s->is_cp_inv_server = json_obj_get_int(settings,
		    "isCPInvServer", 0) ? 1 : 0;
		/*
		 * simracerWeatherConditions (exe FUN_140104f10:552 -> +0xad,
		 * copied to server+0x315): selects the "Snowflake weather:" vs
		 * "Standard weather:" /wt header word.
		 */
		s->simracer_weather = json_obj_get_int(settings,
		    "simracerWeatherConditions", 0) ? 1 : 0;
		s->competition_rating_min = json_obj_get_int(settings,
		    "competitionRatingMin", 0);
		s->competition_rating_max = json_obj_get_int(settings,
		    "competitionRatingMax", INT32_MAX);
		/*
		 * isRaceLocked (handbook III.2.2): default 1.  Inverse
		 * of unsafe_rejoin which already controls the same
		 * mid-race-join gate.  Reading both keeps backwards
		 * compat with operators using either name.  Only override
		 * unsafe_rejoin when isRaceLocked is actually present in
		 * the JSON; otherwise an operator who set unsafeRejoin
		 * earlier would lose their setting to the default.
		 */
		{
			const struct json_node *rl = json_obj_get(settings,
			    "isRaceLocked");
			if (rl != NULL && rl->kind == JSON_NUM) {
				s->is_race_locked = (uint8_t)rl->u.num;
				s->unsafe_rejoin =
				    s->is_race_locked ? 0 : 1;
			} else {
				s->is_race_locked = s->unsafe_rejoin ? 0 : 1;
			}
		}
		s->randomize_track_when_empty = (uint8_t)json_obj_get_int(
		    settings, "randomizeTrackWhenEmpty", 0);
		s->use_igt_dlc_tracks = (uint8_t)json_obj_get_int(
		    settings, "useIgtDlcTracks", 0);
		s->use_bgt_dlc_tracks = (uint8_t)json_obj_get_int(
		    settings, "useBgtDlcTracks", 0);
		log_info("settings.json: randomizeTrackWhenEmpty=%d "
		    "useIgtDlcTracks=%d useBgtDlcTracks=%d",
		    s->randomize_track_when_empty, s->use_igt_dlc_tracks,
		    s->use_bgt_dlc_tracks);
		/*
		 * FUN_1400214b0 step 1 (lines 19-45): a car slot needs a
		 * connection, so reduce maxCarSlots to max(1, maxConnections)
		 * whenever maxConnections is the smaller of the two.  Runs for
		 * every server kind, before the public-MP rating clamp.
		 */
		{
			int eff_conn = s->max_connections < 1
			    ? 1 : s->max_connections;

			if (eff_conn < s->max_car_slots) {
				log_warn("maxConnections %d is smaller than "
				    "maxCarSlots %d, reducing car slots to %d",
				    s->max_connections, s->max_car_slots,
				    eff_conn);
				s->max_car_slots = eff_conn;
			}
		}
		/*
		 * FUN_1400214b0 step 2 (lines 47-74) clamps a public-MP,
		 * non-CP server first to 30 slots and then to the count its
		 * rating requirements allow:
		 *
		 *   rated = 10 + min(3, max(0, TM)) + max(0, SA) * 0.25
		 *
		 * i.e. base 10, +1 per track medal capped at +3, and +0.25
		 * per SA point (reaching 30 needs 3 TM and 70 SA).  With no
		 * requirements rated is 10, reproducing the old flat clamp.
		 * The gate is public-MP, non-CP only: a private
		 * registerToLobby=0 server and a CP server (isCPServer,
		 * FUN_1400214b0 +0x202) keep their operator value, so private
		 * boxes and competition servers are not silently reset to 10.
		 */
		if (s->register_to_lobby && !s->is_cp_server && !s->is_cp_inv_server) {
			double rated = 10.0;
			int cap;

			if (s->max_car_slots > 30)
				s->max_car_slots = 30;
			if (ACC_RATING_REQUIRED(s->track_medals_required)) {
				int tm = s->track_medals_required;

				rated += tm > 3 ? 3 : tm;
			}
			if (ACC_RATING_REQUIRED(s->safety_rating_required))
				rated += s->safety_rating_required * 0.25;
			cap = (int)rated;
			if (s->max_car_slots > cap) {
				log_warn("maxCarSlots %d exceeds the %d slots "
				    "allowed by the rating requirements, "
				    "reducing", s->max_car_slots, cap);
				s->max_car_slots = cap;
			}
		}
		json_free(settings);
	}

	event = load_json(cfg_dir, "event.json");
	if (event != NULL) {
		const struct json_node *sessions;
		size_t i, n;

		copy_str(s->track, sizeof(s->track),
		    json_obj_get_str(event, "track"));
		/* Strip year suffix (e.g. "_2019") to match the
		 * internal track ID the game client expects. */
		{
			size_t tlen = strlen(s->track);
			if (tlen >= 5 && s->track[tlen - 5] == '_' &&
			    s->track[tlen - 4] >= '1' &&
			    s->track[tlen - 4] <= '2' &&
			    s->track[tlen - 3] >= '0' &&
			    s->track[tlen - 3] <= '9' &&
			    s->track[tlen - 2] >= '0' &&
			    s->track[tlen - 2] <= '9' &&
			    s->track[tlen - 1] >= '0' &&
			    s->track[tlen - 1] <= '9')
				s->track[tlen - 5] = '\0';
		}
		/*
		 * Seed the formation / green trigger ranges from the per-
		 * track table (exe FUN_14012c510).  Event.json override
		 * block below still wins if the operator set explicit
		 * values.  Must run AFTER the year-suffix strip so the
		 * name matches ("brands_hatch_2019" -> "brands_hatch").
		 */
		track_zones_apply(s);

		/*
		 * FUN_1400214b0 step 3: clamp maxCarSlots to the track's
		 * pit box count.  FUN_1400214b0:75-81 selects between
		 * +0xa0808 (public, param_2) and +0xa080c (private, param_3)
		 * based on registerToLobby=0 or isCPServer/isCPInvServer.
		 */
		{
			int priv = !s->register_to_lobby ||
			    s->is_cp_server || s->is_cp_inv_server;
			int pc = track_pit_count(s->track, priv);

			if (s->max_car_slots > pc) {
				log_warn("maxCarSlots %d exceeds pit count %d "
				    "for %s, reducing",
				    s->max_car_slots, pc, s->track);
				s->max_car_slots = pc;
			}
		}

		sessions = json_obj_get(event, "sessions");
		n = json_arr_len(sessions);
		if (n > ACC_MAX_SESSIONS)
			n = ACC_MAX_SESSIONS;
		s->session_count = (uint8_t)n;
		for (i = 0; i < n; i++) {
			const struct json_node *sn = json_arr_at(sessions, i);
			const char *type;
			struct SessionDef *d = &s->sessions[i];

			d->duration_min = (uint16_t)json_obj_get_int(sn,
			    "sessionDurationMinutes", 10);
			d->hour_of_day = (uint8_t)json_obj_get_int(sn,
			    "hourOfDay", 12);
			d->date_minute = (uint8_t)json_obj_get_int(sn,
			    "dateMinute", 0);
			d->day_of_weekend = (uint8_t)json_obj_get_int(sn,
			    "dayOfWeekend", 0);
			d->time_multiplier = (uint8_t)json_obj_get_int(sn,
			    "timeMultiplier", 1);
			if (d->time_multiplier == 0)
				d->time_multiplier = 1;
			d->dynamic_track_multiplier = (float)json_obj_get_num(sn,
			    "dynamicTrackMultiplier", 0.0);
			type = json_obj_get_str(sn, "sessionType");
			if (type != NULL && type[0] == 'P')
				d->session_type = 0;
			else if (type != NULL && type[0] == 'Q')
				d->session_type = 4;
			else if (type != NULL && type[0] == 'R')
				d->session_type = 10;
			else
				d->session_type = 0;
		}
		{
			int prw = json_obj_get_int(event,
			    "preRaceWaitingTimeSeconds", 80);
			if (prw < 0)
				prw = 0;
			s->pre_race_waiting_s = (uint16_t)prw;
		}
		if (s->register_to_lobby) {
			if (s->pre_race_waiting_s < 80) {
				log_warn("preRaceWaitingTimeSeconds (%u) has been "
				    "set to 80s, %u seconds are too low for "
				    "public multiplayer",
				    (unsigned)s->pre_race_waiting_s,
				    (unsigned)s->pre_race_waiting_s);
				s->pre_race_waiting_s = 80;
			}
		} else if (s->pre_race_waiting_s < 5) {
			log_warn("preRaceWaitingTimeSeconds (%u) has been set "
			    "to 5s, %u seconds are too low even for private "
			    "servers",
			    (unsigned)s->pre_race_waiting_s,
			    (unsigned)s->pre_race_waiting_s);
			s->pre_race_waiting_s = 5;
		}
		{
			/*
			 * A negative sessionOverTimeSeconds is the exe's
			 * "disable overtime" sentinel; read it as int first so
			 * it does not wrap to 65535 s (18 h) through the
			 * uint16_t cast and hang the session in overtime.
			 * Clamp negative to 0 (no grace period).
			 */
			int ot = json_obj_get_int(event,
			    "sessionOverTimeSeconds", 120);
			if (ot < 0)
				ot = 0;
			if (ot > 65535)
				ot = 65535;
			s->session_overtime_s = (uint16_t)ot;
		}
		s->post_qualy_s = (uint16_t)json_obj_get_int(
		    event, "postQualySeconds", s->post_qualy_s);
		s->post_race_s = (uint16_t)json_obj_get_int(
		    event, "postRaceSeconds", s->post_race_s);
		{
			/*
			 * Clamp to the exe's event-config ranges
			 * (FUN_14011a820): ambient [12,45], track [10,70].
			 * trackTemp 0 means "unset" -> derive ambient+8 (accd
			 * convenience) before the track clamp.  Done via int
			 * intermediates so an out-of-range JSON value is
			 * clamped, not wrapped by the uint8_t cast.
			 */
			int at = json_obj_get_int(event, "ambientTemp", 22);
			int tt = json_obj_get_int(event, "trackTemp", 0);

			if (at < 12)
				at = 12;
			if (at > 45)
				at = 45;
			s->session.ambient_temp = (uint8_t)at;
			if (tt == 0)
				tt = at + 8;
			if (tt < 10)
				tt = 10;
			if (tt > 70)
				tt = 70;
			s->session.track_temp = (uint8_t)tt;
		}
		s->event_version = (uint32_t)json_obj_get_int(event,
		    "configVersion", 0);
		copy_str(s->meta_data, sizeof(s->meta_data),
		    json_obj_get_str(event, "metaData"));
		s->formation_trigger_start = (float)json_obj_get_num(event,
		    "formationTriggerNormalizedRangeStart",
		    s->formation_trigger_start);
		s->green_trigger_start = (float)json_obj_get_num(event,
		    "greenFlagTriggerNormalizedRangeStart",
		    s->green_trigger_start);
		s->green_trigger_end = (float)json_obj_get_num(event,
		    "greenFlagTriggerNormalizedRangeEnd",
		    s->green_trigger_end);

		{
			/*
			 * cloudLevel and rain in event.json are 0.0..1.0
			 * floats; json_obj_get_num returns the JSON_NUM
			 * value or the default if the key is absent or
			 * not numeric.
			 */
			float clouds = (float)json_obj_get_num(event,
			    "cloudLevel", 0.0);
			float rain = (float)json_obj_get_num(event,
			    "rain", 0.0);
			int randomness = json_obj_get_int(event,
			    "weatherRandomness", 0);
			float ws_mean = (float)json_obj_get_num(event,
			    "windSpeedMean", 0.0);
			float ws_dev = (float)json_obj_get_num(event,
			    "windSpeedDeviation", 0.01);
			float wb_mean = (float)json_obj_get_num(event,
			    "weatherBaseMean", 0.4);
			float wb_dev = (float)json_obj_get_num(event,
			    "weatherBaseDeviation", 0.3);
			{
				uint32_t start_s = 0;

				if (s->session_count > 0)
					start_s =
					    (uint32_t)s->sessions[0].date_minute
					    * 60u +
					    (uint32_t)s->sessions[0].hour_of_day
					    * 3600u;
				weather_init(s, clouds, rain, randomness,
				    start_s, ws_mean, ws_dev,
				    wb_mean, wb_dev);
				s->session.weekend_time_s = start_s;
			}
		}

		json_free(event);
	}

	{
		/*
		 * eventRules.json — optional.  Consume the two fields
		 * that drive server-side auto-DQ paths we implement:
		 *   driverStintTime        (min, per-driver stint limit)
		 *   mandatoryPitstopCount  (count, 0 = no requirement)
		 * 0 / missing = no enforcement.
		 */
		struct json_node *rules =
		    load_json(cfg_dir, "eventRules.json");
		if (rules != NULL) {
			int stint_sec = json_obj_get_int(rules,
			    "driverStintTimeSec", -1);
			int stint_legacy_min = json_obj_get_int(rules,
			    "driverStintTime", -1);
			int race_dur_s = 0;
			int si;
			int pit_count = json_obj_get_int(rules,
			    "mandatoryPitstopCount", 0);
			int swap_req = json_obj_get_int(rules,
			    "isMandatoryPitstopSwapDriverRequired", 0);
			int qst = json_obj_get_int(rules,
			    "qualifyStandingType",
			    s->qualify_standing_type);
			int pit_window = json_obj_get_int(rules,
			    "pitWindowLengthSec", -1);
			int max_drv_time = json_obj_get_int(rules,
			    "maxTotalDrivingTime", -1);
			int max_drvs = json_obj_get_int(rules,
			    "maxDriversCount", s->max_drivers_count);
			int refuel = json_obj_get_int(rules,
			    "isRefuellingAllowedInRace",
			    s->refuelling_allowed);
			int refuel_fixed = json_obj_get_int(rules,
			    "isRefuellingTimeFixed",
			    s->refuelling_time_fixed);
			int pit_refuel = json_obj_get_int(rules,
			    "isMandatoryPitstopRefuellingRequired",
			    s->pit_refuelling_required);
			int pit_tyres = json_obj_get_int(rules,
			    "isMandatoryPitstopTyreChangeRequired",
			    s->pit_tyre_change_required);
			int tyre_sets = json_obj_get_int(rules,
			    "tyreSetCount", s->tyre_set_count);

			/*
			 * driverStintTimeSec (seconds) is the original
			 * server's key.  Fall back to the legacy accd
			 * driverStintTime (minutes) when it is absent so
			 * older accd configs keep working.
			 */
			if (stint_sec < 0 && stint_legacy_min > 0) {
				log_info("eventRules.json: driverStintTime "
				    "(minutes) is deprecated; use "
				    "driverStintTimeSec (seconds)");
				stint_sec = stint_legacy_min * 60;
			}
			if (stint_sec < 0)
				stint_sec = 0;
			/*
			 * Fallback cascade matching FUN_14002aca0:366-388
			 * ORDER exactly: back-propagate maxTotalDrivingTime
			 * into the stint ONLY when it is already set, then
			 * derive maxTotalDrivingTime = raceDuration + 10 min
			 * ONLY when the stint is already set.  When BOTH keys
			 * are absent the exe leaves driverStintTimeSec = 0 (no
			 * stint enforcement).  The previous order filled
			 * maxDrivingTime unconditionally and then derived a
			 * non-zero stint, so accd over-emitted the 0x4f stint
			 * sync and enforced a stint the stock server does not.
			 */
			for (si = 0; si < s->session_count &&
			    si < ACC_MAX_SESSIONS; si++)
				if (s->sessions[si].session_type == 10) {
					race_dur_s = (int)
					    s->sessions[si].duration_min * 60;
					break;
				}
			if (stint_sec < 1 && max_drv_time > 0)
				stint_sec = max_drv_time;
			if (stint_sec > 0 && max_drv_time < 1 && race_dur_s > 0)
				max_drv_time = race_dur_s + 600;
			if (stint_sec > 86400)	/* cap at 24h */
				stint_sec = 86400;
			if (pit_count < 0)
				pit_count = 0;
			if (pit_count > 255)
				pit_count = 255;
			if (max_drvs < 1)
				max_drvs = 1;
			if (max_drvs > 255)
				max_drvs = 255;
			if (tyre_sets < 1)
				tyre_sets = 1;
			if (tyre_sets > 255)
				tyre_sets = 255;
			s->driver_stint_time_s = (uint32_t)stint_sec;
			s->mandatory_pit_count = (uint8_t)pit_count;
			s->mandatory_swap_required = swap_req ? 1 : 0;
			s->qualify_standing_type = (uint8_t)
			    (qst >= 0 && qst <= 1 ? qst : 1);
			s->pit_window_length_s = pit_window;
			s->max_total_driving_time_s = max_drv_time;
			s->max_drivers_count = (uint8_t)max_drvs;
			s->refuelling_allowed = refuel ? 1 : 0;
			s->refuelling_time_fixed = refuel_fixed ? 1 : 0;
			s->pit_refuelling_required = pit_refuel ? 1 : 0;
			s->pit_tyre_change_required = pit_tyres ? 1 : 0;
			s->tyre_set_count = (uint8_t)tyre_sets;
			log_info("eventRules.json: stint=%us pits=%u "
			    "swap=%u qstand=%u pitwin=%ds maxdrvtime=%ds "
			    "maxdrvs=%u refuel=%u/fixed=%u "
			    "pit_refuel=%u pit_tyres=%u tyresets=%u",
			    (unsigned)s->driver_stint_time_s,
			    (unsigned)s->mandatory_pit_count,
			    (unsigned)s->mandatory_swap_required,
			    (unsigned)s->qualify_standing_type,
			    s->pit_window_length_s,
			    s->max_total_driving_time_s,
			    (unsigned)s->max_drivers_count,
			    (unsigned)s->refuelling_allowed,
			    (unsigned)s->refuelling_time_fixed,
			    (unsigned)s->pit_refuelling_required,
			    (unsigned)s->pit_tyre_change_required,
			    (unsigned)s->tyre_set_count);
			json_free(rules);
		}
	}

	{
		/*
		 * weatherRules.json — optional weekend-weather constraints.
		 * When isActive is set, the server re-draws the weekend
		 * forecast on every reset until it satisfies every set bound
		 * or abortSimulationsAfterMs elapses (FUN_140133770 driven
		 * by FUN_14002c740).  All bounds default to -1 (ignore);
		 * keys and defaults mirror the exe deserializer FUN_1400fd9d0.
		 */
		struct WeatherRules *wr = &s->weather_rules;
		struct json_node *wrules =
		    load_json(cfg_dir, "weatherRules.json");

		wr->active = 0;
		wr->verbose = 0;
		wr->abort_after_ms = 300;
		wr->temp_min = -1;
		wr->temp_max = -1;
		wr->temp_max_diff = -1;
		wr->rain_min = -1.0f;
		wr->rain_max = -1.0f;
		wr->rain_min_diff = -1.0f;
		wr->rain_max_diff = -1.0f;
		wr->cloud_min = -1.0f;
		wr->cloud_max = -1.0f;
		wr->rain_changes = -1;

		if (wrules != NULL) {
			wr->active = (uint8_t)json_obj_get_bool(wrules,
			    "isActive", 0);
			wr->verbose = (uint8_t)json_obj_get_bool(wrules,
			    "withLogging", 0);
			wr->abort_after_ms = json_obj_get_int(wrules,
			    "abortSimulationsAfterMs", 300);
			wr->temp_min = json_obj_get_int(wrules,
			    "raceTempMin", -1);
			wr->temp_max = json_obj_get_int(wrules,
			    "raceTempMax", -1);
			wr->temp_max_diff = json_obj_get_int(wrules,
			    "maxTempDifference", -1);
			wr->rain_min = (float)json_obj_get_num(wrules,
			    "raceRainMin", -1.0);
			wr->rain_max = (float)json_obj_get_num(wrules,
			    "raceRainMax", -1.0);
			wr->rain_min_diff = (float)json_obj_get_num(wrules,
			    "minRainDifference", -1.0);
			wr->rain_max_diff = (float)json_obj_get_num(wrules,
			    "maxRainDifference", -1.0);
			wr->cloud_min = (float)json_obj_get_num(wrules,
			    "minCloudLevel", -1.0);
			wr->cloud_max = (float)json_obj_get_num(wrules,
			    "maxCloudLevel", -1.0);
			wr->rain_changes = json_obj_get_int(wrules,
			    "raceRainChanges", -1);
			if (wr->active)
				log_info("weatherRules.json: active (temp "
				    "%d..%d, cloud %.2f..%.2f, rain %.2f..%.2f, "
				    "abort after %d ms)",
				    wr->temp_min, wr->temp_max,
				    (double)wr->cloud_min, (double)wr->cloud_max,
				    (double)wr->rain_min, (double)wr->rain_max,
				    wr->abort_after_ms);
			json_free(wrules);
		}
	}

	if (s->max_connections < 1 || s->max_connections > ACC_MAX_CARS) {
		if (s->max_connections > ACC_MAX_CARS)
			log_warn("maxConnections %d exceeds the %d-slot "
			    "build limit, clamping", s->max_connections,
			    ACC_MAX_CARS);
		s->max_connections = ACC_MAX_CARS;
	}

	/*
	 * Load entrylist.json templates if present.  These are
	 * applied as defaults to the corresponding car slots when
	 * a client claims them in the handshake; missing file is
	 * not fatal (open server with no forced grid).
	 */
	(void)entrylist_load(s, cfg_dir);
	(void)bop_load(s, cfg_dir);

	/*
	 * configVersion roll-up — the Kunos exe reads this key from each
	 * config file and stores it but never compares.  We at least log
	 * all four together so operators can spot drift from one editor
	 * leaving a stale version behind, and warn loudly when a file
	 * predates the minimum schema we tested against.
	 */
	{
		const uint32_t min_ver = 1;

		log_info("configVersion: configuration=%u settings=%u "
		    "event=%u entrylist=%u (expected >= %u)",
		    (unsigned)s->configuration_version,
		    (unsigned)s->config_version,
		    (unsigned)s->event_version,
		    (unsigned)s->entrylist_version,
		    (unsigned)min_ver);
		if (s->configuration_version < min_ver)
			log_warn("configuration.json configVersion=%u < %u "
			    "— some keys may be missing defaults",
			    (unsigned)s->configuration_version,
			    (unsigned)min_ver);
		if (s->config_version < min_ver)
			log_warn("settings.json configVersion=%u < %u",
			    (unsigned)s->config_version,
			    (unsigned)min_ver);
		if (s->event_version < min_ver)
			log_warn("event.json configVersion=%u < %u",
			    (unsigned)s->event_version,
			    (unsigned)min_ver);
		/* entrylist.json is optional — only warn when it loaded. */
		if (s->entrylist_version > 0 &&
		    s->entrylist_version < min_ver)
			log_warn("entrylist.json configVersion=%u < %u",
			    (unsigned)s->entrylist_version,
			    (unsigned)min_ver);
	}

	/*
	 * assistRules.json — per-handbook III.2.5.  All fields default
	 * to 0 = allowed (ACC convention).  The handshake builder
	 * (handshake.c:130) maps 0 to wire value 2 ("no restriction")
	 * and 1 to wire value 1 ("disabled"), so unset operators get
	 * the spec's permissive default.  Optional file: missing /
	 * unparseable assistRules.json leaves everything at zero.
	 */
	{
		struct json_node *assist;

		assist = load_json(cfg_dir, "assistRules.json");
		if (assist != NULL) {
			s->assist.stability_control_max = (uint8_t)
			    json_obj_get_int(assist,
				"stabilityControlLevelMax",
				s->assist.stability_control_max);
			s->assist.disable_autosteer = (uint8_t)
			    json_obj_get_int(assist, "disableAutosteer",
				s->assist.disable_autosteer);
			s->assist.disable_auto_pit_limiter = (uint8_t)
			    json_obj_get_int(assist,
				"disableAutoPitLimiter",
				s->assist.disable_auto_pit_limiter);
			s->assist.disable_auto_gear = (uint8_t)
			    json_obj_get_int(assist, "disableAutoGear",
				s->assist.disable_auto_gear);
			s->assist.disable_auto_clutch = (uint8_t)
			    json_obj_get_int(assist, "disableAutoClutch",
				s->assist.disable_auto_clutch);
			s->assist.disable_ideal_line = (uint8_t)
			    json_obj_get_int(assist, "disableIdealLine",
				s->assist.disable_ideal_line);
			s->assist.disable_auto_engine_start = (uint8_t)
			    json_obj_get_int(assist,
				"disableAutoEngineStart",
				s->assist.disable_auto_engine_start);
			s->assist.disable_auto_wiper = (uint8_t)
			    json_obj_get_int(assist, "disableAutoWiper",
				s->assist.disable_auto_wiper);
			s->assist.disable_auto_lights = (uint8_t)
			    json_obj_get_int(assist, "disableAutoLights",
				s->assist.disable_auto_lights);
			log_info("assistRules.json: stability_max=%u "
			    "autosteer=%u pitlim=%u gear=%u clutch=%u "
			    "ideal=%u engstart=%u wiper=%u lights=%u",
			    (unsigned)s->assist.stability_control_max,
			    (unsigned)s->assist.disable_autosteer,
			    (unsigned)s->assist.disable_auto_pit_limiter,
			    (unsigned)s->assist.disable_auto_gear,
			    (unsigned)s->assist.disable_auto_clutch,
			    (unsigned)s->assist.disable_ideal_line,
			    (unsigned)s->assist.disable_auto_engine_start,
			    (unsigned)s->assist.disable_auto_wiper,
			    (unsigned)s->assist.disable_auto_lights);
			json_free(assist);
		}
	}

	/*
	 * Public-MP servers clear forceEntryList (FUN_140023700:520-524),
	 * force allowAutoDQ=1 (FUN_140023700:550-554), and clear
	 * dumpEntryList (FUN_140023700:556-561).  Applied before the
	 * formationLapType remap below because that remap gates on
	 * force_entry_list.
	 */
	if (s->force_entry_list && s->register_to_lobby && !s->is_cp_server && !s->is_cp_inv_server) {
		log_warn("forceEntryList set but is a public server, "
		    "disabling forceEntryList");
		s->force_entry_list = 0;
	}
	if (!s->allow_auto_dq && s->register_to_lobby && !s->is_cp_server && !s->is_cp_inv_server) {
		log_warn("allowAutoDQ is false but is a public server, "
		    "forcing allowAutoDQ=1");
		s->allow_auto_dq = 1;
	}
	if (s->dump_entry_list && s->register_to_lobby && !s->is_cp_server && !s->is_cp_inv_server) {
		log_warn("dumpEntryList set but is a public server, "
		    "disabling dumpEntryList");
		s->dump_entry_list = 0;
	}
	/*
	 * Public-MP servers force formationLapType 1 (manual) to 3, matching
	 * the exe (FUN_140023700:531-543).  "Public MP" = registered to the
	 * lobby, not a championship server, and not using a forced entry
	 * list.  Done here, after entrylist_load has set force_entry_list;
	 * the unconditional 2 -> 3 remap is applied earlier at the read.
	 */
	if (s->formation_lap_type == 1 && s->register_to_lobby &&
	    !s->is_cp_server && !s->is_cp_inv_server && !s->force_entry_list) {
		log_warn("formationLapType 1 (manual) forced to 3 on "
		    "public-MP server");
		s->formation_lap_type = 3;
	}

	return 0;
}
