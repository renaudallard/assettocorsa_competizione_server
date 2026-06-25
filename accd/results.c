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
#include <stdlib.h>
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
	case 9:		return "SP";
	case 10:	return "R";
	default:	return "FP";
	}
}

/*
 * Map a penalty's AC2 category to the kunos results.json reason string.
 * The exe stores the category on each PenaltySheet entry (+0x58) and the
 * results writer (FUN_140129b10) renders it through the cat -> name table
 * FUN_140117330, independent of the wire-code path.  This is a direct
 * mirror of that table; unknown categories fall back to "None" like the
 * exe default.  Note cats 11/12/13 differ from the wire dispatcher: the
 * label table calls 11 IgnoredMandatoryPit, 12 ExceededDriverStintLimit
 * and 13 DriverRanNoStint, so the category (not accd's wire-semantic
 * reason) is the correct source for this string.
 */
static const char *
penalty_category_label(uint8_t category)
{
	switch (category) {
	case 0:		return "Cutting";
	case 1:		return "Collision";
	case 2:		return "IllegalOvertake";
	case 3:		return "PitSpeeding";
	case 4:		return "PitEntry";
	case 5:		return "PitExit";
	case 6:
	case 11:	return "IgnoredMandatoryPit";
	case 7:		return "UnsafeRejoin";
	case 8:		return "Trolling";
	case 9:		return "ReverseInPitlane";
	case 10:	return "WrongWay";
	case 12:	return "ExceededDriverStintLimit";
	case 13:	return "DriverRanNoStint";
	default:	return "None";
	}
}

/*
 * Map a penalty_kind to the results.json penalty name.
 * penaltyValue (PenaltySheet+0x5c = param_3 in FUN_140125f50) is
 * always 0 in the exe; all callers pass 0 for param_3.
 */
static void
pen_kind_json(uint8_t kind, const char **name, int *value)
{
	*value = 0;
	switch (kind) {
	case PEN_DT: case PEN_DTC:	*name = "DriveThrough"; break;
	case PEN_SG10: case PEN_SG10C:	*name = "StopAndGo_10"; break;
	case PEN_SG20: case PEN_SG20C:	*name = "StopAndGo_20"; break;
	case PEN_SG30: case PEN_SG30C:	*name = "StopAndGo_30"; break;
	case PEN_TP5: case PEN_TP15:
	case PEN_TP30: case PEN_TP40:
	case PEN_TP50: case PEN_TP60:	*name = "PostRaceTime"; break;
	case PEN_DQ:			*name = "Disqualified"; break;
	case PEN_RBL:			*name = "RemoveBestLaptime"; break;
	default:			*name = "None"; break;
	}
}

void
results_laps_append(struct Server *s, uint16_t car_id, uint8_t driver_index,
    int32_t lap_time_ms, const int32_t splits_ms[3], int is_valid)
{
	struct ResultsLap *rec;

	if (s->results_lap_count >= ACC_RESULTS_LAP_MAX) {
		if (!s->results_lap_overflow) {
			s->results_lap_overflow = 1;
			log_warn("results: per-session lap log hit %u, "
			    "ignoring further laps",
			    (unsigned)ACC_RESULTS_LAP_MAX);
		}
		return;
	}
	if (s->results_lap_count == s->results_lap_cap) {
		uint32_t ncap = s->results_lap_cap ?
		    s->results_lap_cap * 2 : 64;
		struct ResultsLap *n;

		if (ncap > ACC_RESULTS_LAP_MAX)
			ncap = ACC_RESULTS_LAP_MAX;
		n = realloc(s->results_laps, (size_t)ncap * sizeof(*n));
		if (n == NULL) {
			log_warn("results: lap log realloc(%u) failed",
			    (unsigned)ncap);
			return;
		}
		s->results_laps = n;
		s->results_lap_cap = ncap;
	}
	rec = &s->results_laps[s->results_lap_count++];
	rec->car_id = car_id;
	rec->driver_index = driver_index;
	rec->is_valid = is_valid ? 1 : 0;
	rec->lap_time_ms = lap_time_ms;
	rec->splits_ms[0] = splits_ms[0];
	rec->splits_ms[1] = splits_ms[1];
	rec->splits_ms[2] = splits_ms[2];
}

