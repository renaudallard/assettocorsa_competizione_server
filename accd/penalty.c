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
uint8_t
penalty_pen_kind_of(uint8_t exe_kind, int collision, int32_t value)
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
	uint8_t pen_kind = penalty_pen_kind_of(exe_kind, collision, value);

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
	/*
	 * laps_remaining holds the caller-supplied value verbatim.
	 * For DT/SG it's the lap countdown; for TP it's the time
	 * penalty in seconds; in either case kunos's pcap (2026-05-10
	 * matrix test) shows the original 0x41 value byte ride through
	 * the per-car tail byte 1 of the 0x36 leaderboard.  Earlier
	 * code clamped DT/SG to 3 and TP to 0, dropping the input.
	 */
	e->laps_remaining = value;
	if (exe_kind == EXE_DQ)
		s->cars[car_id].race.disqualified = 1;
	e->issued_ms = mono_ms();
	/*
	 * Bump standings_seq so tick_run's leaderboard broadcaster fires
	 * within one tick.  Without this, the new penalty rides the 75 s
	 * useAsyncLeaderboard cadence; the chat text reaches the client
	 * but the per-car penalty fields in 0x36 (the source the AC2
	 * client's penalty HUD state machine reads) aren't updated until
	 * the next periodic emit, so DT/SG never visibly counts down.
	 */
	s->session.standings_seq++;
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
 * Our deviation from exe: the FIRST-ever direct caller entry materializes
 * in the fresh branch so admin /dt visibly adds a Penalty on the first
 * call.  Subsequent ladder steps that land on a fresh next-kind sheet
 * register only — exactly one visible Penalty per call, matching the exe
 * (so two missed mandatory pits produce DT + SG30, not DT + DT + SG30).
 */
