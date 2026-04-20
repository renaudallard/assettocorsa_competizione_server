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
 * results.c -- session results JSON file writer.
 *
 * Pure printf, no library.  Writes a flat JSON tree matching
 * §9 of NOTEBOOK_B.md and the format produced by the Kunos
 * server (modulo field order, which is not significant in JSON).
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "penalty.h"
#include "results.h"
#include "state.h"

static void
fprint_json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s != '\0'; s++) {
		switch (*s) {
		case '"':	fputs("\\\"", f); break;
		case '\\':	fputs("\\\\", f); break;
		case '\b':	fputs("\\b", f); break;
		case '\f':	fputs("\\f", f); break;
		case '\n':	fputs("\\n", f); break;
		case '\r':	fputs("\\r", f); break;
		case '\t':	fputs("\\t", f); break;
		default:
			if ((unsigned char)*s < 0x20)
				fprintf(f, "\\u%04x", (unsigned char)*s);
			else
				fputc(*s, f);
			break;
		}
	}
	fputc('"', f);
}

static const char *
session_type_str(uint8_t t)
{
	switch (t) {
	case 0:		return "FP";
	case 4:		return "Q";
	case 10:	return "R";
	default:	return "FP";
	}
}

int
results_write(struct Server *s)
{
	char dir[256];
	char path[512];
	char ts[32];
	struct tm tm;
	time_t now;
	FILE *f;
	int i, first;
	uint8_t st;
	uint8_t sidx = s->session.session_index;

	if (sidx >= s->session_count)
		return -1;
	st = s->sessions[sidx].session_type;

	/* Ensure results/ directory exists. */
	snprintf(dir, sizeof(dir), "results");
	if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
		log_warn("results: mkdir %s: %s", dir, strerror(errno));
		return -1;
	}

	now = time(NULL);
	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);
	snprintf(path, sizeof(path), "%s/%s_%s.json",
	    dir, ts, session_type_str(st));

	f = fopen(path, "w");
	if (f == NULL) {
		log_warn("results: fopen %s: %s", path, strerror(errno));
		return -1;
	}

	fprintf(f, "{\n");
	fprintf(f, "  \"sessionType\": \"%s\",\n", session_type_str(st));
	fprintf(f, "  \"trackName\": ");
	fprint_json_str(f, s->track);
	fprintf(f, ",\n");
	fprintf(f, "  \"sessionIndex\": %u,\n", (unsigned)sidx);
	fprintf(f, "  \"raceWeekendIndex\": 0,\n");
	fprintf(f, "  \"serverName\": ");
	fprint_json_str(f, s->server_name);
	fprintf(f, ",\n");
	fprintf(f, "  \"sessionResult\": {\n");
	fprintf(f, "    \"bestlap\": %d,\n",
	    s->cars[0].used ? s->cars[0].race.best_lap_ms : 0);
	fprintf(f, "    \"bestSplits\": [%d, %d, %d],\n",
	    s->cars[0].used ? s->cars[0].race.best_sectors_ms[0] : 0,
	    s->cars[0].used ? s->cars[0].race.best_sectors_ms[1] : 0,
	    s->cars[0].used ? s->cars[0].race.best_sectors_ms[2] : 0);
	fprintf(f, "    \"isWetSession\": 0,\n");
	fprintf(f, "    \"type\": 0,\n");
	fprintf(f, "    \"leaderBoardLines\": [");

	first = 1;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		struct CarEntry *car = &s->cars[i];
		struct DriverInfo *d;

		if (!car->used)
			continue;
		if (!first)
			fprintf(f, ",");
		fprintf(f, "\n      {\n");
		fprintf(f, "        \"car\": {\n");
		fprintf(f, "          \"carId\": %u,\n", car->car_id);
		fprintf(f, "          \"raceNumber\": %d,\n",
		    car->race_number);
		fprintf(f, "          \"carModel\": %u,\n", car->car_model);
		fprintf(f, "          \"cupCategory\": %u,\n",
		    car->cup_category);
		fprintf(f, "          \"teamName\": ");
		fprint_json_str(f, car->team_name);
		fprintf(f, ",\n");
		fprintf(f, "          \"nationality\": %u,\n",
		    car->nationality);
		fprintf(f, "          \"carGuid\": -1,\n");
		fprintf(f, "          \"teamGuid\": -1,\n");
		fprintf(f, "          \"drivers\": [");
		{
			int dj;
			int dfirst = 1;

			for (dj = 0; dj < car->driver_count &&
			    dj < ACC_MAX_DRIVERS_PER_CAR; dj++) {
				d = &car->drivers[dj];
				if (!dfirst) fprintf(f, ",");
				fprintf(f, "\n            {\n");
				fprintf(f, "              \"firstName\": ");
				fprint_json_str(f, d->first_name);
				fprintf(f, ",\n");
				fprintf(f, "              \"lastName\": ");
				fprint_json_str(f, d->last_name);
				fprintf(f, ",\n");
				fprintf(f, "              \"shortName\": ");
				fprint_json_str(f, d->short_name);
				fprintf(f, ",\n");
				fprintf(f, "              \"playerId\": ");
				fprint_json_str(f, d->steam_id);
				fprintf(f, "\n");
				fprintf(f, "            }");
				dfirst = 0;
			}
		}
		fprintf(f, "\n          ]\n");
		fprintf(f, "        },\n");
		fprintf(f, "        \"currentDriver\": {\n");
		d = &car->drivers[car->current_driver_index <
		    car->driver_count ? car->current_driver_index : 0];
		fprintf(f, "          \"firstName\": ");
		fprint_json_str(f, d->first_name);
		fprintf(f, ",\n");
		fprintf(f, "          \"lastName\": ");
		fprint_json_str(f, d->last_name);
		fprintf(f, ",\n");
		fprintf(f, "          \"shortName\": ");
		fprint_json_str(f, d->short_name);
		fprintf(f, ",\n");
		fprintf(f, "          \"playerId\": ");
		fprint_json_str(f, d->steam_id);
		fprintf(f, "\n");
		fprintf(f, "        },\n");
		fprintf(f, "        \"currentDriverIndex\": %u,\n",
		    car->current_driver_index);
		fprintf(f, "        \"timing\": {\n");
		fprintf(f, "          \"lastLap\": %d,\n",
		    car->race.last_lap_ms);
		fprintf(f, "          \"lastSplits\": [%d, %d, %d],\n",
		    car->race.sector_ms[0], car->race.sector_ms[1],
		    car->race.sector_ms[2]);
		fprintf(f, "          \"bestLap\": %d,\n",
		    car->race.best_lap_ms);
		fprintf(f, "          \"bestSplits\": [%d, %d, %d],\n",
		    car->race.best_sectors_ms[0],
		    car->race.best_sectors_ms[1],
		    car->race.best_sectors_ms[2]);
		fprintf(f, "          \"totalTime\": %d,\n",
		    car->race.race_time_ms);
		fprintf(f, "          \"lapCount\": %d,\n",
		    car->race.lap_count);
		fprintf(f, "          \"lastSplitId\": 0\n");
		fprintf(f, "        },\n");
		fprintf(f, "        \"missingMandatoryPitstop\": %d,\n",
		    car->race.mandatory_pit_served ? 0 : 1);
		fprintf(f, "        \"driverPenalties\": [");
		{
			int pi;
			int pfirst = 1;
			for (pi = 0; pi < car->race.pen.count; pi++) {
				const struct PenaltyEntry *p =
				    &car->race.pen.slots[pi];
				if (!pfirst)
					fprintf(f, ",");
				fprintf(f, "\n          {");
				fprintf(f, " \"penalty\": ");
				fprint_json_str(f, penalty_name(p->kind));
				fprintf(f, ", \"reason\": %u, \"served\": %d,"
				    " \"wireValue\": %u }",
				    (unsigned)p->reason, (int)p->served,
				    (unsigned)penalty_wire_value(
					p->kind, p->reason));
				pfirst = 0;
			}
		}
		fprintf(f, "\n        ]\n");
		fprintf(f, "      }");
		first = 0;
	}
	fprintf(f, "\n    ]\n");
	fprintf(f, "  }\n");
	fprintf(f, "}\n");
	if (fclose(f) != 0) {
		log_warn("results: fclose %s: %s", path, strerror(errno));
		return -1;
	}

	log_info("results: wrote %s", path);
	return 0;
}
