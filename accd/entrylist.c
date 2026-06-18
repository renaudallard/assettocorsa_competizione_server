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
#include "io.h"
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

	s->force_entry_list = json_obj_get_int(root,
	    "forceEntryList", 0);
	s->entrylist_version = (uint32_t)json_obj_get_int(root,
	    "configVersion", 0);

	entries = json_obj_get(root, "entries");
	n = json_arr_len(entries);
	if (n > ACC_MAX_CARS)
		n = ACC_MAX_CARS;

	for (i = 0; i < n; i++) {
		const struct json_node *e = json_arr_at(entries, i);
		const struct json_node *drivers;
		struct CarEntry *car = &s->cars[i];
		size_t dj, dn;

		car->car_id = (uint16_t)(ACC_CAR_ID_BASE + i);
		/*
		 * Entrylist entries are templates -- the actual
		 * `used` flag is set when a client claims this
		 * slot via the handshake.  Preloaded driver names,
		 * ballast, restrictor, etc. survive that
		 * transition.
		 */
		car->race_number = json_obj_get_int(e, "raceNumber",
		    (int)i);
		{
			/*
			 * The exe keeps forcedCarModel as a 32-bit value; we
			 * store uint8, so treat an out-of-range value (negative
			 * or >= 0xff) as unset rather than truncating it into a
			 * valid model id (e.g. 256 -> 0 would force model 0).
			 */
			int fm = json_obj_get_int(e, "forcedCarModel", -1);
			if (fm < 0 || fm > 0xfe)
				fm = 0xff;	/* unset sentinel */
			car->car_model = (uint8_t)fm;
		}
		car->forced_car_model = car->car_model;
		if (car->car_model == 0xff)
			car->car_model = 0;
		car->override_car_model_custom = (uint8_t)
		    (json_obj_get_int(e, "overrideCarModelForCustomCar", 0)
		    != 0);
		{
			/*
			 * Normalise defaultGridPosition to 0-based, matching
			 * the exe (FUN_140104340:299-306): store JSON value-1,
			 * with JSON 0 / absent / negative meaning unset (-1).
			 * Both consumers (session.c, handshake.c) then read it
			 * directly as a 0-based slot.
			 */
			int gp = json_obj_get_int(e, "defaultGridPosition", 0);
			car->default_grid_position = (gp < 1) ? -1 : gp - 1;
		}
		{
			/*
			 * Clamp ballast to [-40, 40] kg before the int8 cast so
			 * an out-of-range entrylist value cannot wrap (e.g. 200
			 * -> -56, flipping a heavy ballast to a negative one),
			 * matching the exe (FUN_140023700:298-307) and the admin
			 * /ballast clamp.
			 */
			int kg = json_obj_get_int(e, "ballastKg", 0);
			if (kg < -40) kg = -40;
			if (kg > 40) kg = 40;
			car->ballast_kg = (int8_t)kg;
		}
		{
			/*
			 * restrictor is an integer PERCENT in the JSON (exe
			 * FUN_140104340:460-477 reads it as int, stores *0.01);
			 * clamp to [0, 20] % (exe FUN_140023700:308-317) and keep
			 * the normalised 0..0.20 fraction the wire / BoP path
			 * expects.
			 */
			int rest = json_obj_get_int(e, "restrictor", 0);
			if (rest < 0) rest = 0;
			if (rest > 20) rest = 20;
			car->restrictor = (float)rest * 0.01f;
		}
		{
			int ddi = json_obj_get_int(e, "defaultDriverIndex", 0);
			/*
			 * Clamp to the per-car driver-array bound so a
			 * mistaken "defaultDriverIndex": 200 in
			 * entrylist.json doesn't seed an out-of-range
			 * current_driver_index that later handlers (h_chat,
			 * write_car_leaderboard_record, swap-state) would
			 * dereference past the 4-element drivers[] array.
			 */
			if (ddi < 0)
				ddi = 0;
			if (ddi >= ACC_MAX_DRIVERS_PER_CAR)
				ddi = ACC_MAX_DRIVERS_PER_CAR - 1;
			car->current_driver_index = (uint8_t)ddi;
		}
		car->is_server_admin = (uint8_t)json_obj_get_int(e,
		    "isServerAdmin", 0);
		copy_str(car->team_name, sizeof(car->team_name),
		    json_obj_get_str(e, "teamName"));
		copy_str(car->custom_car, sizeof(car->custom_car),
		    json_obj_get_str(e, "customCar"));

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
		/*
		 * Derive cup_category from the default driver's category,
		 * matching the 0x36 leaderboard cup byte: Bronze->Am,
		 * Silver->ProAm, Gold/Platinum->Pro, else 4.  The exe has no
		 * cupCategory entrylist key; it derives the same way.
		 */
		switch (car->drivers[car->current_driver_index].driver_category) {
		case 0:	 car->cup_category = 2; break;	/* Bronze -> Am */
		case 1:	 car->cup_category = 3; break;	/* Silver -> ProAm */
		case 2:
		case 3:	 car->cup_category = 0; break;	/* Gold/Plat -> Pro */
		default: car->cup_category = 4; break;
		}
		/*
		 * Team-entry sanity, mirroring the exe (FUN_140023700:
		 * 362-366): a multi-driver entry with overrideDriverInfo:0
		 * fails to populate co-driver names in the exe.  accd reads
		 * names from the entrylist regardless, so this is advisory,
		 * but surface the same operator hint.
		 */
		if (dn > 1 &&
		    json_obj_get_int(e, "overrideDriverInfo", 0) == 0)
			log_warn("entrylist: entry %zu has %zu drivers but "
			    "overrideDriverInfo:0 - fill out driver names "
			    "for teams", i, dn);
		loaded++;
	}

	/*
	 * Team-entry expansion: an entrylist entry with >1 registered
	 * drivers under forceEntryList=1 is a kunos "team entry".
	 * Kunos allocates a separate car_id per driver in the entry
	 * but every car shares the entry-level fields (raceNumber,
	 * car_model, team_name, drivers[], driver_count, ballast,
	 * restrictor, ...).  Mirror by replicating the proto slot's
	 * entry-level state into companion slots starting after the
	 * last loaded entry.  Each member of the group — anchor and
	 * companions — has team_entry_id == anchor_slot.  Standalone
	 * single-driver entries leave team_entry_id at its -1 default
	 * so every group-iteration path short-circuits to the legacy
	 * 1:1 conn->car invariant.
	 */
	if (s->force_entry_list) {
		int next_free = loaded;
		for (i = 0; i < (size_t)loaded; i++) {
			struct CarEntry *anchor = &s->cars[i];
			int dn = anchor->driver_count;
			int companion;

			if (dn <= 1)
				continue;
			if (next_free + (dn - 1) > ACC_MAX_CARS) {
				log_warn("entrylist: team entry %zu needs "
				    "%d slots but %d remaining; "
				    "truncating team", i, dn,
				    ACC_MAX_CARS - next_free);
				dn = ACC_MAX_CARS - next_free + 1;
				if (dn < 2)
					break;
			}
			anchor->team_entry_id = (int8_t)i;
			/*
			 * Initial swap_state[] for the anchor: all team
			 * drivers at state 2 (CONNECTED / IN_TEAM).
			 * Pcap (run_swap_multi.sh, 2026-05-13) shows
			 * kunos's first 0x47 for the anchor car carries
			 * [2, 2] for a 2-driver team.  Companions
			 * (below) get [2, 1, 1, ...] — driver 0 at
			 * state 2 plus driver 1+ at state 1 (REGISTERED
			 * but-not-in-this-seat).
			 */
			for (int d = 0; d < anchor->driver_count &&
			    d < ACC_MAX_DRIVERS_PER_CAR; d++)
				anchor->swap_state[d] = 2;
			for (companion = 1; companion < dn; companion++) {
				int cslot = next_free++;
				struct CarEntry *c = &s->cars[cslot];
				int d;

				/* car_id is server-assigned per slot index;
				 * keep it (server_init set it). */
				c->race_number = anchor->race_number;
				c->car_model = anchor->car_model;
				c->forced_car_model = anchor->forced_car_model;
				c->cup_category = anchor->cup_category;
				c->nationality = anchor->nationality;
				memcpy(c->team_name, anchor->team_name,
				    sizeof(c->team_name));
				c->default_grid_position =
				    anchor->default_grid_position;
				c->ballast_kg = anchor->ballast_kg;
				c->restrictor = anchor->restrictor;
				memcpy(c->custom_car, anchor->custom_car,
				    sizeof(c->custom_car));
				c->current_driver_index =
				    anchor->current_driver_index;
				c->driver_count = anchor->driver_count;
				c->is_server_admin = anchor->is_server_admin;
				memcpy(c->drivers, anchor->drivers,
				    sizeof(c->drivers));
				c->team_entry_id = (int8_t)i;
				/*
				 * Companion's initial swap_state:
				 *   driver 0 = 2 (the team's primary slot)
				 *   driver 1+ = 1 (registered)
				 * Matches kunos's [2, 1] for the companion
				 * car in the run_swap_multi pcap.
				 */
				c->swap_state[0] = 2;
				for (d = 1; d < c->driver_count &&
				    d < ACC_MAX_DRIVERS_PER_CAR; d++)
					c->swap_state[d] = 1;
			}
			log_info("entrylist: team entry %zu (race %d) "
			    "expanded to %d slots", i,
			    (int)anchor->race_number, dn);
		}
	}

	json_free(root);
	log_info("entrylist: loaded %d cars from %s", loaded, path);
	return loaded;
}

