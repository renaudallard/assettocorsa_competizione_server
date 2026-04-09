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
		s->ignore_premature_disconnects = json_obj_get_int(
		    settings, "ignorePrematureDisconnects",
		    s->ignore_premature_disconnects);
		s->dump_leaderboards = json_obj_get_int(settings,
		    "dumpLeaderboards", s->dump_leaderboards);
		json_free(settings);
	}

	event = load_json(cfg_dir, "event.json");
	if (event != NULL) {
		const struct json_node *sessions;
		size_t i, n;

		copy_str(s->track, sizeof(s->track),
		    json_obj_get_str(event, "track"));

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
		json_free(event);
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

	return 0;
}
