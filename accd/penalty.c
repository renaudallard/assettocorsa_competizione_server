/*
 * penalty.c -- penalty queue per car.
 *
 * The actual auto-penalty detection (pit speeding, off-track,
 * etc.) lives in handlers.c phase 9; this module just owns the
 * data structure and the chat-command-driven assignments.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "penalty.h"
#include "state.h"

int
penalty_kind_from_string(const char *cmd)
{
	if (cmd == NULL)
		return PEN_NONE;
	if (strcmp(cmd, "tp5") == 0)	return PEN_TP5;
	if (strcmp(cmd, "tp5c") == 0)	return PEN_TP5;
	if (strcmp(cmd, "tp15") == 0)	return PEN_TP15;
	if (strcmp(cmd, "tp15c") == 0)	return PEN_TP15;
	if (strcmp(cmd, "dt") == 0)	return PEN_DT;
	if (strcmp(cmd, "dtc") == 0)	return PEN_DTC;
	if (strcmp(cmd, "sg10") == 0)	return PEN_SG10;
	if (strcmp(cmd, "sg10c") == 0)	return PEN_SG10C;
	if (strcmp(cmd, "sg20") == 0)	return PEN_SG20;
	if (strcmp(cmd, "sg20c") == 0)	return PEN_SG20C;
	if (strcmp(cmd, "sg30") == 0)	return PEN_SG30;
	if (strcmp(cmd, "sg30c") == 0)	return PEN_SG30C;
	if (strcmp(cmd, "dq") == 0)	return PEN_DQ;
	return PEN_NONE;
}

int
penalty_enqueue(struct Server *s, int car_id, uint8_t kind, int collision)
{
	struct PenaltyQueue *q;
	struct PenaltyEntry *e;
	struct timespec ts;

	if (car_id < 0 || car_id >= ACC_MAX_CARS || !s->cars[car_id].used)
		return -1;
	q = &s->cars[car_id].race.pen;
	if (q->count >= ACC_MAX_PENALTIES)
		return -1;

	e = &q->slots[q->count++];
	e->kind = kind;
	e->collision = collision ? 1 : 0;
	e->served = 0;
	switch (kind) {
	case PEN_DT:
	case PEN_DTC:
		e->laps_remaining = 3;
		break;
	default:
		e->laps_remaining = 0;
		break;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	e->issued_ms = (uint64_t)ts.tv_sec * 1000ull +
	    (uint64_t)ts.tv_nsec / 1000000ull;
	return 0;
}

void
penalty_serve_front(struct Server *s, int car_id)
{
	struct PenaltyQueue *q;
	int i;

	if (car_id < 0 || car_id >= ACC_MAX_CARS)
		return;
	q = &s->cars[car_id].race.pen;
	if (q->count == 0)
		return;
	q->slots[0].served = 1;
	/* Slide the rest down. */
	for (i = 1; i < q->count; i++)
		q->slots[i - 1] = q->slots[i];
	q->count--;
}

void
penalty_clear(struct Server *s, int car_id)
{
	struct PenaltyQueue *q;

	if (car_id < 0 || car_id >= ACC_MAX_CARS)
		return;
	q = &s->cars[car_id].race.pen;
	q->count = 0;
	memset(q->slots, 0, sizeof(q->slots));
}

void
penalty_clear_all(struct Server *s)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS; i++)
		penalty_clear(s, i);
}

const char *
penalty_name(uint8_t kind)
{
	switch (kind) {
	case PEN_NONE:		return "none";
	case PEN_TP5:		return "5s time penalty";
	case PEN_TP15:		return "15s time penalty";
	case PEN_DT:		return "Drivethrough penalty";
	case PEN_DTC:		return "Drivethrough penalty";
	case PEN_SG10:		return "Stop and Go 10s penalty";
	case PEN_SG10C:		return "Stop and Go 10s penalty";
	case PEN_SG20:		return "Stop and Go 20s penalty";
	case PEN_SG20C:		return "Stop and Go 20s penalty";
	case PEN_SG30:		return "Stop and Go 30s penalty";
	case PEN_SG30C:		return "Stop and Go 30s penalty";
	case PEN_DQ:		return "Disqualified by Race Control";
	default:		return "?";
	}
}

int
penalty_format_chat(char *out, size_t outsz, uint8_t kind,
    int collision, int car_num)
{
	const char *suffix = collision ? " - causing a collision" : "";

	switch (kind) {
	case PEN_TP5:
		return snprintf(out, outsz,
		    "5s penalty for car #%d%s", car_num, suffix);
	case PEN_TP15:
		return snprintf(out, outsz,
		    "15s penalty for car #%d%s", car_num, suffix);
	case PEN_DT:
	case PEN_DTC:
		return snprintf(out, outsz,
		    "Drivethrough penalty for car #%d%s",
		    car_num, suffix);
	case PEN_SG10:
	case PEN_SG10C:
		return snprintf(out, outsz,
		    "Stop and Go 10s penalty for car #%d%s",
		    car_num, suffix);
	case PEN_SG20:
	case PEN_SG20C:
		return snprintf(out, outsz,
		    "Stop and Go 20s penalty for car #%d%s",
		    car_num, suffix);
	case PEN_SG30:
	case PEN_SG30C:
		return snprintf(out, outsz,
		    "Stop and Go 30s penalty for car #%d%s",
		    car_num, suffix);
	case PEN_DQ:
		return snprintf(out, outsz,
		    "Car #%d was disqualified by Race Control",
		    car_num);
	default:
		return snprintf(out, outsz, "Penalty for car #%d", car_num);
	}
}