/*
 * Write a JSON escape of s into out (NUL-terminated).  Enough for
 * the fields we emit (names, steam IDs) — handles ASCII controls,
 * quote, backslash.  Caller's buffer must be big enough; we cap at
 * roughly 2x input.
 */
static void
json_escape(char *out, size_t outsz, const char *s)
{
	size_t o = 0;

	if (s == NULL) {
		if (outsz > 0) out[0] = '\0';
		return;
	}
	for (; *s != '\0' && o + 2 < outsz; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			if (o + 3 >= outsz) break;
			out[o++] = '\\';
			out[o++] = (char)c;
		} else if (c < 0x20) {
			if (o + 7 >= outsz) break;
			o += (size_t)snprintf(out + o, outsz - o,
			    "\\u%04x", c);
		} else {
			out[o++] = (char)c;
		}
	}
	if (o < outsz)
		out[o] = '\0';
	else if (outsz > 0)
		out[outsz - 1] = '\0';
}

int
entrylist_save(const struct Server *s, const char *cfg_dir)
{
	char path[512];
	char tmp_path[520];
	FILE *fp;
	int i, first_entry = 1;

	snprintf(path, sizeof(path), "%s/entrylist.json", cfg_dir);
	fp = atomic_open(tmp_path, sizeof(tmp_path), path,
	    "entrylist_save");
	if (fp == NULL)
		return -1;
	fputs("{\n  \"entries\": [\n", fp);
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		const struct CarEntry *car = &s->cars[i];
		int d;
		char buf[192];

		if (!car->used)
			continue;
		/*
		 * Team-entry companion slots share the anchor's entry-
		 * level fields (raceNumber, car_model, drivers[], ...).
		 * Emit only once per group — at the anchor — so the next
		 * load doesn't see each companion as a fresh entry.
		 * team_entry_id == self means anchor; > anchor means
		 * companion.
		 */
		if (car->team_entry_id >= 0 && car->team_entry_id != i)
			continue;
		if (!first_entry)
			fputs(",\n", fp);
		first_entry = 0;
		fputs("    {\n", fp);
		fprintf(fp, "      \"raceNumber\": %d,\n",
		    (int)car->race_number);
		fprintf(fp, "      \"forcedCarModel\": %d,\n",
		    car->forced_car_model == 0xff ? -1
		    : (int)car->forced_car_model);
		fprintf(fp, "      \"overrideCarModelForCustomCar\": %d,\n",
		    (int)car->override_car_model_custom);
		fprintf(fp, "      \"defaultGridPosition\": %d,\n",
		    car->default_grid_position < 0 ? 0
		    : car->default_grid_position + 1);
		fprintf(fp, "      \"ballastKg\": %d,\n",
		    (int)car->ballast_kg);
		fprintf(fp, "      \"restrictor\": %d,\n",
		    (int)(car->restrictor * 100.0f + 0.5f));
		fprintf(fp, "      \"defaultDriverIndex\": %d,\n",
		    (int)car->current_driver_index);
		json_escape(buf, sizeof(buf), car->team_name);
		fprintf(fp, "      \"teamName\": \"%s\",\n", buf);
		if (car->custom_car[0] != '\0') {
			json_escape(buf, sizeof(buf), car->custom_car);
			fprintf(fp, "      \"customCar\": \"%s\",\n", buf);
		}
		fprintf(fp, "      \"overrideDriverInfo\": %d,\n",
		    car->used ? 1 : 0);
		fprintf(fp, "      \"isServerAdmin\": %d,\n",
		    car->is_server_admin ? 1 : 0);
		fputs("      \"drivers\": [\n", fp);
		for (d = 0; d < car->driver_count; d++) {
			const struct DriverInfo *di = &car->drivers[d];

			if (d > 0)
				fputs(",\n", fp);
			fputs("        {\n", fp);
			json_escape(buf, sizeof(buf), di->first_name);
			fprintf(fp, "          \"firstName\": \"%s\",\n", buf);
			json_escape(buf, sizeof(buf), di->last_name);
			fprintf(fp, "          \"lastName\": \"%s\",\n", buf);
			json_escape(buf, sizeof(buf), di->short_name);
			fprintf(fp, "          \"shortName\": \"%s\",\n", buf);
			json_escape(buf, sizeof(buf), di->steam_id);
			fprintf(fp, "          \"playerID\": \"%s\",\n", buf);
			fprintf(fp, "          \"driverCategory\": %d,\n",
			    (int)di->driver_category);
			fprintf(fp, "          \"nationality\": %d\n",
			    (int)di->nationality);
			fputs("        }", fp);
		}
		fputs("\n      ]\n    }", fp);
	}
	fprintf(fp, "\n  ],\n  \"forceEntryList\": %d,\n  \"configVersion\": 1\n}\n",
	    s->force_entry_list ? 1 : 0);
	if (atomic_close(fp, tmp_path, path, "entrylist_save") < 0)
		return -1;
	log_info("entrylist_save: wrote %s", path);
	return 0;
}