void
results_laps_reset(struct Server *s)
{
	/* Keep the allocation for reuse across sessions. */
	s->results_lap_count = 0;
	s->results_lap_overflow = 0;
}

void
results_laps_free(struct Server *s)
{
	free(s->results_laps);
	s->results_laps = NULL;
	s->results_lap_count = 0;
	s->results_lap_cap = 0;
	s->results_lap_overflow = 0;
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
	/*
	 * raceWeekendIndex: the exe (FUN_14010ee90) reads an int from
	 * param_1+0x2c.  FUN_140129b10 populates it from ServerState
	 * +0xa0914, a counter that starts at 0 and is incremented by
	 * "Resetting weekend to friday night" (FUN_14002c740:73).
	 * Verified against a kunos wine result: sessionIndex 0, value 0.
	 * accd has no weekend persistence so it is always weekend 0.
	 */
	fprintf(f, "  \"raceWeekendIndex\": 0,\n");
	fprintf(f, "  \"metaData\": ");
	fprint_json_str(f, s->meta_data);
	fprintf(f, ",\n");
	fprintf(f, "  \"serverName\": ");
	fprint_json_str(f, s->server_name);
	fprintf(f, ",\n");
	/*
	 * The exe (FUN_14010ee90) writes sessionType a second time here,
	 * immediately before sessionResult.  Verified against a real kunos
	 * wine result file.  JSON parsers take the last occurrence; both
	 * values are identical so no consumer is affected.
	 */
	fprintf(f, "  \"sessionType\": \"%s\",\n", session_type_str(st));
	fprintf(f, "  \"sessionResult\": {\n");
	{
		int32_t best_lap = 0;
		int32_t best_sec[3] = { 0, 0, 0 };
		int k;

		for (k = 0; k < ACC_MAX_CARS; k++) {
			const struct CarRaceState *r = &s->cars[k].race;
			int j;

			/*
			 * Include DQ'd cars: the exe session-best path
			 * (FUN_140127850:52-74) gates only on cut/out-lap/
			 * invalid flags, with no DQ predicate, so a valid fast
			 * lap by a later-DQ'd driver still counts.  Matches the
			 * wire side (write_car_leaderboard_record).
			 */
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
	/*
	 * isWetSession is the declared-wet flag (FUN_14010ec60 reads a
	 * byte at +0x78); accd has no declared-wet concept (weather is
	 * dynamic), so it stays 0.  type is the numeric session type
	 * (+0x48), emitted and read raw, so emit accd's session_type
	 * rather than 0.
	 */
	fprintf(f, "    \"isWetSession\": 0,\n");
	fprintf(f, "    \"type\": %u,\n", (unsigned)st);
	fprintf(f, "    \"leaderBoardLines\": [");

	first = 1;
	{
	int order[ACC_MAX_CARS];
	int ord_n = 0, oi;

	/*
	 * Emit leaderBoardLines in finishing-classification order, not raw
	 * slot order: the array order conveys position (there is no explicit
	 * position field), matching the 0x36/0x3e wire which iterates by
	 * race.position.  Collect identified cars, then insertion-sort by
	 * the position the standings sort assigned (unset/0 trails).
	 */
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++)
		if (s->cars[i].driver_count > 0)
			order[ord_n++] = i;
	for (oi = 1; oi < ord_n; oi++) {
		int key = order[oi];
		int kp = s->cars[key].race.position > 0
		    ? s->cars[key].race.position : ACC_MAX_CARS + 1;
		int jj = oi - 1;
		while (jj >= 0) {
			int pp = s->cars[order[jj]].race.position > 0
			    ? s->cars[order[jj]].race.position
			    : ACC_MAX_CARS + 1;
			if (pp <= kp)
				break;
			order[jj + 1] = order[jj];
			jj--;
		}
		order[jj + 1] = key;
	}
	for (oi = 0; oi < ord_n; oi++) {
		struct CarEntry *car;
		struct DriverInfo *d;

		i = order[oi];
		car = &s->cars[i];
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
		fprintf(f, "        \"car\": {\n");
		fprintf(f, "          \"carId\": %u,\n", car->car_id);
		fprintf(f, "          \"raceNumber\": %d,\n",
		    car->race_number);
		fprintf(f, "          \"carModel\": %u,\n", car->car_model);
		fprintf(f, "          \"cupCategory\": %u,\n",
		    car->cup_category);
		/*
		 * carGroup: always emitted by the exe (FUN_14010c410) right
		 * after cupCategory; a server-level setting (settings.json).
		 */
		fprintf(f, "          \"carGroup\": ");
		fprint_json_str(f, s->car_group);
		fprintf(f, ",\n");
		fprintf(f, "          \"teamName\": ");
		fprint_json_str(f, car->team_name);
		fprintf(f, ",\n");
		/*
		 * car->nationality is never populated (always 0); the real
		 * value lives per-driver, so source it from the default
		 * driver like the exe's car/team nationality.
		 */
		fprintf(f, "          \"nationality\": %u,\n",
		    car->drivers[car->current_driver_index <
		    car->driver_count ? car->current_driver_index : 0]
		    .nationality);
		fprintf(f, "          \"carGuid\": 0,\n");
		fprintf(f, "          \"teamGuid\": 0,\n");
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
		fprintf(f, "\n          ]");
		/*
		 * ballastKg / restrictor: the exe (FUN_14010c410) appends
		 * these to the car object only when non-zero.  ballast is kg;
		 * restrictor is the normalized fraction accd stores (0..0.20).
		 */
		if (car->ballast_kg != 0)
			fprintf(f, ",\n          \"ballastKg\": %d",
			    (int)car->ballast_kg);
		if (car->restrictor != 0.0f)
			fprintf(f, ",\n          \"restrictor\": %g",
			    (double)car->restrictor);
		fprintf(f, "\n        },\n");
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
		fprintf(f, "          \"lapCount\": %d,\n",
		    car->race.lap_count);
		/*
		 * lastSplitId: how many sector splits the car completed in
		 * its current (partial) lap when the session ended.
		 * sector_ms[] holds in-progress split times; each entry is
		 * set on a split event and reset to 0 on lap completion.
		 */
		{
			int lsi = 0;
			if (car->race.sector_ms[1] != 0)
				lsi = 2;
			else if (car->race.sector_ms[0] != 0)
				lsi = 1;
			fprintf(f, "          \"lastSplitId\": %d\n", lsi);
		}
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
			car->race.mandatory_pit_served < s->mandatory_pit_count)
		    ? (int)(s->mandatory_pit_count -
			car->race.mandatory_pit_served) : 0);
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
				/*
				 * Emit as a JSON float (exe FUN_14010f660 always
				 * writes a decimal point); value stays in ms.
				 */
				fprintf(f, "%d.0",
				    (int)car->race.driver_stint_ms[dj]);
				dfirst = 0;
			}
		}
		fprintf(f, "]\n");
		fprintf(f, "      }");
		first = 0;
	}
	}
	fprintf(f, "\n    ]\n");
	fprintf(f, "  },\n");

	/*
	 * Top-level laps[] per handbook §VIII.1 — every completed lap of
	 * this session in global completion order, sourced from the
	 * per-session results log.  This mirrors the exe, whose top-level
	 * writer (FUN_14010ee90) iterates the whole OfficialTiming
	 * closed-laps vector: all laps (no 16-lap cap), interleaved across
	 * cars by completion order, invalid laps included with their real
	 * laptime and isValidForBest=false.  The 16-slot per-car ring that
	 * feeds 0x36 / 0x56 is deliberately not used here.
	 */
	fprintf(f, "  \"laps\": [");
	{
		uint32_t li;
		int lap_first = 1;

		for (li = 0; li < s->results_lap_count; li++) {
			const struct ResultsLap *lp = &s->results_laps[li];

			if (!lap_first)
				fprintf(f, ",");
			fprintf(f, "\n    {");
			fprintf(f, " \"carId\": %u,", (unsigned)lp->car_id);
			fprintf(f, " \"driverIndex\": %u,",
			    (unsigned)lp->driver_index);
			fprintf(f, " \"laptime\": %d,", lp->lap_time_ms);
			fprintf(f, " \"isValidForBest\": %s,",
			    lp->is_valid ? "true" : "false");
			fprintf(f, " \"splits\": [%d, %d, %d]",
			    lp->splits_ms[0], lp->splits_ms[1],
			    lp->splits_ms[2]);
			fprintf(f, " }");
			lap_first = 0;
		}
	}
	fprintf(f, "\n  ],\n");

	/*
	 * Top-level penalties[] per handbook §VIII.1.  carId +
	 * driverIndex name the receiver, reason / penalty / penaltyValue
	 * describe the kind, violationInLap / clearedInLap track its
	 * lifecycle.  FUN_140129b10 computes clearedInLap via a loop
	 * that starts at -1 and breaks when cleared_time < lap_start;
	 * with cleared_time=-1.0 (unserved sentinel) this yields 0 for
	 * any car that has at least one lap, so we emit 0 here too.
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
				const char *pname;
				int pvalue;

				/*
				 * The exe only emits penalties that passed
				 * through FUN_140126b50 (pit-served) or were
				 * marked by penalty_convert_race_end (race_end_tp
				 * set).  TP penalties from admin /tp* commands
				 * have served=0 and race_end_tp=0 and appear in
				 * neither array in the exe.
				 */
				if (!p->served && p->race_end_tp == 0)
					continue;
				pen_kind_json(p->kind, &pname, &pvalue);
				if (!pen_first)
					fprintf(f, ",");
				fprintf(f, "\n    {");
				fprintf(f, " \"carId\": %u,", cc->car_id);
				fprintf(f, " \"driverIndex\": %u,",
				    (unsigned)p->driver_index);
				fprintf(f, " \"reason\": ");
				fprint_json_str(f, penalty_category_label(p->category));
				fprintf(f, ",");
				fprintf(f, " \"penalty\": ");
				fprint_json_str(f, pname);
				fprintf(f, ", \"penaltyValue\": %d,",
				    pvalue);
				fprintf(f, " \"violationInLap\": %d,",
				    p->violation_lap);
				fprintf(f, " \"clearedInLap\": %d",
				    (p->cleared_lap < 0 && p->race_end_tp == 0)
				    ? 0 : p->cleared_lap);
				fprintf(f, " }");
				pen_first = 0;
			}
		}
	}
	fprintf(f, "\n  ],\n");
	/*
	 * post_race_penalties: the DT/SG penalties converted to a
	 * post-race time penalty at race end.  FUN_14010ee90 emits this
	 * as a third top-level array after penalties[].  accd flags the
	 * conversion on the same queue entry via race_end_tp, so emit
	 * those entries here with the converted TP kind.
	 */
	fprintf(f, "  \"post_race_penalties\": [");
	{
		int prp_first = 1, ci, pi;

		for (ci = 0; ci < ACC_MAX_CARS && ci < s->max_connections;
		    ci++) {
			struct CarEntry *cc = &s->cars[ci];

			if (cc->driver_count == 0)
				continue;
			for (pi = 0; pi < cc->race.pen.count; pi++) {
				const struct PenaltyEntry *p =
				    &cc->race.pen.slots[pi];
				const char *pname;
				int pvalue;

				if (p->race_end_tp == 0)
					continue;
				pen_kind_json(p->race_end_tp, &pname, &pvalue);
				if (!prp_first)
					fprintf(f, ",");
				fprintf(f, "\n    {");
				fprintf(f, " \"carId\": %u,", cc->car_id);
				fprintf(f, " \"driverIndex\": %u,",
				    (unsigned)p->driver_index);
				fprintf(f, " \"reason\": ");
				fprint_json_str(f, penalty_category_label(p->category));
				fprintf(f, ",");
				fprintf(f, " \"penalty\": ");
				fprint_json_str(f, pname);
				fprintf(f, ", \"penaltyValue\": %d,", pvalue);
				/*
				 * FUN_140127440:59 calls FUN_140125f50 with
				 * param_3 (race-end timestamp) as violation_time
				 * (+0x60).  FUN_140129b10's loop counts laps
				 * whose start_time < violation_time, yielding N-1
				 * (all completed laps).  p->cleared_lap holds
				 * lap_count-1 (set by penalty_convert_race_end).
				 * cleared_time stays -1.0 (fresh TP, never
				 * served), so clearedInLap loop yields 0.
				 */
				fprintf(f, " \"violationInLap\": %d,",
				    (int)p->cleared_lap);
				fprintf(f, " \"clearedInLap\": %d", 0);
				fprintf(f, " }");
				prp_first = 0;
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
