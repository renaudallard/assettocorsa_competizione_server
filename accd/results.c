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
#include <unistd.h>

#include "io.h"
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
	char tmp_path[520];
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
	/*
	 * Two sessions of the same type closing in the same wall-clock
	 * second (admin DQ chain, force advance) used to overwrite the
	 * earlier file via the atomic_open rename.  If the file already
	 * exists, append "_2", "_3", ... until a free name turns up.
	 */
	{
		struct stat sb;
		int suffix;

		for (suffix = 2; suffix < 100 && stat(path, &sb) == 0;
		    suffix++)
			snprintf(path, sizeof(path), "%s/%s_%s_%d.json",
			    dir, ts, session_type_str(st), suffix);
	}

	f = atomic_open(tmp_path, sizeof(tmp_path), path, "results");
	if (f == NULL)
		return -1;

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
	fprintf(f, "  \"metaData\": ");
	fprint_json_str(f, s->meta_data);
	fprintf(f, ",\n");
	fprintf(f, "  \"sessionResult\": {\n");
	{
		int32_t best_lap = 0;
		int32_t best_sec[3] = { 0, 0, 0 };
		int k;

		for (k = 0; k < ACC_MAX_CARS; k++) {
			const struct CarRaceState *r = &s->cars[k].race;
			int j;

			if (r->disqualified)
				continue;
			if (r->best_lap_ms > 0 &&
			    (best_lap == 0 || r->best_lap_ms < best_lap))
				best_lap = r->best_lap_ms;
			for (j = 0; j < 3; j++)
				if (r->best_sectors_ms[j] > 0 &&
				    (best_sec[j] == 0 ||
				    r->best_sectors_ms[j] < best_sec[j]))
					best_sec[j] = r->best_sectors_ms[j];
		}
		fprintf(f, "    \"bestlap\": %d,\n", (int)best_lap);
		fprintf(f, "    \"bestSplits\": [%d, %d, %d],\n",
		    (int)best_sec[0], (int)best_sec[1], (int)best_sec[2]);
	}
	fprintf(f, "    \"isWetSession\": 0,\n");
	fprintf(f, "    \"type\": 0,\n");
	fprintf(f, "    \"leaderBoardLines\": [");

	first = 1;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		struct CarEntry *car = &s->cars[i];
		struct DriverInfo *d;

		/*
		 * Emit any slot with an identity (driver_count > 0),
		 * not just currently-connected ones — a driver who ran
		 * valid laps and then disconnected should still appear
		 * in the results file.  session_reset zeroes race state
		 * between sessions so we can't leak stale data here.
		 */
		if (car->driver_count == 0)
			continue;
		if (!first)
			fprintf(f, ",");
		fprintf(f, "\n      {\n");
		fprintf(f, "        \"position\": %d,\n",
		    (int)car->race.position);
		fprintf(f, "        \"disqualified\": %s,\n",
		    car->race.disqualified ? "true" : "false");
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
		/*
		 * lastSplits must reflect the splits of the most recently
		 * completed lap.  Read from last_lap_splits_ms (snapshot
		 * taken in h_sector_split_single before the per-lap
		 * sector_ms reset) — reading sector_ms directly would
		 * yield [0,0,0] right after a lap completion, or the
		 * in-progress partial splits of the lap currently being
		 * driven (e.g. [s0, s1, 0]) if dumped mid-lap.
		 */
		fprintf(f, "          \"lastSplits\": [%d, %d, %d],\n",
		    car->race.last_lap_splits_ms[0],
		    car->race.last_lap_splits_ms[1],
		    car->race.last_lap_splits_ms[2]);
		fprintf(f, "          \"bestLap\": %d,\n",
		    car->race.best_lap_ms);
		fprintf(f, "          \"bestSplits\": [%d, %d, %d],\n",
		    car->race.best_sectors_ms[0],
		    car->race.best_sectors_ms[1],
		    car->race.best_sectors_ms[2]);
		fprintf(f, "          \"totalTime\": %d,\n",
		    car->race.race_time_ms);
		fprintf(f, "          \"totalPenaltyMs\": %u,\n",
		    (unsigned)penalty_total_ms(&car->race.pen));
		fprintf(f, "          \"lapCount\": %d,\n",
		    car->race.lap_count);
		fprintf(f, "          \"lastSplitId\": 0\n");
		fprintf(f, "        },\n");
		/*
		 * missingMandatoryPitstop only applies to races with a
		 * configured mandatoryPitstopCount; in practice / qualy
		 * or a race with pit_count=0 there is nothing to miss, so
		 * emit 0 to avoid a stats-service bug where every car is
		 * reported as non-compliant.
		 */
		fprintf(f, "        \"missingMandatoryPitstop\": %d,\n",
		    (st == 10 && s->mandatory_pit_count > 0 &&
			car->race.mandatory_pit_served <
			    s->mandatory_pit_count) ? 1 : 0);
		fprintf(f, "        \"towPenalty\": %d,\n",
		    (int)car->race.in_tow);
		/*
		 * driverTotalTimes[] per handbook §VIII.1 — per-driver
		 * accumulated stint time (ms) for endurance / driver-
		 * swap classification.  Populated from
		 * driver_stint_ms[d].
		 */
		fprintf(f, "        \"driverTotalTimes\": [");
		{
			int dj;
			int dfirst = 1;

			for (dj = 0; dj < car->driver_count &&
			    dj < ACC_MAX_DRIVERS_PER_CAR; dj++) {
				if (!dfirst)
					fprintf(f, ", ");
				fprintf(f, "%d",
				    (int)car->race.driver_stint_ms[dj]);
				dfirst = 0;
			}
		}
		fprintf(f, "]\n");
		fprintf(f, "      }");
		first = 0;
	}
	fprintf(f, "\n    ]\n");
	fprintf(f, "  },\n");

	/*
	 * Top-level laps[] per handbook §VIII.1 — flat list of every
	 * lap any car completed in this session, in chronological
	 * order (we approximate by walking each car's ring buffer in
	 * order).  isValidForBest = 0 ms entry means we recorded the
	 * sentinel for an invalid lap; treat negative / zero as
	 * invalid.
	 */
	fprintf(f, "  \"laps\": [");
	{
		int lap_first = 1, ci, hi;

		for (ci = 0; ci < ACC_MAX_CARS && ci < s->max_connections;
		    ci++) {
			struct CarEntry *cc = &s->cars[ci];
			int hcount;

			if (cc->driver_count == 0)
				continue;
			hcount = cc->race.lap_history_count > ACC_LAP_HISTORY
			    ? ACC_LAP_HISTORY
			    : (int)cc->race.lap_history_count;
			/*
			 * Wrap-aware ring walk: once the count exceeds
			 * ACC_LAP_HISTORY the oldest retained lap is at
			 * count % ACC_LAP_HISTORY, not at slot 0.  Without
			 * this the JSON laps[] reorders later laps in front
			 * of earlier ones for any car with > 16 laps.
			 */
			{
			uint32_t total = cc->race.lap_history_count;
			int start = total <= (uint32_t)ACC_LAP_HISTORY
			    ? 0 : (int)(total % ACC_LAP_HISTORY);
			for (hi = 0; hi < hcount; hi++) {
				int idx = (start + hi) % ACC_LAP_HISTORY;
				int lap_ms = cc->race.lap_history_ms[idx];
				int s0 = cc->race.lap_splits_ms[idx][0];
				int s1 = cc->race.lap_splits_ms[idx][1];
				int s2 = cc->race.lap_splits_ms[idx][2];
				int valid = (lap_ms > 0 &&
				    lap_ms != 0x7fffffff);

				if (!lap_first)
					fprintf(f, ",");
				fprintf(f, "\n    {");
				fprintf(f, " \"carId\": %u,", cc->car_id);
				fprintf(f, " \"driverIndex\": %u,",
				    (unsigned)cc->current_driver_index);
				fprintf(f, " \"laptime\": %d,",
				    valid ? lap_ms : 0);
				fprintf(f, " \"isValidForBest\": %s,",
				    valid ? "true" : "false");
				fprintf(f, " \"splits\": [%d, %d, %d]",
				    s0, s1, s2);
				fprintf(f, " }");
				lap_first = 0;
			}
			}
		}
	}
	fprintf(f, "\n  ],\n");

	/*
	 * Top-level penalties[] per handbook §VIII.1.  carId +
	 * driverIndex name the receiver, reason / penalty / penaltyValue
	 * describe the kind, served / violationInLap / clearedInLap
	 * track its lifecycle.  We don't yet track which lap each
	 * penalty was issued on or served on, so violationInLap and
	 * clearedInLap default to -1 (unknown).
	 */
	fprintf(f, "  \"penalties\": [");
	{
		int pen_first = 1, ci, pi;

		for (ci = 0; ci < ACC_MAX_CARS && ci < s->max_connections;
		    ci++) {
			struct CarEntry *cc = &s->cars[ci];

			if (cc->driver_count == 0)
				continue;
			for (pi = 0; pi < cc->race.pen.count; pi++) {
				const struct PenaltyEntry *p =
				    &cc->race.pen.slots[pi];
				const char *pname = "Unknown";
				int pvalue = 0;

				switch (p->kind) {
				case PEN_DT: case PEN_DTC:
					pname = "DriveThrough"; pvalue = 3;
					break;
				case PEN_SG10: case PEN_SG10C:
					pname = "StopAndGo_10"; pvalue = 10;
					break;
				case PEN_SG20: case PEN_SG20C:
					pname = "StopAndGo_20"; pvalue = 20;
					break;
				case PEN_SG30: case PEN_SG30C:
					pname = "StopAndGo_30"; pvalue = 30;
					break;
				case PEN_TP5:
					pname = "TimePenalty_5"; pvalue = 5;
					break;
				case PEN_TP15:
					pname = "TimePenalty_15"; pvalue = 15;
					break;
				default:
					break;
				}
				if (!pen_first)
					fprintf(f, ",");
				fprintf(f, "\n    {");
				fprintf(f, " \"carId\": %u,", cc->car_id);
				fprintf(f, " \"driverIndex\": %u,",
				    (unsigned)cc->current_driver_index);
				fprintf(f, " \"reason\": %u,",
				    (unsigned)p->reason);
				fprintf(f, " \"penalty\": ");
				fprint_json_str(f, pname);
				fprintf(f, ", \"penaltyValue\": %d,",
				    pvalue);
				fprintf(f, " \"served\": %s,",
				    p->served ? "true" : "false");
				fprintf(f, " \"violationInLap\": -1,");
				fprintf(f, " \"clearedInLap\": -1");
				fprintf(f, " }");
				pen_first = 0;
			}
		}
	}
	fprintf(f, "\n  ]\n");
	fprintf(f, "}\n");
	if (atomic_close(f, tmp_path, path, "results") < 0)
		return -1;

	log_info("results: wrote %s", path);
	return 0;
}
