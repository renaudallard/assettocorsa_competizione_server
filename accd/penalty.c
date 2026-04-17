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

uint8_t
penalty_exe_kind_of(uint8_t pen_kind)
{
	switch (pen_kind) {
	case PEN_DT:
	case PEN_DTC:		return EXE_DT;
	case PEN_SG10:
	case PEN_SG10C:		return EXE_SG10;
	case PEN_SG20:
	case PEN_SG20C:		return EXE_SG20;
	case PEN_SG30:
	case PEN_SG30C:		return EXE_SG30;
	case PEN_TP5:
	case PEN_TP15:		return EXE_TP;
	case PEN_DQ:		return EXE_DQ;
	default:		return EXE_NONE;
	}
}

/*
 * Translate exe_kind + collision flag to our internal PEN_* enum so
 * materialized Penalty entries carry the right kind for leaderboard
 * rendering.  Collision only distinguishes /dt vs /dtc (and /sgXX vs
 * /sgXXc); all others ignore collision.
 */
static uint8_t
pen_kind_of_exe(uint8_t exe_kind, int collision, int32_t value)
{
	switch (exe_kind) {
	case EXE_DT:	return collision ? PEN_DTC  : PEN_DT;
	case EXE_SG10:	return collision ? PEN_SG10C : PEN_SG10;
	case EXE_SG20:	return collision ? PEN_SG20C : PEN_SG20;
	case EXE_SG30:	return collision ? PEN_SG30C : PEN_SG30;
	case EXE_TP:	return value >= 15 ? PEN_TP15 : PEN_TP5;
	case EXE_DQ:	return PEN_DQ;
	default:	return PEN_NONE;
	}
}

/*
 * Append a materialized Penalty to the car's PenaltyQueue — the
 * equivalent of FUN_140126b50 in the exe, which pushes a new Penalty
 * object onto the sheet entry's vector.
 */
static void
penalty_materialize(struct Server *s, int car_id, uint8_t exe_kind,
    int collision, int32_t value, uint8_t reason)
{
	struct PenaltyQueue *q;
	struct PenaltyEntry *e;
	struct timespec ts;
	uint8_t pen_kind = pen_kind_of_exe(exe_kind, collision, value);

	if (car_id < 0 || car_id >= ACC_MAX_CARS || !s->cars[car_id].used)
		return;
	q = &s->cars[car_id].race.pen;
	if (q->count >= ACC_MAX_PENALTIES)
		return;

	e = &q->slots[q->count++];
	e->kind = pen_kind;
	e->reason = reason;
	e->collision = collision ? 1 : 0;
	e->served = 0;
	switch (exe_kind) {
	case EXE_DT:
	case EXE_SG10:
	case EXE_SG20:
	case EXE_SG30:
		e->laps_remaining = 3;	/* serve within 3 laps */
		break;
	default:
		e->laps_remaining = 0;
		break;
	}
	if (exe_kind == EXE_DQ)
		s->cars[car_id].race.disqualified = 1;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	e->issued_ms = (uint64_t)ts.tv_sec * 1000ull +
	    (uint64_t)ts.tv_nsec / 1000000ull;
}

/*
 * penalty_enqueue — behavioral match for FUN_140125f50.  Maintains
 * per-car per-exe_kind PenaltySheet state (counter + severity).
 *
 * Exe semantics per decomp:
 *  - EXE_TP (kind 5): counter accumulates `value`.  At 0x100 seconds
 *    total, escalate to DQ.  Admin /tp5 adds 5, /tp15 adds 15.
 *  - EXE_DT/SG10/SG20/SG30: first call sets severity without
 *    materializing; second+ call materializes + steps the ladder.
 *    Ladder: bVar2=1(DT) → bVar6; bVar2=2/3 → bVar6; bVar2=4 → DQ
 *    only if force; where bVar6 = (force+2)*2 = 4 (SG30) or 6 (DQ).
 *  - EXE_DQ: materialize immediately, race->disqualified=1.
 *
 * Our deviation from exe: we materialize on the fresh-entry path
 * too, so admin /dt visibly adds a Penalty on the first call.
 * The exe's silent-first-call would confuse admins and break the
 * pre-refactor behavior of the reimpl.
 */
