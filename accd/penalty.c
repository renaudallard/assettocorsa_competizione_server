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
#include "session.h"
#include "state.h"
#include "tick.h"

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
	case EXE_RBL:	return PEN_RBL;
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
    int collision, int32_t value, uint8_t reason, uint8_t category)
{
	struct PenaltyQueue *q;
	struct PenaltyEntry *e;
	uint8_t pen_kind = penalty_pen_kind_of(exe_kind, collision, value);

	if (car_id < 0 || car_id >= ACC_MAX_CARS || !s->cars[car_id].used)
		return;
	q = &s->cars[car_id].race.pen;
	if (q->count >= ACC_MAX_PENALTIES) {
		/*
		 * Ring-buffer eviction: drop slot 0 (oldest) so the
		 * newest entry always lands at slots[count-1] and the
		 * per-car tail bytes track the most recent penalty.
		 * Kunos's FUN_140125f50:152 keeps a single-entry-per-car
		 * list at param_1+0x30; we approximate by keeping a
		 * sliding window of the latest ACC_MAX_PENALTIES.
		 */
		int j;
		for (j = 0; j < ACC_MAX_PENALTIES - 1; j++)
			q->slots[j] = q->slots[j + 1];
		q->count = ACC_MAX_PENALTIES - 1;
	}

	/*
	 * "Second-DQ overwrite" semantics: kunos's per-car +0x30 list
	 * stores the original 0x41 value byte only on a fresh entry; a
	 * subsequent DQ event (regardless of who issued it — admin /dq,
	 * TP-accum overflow, ladder step) "overwrites" the existing DQ
	 * and resets the value byte to 0.  The new entry itself also
	 * carries value=0 in this case.  Mirror by scanning the queue
	 * before push: any prior PEN_DQ flips the incoming value to 0
	 * and zeroes the existing DQ entries' laps_remaining so the
	 * per-car tail (which scans from the back) reports 0 on both
	 * sides.  Produces the `13 03 -> 13 00` transition pcap-observed
	 * in the TP-then-admin-DQ scenario.
	 */
	if (exe_kind == EXE_DQ) {
		int j, prior_dq = 0;
		for (j = 0; j < q->count; j++) {
			if (q->slots[j].kind == PEN_DQ) {
				q->slots[j].laps_remaining = 0;
				prior_dq = 1;
			}
		}
		if (prior_dq)
			value = 0;
	}
	e = &q->slots[q->count++];
	/*
	 * Zero the whole entry first so the eviction shift above doesn't
	 * leak the previous occupant's `pending`, `admin`, or
	 * `race_end_tp` flags into a fresh push.  Without this, a real
	 * server-detected penalty (pit-speeding DQ, auto-DT) landing in
	 * a slot that previously held an admin-issued penalty silently
	 * inherits `admin=1` and is hidden from the 0x36 active_pen /
	 * pq_emit lists.
	 */
	memset(e, 0, sizeof(*e));
	e->kind = pen_kind;
	e->reason = reason;
	/*
	 * AC2 category for the results.json reason label.  Kunos stores
	 * the caller's category at PenaltySheet entry +0x58 and the
	 * results writer (FUN_140129b10) renders it through the cat -> name
	 * table FUN_140117330, independent of the wire-code path.  The
	 * caller passes the post-rewrite category (e.g. cat 6 mandatory-pit
	 * becomes 8 Trolling once penalty_enqueue's TP130 rewrite fires),
	 * matching FUN_140125f50 which overwrites +0x58 to 8 in that case.
	 */
	e->category = category;
	e->collision = collision ? 1 : 0;
	e->driver_index = s->cars[car_id].current_driver_index;
	e->served = 0;
	/*
	 * Record the lap the violation landed on (1-based) so results.json
	 * can report violationInLap; cleared_lap stays open until the
	 * penalty is served.  memset above zeroed both, so set them here.
	 */
	/*
	 * Exe FUN_140129b10 counts laps that closed before the violation
	 * timestamp (0-based rank among completed laps), which equals
	 * lap_count at the moment of the penalty.  The previous +1 was
	 * the current lap number (1-based), one higher than the exe value.
	 */
	e->violation_lap = (int16_t)s->cars[car_id].race.lap_count;
	e->cleared_lap = -1;
	/*
	 * laps_remaining: caller-supplied value verbatim.  For DT/SG
	 * it's the lap countdown; for TP it's the time penalty in
	 * seconds; in either case kunos's pcap shows the original
	 * 0x41 value byte ride through the per-car tail byte 1 of
	 * the 0x36 leaderboard.
	 */
	e->laps_remaining = value;
	if (exe_kind == EXE_DQ) {
		s->cars[car_id].race.disqualified = 1;
		session_recompute_standings(s);
	}
	e->issued_ms = mono_ms();
	/*
	 * Every materialise (DT/SG/TP/RBL ladder step + DQ) mutates the
	 * per-car tail bytes in the 0x36 leaderboard.  Kunos pcap
	 * (2026-05-09 + 2026-05-13 TP-then-admin-DQ) shows 0x36 fires
	 * within ~50 ms of every 0x41 receipt.  Flag the leaderboard
	 * dirty; the tick loop drains and runs the memcmp-gated emit so
	 * a no-op materialise (e.g. the RBL kind=7 case that doesn't
	 * change payload bytes — handlers.c:1001 comment) still
	 * suppresses the wire fan-out.
	 */
	leaderboard_request_emit(s);
}