int
penalty_enqueue(struct Server *s, int car_id, uint8_t exe_kind,
    uint8_t category, int32_t value, int force, int collision,
    uint8_t reason)
{
	struct CarRaceState *race;
	struct PenaltySheetState *st;
	uint64_t now_ms;
	int iter;

	if (car_id < 0 || car_id >= ACC_MAX_CARS || !s->cars[car_id].used)
		return -1;
	if (exe_kind == EXE_NONE || exe_kind > EXE_DQ)
		return -1;

	race = &s->cars[car_id].race;
	now_ms = mono_ms();

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
	 * hang the server.  At most one materialize per ladder call at
	 * the next fresh kind — matches exe FUN_140125f50, which runs the
	 * fresh-branch write as a plain register (no materialize) and only
	 * fires FUN_140126b50 on the non-fresh "step" path.  Our admin UX
	 * deviation materializes on the FIRST-EVER call (from_step==0) so
	 * that /dt adds a visible Penalty immediately; every subsequent
	 * ladder step lands on a register-only fresh sheet.
	 */
	int from_step = 0;
	for (iter = 0; iter < 8; iter++) {
		st = &race->pen_state[exe_kind];

		if (st->severity == 0) {
			/*
			 * Fresh.  Register severity + associated metadata
			 * so the next miss finds us in the non-fresh path.
			 * Materialize only when this is a direct caller entry
			 * (admin /dt etc.); skip the materialize when we
			 * arrived here from a ladder step — the step itself
			 * already emitted the visible Penalty for the previous
			 * kind.
			 */
			st->severity = exe_kind;
			st->category = category;
			st->issued_ms = now_ms;
			st->reason = reason;
			st->counter = value;
			if (!from_step)
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
		from_step = 1;

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

int
penalty_kind_is_dtsg(uint8_t k)
{
	return k == PEN_DT  || k == PEN_DTC  ||
	    k == PEN_SG10 || k == PEN_SG10C ||
	    k == PEN_SG20 || k == PEN_SG20C ||
	    k == PEN_SG30 || k == PEN_SG30C;
}

int
penalty_first_unserved_dtsg(const struct PenaltyQueue *q)
{
	int i;

	for (i = 0; i < q->count; i++) {
		if (q->slots[i].served)
			continue;
		if (penalty_kind_is_dtsg(q->slots[i].kind))
			return i;
	}
	return -1;
}

void
penalty_serve_front(struct Server *s, int car_id)
{
	struct PenaltyQueue *q;
	int idx;

	if (car_id < 0 || car_id >= ACC_MAX_CARS)
		return;
	q = &s->cars[car_id].race.pen;
	if (q->count == 0)
		return;
	/*
	 * Only DT / SG kinds are serve-able — TP5/TP15 are fixed
	 * post-race time penalties, DQ is terminal.  A TP enqueued
	 * before a real DT (admin /tp then a cut DT) at slots[0]
	 * would otherwise block service of the trailing DT.
	 */
	idx = penalty_first_unserved_dtsg(q);
	if (idx < 0)
		return;
	/*
	 * Mark the entry served instead of dropping it from the queue.
	 * Keeping the served record around lets results.json report
	 * "served": true for the penalties the driver actually paid
	 * off — without this, all entries that survive to session end
	 * are unserved (the served ones were silently removed) so the
	 * served field always reported false.
	 */
	q->slots[idx].served = 1;
	q->slots[idx].laps_remaining = 0;
	/*
	 * Bump standings_seq so the next 0x36 advertises the served
	 * flag, allowing the AC2 client to clear the DT/SG HUD prompt.
	 */
	s->session.standings_seq++;
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

uint32_t
penalty_total_ms(const struct PenaltyQueue *q)
{
	uint32_t total = 0;
	int i;

	if (q == NULL)
		return 0;
	for (i = 0; i < q->count; i++) {
		const struct PenaltyEntry *p = &q->slots[i];
		switch (p->kind) {
		case PEN_TP5:		total += 5000;		break;
		case PEN_TP15:		total += 15000;		break;
		case PEN_TP30:		total += 30000;		break;
		case PEN_TP40:		total += 40000;		break;
		case PEN_TP50:		total += 50000;		break;
		case PEN_TP60:		total += 60000;		break;
		case PEN_DT:
		case PEN_DTC:
			if (!p->served && p->laps_remaining > 0)
				total += 30000;
			break;
		case PEN_SG10:
		case PEN_SG10C:
			if (!p->served && p->laps_remaining > 0)
				total += 40000;
			break;
		case PEN_SG20:
		case PEN_SG20C:
			if (!p->served && p->laps_remaining > 0)
				total += 50000;
			break;
		case PEN_SG30:
		case PEN_SG30C:
			if (!p->served && p->laps_remaining > 0)
				total += 60000;
			break;
		default:
			break;
		}
	}
	return total;
}

/*
 * Convert every car's unserved DT / SG entries to the corresponding
 * post-race time penalty (DT -> 30 s, SG10 -> 40 s, SG20 -> 50 s,
 * SG30 -> 60 s) per handbook V.1.8.11 / exe FUN_140127440.  Called
 * from session.c at race-only session end; the converted entries
 * are kept in the queue (so results.json shows the converted-TP
 * kinds rather than the unserved DT/SG) but their `served` flag
 * stays false because TP penalties are not "served" — they're
 * applied as a final-time delta.  The same total ms still falls
 * out of penalty_total_ms because both code paths hit the same
 * 30/40/50/60-second buckets.
 */
void
penalty_convert_race_end(struct PenaltyQueue *q)
{
	int i;
	uint8_t new_kind;

	if (q == NULL)
		return;
	for (i = 0; i < q->count; i++) {
		struct PenaltyEntry *p = &q->slots[i];
		if (p->served || p->laps_remaining <= 0)
			continue;
		switch (p->kind) {
		case PEN_DT:	case PEN_DTC:	new_kind = PEN_TP30; break;
		case PEN_SG10:	case PEN_SG10C:	new_kind = PEN_TP40; break;
		case PEN_SG20:	case PEN_SG20C:	new_kind = PEN_TP50; break;
		case PEN_SG30:	case PEN_SG30C:	new_kind = PEN_TP60; break;
		default:
			continue;
		}
		p->kind = new_kind;
		p->collision = 0;
		p->laps_remaining = 0;
		/* reason stays as it was — race-control / cutting / etc. */
	}
}

const char *
penalty_name(uint8_t kind)
{
	switch (kind) {
	case PEN_NONE:		return "none";
	case PEN_TP5:		return "5s time penalty";
	case PEN_TP15:		return "15s time penalty";
	case PEN_TP30:		return "30s time penalty";
	case PEN_TP40:		return "40s time penalty";
	case PEN_TP50:		return "50s time penalty";
	case PEN_TP60:		return "60s time penalty";
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
 * wire value, byte-for-byte identical to kunos accServer.exe
 * FUN_1400f03b0 (the (kind, cat) → wire dispatcher).  Default for
 * unknown combos is 0 (No_Penalty).
 *
 * Notes on wire codes that are unrenderable on the AC2 client widget:
 *   - 27 (%Disqualified_ExceededDriverStintLimit) has '%' prefix that
 *     the AC2 config loader at FUN_1412a2d50:235 does NOT strip; the
 *     localized key lookup fails at the widget.
 *   - 33..35 (!DriveThrough/SG30/DQ_WrongPositionOnStart) have '!'
 *     prefix that FUN_1412a2d50:426-429 strips and DELETES the entry
 *     from the runtime penalty-shortcut hash map.
 * kunos emits these codes verbatim despite the client not rendering
 * the widget for them — accd matches that behaviour to keep the wire
 * byte-identical.  The chat banner via 0x2b reaches the player and
 * other-client leaderboard rendering does fail in the same way as
 * kunos for these specific combos.
 */
uint16_t
penalty_wire_value(uint8_t kind, uint8_t reason)
{
	/*
	 * kunos kind=5 (PostRaceTime) is universal: it produces wire 14
	 * regardless of cat.  Our PEN_TP* family (TP5/TP15/TP30/TP40/
	 * TP50/TP60) all map to the same exe kind=5, so emit wire 14
	 * up-front before the per-reason switch.
	 */
	switch (kind) {
	case PEN_TP5: case PEN_TP15:
	case PEN_TP30: case PEN_TP40:
	case PEN_TP50: case PEN_TP60:	return 14;
	}

	switch (reason) {
	case REASON_CUTTING:
		switch (kind) {
		case PEN_DT: case PEN_DTC:	return 1;
		case PEN_SG10: case PEN_SG10C:	return 2;
		case PEN_SG20: case PEN_SG20C:	return 3;
		case PEN_SG30: case PEN_SG30C:	return 4;
		case PEN_DQ:			return 5;
		/* RemoveBestLaptime → 6 (kind=7 in exe; not yet emitted). */
		}
		break;
	case REASON_PIT_SPEEDING:
		switch (kind) {
		case PEN_DT: case PEN_DTC:	return 7;
		case PEN_SG10: case PEN_SG10C:	return 8;
		case PEN_SG20: case PEN_SG20C:	return 9;
		case PEN_SG30: case PEN_SG30C:	return 10;
		case PEN_DQ:			return 11;
		/* RemoveBestLaptime → 12 (not yet emitted). */
		}
		break;
	case REASON_IGNORED_MANDATORY_PIT:
		if (kind == PEN_DQ) return 13;
		break;
	case REASON_RACE_CONTROL:
		switch (kind) {
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
		/*
		 * Per kunos FUN_1400f03b0 case 4 ('\r' = 13): wire 27 is
		 * emitted ONLY for kind=SG30, NOT for DQ.  Despite wire 27's
		 * label being "Disqualified_ExceededDriverStintLimit", the
		 * exe assigns it to the StopAndGo_30 column.  We mirror the
		 * exe verbatim.
		 */
		if (kind == PEN_SG30 || kind == PEN_SG30C) return 27;
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
    uint8_t reason, int collision, int car_num)
{
	const char *suffix = "";

	if (collision) {
		suffix = " - causing a collision";
	} else {
		switch (reason) {
		case REASON_CUTTING:
			suffix = " - cutting";
			break;
		case REASON_PIT_SPEEDING:
			suffix = " - pit speeding";
			break;
		case REASON_IGNORED_MANDATORY_PIT:
			suffix = " - ignored mandatory pit";
			break;
		case REASON_PIT_ENTRY:
			suffix = " - pit entry infraction";
			break;
		case REASON_PIT_EXIT:
			suffix = " - pit exit infraction";
			break;
		case REASON_WRONG_WAY:
			suffix = " - wrong way";
			break;
		case REASON_LIGHTS_OFF:
			suffix = " - lights off";
			break;
		case REASON_IGNORED_DRIVER_STINT:
			suffix = " - ignored driver stint";
			break;
		case REASON_EXCEEDED_DRIVER_STINT_LIMIT:
			suffix = " - exceeded driver stint limit";
			break;
		case REASON_DRIVER_RAN_NO_STINT:
			suffix = " - driver ran no stint";
			break;
		case REASON_DAMAGED_CAR:
			suffix = " - damaged car";
			break;
		case REASON_SPEEDING_ON_START:
			suffix = " - speeding on start";
			break;
		case REASON_WRONG_POSITION_ON_START:
			suffix = " - wrong position on start";
			break;
		default:
			suffix = "";
			break;
		}
	}

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
		if (reason == REASON_RACE_CONTROL || reason == REASON_NONE)
			return snprintf(out, outsz,
			    "Car #%d was disqualified by Race Control",
			    car_num);
		return snprintf(out, outsz,
		    "Car #%d was disqualified%s", car_num, suffix);
	default:
		return snprintf(out, outsz, "Penalty for car #%d", car_num);
	}
}
