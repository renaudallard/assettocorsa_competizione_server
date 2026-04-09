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
 * entrylist.c -- entrylist.json reader.
 *
 * Accepts either UTF-16 LE (the format accServer.exe writes) or
 * plain UTF-8 (so the file can be edited by hand).  Detection is
 * by BOM sniffing; see decode_cfg_bytes in config.c for the same
 * logic used for configuration.json / settings.json / event.json.
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "entrylist.h"
#include "json.h"
#include "log.h"
#include "state.h"

#define EL_MAX_SIZE	(1u << 20)

static char *
read_file(const char *path, size_t *outlen)
{
	int fd = open(path, O_RDONLY);
	struct stat st;
	char *buf;
	ssize_t n;
	size_t off = 0;

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) < 0) { close(fd); return NULL; }
	if (st.st_size <= 0 || st.st_size > (off_t)EL_MAX_SIZE) {
		close(fd);
		return NULL;
	}
	buf = malloc((size_t)st.st_size);
	if (buf == NULL) { close(fd); return NULL; }
	while (off < (size_t)st.st_size) {
		n = read(fd, buf + off, (size_t)st.st_size - off);
		if (n < 0) {
			if (errno == EINTR) continue;
			free(buf); close(fd); return NULL;
		}
		if (n == 0) break;
		off += (size_t)n;
	}
	close(fd);
	*outlen = off;
	return buf;
}

static char *
decode_cfg_bytes(const char *in, size_t inlen)
{
	iconv_t cd;
	char *out, *outp;
	const char *inp;
	size_t outsz, inrem, outrem;

	/* Optional UTF-8 BOM -- strip it. */
	if (inlen >= 3 &&
	    (unsigned char)in[0] == 0xef &&
	    (unsigned char)in[1] == 0xbb &&
	    (unsigned char)in[2] == 0xbf) {
		in += 3;
		inlen -= 3;
	}

	/* No UTF-16 LE BOM -- return as UTF-8 verbatim. */
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

	in += 2;
	inlen -= 2;
	cd = iconv_open("UTF-8", "UTF-16LE");
	if (cd == (iconv_t)-1)
		return NULL;
	outsz = inlen * 2 + 1;
	out = malloc(outsz);
	if (out == NULL) { iconv_close(cd); return NULL; }
	inp = in;
	inrem = inlen;
	outp = out;
	outrem = outsz - 1;
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
	snprintf(dst, dstsz, "%s", src);
}

int
entrylist_load(struct Server *s, const char *cfg_dir)
{
	char path[512];
	char *raw, *utf8;
	size_t rawlen;
	char err[256] = "";
	struct json_node *root;
	const struct json_node *entries;
	size_t i, n;
	int loaded = 0;

	snprintf(path, sizeof(path), "%s/entrylist.json", cfg_dir);
	raw = read_file(path, &rawlen);
	if (raw == NULL) {
		log_warn("entrylist: cannot read %s: %s",
		    path, strerror(errno));
		return -1;
	}
	utf8 = decode_cfg_bytes(raw, rawlen);
	free(raw);
	if (utf8 == NULL) {
		log_warn("entrylist: decode failed for %s", path);
		return -1;
	}
	root = json_parse(utf8, strlen(utf8), err, sizeof(err));
	free(utf8);
	if (root == NULL) {
		log_warn("entrylist: parse failed: %s", err);
		return -1;
	}

	entries = json_obj_get(root, "entries");
	n = json_arr_len(entries);
	if (n > ACC_MAX_CARS)
		n = ACC_MAX_CARS;

	for (i = 0; i < n; i++) {
		const struct json_node *e = json_arr_at(entries, i);
		const struct json_node *drivers;
		struct CarEntry *car = &s->cars[i];
		size_t dj, dn;

		car->car_id = (uint16_t)i;
		/*
		 * Entrylist entries are templates -- the actual
		 * `used` flag is set when a client claims this
		 * slot via the handshake.  Preloaded driver names,
		 * ballast, restrictor, etc. survive that
		 * transition.
		 */
		car->race_number = json_obj_get_int(e, "raceNumber",
		    (int)i);
		car->car_model = (uint8_t)json_obj_get_int(e,
		    "forcedCarModel", -1);
		if (car->car_model == 0xff)
			car->car_model = 0;
		car->cup_category = (uint8_t)json_obj_get_int(e,
		    "overrideCarModelForCustomCar", 0);
		car->default_grid_position = json_obj_get_int(e,
		    "defaultGridPosition", -1);
		car->ballast_kg = (uint8_t)json_obj_get_int(e,
		    "ballastKg", 0);
		car->restrictor = (float)json_obj_get_num(e,
		    "restrictor", 0.0);
		car->current_driver_index = (uint8_t)json_obj_get_int(e,
		    "defaultDriverIndex", 0);
		copy_str(car->team_name, sizeof(car->team_name),
		    json_obj_get_str(e, "teamName"));

		drivers = json_obj_get(e, "drivers");
		dn = json_arr_len(drivers);
		if (dn > ACC_MAX_DRIVERS_PER_CAR)
			dn = ACC_MAX_DRIVERS_PER_CAR;
		car->driver_count = (uint8_t)dn;
		for (dj = 0; dj < dn; dj++) {
			const struct json_node *dnode =
			    json_arr_at(drivers, dj);
			struct DriverInfo *d = &car->drivers[dj];

			copy_str(d->first_name, sizeof(d->first_name),
			    json_obj_get_str(dnode, "firstName"));
			copy_str(d->last_name, sizeof(d->last_name),
			    json_obj_get_str(dnode, "lastName"));
			copy_str(d->short_name, sizeof(d->short_name),
			    json_obj_get_str(dnode, "shortName"));
			copy_str(d->steam_id, sizeof(d->steam_id),
			    json_obj_get_str(dnode, "playerID"));
			d->driver_category = (uint8_t)json_obj_get_int(
			    dnode, "driverCategory", 0);
			d->nationality = (uint16_t)json_obj_get_int(
			    dnode, "nationality", 0);
		}
		loaded++;
	}

	json_free(root);
	log_info("entrylist: loaded %d cars from %s", loaded, path);
	return loaded;
}