/*
 * Maintain a SINGLE live PostRaceTime entry per car, mirroring the exe's
 * PenaltySheet +0x70 counter (FUN_140125f50:97-100 accumulates one
 * counter and never pushes a Penalty below the 256 s threshold).  Update
 * the existing entry in place, or create it on the first report, so
 * laps_remaining always carries the true cumulative seconds and the queue
 * never grows one slot per report.  Without this, three reports left three
 * slots each holding the running total, so penalty_total_ms summed the
 * coarse PEN_TP5/PEN_TP15 buckets instead of the real accumulated time.
 */
static int
penalty_set_tp(struct Server *s, int car_id, int32_t total_sec,
    uint8_t reason, uint8_t category, int collision)
{
	struct PenaltyQueue *q;
	int i;

	if (car_id < 0 || car_id >= ACC_MAX_CARS || !s->cars[car_id].used)
		return -1;
	q = &s->cars[car_id].race.pen;
	for (i = 0; i < q->count; i++) {
		struct PenaltyEntry *p = &q->slots[i];
		if (p->served || p->race_end_tp != 0)
			continue;
		if (p->kind != PEN_TP5 && p->kind != PEN_TP15)
			continue;
		/*
		 * Exe FUN_140125f50:85-93 updates only kind (+0x59) and
		 * laps_remaining (+0x70) on an existing TP entry; reason
		 * (+0x58) and category are left from the initial creation.
		 */
		p->kind = penalty_pen_kind_of(EXE_TP, collision, total_sec);
		p->laps_remaining = total_sec;
		p->issued_ms = mono_ms();
		leaderboard_request_emit(s);
		return i;
	}
	penalty_materialize(s, car_id, EXE_TP, collision, total_sec,
	    reason, category);
	return (int)(q->count - 1);
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

	if (car_id < 0 || car_id >= ACC_MAX_CARS || !s->cars[car_id].used)
		return -1;
	if (exe_kind == EXE_NONE || exe_kind > EXE_RBL)
		return -1;

	race = &s->cars[car_id].race;
	now_ms = mono_ms();

	/* Immediate-effect special case: RBL (RemoveBestLaptime). */
	if (exe_kind == EXE_RBL) {
		penalty_materialize(s, car_id, EXE_RBL, collision,
		    value, reason, category);
		return (int)(race->pen.count - 1);
	}

	/*
	 * Kunos's FUN_140125f50:140-144 rewrites a non-forced DQ for
	 * IgnoredMandatoryPit (cat=6) into a 130-second TP penalty,
	 * with the stored category overridden to 8 (Trolling).  The
	 * wire emit ends up as wire 14 (TP universal) with value 0x82.
	 * Mirror that here so the post-enqueue 0x36 tail matches kunos
	 * byte-for-byte.
	 */
	if (exe_kind == EXE_DQ && !force &&
	    reason == REASON_IGNORED_MANDATORY_PIT) {
		exe_kind = EXE_TP;
		value = 130;
		category = 8;
	}

	/* Immediate-effect special case: DQ. */
	if (exe_kind == EXE_DQ) {
		/*
		 * If the per-car ladder already reached DQ, kunos treats
		 * a subsequent DQ-direct (kind=6) report as terminal: no
		 * new Penalty is pushed, no 0x36 broadcast.  Pcap
		 * (2026-05-11 4-bot run): r6 = cat=0 kind=6 after r2
		 * already escalated to DQ produces NO 0x36 emit.
		 */
		if (race->dtsg_ladder_sev == EXE_DQ) {
			/*
			 * Kunos pcap (2026-05-13 TP-then-admin-DQ) shows the
			 * dedup'd admin /dq for an already-DQ'd category
			 * resets the existing DQ entry's per-car tail value
			 * byte to 0 — "overwrite" semantics for the +0x30
			 * list even though no new Penalty is pushed.  Mirror
			 * by zeroing laps_remaining on the queued DQ entries.
			 * The deep-compare cache on the next tick picks up
			 * the byte change and fans a follow-up 0x36 with the
			 * reset tail; kunos's `13 03 -> 13 00` transition is
			 * what we're matching here.
			 */
			int j;
			for (j = 0; j < race->pen.count; j++) {
				if (race->pen.slots[j].kind == PEN_DQ)
					race->pen.slots[j].laps_remaining = 0;
			}
			/*
			 * Same dirty-flag pattern as the ladder in-place
			 * edit: the queue mutation changed b1 in the per-car
			 * tail without going through penalty_materialize, so
			 * the request_emit there doesn't fire.  Flag it here
			 * so the next tick drains the pending bit.
			 */
			leaderboard_request_emit(s);
			return -1;
		}
		/* Exe FUN_140125f50:146-157: when param_5==EXE_DQ and the DT/SG
		 * sheet has a non-zero severity byte, FUN_140126b50 is called
		 * first to zero it before the DQ materialises.  FUN_140127440
		 * (race-end converter) reads severity=0 and skips conversion,
		 * preventing the DT/SG from generating an unearned time penalty.
		 * Mirror: mark all unserved DT/SG entries as served. */
		{
			int j;
			for (j = 0; j < race->pen.count; j++) {
				struct PenaltyEntry *pe = &race->pen.slots[j];
				if (pe->served)
					continue;
				switch (pe->kind) {
				case PEN_DT: case PEN_DTC:
				case PEN_SG10: case PEN_SG10C:
				case PEN_SG20: case PEN_SG20C:
				case PEN_SG30: case PEN_SG30C:
					pe->served = 1;
					pe->laps_remaining = 0;
					break;
				default:
					break;
				}
			}
		}
		penalty_materialize(s, car_id, EXE_DQ, collision,
		    value, reason, category);
		race->dtsg_ladder_sev = EXE_DQ;
		st = &race->pen_state[EXE_DQ];
		st->severity = EXE_DQ;
		st->category = category;
		st->issued_ms = now_ms;
		st->reason = reason;
		st->counter = value;
		return (int)(race->pen.count - 1);
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
		/*
		 * Update the car's single PostRaceTime entry with the
		 * CUMULATIVE counter so the 0x36 per-car tail b1 reads the
		 * running total — kunos pcap (run_tp_accum.sh) shows the
		 * tail progressing `0e 32 / 0e 64 / 0e 96` (50 / 100 /
		 * 150) on three successive TP reports of value=50 each.
		 * penalty_set_tp keeps it to one slot (the exe's +0x70
		 * counter), so the post-race time fold reads the true total.
		 */
		int tp_slot = penalty_set_tp(s, car_id, (int32_t)st->counter,
		    reason, category, collision);
		if (st->counter >= 0x100) {
			log_info("car %d total TP exceeded 256s -> DQ",
			    car_id);
			/*
			 * Kunos overrides the reason to RACE_CONTROL when
			 * TP accumulation crosses the 256 s threshold.
			 * Pcap (2026-05-11 TP-accum scenario): kunos's
			 * tail wire is 19 = REASON_RACE_CONTROL + PEN_DQ,
			 * not 5 = REASON_CUTTING + PEN_DQ even when the
			 * triggering reports were Cutting.
			 *
			 * Tail value 0: run_tp_accum.sh's kunos pcap (no
			 * admin /dq) ends at `13 00`.  When admin /dq fires
			 * BEFORE the threshold cross (run_tp_then_admin_dq),
			 * the admin DQ materialises with value=3 first and
			 * the auto-DQ below lands as the "second DQ" path
			 * in penalty_materialize, which resets the prior
			 * entry and emits value=0.  Either way the auto-DQ
			 * materialise itself carries value=0.
			 */
			/*
			 * Category 8 (Trolling) for the results label: the exe
			 * sets local_res20='\b' on the 256 s threshold cross
			 * (FUN_140125f50:99) before materialising the DQ.
			 */
			penalty_materialize(s, car_id, EXE_DQ, 0, 0,
			    REASON_RACE_CONTROL, 8);
			race->pen_state[EXE_DQ].severity = EXE_DQ;
			race->pen_state[EXE_DQ].issued_ms = now_ms;
			/*
			 * Mark the car's ladder terminal so a follow-up
			 * direct DQ hits the dedup guard and doesn't
			 * materialise twice.
			 */
			race->dtsg_ladder_sev = EXE_DQ;
		}
		return tp_slot;
	}

	/*
	 * DT/SG ladder — kunos's FUN_140125f50 keys the sheet per-car
	 * (search key = carId at entry+0x28; category at +0x5c is
	 * metadata, not a key).  Any new DT/SG report for the same car
	 * steps the single per-car ladder regardless of category:
	 *   force=0:  DT -> SG30 (terminal)
	 *   force=1:  DT -> DQ
	 * SG30 -> DQ only when force=1.  DQ is terminal.
	 */
	{
		uint8_t old_sev;
		uint8_t new_sev;
		uint8_t bVar6;
		int step_slot = -1;

		old_sev = race->dtsg_ladder_sev;
		bVar6 = (uint8_t)((force + 2) * 2);
		/* bVar6 = 4 (SG30) if force=0, 6 (DQ) if force=1. */

		if (old_sev == 0) {
			/* First DT/SG for this car: record cat for label. */
			race->dtsg_ladder_cat = category;
			race->dtsg_ladder_sev = exe_kind;
			penalty_materialize(s, car_id, exe_kind, collision,
			    value, reason, category);
			return (int)(race->pen.count - 1);
		}

		/*
		 * Non-fresh: compute the ladder step target.  Mirrors
		 * kunos's FUN_140125f50:159-189 with bVar6 = (force+2)*2,
		 * giving 4 (SG30) when force=0, 6 (DQ) when force=1.
		 */
		switch (old_sev) {
		case EXE_DT:
			new_sev = (exe_kind > EXE_DT) ? bVar6 : EXE_SG30;
			break;
		case EXE_SG10:
		case EXE_SG20:
			new_sev = bVar6;
			break;
		case EXE_SG30:
			if (force == 0)
				return -1;	/* terminal */
			new_sev = EXE_DQ;
			break;
		default:
			return -1;		/* DQ reached, terminal */
		}

		/*
		 * Replace the existing entry in place.  The per-car model
		 * guarantees at most one active DT/SG slot; find it by
		 * scanning backwards for the first unserved DT/SG entry.
		 * Mirrors kunos's remove+recreate at FUN_140125f50:161.
		 */
		{
			uint8_t new_pen = penalty_pen_kind_of(new_sev, 0, 0);
			struct PenaltyQueue *q = &race->pen;
			int i;
			for (i = q->count - 1; i >= 0; i--) {
				if (q->slots[i].served)
					continue;
				if (!penalty_kind_is_dtsg(q->slots[i].kind))
					continue;
				q->slots[i].kind = new_pen;
				/*
				 * The exe hardcodes laps_remaining=3 for all
				 * ladder steps (140125f50.c lines 181, 187).
				 * Exception: DT incoming with current slot
				 * already at SG+ uses 0 (line 163).
				 */
				q->slots[i].laps_remaining =
				    (old_sev == EXE_DT && exe_kind > EXE_DT)
				    ? 0 : 3;
				q->slots[i].issued_ms = now_ms;
				/* Reason updates to the incoming report's. */
				q->slots[i].reason = reason;
				/*
				 * Clear the admin flag on promotion.  The exe has
				 * no admin-flag concept; a promoted entry is a
				 * server-confirmed event and must be visible in
				 * the active_pen prefix and pq_emit array.
				 */
				q->slots[i].admin = 0;
				step_slot = i;
				break;
			}
			(void)collision;
		}

		race->dtsg_ladder_sev = new_sev;

		if (new_sev == EXE_DQ) {
			s->cars[car_id].race.disqualified = 1;
			session_recompute_standings(s);
		}
		/*
		 * Ladder in-place edit changes the queued entry's kind,
		 * which the 0x36 per-car tail b0 reads.  Flag the
		 * leaderboard dirty so the next tick emits the post-step
		 * shape (DT->SG10, ..., SG30->DQ).  penalty_materialize
		 * already fires request_emit for fresh-entry paths; this
		 * covers the in-place mutation that doesn't go through
		 * materialize.
		 */
		leaderboard_request_emit(s);
		return step_slot;
	}
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
	 * Mark the entry served instead of dropping it from the queue, and
	 * stamp the lap it was cleared on.  Keeping the served record lets
	 * results.json report clearedInLap (>= 0 means served, the way the
	 * original server signals it) — without this the entry would be
	 * silently removed and the penalty would look perpetually open.
	 */
	q->slots[idx].served = 1;
	/* 0-based lap index matching violation_lap and exe FUN_140126b50. */
	q->slots[idx].cleared_lap =
	    (int16_t)s->cars[car_id].race.lap_count;
	q->slots[idx].laps_remaining = 0;
	/*
	 * Mirror FUN_140126b50: after serving a DT/SG the PenaltySheet's
	 * category (+0x58) and severity (+0x59) are zeroed via FUN_140124e00,
	 * so the next DT/SG report lands in the fresh branch (bVar2==0) and
	 * restarts the ladder.  Without this reset a served DT is still seen
	 * as the current ladder level and the next DT immediately escalates
	 * to SG30 instead of starting fresh.
	 */
	s->cars[car_id].race.dtsg_ladder_sev = 0;
	s->cars[car_id].race.dtsg_ladder_cat = 0;
}

