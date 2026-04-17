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
 * ratings.c -- local Safety / Trust rating store (see ratings.h).
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"
#include "log.h"
#include "ratings.h"
#include "state.h"

#define RATINGS_NEUTRAL		5000	/* 50.00 × 100 */
#define RATINGS_MAX		9999	/* 99.99 × 100 */
#define RATINGS_DELTA_CLEAN	5
#define RATINGS_DELTA_CUT	25

static void
ratings_path(const struct Server *s, char *buf, size_t bufsz)
{
	if (s->cfg_dir[0] == '\0')
		snprintf(buf, bufsz, "cfg/ratings.json");
	else
		snprintf(buf, bufsz, "%s/ratings.json", s->cfg_dir);
}

static struct RatingEntry *
lookup(struct Server *s, const char *steam_id, int create)
{
	int i, free_slot = -1;

	if (steam_id == NULL || steam_id[0] == '\0')
		return NULL;
	for (i = 0; i < ACC_RATINGS_MAX; i++) {
		struct RatingEntry *e = &s->ratings[i];
		if (e->steam_id[0] == '\0') {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (strcmp(e->steam_id, steam_id) == 0)
			return e;
	}
	if (!create || free_slot < 0)
		return NULL;
	{
		struct RatingEntry *e = &s->ratings[free_slot];
		snprintf(e->steam_id, sizeof(e->steam_id), "%s",
		    steam_id);
		e->sa_x100 = RATINGS_NEUTRAL;
		e->tr_x100 = RATINGS_NEUTRAL;
		return e;
	}
}

void
ratings_load(struct Server *s)
{
	char path[320];
	FILE *f;
	long size;
	char *buf = NULL;
	struct json_node *root = NULL;
	char err[128];

	ratings_path(s, path, sizeof(path));
	f = fopen(path, "rb");
	if (f == NULL)
		return;
	if (fseek(f, 0, SEEK_END) != 0)
		goto done;
	size = ftell(f);
	if (size <= 0 || size > (long)(1 << 20))
		goto done;
	rewind(f);
	buf = malloc((size_t)size + 1);
	if (buf == NULL)
		goto done;
	if ((long)fread(buf, 1, (size_t)size, f) != size)
		goto done;
	buf[size] = '\0';
	root = json_parse(buf, (size_t)size, err, sizeof(err));
	if (root == NULL) {
		log_warn("ratings_load: parse %s: %s", path, err);
		goto done;
	}
	if (root->kind == JSON_OBJ) {
		size_t i;
		for (i = 0; i < root->u.obj.count; i++) {
			const struct json_pair *p = &root->u.obj.pairs[i];
			struct RatingEntry *e;
			int sa, tr;

			if (p->val == NULL || p->val->kind != JSON_OBJ)
				continue;
			e = lookup(s, p->key, 1);
			if (e == NULL)
				break;		/* store full */
			sa = json_obj_get_int(p->val, "sa",
			    RATINGS_NEUTRAL);
			tr = json_obj_get_int(p->val, "tr",
			    RATINGS_NEUTRAL);
			if (sa < 0) sa = 0;
			if (sa > RATINGS_MAX) sa = RATINGS_MAX;
			if (tr < 0) tr = 0;
			if (tr > RATINGS_MAX) tr = RATINGS_MAX;
			e->sa_x100 = (uint16_t)sa;
			e->tr_x100 = (uint16_t)tr;
		}
	}
	log_info("ratings_load: %s", path);
done:
	if (root != NULL)
		json_free(root);
	free(buf);
	fclose(f);
}

void
ratings_save(struct Server *s)
{
	char path[320];
	char tmp[336];
	FILE *f;
	int i, first = 1;

	ratings_path(s, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "wb");
	if (f == NULL) {
		log_warn("ratings_save: open %s: %s", tmp,
		    strerror(errno));
		return;
	}
	fputs("{\n", f);
	for (i = 0; i < ACC_RATINGS_MAX; i++) {
		const struct RatingEntry *e = &s->ratings[i];
		if (e->steam_id[0] == '\0')
			continue;
		if (!first)
			fputs(",\n", f);
		fprintf(f,
		    "  \"%s\": { \"sa\": %u, \"tr\": %u }",
		    e->steam_id,
		    (unsigned)e->sa_x100, (unsigned)e->tr_x100);
		first = 0;
	}
	fputs("\n}\n", f);
	if (fclose(f) != 0) {
		log_warn("ratings_save: write %s: %s", tmp,
		    strerror(errno));
		(void)unlink(tmp);
		return;
	}
	if (rename(tmp, path) != 0) {
		log_warn("ratings_save: rename %s: %s", path,
		    strerror(errno));
		(void)unlink(tmp);
		return;
	}
}

void
ratings_get(const struct Server *s, const char *steam_id,
    uint16_t *sa, uint16_t *tr)
{
	int i;

	if (sa != NULL) *sa = RATINGS_NEUTRAL;
	if (tr != NULL) *tr = RATINGS_NEUTRAL;
	if (steam_id == NULL || steam_id[0] == '\0')
		return;
	for (i = 0; i < ACC_RATINGS_MAX; i++) {
		const struct RatingEntry *e = &s->ratings[i];
		if (e->steam_id[0] == '\0')
			continue;
		if (strcmp(e->steam_id, steam_id) == 0) {
			if (sa != NULL) *sa = e->sa_x100;
			if (tr != NULL) *tr = e->tr_x100;
			return;
		}
	}
}

void
ratings_on_lap(struct Server *s, const char *steam_id,
    int has_cut, int is_out_lap)
{
	struct RatingEntry *e;

	if (is_out_lap)
		return;			/* out laps are pace-neutral */
	e = lookup(s, steam_id, 1);
	if (e == NULL)
		return;
	if (has_cut) {
		if (e->sa_x100 > RATINGS_DELTA_CUT)
			e->sa_x100 -= RATINGS_DELTA_CUT;
		else
			e->sa_x100 = 0;
	} else {
		if ((uint32_t)e->sa_x100 + RATINGS_DELTA_CLEAN <=
		    RATINGS_MAX)
			e->sa_x100 += RATINGS_DELTA_CLEAN;
		else
			e->sa_x100 = RATINGS_MAX;
	}
	s->ratings_dirty = 1;
}

int
ratings_is_dirty(const struct Server *s)
{
	return s->ratings_dirty;
}

void
ratings_clear_dirty(struct Server *s)
{
	s->ratings_dirty = 0;
}
