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
#include "results.h"
#include "state.h"

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
	strftime(ts, sizeof(ts), "%y%m%d_%H%M%S", &tm);
	snprintf(path, sizeof(path), "%s/%s_%s.json",
	    dir, ts, session_type_str(st));

	f = fopen(path, "w");
	if (f == NULL) {
		log_warn("results: fopen %s: %s", path, strerror(errno));
		return -1;
	}

	fprintf(f, "{\n");
	fprintf(f, "  \"sessionType\": \"%s\",\n", session_type_str(st));
	fprintf(f, "  \"trackName\": \"%s\",\n", s->track);
	fprintf(f, "  \"sessionIndex\": %u,\n", (unsigned)sidx);
	fprintf(f, "  \"raceWeekendIndex\": 0,\n");
	fprintf(f, "  \"serverName\": \"%s\",\n", s->server_name);
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
		fprintf(f, "          \"teamName\": \"%s\",\n",
		    car->team_name);
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
				fprintf(f, "              \"firstName\": \"%s\",\n",
				    d->first_name);
				fprintf(f, "              \"lastName\": \"%s\",\n",
				    d->last_name);
				fprintf(f, "              \"shortName\": \"%s\",\n",
				    d->short_name);
				fprintf(f, "              \"playerId\": \"%s\"\n",
				    d->steam_id);
				fprintf(f, "            }");
				dfirst = 0;
			}
		}
		fprintf(f, "\n          ]\n");
		fprintf(f, "        },\n");
		fprintf(f, "        \"currentDriver\": {\n");
		d = &car->drivers[car->current_driver_index <
		    car->driver_count ? car->current_driver_index : 0];
		fprintf(f, "          \"firstName\": \"%s\",\n", d->first_name);
		fprintf(f, "          \"lastName\": \"%s\",\n", d->last_name);
		fprintf(f, "          \"shortName\": \"%s\",\n", d->short_name);
		fprintf(f, "          \"playerId\": \"%s\"\n", d->steam_id);
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
		fprintf(f, "        \"missingMandatoryPitstop\": %d\n",
		    car->race.mandatory_pit_served ? 0 : 1);
		fprintf(f, "      }");
		first = 0;
	}
	fprintf(f, "\n    ]\n");
	fprintf(f, "  }\n");
	fprintf(f, "}\n");
	fclose(f);

	log_info("results: wrote %s", path);
	return 0;
}
