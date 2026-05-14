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
	s->max_connections = json_obj_get_int(configuration,
	    "maxConnections", s->max_connections);
	s->lan_discovery = json_obj_get_int(configuration,
	    "lanDiscovery", s->lan_discovery);
	s->stats_udp_port = json_obj_get_int(configuration,
	    "statsUdpPort", 0);
	s->configuration_version = (uint32_t)json_obj_get_int(
	    configuration, "configVersion", 0);
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
		s->register_to_lobby = json_obj_get_int(settings,
		    "registerToLobby", s->register_to_lobby);
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
		s->track_medals_required = (uint8_t)json_obj_get_int(
		    settings, "trackMedalsRequirement", 0);
		s->safety_rating_required = (uint8_t)json_obj_get_int(
		    settings, "safetyRatingRequirement", 0);
		s->racecraft_rating_required = (uint8_t)json_obj_get_int(
		    settings, "racecraftRatingRequirement", 0);
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
		if (s->max_car_slots > 10 &&
		    s->track_medals_required < 3 &&
		    s->safety_rating_required < 70)
			s->max_car_slots = 10;
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
			d->day_of_weekend = (uint8_t)json_obj_get_int(sn,
			    "dayOfWeekend", 0);
			d->time_multiplier = (uint8_t)json_obj_get_int(sn,
			    "timeMultiplier", 1);
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
		s->pre_race_waiting_s = (uint16_t)json_obj_get_int(
		    event, "preRaceWaitingTimeSeconds", 80);
		s->session_overtime_s = (uint16_t)json_obj_get_int(
		    event, "sessionOverTimeSeconds", 120);
		s->post_qualy_s = (uint16_t)json_obj_get_int(
		    event, "postQualySeconds", s->post_qualy_s);
		s->post_race_s = (uint16_t)json_obj_get_int(
		    event, "postRaceSeconds", s->post_race_s);
		s->session.ambient_temp = (uint8_t)json_obj_get_int(
		    event, "ambientTemp", 22);
		s->session.track_temp = (uint8_t)json_obj_get_int(
		    event, "trackTemp", 0);
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
		/*
		 * Fall back to ambient+8 only when the operator didn't set
		 * trackTemp (or set it to 0).  Earlier code overwrote the
		 * JSON value unconditionally, so any non-zero trackTemp in
		 * event.json was silently ignored.
		 */
		if (s->session.track_temp == 0)
			s->session.track_temp = (uint8_t)(
			    s->session.ambient_temp + 8);

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
			{
				uint32_t start_s =
				    (uint32_t)s->sessions[0].hour_of_day
				    * 3600u;
				weather_init(s, clouds, rain, randomness,
				    start_s);
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
			int stint_min = json_obj_get_int(rules,
			    "driverStintTime", 0);
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

			if (stint_min < 0)
				stint_min = 0;
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
			s->driver_stint_time_s = (uint32_t)stint_min * 60u;
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

	if (s->max_connections < 1 || s->max_connections > ACC_MAX_CARS)
		s->max_connections = ACC_MAX_CARS;

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

	return 0;
}