void
penalty_clear(struct Server *s, int car_id)
{
	struct PenaltyQueue *q;
	int i, n = 0;

	if (car_id < 0 || car_id >= ACC_MAX_CARS)
		return;
	q = &s->cars[car_id].race.pen;
	/*
	 * Mirror exe FUN_140126b50: serve unserved DT/SG and DQ entries
	 * (stamp cleared_lap, set served=1) so results.json reports
	 * clearedInLap.  Served DQ entries stay in the queue; the 0x36
	 * tail scan in handshake.c skips served entries, so the wire
	 * reflects no active penalty after the clear.  TP entries are
	 * kept untouched.
	 */
	for (i = 0; i < q->count; i++) {
		struct PenaltyEntry *p = &q->slots[i];
		int is_tp = p->race_end_tp != 0 ||
		    p->kind == PEN_TP5  || p->kind == PEN_TP15 ||
		    p->kind == PEN_TP30 || p->kind == PEN_TP40 ||
		    p->kind == PEN_TP50 || p->kind == PEN_TP60;
		if (!is_tp && !p->served) {
			/* Serve DT/SG/DQ in place so results.json gets clearedInLap. */
			p->served = 1;
			p->cleared_lap =
			    (int16_t)s->cars[car_id].race.lap_count;
			p->laps_remaining = 0;
		}
		if (n != i)
			q->slots[n] = q->slots[i];
		n++;
	}
	if (n < q->count) {
		memset(&q->slots[n], 0,
		    (size_t)(q->count - n) * sizeof(q->slots[0]));
		q->count = n;
	}
	/*
	 * Reset the per-car DT/SG ladder so the next report lands on
	 * the fresh branch rather than being treated as an escalation.
	 */
	s->cars[car_id].race.dtsg_ladder_sev = 0;
	s->cars[car_id].race.dtsg_ladder_cat = 0;
	/*
	 * Clear DT/SG/DQ accumulators; preserve pen_state[EXE_TP].
	 */
	memset(s->cars[car_id].race.pen_state, 0,
	    EXE_TP * sizeof(*s->cars[car_id].race.pen_state));
	memset(&s->cars[car_id].race.pen_state[EXE_DQ], 0,
	    sizeof(*s->cars[car_id].race.pen_state));
	/*
	 * Mirror exe FUN_140126b50: after serving the DQ entry, clear
	 * the disqualified flag so the comparator stops ranking the car
	 * last and the standings update reflects the cleared state.
	 */
	s->cars[car_id].race.disqualified = 0;
	leaderboard_request_emit(s);
}