/*
 * Load + range-clamp + log cfg/bop.json.  The table is NOT applied to
 * any car: the original server likewise only loads, clamps, and logs
 * its BoP vector and never writes it to a CarEntry (the applier was
 * never wired up).  Kept for forward-compat and operator visibility.
 */
int
bop_load(struct Server *s, const char *cfg_dir)
{
	char path[512];
	char *raw, *utf8;
	size_t rawlen;
	char err[256] = "";
	struct json_node *root;
	const struct json_node *entries;
	size_t i, n;

	s->bop_count = 0;

	snprintf(path, sizeof(path), "%s/bop.json", cfg_dir);
	raw = read_file(path, &rawlen);
	if (raw == NULL) {
		/* Optional file: not present is normal. */
		return 0;
	}
	utf8 = decode_cfg_bytes(raw, rawlen);
	free(raw);
	if (utf8 == NULL) {
		log_warn("bop: decode failed for %s", path);
		return -1;
	}
	root = json_parse(utf8, strlen(utf8), err, sizeof(err));
	free(utf8);
	if (root == NULL) {
		log_warn("bop: parse failed: %s", err);
		return -1;
	}

	entries = json_obj_get(root, "entries");
	n = json_arr_len(entries);
	for (i = 0; i < n && s->bop_count < ACC_MAX_BOP; i++) {
		const struct json_node *e = json_arr_at(entries, i);
		const char *tname = json_obj_get_str(e, "track");
		int model = json_obj_get_int(e, "carModel", -1);
		int kg = json_obj_get_int(e, "ballastKg", 0);
		int rest = json_obj_get_int(e, "restrictor", 0);
		struct BoPEntry *out;

		if (tname == NULL || model < 0 || model > 255)
			continue;
		/* Clamp to the exe's BoP range: ballast [-40, 40] kg,
		 * restrictor [0, 20] % (FUN_140023700). */
		if (kg < -40)
			kg = -40;
		if (kg > 40)
			kg = 40;
		if (rest < 0)
			rest = 0;
		if (rest > 20)
			rest = 20;
		out = &s->bop[s->bop_count++];
		snprintf(out->track, sizeof(out->track), "%s", tname);
		out->car_model = (uint8_t)model;
		out->ballast_kg = (int8_t)kg;
		out->restrictor_pct = (uint8_t)rest;
	}
	json_free(root);
	log_info("bop.json: %d entries loaded", s->bop_count);
	return s->bop_count;
}