int
penalty_enqueue(struct Server *s, int car_id, uint8_t exe_kind,
    uint8_t category, int32_t value, int force, int collision,
    uint8_t reason)
{
	struct CarRaceState *race;
	struct PenaltySheetState *st;
	struct timespec now_ts;
	uint64_t now_ms;
	int iter;

	if (car_id < 0 || car_id >= ACC_MAX_CARS || !s->cars[car_id].used)
		return -1;
	if (exe_kind == EXE_NONE || exe_kind > EXE_DQ)
		return -1;

	race = &s->cars[car_id].race;
	clock_gettime(CLOCK_MONOTONIC, &now_ts);
	now_ms = (uint64_t)now_ts.tv_sec * 1000ull +
	    (uint64_t)now_ts.tv_nsec / 1000000ull;

	/* Immediate-effect special case: DQ. */
	if (exe_kind == EXE_DQ) {
		penalty_materialize(s, car_id, EXE_DQ, collision,
		    value, reason);
		st = &race->pen_state[EXE_DQ];
		st->severity = EXE_DQ;
		st->category = category;
		st->issued_ms = now_ms;
		st->reason = reason;
		st->counter = value;
		return 0;
	}

	/* Post-race time penalty: counter is seconds, threshold 256. */
	if (exe_kind == EXE_TP) {
		st = &race->pen_state[EXE_TP];
		if (st->severity == 0) {
			st->severity = EXE_TP;
			st->category = category;
			st->reason = reason;
		}
		st->counter += value;
		st->issued_ms = now_ms;
		penalty_materialize(s, car_id, EXE_TP, collision,
		    value, reason);
		if (st->counter >= 0x100) {
			log_info("car %d total TP exceeded 256s -> DQ",
			    car_id);
			penalty_materialize(s, car_id, EXE_DQ, 0, 0, reason);
			race->pen_state[EXE_DQ].severity = EXE_DQ;
			race->pen_state[EXE_DQ].issued_ms = now_ms;
		}
		return 0;
	}

	/*
	 * DT/SG ladder — bounded loop mimics exe's `goto LAB_140125fb0`
	 * pattern, cap iterations so a pathological severity loop can't
	 * hang the server.
	 */
	for (iter = 0; iter < 8; iter++) {
		st = &race->pen_state[exe_kind];

		if (st->severity == 0) {
			/*
			 * Fresh: materialize + set severity (deviation
			 * from exe — see header comment).  Counter
			 * initialized to value; the first repeat call
			 * finds severity != 0 and steps the ladder.
			 */
			st->severity = exe_kind;
			st->category = category;
			st->issued_ms = now_ms;
			st->reason = reason;
			st->counter = value;
			penalty_materialize(s, car_id, exe_kind,
			    collision, value, reason);
			return 0;
		}

		/*
		 * Non-fresh: materialize current severity and step the
		 * ladder.  Unlike TP there's no counter accumulation
		 * gate for the DT/SG path — each repeat call escalates.
		 */
		penalty_materialize(s, car_id, st->severity, collision,
		    value, reason);
		st->issued_ms = now_ms;

		{
			uint8_t bVar2 = st->severity;
			uint8_t bVar6 = (uint8_t)((force + 2) * 2);
			/* bVar6 = 4 (SG30) if force=0, 6 (DQ) if force=1 */

			if (bVar2 == EXE_DT) {
				if (exe_kind > EXE_DT) {
					exe_kind = bVar6;
					value = 0;
					continue;
				}
				exe_kind = EXE_SG30;
				value = 3;
				continue;
			}
			if (bVar2 == EXE_SG10 || bVar2 == EXE_SG20) {
				exe_kind = bVar6;
				value = 3;
				continue;
			}
			if (bVar2 == EXE_SG30) {
				if (force == 0)
					return 0;
				exe_kind = EXE_DQ;
				value = 3;
				continue;
			}
			/* severity 5 or 6 reached: terminal */
			return 0;
		}
	}
	log_warn("penalty_enqueue: escalation loop overflow car=%d",
	    car_id);
	return -1;
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
	/* Remove the front entry and slide the rest down. */
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

/*
 * Map internal (kind, reason) to the 0..35 ServerMonitorPenaltyShortcut
 * wire value documented in notebook-b §12B.4.  Default for unknown
 * combos is 0 (No_Penalty) — better to emit "no penalty" than a
 * wrong semantic.
 */
uint16_t
penalty_wire_value(uint8_t kind, uint8_t reason)
{
	switch (reason) {
	case REASON_CUTTING:
		switch (kind) {
		case PEN_DT: case PEN_DTC:	return 1;
		case PEN_SG10: case PEN_SG10C:	return 2;
		case PEN_SG20: case PEN_SG20C:	return 3;
		case PEN_SG30: case PEN_SG30C:	return 4;
		case PEN_DQ:			return 5;
		}
		break;
	case REASON_PIT_SPEEDING:
		switch (kind) {
		case PEN_DT: case PEN_DTC:	return 7;
		case PEN_SG10: case PEN_SG10C:	return 8;
		case PEN_SG20: case PEN_SG20C:	return 9;
		case PEN_SG30: case PEN_SG30C:	return 10;
		case PEN_DQ:			return 11;
		}
		break;
	case REASON_IGNORED_MANDATORY_PIT:
		if (kind == PEN_DQ) return 13;
		break;
	case REASON_RACE_CONTROL:
		switch (kind) {
		case PEN_TP5: case PEN_TP15:	return 14;
		case PEN_DT: case PEN_DTC:	return 15;
		case PEN_SG10: case PEN_SG10C:	return 16;
		case PEN_SG20: case PEN_SG20C:	return 17;
		case PEN_SG30: case PEN_SG30C:	return 18;
		case PEN_DQ:			return 19;
		}
		break;
	case REASON_PIT_ENTRY:		if (kind == PEN_DQ) return 20; break;
	case REASON_PIT_EXIT:		if (kind == PEN_DQ) return 21; break;
	case REASON_WRONG_WAY:		if (kind == PEN_DQ) return 22; break;
	case REASON_LIGHTS_OFF:		if (kind == PEN_DQ) return 23; break;
	case REASON_IGNORED_DRIVER_STINT:
		switch (kind) {
		case PEN_DT: case PEN_DTC:	return 24;
		case PEN_SG30: case PEN_SG30C:	return 25;
		case PEN_DQ:			return 26;
		}
		break;
	case REASON_EXCEEDED_DRIVER_STINT_LIMIT:
		if (kind == PEN_DQ) return 27;
		break;
	case REASON_DRIVER_RAN_NO_STINT:
		if (kind == PEN_DQ) return 28;
		break;
	case REASON_DAMAGED_CAR:
		if (kind == PEN_DQ) return 29;
		break;
	case REASON_SPEEDING_ON_START:
		switch (kind) {
		case PEN_DT: case PEN_DTC:	return 30;
		case PEN_SG30: case PEN_SG30C:	return 31;
		case PEN_DQ:			return 32;
		}
		break;
	case REASON_WRONG_POSITION_ON_START:
		switch (kind) {
		case PEN_DT: case PEN_DTC:	return 33;
		case PEN_SG30: case PEN_SG30C:	return 34;
		case PEN_DQ:			return 35;
		}
		break;
	}
	return 0;	/* No_Penalty */
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
