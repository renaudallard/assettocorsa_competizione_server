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
    int collision, int32_t value, uint8_t reason)
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
	e->collision = collision ? 1 : 0;
	e->served = 0;
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
		    value, reason);
		return 0;
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
		 * If this category already reached DQ via the per-cat
		 * ladder (e.g. cat=0 r2 escalated from DT to DQ), kunos
		 * treats a subsequent DQ-direct (kind=6) report as
		 * terminal — no new Penalty pushed, no broadcast.  Pcap
		 * (2026-05-11 4-bot run): kunos's r6 = cat=0 kind=6 lands
		 * after r2 already ran the cat=0 ladder to DQ and produces
		 * NO 0x36 emit; the next mid-session emit is r15 = cat=3
		 * kind=6 which is the first cat=3 DQ event.
		 */
		if (category < sizeof(race->pen_cat_severity) &&
		    race->pen_cat_severity[category] == EXE_DQ) {
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
			return 0;
		}
		penalty_materialize(s, car_id, EXE_DQ, collision,
		    value, reason);
		if (category < sizeof(race->pen_cat_severity))
			race->pen_cat_severity[category] = EXE_DQ;
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
		/*
		 * Materialize with the CUMULATIVE counter as the entry's
		 * laps_remaining so the 0x36 per-car tail b1 reads the
		 * running total — kunos pcap (run_tp_accum.sh) shows the
		 * tail progressing `0e 32 / 0e 64 / 0e 96` (50 / 100 /
		 * 150) on three successive TP reports of value=50 each.
		 * accd was passing the per-report value, which left b1
		 * pinned at 50 forever and made the deep-compare cache
		 * skip intermediate emits (same bytes after each report).
		 */
		penalty_materialize(s, car_id, EXE_TP, collision,
		    (int32_t)st->counter, reason);
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
			penalty_materialize(s, car_id, EXE_DQ, 0, 0,
			    REASON_RACE_CONTROL);
			race->pen_state[EXE_DQ].severity = EXE_DQ;
			race->pen_state[EXE_DQ].issued_ms = now_ms;
			/*
			 * Mirror the ladder-step path above: mark this
			 * category as terminal so a follow-up direct DQ
			 * on the same category hits the dedup guard at
			 * line 228 and doesn't materialise twice.
			 */
			if (category < sizeof(race->pen_cat_severity))
				race->pen_cat_severity[category] = EXE_DQ;
		}
		return 0;
	}

	/*
	 * DT/SG ladder — kunos's FUN_140125f50 keeps a per-car sheet
	 * keyed by category.  A second report for the same category
	 * steps the ladder regardless of the incoming kind.  Each step
	 * materialises the current severity + advances:
	 *   force=0:  DT -> SG30 (terminal)
	 *   force=1:  DT -> DQ
	 * SG30 -> DQ only when force=1.  DQ is terminal.  We mirror
	 * this with a per-category byte (pen_cat_severity[category])
	 * so different-kind reports for the same cat collapse onto
	 * one ladder, matching the pcap-observed kunos tail of 05 00
	 * after four sequential cat=0 reports.
	 */
	{
		uint8_t old_sev;
		uint8_t new_sev;
		uint8_t bVar6;

		if (category >= sizeof(race->pen_cat_severity))
			category = 0;	/* defensive: clamp out-of-enum */

		old_sev = race->pen_cat_severity[category];
		bVar6 = (uint8_t)((force + 2) * 2);
		/* bVar6 = 4 (SG30) if force=0, 6 (DQ) if force=1. */

		if (old_sev == 0) {
			/*
			 * Fresh category: keep the existing per-exe_kind
			 * sheet (used by other code paths, e.g. admin /sg
			 * counter) AND record the cat ladder state.
			 */
			st = &race->pen_state[exe_kind];
			st->severity = exe_kind;
			st->category = category;
			st->issued_ms = now_ms;
			st->reason = reason;
			st->counter = value;
			race->pen_cat_severity[category] = exe_kind;
			penalty_materialize(s, car_id, exe_kind, collision,
			    value, reason);
			return 0;
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
				return 0;	/* terminal */
			new_sev = EXE_DQ;
			break;
		default:
			return 0;		/* DQ reached, terminal */
		}

		/*
		 * Replace the existing cat entry in place so the post-step
		 * wire reaches the next broadcast.  Mirrors kunos's
		 * remove+recreate at FUN_140125f50:161 (FUN_140126b50
		 * removes) which then loops back to LAB_140125fb0 and
		 * creates a new entry with kind=bVar6 value=0.
		 */
		{
			uint8_t old_pen = penalty_pen_kind_of(old_sev,
			    collision, value);
			uint8_t new_pen = penalty_pen_kind_of(new_sev, 0, 0);
			struct PenaltyQueue *q = &race->pen;
			int i;
			for (i = q->count - 1; i >= 0; i--) {
				if (!q->slots[i].served &&
				    q->slots[i].kind == old_pen &&
				    q->slots[i].reason == reason) {
					q->slots[i].kind = new_pen;
					q->slots[i].laps_remaining = 0;
					q->slots[i].issued_ms = now_ms;
					break;
				}
			}
		}

		race->pen_cat_severity[category] = new_sev;

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
	}
	return 0;
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
	/*
	 * Reset the per-cat ladder so a subsequent client 0x41 for any
	 * cat lands on the fresh branch (and emits the report's wire in
	 * the tail) rather than being treated as an escalation step.
	 */
	memset(s->cars[car_id].race.pen_cat_severity, 0,
	    sizeof(s->cars[car_id].race.pen_cat_severity));
	/*
	 * 0x36 per-car tail bytes change when the queue is wiped — the
	 * memcmp-cache must re-emit so AC2 stops rendering the cleared
	 * penalty.  Same family as the chat.c:236 post-eviction fix.
	 */
	leaderboard_request_emit(s);
}

void
penalty_clear_all(struct Server *s)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct PenaltyQueue *q;
		if (!s->cars[i].used)
			continue;
		q = &s->cars[i].race.pen;
		q->count = 0;
		memset(q->slots, 0, sizeof(q->slots));
		memset(s->cars[i].race.pen_cat_severity, 0,
		    sizeof(s->cars[i].race.pen_cat_severity));
	}
	/* Single emit covers the multi-car wipe. */
	leaderboard_request_emit(s);
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
			switch (p->race_end_tp) {
			case PEN_TP30: total += 30000; break;
			case PEN_TP40: total += 40000; break;
			case PEN_TP50: total += 50000; break;
			case PEN_TP60: total += 60000; break;
			}
			continue;
		}
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
		 */
		p->race_end_tp = new_kind;
		p->collision = 0;
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