void
penalty_clear_all(struct Server *s)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		if (!s->cars[i].used)
			continue;
		penalty_clear(s, i);
	}
}

/*
 * penalty_clear_tp — drop only the car's time penalties, leaving DT/SG/DQ
 * intact.  Mirrors the stock server's /cleartp (admin action 7), which is
 * a time-penalty-only clear distinct from /clear's full PenaltySheet wipe
 * (verified against FUN_140021680's separate "clear" / "cleartp" arms and
 * the distinct "post race time penalties" race-control banner).  Targets
 * admin /tp5 /tp15 entries (PEN_TP5/PEN_TP15) and the race-end DT/SG -> TP
 * conversions (race_end_tp set, see penalty_convert_race_end).  The per-cat
 * escalation ladder is left untouched because the DT/SG entries it tracks
 * survive the clear.
 */
void
penalty_clear_tp(struct Server *s, int car_id)
{
	struct PenaltyQueue *q;
	int i, n = 0;

	if (car_id < 0 || car_id >= ACC_MAX_CARS)
		return;
	q = &s->cars[car_id].race.pen;
	/*
	 * Drop the post-race time accumulator so a /cleartp'd total does
	 * not re-materialise in full on the next TP report (the EXE_TP
	 * path only re-inits when severity is 0).  Leave the DT/SG ladder
	 * (dtsg_ladder_sev) alone -- those entries survive /cleartp.
	 */
	memset(&s->cars[car_id].race.pen_state[EXE_TP], 0,
	    sizeof(s->cars[car_id].race.pen_state[EXE_TP]));
	for (i = 0; i < q->count; i++) {
		const struct PenaltyEntry *p = &q->slots[i];
		int is_tp = p->race_end_tp != 0 ||
		    p->kind == PEN_TP5 || p->kind == PEN_TP15 ||
		    p->kind == PEN_TP30 || p->kind == PEN_TP40 ||
		    p->kind == PEN_TP50 || p->kind == PEN_TP60;
		if (is_tp)
			continue;	/* drop this time penalty */
		if (n != i)
			q->slots[n] = q->slots[i];
		n++;
	}
	if (n != q->count) {
		memset(&q->slots[n], 0,
		    (size_t)(q->count - n) * sizeof(q->slots[0]));
		q->count = n;
		/* Tail bytes change when a TP is removed; re-emit 0x36. */
		leaderboard_request_emit(s);
	}
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
		/*
		 * race_end_tp is set by penalty_convert_race_end when an
		 * unserved DT/SG would have escalated to a post-race TP.
		 * Honor that here so total_ms reflects the converted
		 * amount even though p->kind keeps the original wire code
		 * (kunos's pq_emit shows DT/SG verbatim after race-end).
		 */
		if (p->race_end_tp != 0) {
			total += p->race_end_tp_ms;
			continue;
		}
		switch (p->kind) {
		case PEN_TP5:
		case PEN_TP15:
			/*
			 * Live PostRaceTime: laps_remaining holds the true
			 * accumulated seconds (single per-car entry, see
			 * penalty_set_tp), so charge that rather than the
			 * coarse PEN_TP5 / PEN_TP15 enum bucket.
			 */
			if (p->laps_remaining > 0)
				total += (uint32_t)p->laps_remaining * 1000u;
			break;
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
penalty_convert_race_end(struct PenaltyQueue *q, int16_t lap_count,
    float time_multiplier)
{
	int i;
	uint8_t new_kind;
	uint32_t base_ms;
	int car_dq = 0;

	if (q == NULL)
		return;
	/*
	 * An unserved DT/SG with laps_remaining == 0 has two sources.  The
	 * serve-deadline countdown reaching 0 force-DQs the car (handlers.c),
	 * so that DT/SG is superseded by the DQ and must NOT convert.  A
	 * force=0 ladder escalation (DT -> SG30 terminal) also resets the
	 * value to 0 on the wire but leaves the car classified, and the exe
	 * FUN_140127440 still converts it -- that converter gates on the
	 * +0x59 severity byte alone, with no laps-remaining check.  Tell the
	 * two apart by the presence of a DQ entry: a DQ'd car's standings are
	 * already terminal, so skip its zero-lap entries; otherwise convert.
	 */
	for (i = 0; i < q->count; i++)
		if (q->slots[i].kind == PEN_DQ) {
			car_dq = 1;
			break;
		}
	for (i = 0; i < q->count; i++) {
		struct PenaltyEntry *p = &q->slots[i];
		if (p->served)
			continue;
		if (p->laps_remaining <= 0 && car_dq)
			continue;
		switch (p->kind) {
		case PEN_DT:	case PEN_DTC:	new_kind = PEN_TP30; base_ms = 30000; break;
		case PEN_SG10:	case PEN_SG10C:	new_kind = PEN_TP40; base_ms = 40000; break;
		case PEN_SG20:	case PEN_SG20C:	new_kind = PEN_TP50; base_ms = 50000; break;
		case PEN_SG30:	case PEN_SG30C:	new_kind = PEN_TP60; base_ms = 60000; break;
		default:
			continue;
		}
		/*
		 * DO NOT rewrite p->kind.  Kunos's pcap (1-min race
		 * scenario, run_race_end.sh) shows the original DT/SG
		 * wire code STAYS in pq_emit + per-session result emit
		 * even after race-end conversion.  Only penalty_total_ms
		 * uses the converted target (kind 1->TP30 = 30 s, etc.)
		 * to compute the post-race time delta.
		 *
		 * The per-car tail (handshake.c) hides the entry once
		 * race_end_tp is set so AC2's active_pen widget doesn't
		 * show a phantom DT after the race ends.
		 *
		 * Exe FUN_140127440 multiplies base seconds by the race
		 * session's timeMultiplier (ServerConfiguration+0xac).
		 */
		p->race_end_tp = new_kind;
		p->race_end_tp_ms = (uint32_t)((float)base_ms * time_multiplier);
		p->collision = 0;
		/*
		 * Mirror FUN_140127440: after converting, exe calls
		 * FUN_140126b50 to serve the original DT/SG entry at
		 * race-end time T.  FUN_140129b10 then counts how many
		 * laps started before T: all N completed laps qualify,
		 * so the loop result is N-1 (0-indexed last lap).
		 */
		p->cleared_lap = (int16_t)(lap_count - 1);
		/*
		 * Preserve laps_remaining — kunos's 0x3e per-car tail
		 * reads the original DT/SG value (e.g. 3 for a 3-cut
		 * report) even after race-end conversion.  Clearing it
		 * to 0 here would zero the b1 byte at the section end
		 * (pcap diff 2026-05-11 race-end test).  penalty_total_ms
		 * uses race_end_tp not laps_remaining so the converted
		 * 30s/40s/50s/60s buckets are unaffected.
		 */
		/* kind, reason stay as-is — race-control / cutting / etc. */
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
		case PEN_RBL:			return 6;
		}
		break;
	case REASON_PIT_SPEEDING:
		switch (kind) {
		case PEN_DT: case PEN_DTC:	return 7;
		case PEN_SG10: case PEN_SG10C:	return 8;
		case PEN_SG20: case PEN_SG20C:	return 9;
		case PEN_SG30: case PEN_SG30C:	return 10;
		case PEN_DQ:			return 11;
		case PEN_RBL:			return 12;
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
