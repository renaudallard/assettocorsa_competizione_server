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
 * handlers.c -- per-msg-id handlers.
 *
 * These correspond one-for-one to the 21 TCP dispatcher cases and
 * the 7 UDP cases in the binary's main message dispatcher.  See
 * notebook-b/NOTEBOOK_B.md §5.6.1 and §5.6.2.
 *
 * Implementation strategy: the relay-path handlers (tier 1) read
 * the minimum fields they need for validation, then build a fresh
 * outgoing body and broadcast it via bcast_all.  The transform-
 * path handlers (tier 2, e.g. lap completed -> 0x1b) do the
 * per-recipient broadcast the same way since no per-recipient
 * delta transformation has been implemented yet.
 *
 * Car state mutations are TODO-flagged for each handler that
 * needs a larger per-car state struct than we currently have.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <math.h>

#include "bcast.h"
#include "chat.h"
#include "handlers.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "penalty.h"
#include "prim.h"
#include "ratings.h"
#include "session.h"
#include "state.h"
#include "tick.h"

/*
 * Helper: verify that the given carId matches the car slot owned
 * by this connection.  Returns 0 if valid, -1 if not.
 */
static int
check_car_owner(struct Conn *c, uint16_t wire_car_id)
{
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS)
		return -1;
	/* Compare against the wire ID (base 1001), not the slot index. */
	return ((uint16_t)(ACC_CAR_ID_BASE + c->car_id) == wire_car_id) ? 0 : -1;
}

/* ----- 0x19 SA contact report -> broadcast 0x1b ----------------- */
/*
 * Despite the legacy ACP_LAP_COMPLETED name (kept in msg.h to avoid
 * touching every call site), 0x19 is actually the safety-rating
 * contact report, NOT a lap-completed event.  The real lap-completed
 * event is 0x21 (handled by h_sector_split_single — the msg.h
 * ACP_SECTOR_SPLIT_SINGLE name is also a misnomer).
 *
 * Body (10 B with msg id):
 *   u8  0x19
 *   u16 reporter_car_id      (the player's own car, source Car+0x958
 *                             on AC2 client)
 *   u16 target_car_id        (the other car involved, or 0xffff for a
 *                             wall hit)
 *   i32 timestamp            (raw client clock; exe normalises via
 *                             FUN_140042030)
 *   u8  quality              (signed: <0 -> -1.0f "invalid contact",
 *                             non-negative -> byte/10.0f rating quality)
 *
 * AC2 client emit sites: 1410be570.c:40,109 (wall / car-to-car
 * contact) and 140e3f9c0.c:960 (SA-contact handler with the
 * "SA contact: %f obwp" log).  Server case 0x19 at exe
 * 1400142f0.c:174-205 builds a queued-broadcast lambda.  The 0x1b
 * wire builder at 1400179b0.c reads u16 carA + u16 carB + double +
 * float quality, preserving order.
 *
 * Pre-2026-05-08 this handler treated the body as
 * (cup_position, track_position, lap_time_ms, quality) and stored
 * the contact i32 timestamp into race->current_lap_ms — a
 * documented bookkeeping bug.  See notebook-b §5.6.1 0x19 row.
 */

int
h_lap_completed(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint16_t reporter_car_id, target_car_id;
	int32_t timestamp;
	uint8_t quality;
	struct ByteBuf out;
	int rc;

	(void)s;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0)
		return -1;
	if (rd_u16(&r, &reporter_car_id) < 0 ||
	    rd_u16(&r, &target_car_id) < 0 ||
	    rd_i32(&r, &timestamp) < 0 ||
	    rd_u8(&r, &quality) < 0) {
		log_warn("h_sa_contact: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS)
		return 0;

	log_info("SA contact: conn=%u reporter=%u target=%u ts=%d qual=%u",
	    (unsigned)c->conn_id, (unsigned)reporter_car_id,
	    (unsigned)target_car_id, (int)timestamp, (unsigned)quality);

	/*
	 * Pure relay — no race state mutation.  The contact event is
	 * informational for other clients (HUD overlay) and for the
	 * server's safety-rating module (handled separately in
	 * ratings.c if/when implemented).  Exe relays unconditionally
	 * (no isSessionOver guard at case 0x19) and sends to ALL
	 * including the sender.
	 */
	bb_init(&out);
	if (wr_u8(&out, SRV_LAP_BROADCAST) < 0 ||
	    wr_u16(&out, reporter_car_id) < 0 ||
	    wr_u16(&out, target_car_id) < 0 ||
	    wr_i32(&out, timestamp) < 0 ||
	    wr_u8(&out, quality) < 0)
		goto out;
	rc = bcast_all(s, out.data, out.wpos, BCAST_EXCEPT_NONE);
	(void)rc;
out:
	bb_free(&out);
	return 0;
}

/* ----- 0x20 ACP_SECTOR_SPLIT ------------------------------------ */
/*
 * True wire format (12 bytes):
 *   u8 msg_id + i32 sector_time_ms + u8 sector_index +
 *   i32 clock_ms + u16 car_field
 *
 * sector_index: 0/1/2 for the three track sectors.
 * sector_time_ms: time for this sector only (not cumulative).
 * clock_ms: total race time.
 *
 * Lap completion: track sector times and increment lap_count
 * when sector_index wraps back to 0 (i.e. the car crossed
 * the start/finish line after completing all 3 sectors).
 *
 * The exe never relays 0x3a (bulk); it only sends 0x3b
 * (single split relay) from the 0x21 handler.
 */

int
h_sector_split_bulk(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, sector_index;
	int32_t sector_time_ms, clock_ms;
	uint16_t car_field;
	struct CarRaceState *race;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_i32(&r, &sector_time_ms) < 0 ||
	    rd_u8(&r, &sector_index) < 0 ||
	    rd_i32(&r, &clock_ms) < 0 ||
	    rd_u16(&r, &car_field) < 0) {
		log_warn("h_sector_split: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS)
		return 0;
	if (s->session.phase >= PHASE_COMPLETED) {
		log_info("sector split ignored: session over (car=%d)",
		    c->car_id);
		return 0;
	}
	race = &s->cars[c->car_id].race;

	/*
	 * Per kunos's FUN_1400142f0 dispatch table, 0x20 is the per-
	 * sector split for sectors 0 and 1 only -- the S/F crossing
	 * is signalled exclusively by 0x21 (h_sector_split_single).
	 * Lap-complete bookkeeping (lap_count++, last_lap, best_lap,
	 * lap_history, ratings_on_lap, lobby_notify_lap, penalty serve
	 * countdown, recompute_standings) lives in that handler.  This
	 * handler just records the per-sector time and the raw clock so
	 * lap-complete can read them when it fires at S/F.  The old
	 * 'sector_index==2 && all sectors set' workaround that used
	 * to live here counted laps only for the test bot (which
	 * incorrectly emitted 0x20 with sector=2); real ACC clients
	 * send 0x20 only for sectors 0/1 and P/Q laps never got
	 * counted server-side.
	 */
	if (sector_index < 3)
		race->sector_ms[sector_index] = sector_time_ms;
	race->race_time_ms = clock_ms;
	(void)car_field;	/* per-split client flag bits unused here */
	log_info("sector split: car=%d sector=%u time=%dms clock=%d",
	    c->car_id, (unsigned)sector_index, (int)sector_time_ms,
	    (int)clock_ms);
	return 0;
}

/* ----- 0x21 ACP_SECTOR_SPLIT (single) -> broadcast 0x3b ---------- */

int
h_sector_split_single(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, flag_b, flag_d;
	int32_t split_time, lap_time;
	uint16_t car_field;
	struct ByteBuf out;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_i32(&r, &split_time) < 0 ||
	    rd_i32(&r, &lap_time) < 0 ||
	    rd_u8(&r, &flag_b) < 0 ||
	    rd_u16(&r, &car_field) < 0 ||
	    rd_u8(&r, &flag_d) < 0) {
		log_warn("h_sector_split_single: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS)
		return 0;
	/* Skip bookkeeping past session end — match the exe's
	 * isSessionOver guard. */
	if (s->session.phase >= PHASE_COMPLETED)
		return 0;
	{
		struct CarRaceState *race = &s->cars[c->car_id].race;
		/*
		 * 0x21 is kunos's lap-completed event (FUN_1400142f0 case
		 * 0x21 logs "New laptime: %d for carId %d").  Per the body
		 * layout in the exe:
		 *   first  u32 = laptime   (we still call it split_time)
		 *   second u32 = raw_ts    (we still call it lap_time)
		 *   u8 flag_b / u16 car_field / u8 flag_d trail.
		 * The full lap-complete bookkeeping (lap_count++, last_lap,
		 * best_lap, lap_history, ratings_on_lap, lobby_notify_lap,
		 * penalty serve countdown, recompute_standings) runs once
		 * per S/F crossing, not gated on a "sector_index==2 + all
		 * sectors set" guess like the prior 0x20-only workaround.
		 */
		int32_t lap_ms = split_time;
		int has_cut = (car_field & 0x0001) != 0;
		int is_out_lap = (car_field & 0x0004) != 0;
		int invalid = has_cut || is_out_lap ||
		    race->out_of_track_latched;

		/*
		 * Reject negative wire lap_ms.  The field is signed only
		 * because the kunos wire shape is i32; a real client never
		 * produces lap_ms < 0.  Without the guard, a crafted 0x21
		 * with lap_time = -1000 would set best_lap_ms negative
		 * (the existing best==0 sentinel fires), and session.c's
		 * cmp_cars would then sort the attacker ahead of every
		 * legitimate driver.  Same path also pollutes lobby +
		 * SMPR last_lap_ms.  Treat negative as "invalid lap" so
		 * lap_count still advances but no lap-time field is
		 * updated from the corrupt value.
		 */
		if (lap_ms < 0)
			invalid = 1;

		if (!race->formation_lap_done)
			race->formation_lap_done = 1;

		race->lap_count++;
		if (!invalid)
			race->last_lap_ms = lap_ms;
		if (!invalid && (race->best_lap_ms == 0 ||
		    lap_ms < race->best_lap_ms))
			race->best_lap_ms = lap_ms;

		/*
		 * Per-car lap history (drives 0x36 list 2 + 0x56 garage).
		 *
		 * Invalid laps (cut / out-lap / latched out-of-track) are
		 * skipped — kunos pcap (TP-then-admin-DQ scenario,
		 * 2026-05-13) shows the std::vector at car ctor stays
		 * INT32_MAX-sentinel-padded for invalid completions; our
		 * earlier behaviour of writing lap_ms=0 into the ring slot
		 * grew the 0x36 leaderboard payload by 4 B per recorded
		 * invalid lap and produced visible byte-level divergence
		 * against the exe.  lap_count above still ticks regardless
		 * so the per-car lap counter is preserved.
		 */
		if (!invalid) {
			uint32_t slot = race->lap_history_count
			    % ACC_LAP_HISTORY;
			int si;
			race->lap_history_ms[slot] = lap_ms;
			for (si = 0; si < 3; si++)
				race->lap_splits_ms[slot][si] =
				    race->sector_ms[si];
			race->lap_history_count++;
		}

		/* Best-sector tracking from the just-completed lap's
		 * stored sector_ms[].  Skip invalid laps so a slow out-lap
		 * or cut lap doesn't lock the session-best column. */
		if (!invalid) {
			int si;
			for (si = 0; si < 3; si++) {
				int32_t st = race->sector_ms[si];
				if (st > 0 &&
				    (race->best_sectors_ms[si] == 0 ||
				    st < race->best_sectors_ms[si]))
					race->best_sectors_ms[si] = st;
			}
		}

		/* Per-lap state reset + optional cut-counter clear
		 * broadcast.  Matches the kunos pcap (zero 0x3c emits on
		 * lap-boundaries for clean laps; one on a cut clear). */
		{
			uint8_t had_cuts = race->cuts_this_lap;
			race->current_lap_ms = 0;
			race->out_of_track_latched = 0;
			race->cuts_this_lap = 0;
			race->last_cut_ms = 0;
			race->sector_ms[0] = 0;
			race->sector_ms[1] = 0;
			race->sector_ms[2] = 0;
			if (had_cuts > 0) {
				struct ByteBuf reset;
				bb_init(&reset);
				if (wr_u8(&reset, SRV_OUT_OF_TRACK_RELAY) == 0 &&
				    wr_u16(&reset,
					s->cars[c->car_id].car_id) == 0 &&
				    wr_u16(&reset, 0) == 0 &&
				    wr_u32(&reset, 0) == 0)
					(void)bcast_all(s, reset.data,
					    reset.wpos, BCAST_EXCEPT_NONE);
				bb_free(&reset);
			}
		}

		/* Local rating EWMA: clean lap +5 SA, cut -25, out-lap
		 * skipped.  Keyed by current driver's steam_id. */
		ratings_on_lap(s,
		    s->cars[c->car_id].drivers[
			s->cars[c->car_id].current_driver_index
		    ].steam_id, has_cut, is_out_lap);

		/* kson lobby: only valid laps (invalid bumps their ghost
		 * best, which we don't want). */
		if (!invalid)
			lobby_notify_lap(&s->lobby,
			    s->cars[c->car_id].car_id,
			    (uint16_t)s->cars[c->car_id].race_number,
			    lap_ms, race->race_time_ms);

		/* DT/SG serve-deadline countdown.  Three racing laps to
		 * serve, else auto-DQ (or SG30 if allowAutoDQ=0). */
		if (race->pen.count > 0 && !race->disqualified) {
			int pi = penalty_first_unserved_dtsg(&race->pen);
			struct PenaltyEntry *front = pi >= 0
			    ? &race->pen.slots[pi] : NULL;
			if (front != NULL && front->laps_remaining > 0) {
				front->laps_remaining--;
				if (front->laps_remaining == 0) {
					uint8_t inherited = front->reason;
					char chat[128];
					if (s->allow_auto_dq) {
						uint8_t cat;
						log_info("Car %d failed to "
						    "serve %s -> DQ",
						    c->car_id,
						    penalty_name(front->kind));
						cat = (uint8_t)penalty_wire_value(
						    PEN_DQ, inherited);
						penalty_enqueue(s, c->car_id,
						    EXE_DQ, cat, 3, 1, 0,
						    inherited);
						penalty_format_chat(chat,
						    sizeof(chat), PEN_DQ,
						    inherited, 0,
						    s->cars[c->car_id].race_number);
						chat_broadcast(s, chat, 4);
					} else {
						uint8_t cat;
						log_info("Car %d failed to "
						    "serve %s -> SG30 "
						    "(allowAutoDQ=0)",
						    c->car_id,
						    penalty_name(front->kind));
						cat = (uint8_t)penalty_wire_value(
						    PEN_SG30, inherited);
						penalty_enqueue(s, c->car_id,
						    EXE_SG30, cat, 3, 0, 0,
						    inherited);
						penalty_format_chat(chat,
						    sizeof(chat), PEN_SG30,
						    inherited, 0,
						    s->cars[c->car_id].race_number);
						chat_broadcast(s, chat, 4);
					}
				}
			}
		}

		log_info("lap completed: car=%d lap=%d time=%dms clock=%d%s",
		    c->car_id, race->lap_count, (int)lap_ms,
		    (int)lap_time, invalid ? " (INVALID)" : "");

		/*
		 * Kunos-format Lap line for log scrapers (accweb, etc.):
		 *   Lap carId N, driverId N, lapTime M:S:fff,
		 *   timestampMS N.000000, flags: N,
		 *   S1 M:S:fff, S2 M:S:fff, S3 M:S:fff,
		 *   fuel 0.000000[, hasCut][, InLap][, OutLap][, SessionOver]
		 *
		 * fuel is emitted as 0.000000 -- accd doesn't track per-
		 * lap fuel at the server level (the client-side telemetry
		 * carrying fuel is the 0x1e physics relay, which we don't
		 * snapshot per lap).  Operators who need fuel telemetry
		 * use the 0x1e relay directly.
		 */
		{
			uint16_t cf = car_field;
			int is_inlap = (cf & 0x0008) != 0;
			int session_over = (cf & 0x0400) != 0;
			int lm = lap_ms < 0 ? 0 : lap_ms;
			int s1 = race->sector_ms[0] < 0 ? 0 : race->sector_ms[0];
			int s2 = race->sector_ms[1] < 0 ? 0 : race->sector_ms[1];
			int s3 = race->sector_ms[2] < 0 ? 0 : race->sector_ms[2];
			uint8_t didx = s->cars[c->car_id].current_driver_index;
			char tail[64];

			tail[0] = '\0';
			if (has_cut)
				strncat(tail, ", hasCut",
				    sizeof(tail) - strlen(tail) - 1);
			if (is_inlap)
				strncat(tail, ", InLap",
				    sizeof(tail) - strlen(tail) - 1);
			if (is_out_lap)
				strncat(tail, ", OutLap",
				    sizeof(tail) - strlen(tail) - 1);
			if (session_over)
				strncat(tail, ", SessionOver",
				    sizeof(tail) - strlen(tail) - 1);

			log_kunos("Lap carId %d, driverId %u, "
			    "lapTime %d:%d:%d, timestampMS %d.000000, "
			    "flags: %u, "
			    "S1 %d:%d:%d, S2 %d:%d:%d, S3 %d:%d:%d, "
			    "fuel 0.000000%s",
			    ACC_CAR_ID_BASE + c->car_id,
			    (unsigned)didx,
			    lm / 60000, (lm / 1000) % 60, lm % 1000,
			    (int)lap_time,
			    (unsigned)cf,
			    s1 / 60000, (s1 / 1000) % 60, s1 % 1000,
			    s2 / 60000, (s2 / 1000) % 60, s2 % 1000,
			    s3 / 60000, (s3 / 1000) % 60, s3 % 1000,
			    tail);
		}

		session_recompute_standings(s);

		if (s->session.phase == PHASE_OVERTIME) {
			session_overtime_car_finished(s);
			session_quali_drop_eligibility(s, c->car_id);
		}
	}

	/* Build the transformed 0x3b broadcast. Body:
	 *   u16 car_id + u32 split_time + u8 flag + u32 lap_time +
	 *   u16 flags. */
	bb_init(&out);
	if (wr_u8(&out, SRV_SECTOR_SPLIT_RELAY) < 0 ||
	    wr_u16(&out, s->cars[c->car_id].car_id) < 0 ||
	    wr_u32(&out, (uint32_t)split_time) < 0 ||
	    wr_u8(&out, flag_b) < 0 ||
	    wr_u32(&out, (uint32_t)lap_time) < 0 ||
	    wr_u16(&out, car_field) < 0)
		goto done;
	(void)bcast_all(s, out.data, out.wpos, c->conn_id);
done:
	bb_free(&out);
	return 0;
}

/* ----- 0x2a ACP_CHAT -> chat_process + 0x2b broadcast ----------- */

int
h_chat(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	char *text = NULL;
	const char *sender;
	int handled;
	struct ByteBuf out;

	/*
	 * AC2 client outbound 0x2a wire (FUN_14352d760 + FUN_143461160 +
	 * trailing i32 from param_1+0x650):
	 *   u8  0x2a
	 *   str_a text         (single string; max 35 codepoints per
	 *                       kunos's FUN_1400142f0 case 0x2a)
	 *   i32 client_ts_ms   (server-side ignored by kunos)
	 *
	 * Sender is server-side: the per-car driver name resolved from
	 * the connection's car_id, not the wire.  Previously this handler
	 * read two str_a as (sender, text) which doesn't match the
	 * client's actual emit — every chat command except echo broke
	 * for real AC2 clients.
	 */
	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0)
		return -1;
	if (rd_str_a(&r, &text) < 0) {
		log_warn("h_chat: short body from conn=%u",
		    (unsigned)c->conn_id);
		free(text);
		return 0;
	}
	/* The trailing i32 timestamp is read for completeness but
	 * doesn't gate anything — kunos doesn't consume it either. */
	{
		int32_t client_ts_ms;
		(void)rd_i32(&r, &client_ts_ms);
		(void)client_ts_ms;
	}
	/*
	 * Sender: driver display name from the car_entry.  Falls back
	 * to "BOT_N" / numeric id if the car has no driver string.
	 */
	sender = "<anon>";
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS &&
	    s->cars[c->car_id].used) {
		struct CarEntry *car = &s->cars[c->car_id];
		uint8_t idx = car->current_driver_index;
		/*
		 * current_driver_index is clamped on entrylist load
		 * (entrylist.c) and on every driver-swap path, but
		 * confirm here before deref'ing the drivers[] array —
		 * an OOB read past the 4-element array would land on
		 * an arbitrary CarEntry tail field.
		 */
		if (idx < car->driver_count &&
		    idx < ACC_MAX_DRIVERS_PER_CAR &&
		    car->drivers[idx].short_name[0] != '\0')
			sender = car->drivers[idx].short_name;
	}
	log_info("CHAT %s: %s", sender, text);
	/* accweb regex: ^CHAT (.*?): (.*)$  -- stdout plain form */
	log_kunos("CHAT %s: %s", sender, text);
	if (text != NULL && strstr(text, "%%") != NULL) {
		log_warn("h_chat: dropping message with format "
		    "specifier from conn=%u", (unsigned)c->conn_id);
		goto out;
	}
	handled = chat_process(s, c, text);
	if (handled == 0) {
		bb_init(&out);
		/*
		 * chat_type=0 is the regular driver-to-driver lane that
		 * the AC2 client renders in the chat window with the
		 * driver name.  Previously hardcoded to 4 (server / SRV
		 * notification overlay), which made every player message
		 * surface as a system banner instead of in the chat list
		 * -- operator-reported on celeborn 2026-05-14.  Server
		 * announcements (chat_broadcast, /admin elevation reply,
		 * /resetWeekend) keep their 4 / 5 codes; only the player-
		 * relay path moves to 0.
		 */
		if (wr_u8(&out, SRV_CHAT_OR_STATE) == 0 &&
		    wr_str_a(&out, sender) == 0 &&
		    wr_str_a(&out, text) == 0 &&
		    wr_i32(&out, 0) == 0 &&
		    wr_u8(&out, 0) == 0)
			(void)bcast_all(s, out.data, out.wpos,
			    c->conn_id);
		bb_free(&out);
	}
out:
	free(text);
	return 0;
}

/* ----- 0x2e ACP_CAR_SYSTEM_UPDATE -> broadcast 0x2e ------------- */

int
h_car_system_update(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint16_t car_id;
	uint64_t sys_data;
	struct ByteBuf out;
	int rc;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u16(&r, &car_id) < 0 ||
	    rd_u64(&r, &sys_data) < 0) {
		log_warn("h_car_system_update: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	if (check_car_owner(c, car_id) < 0) {
		log_warn("Received ACP_CAR_SYSTEM_UPDATE for wrong car "
		    "- senderId %u, carId %u",
		    (unsigned)c->conn_id, (unsigned)car_id);
		return 0;
	}
	s->cars[c->car_id].last_sys_data = sys_data;
	log_info("car system: car=%u data=%016llx",
	    (unsigned)car_id, (unsigned long long)sys_data);

	bb_init(&out);
	if (wr_u8(&out, SRV_CAR_SYSTEM_RELAY) < 0 ||
	    wr_u16(&out, car_id) < 0 ||
	    wr_u64(&out, sys_data) < 0)
		goto out;
	rc = bcast_all(s, out.data, out.wpos, c->conn_id);
	log_info("Updated %d clients with new carSystem for car %u (%llu)",
	    rc, (unsigned)car_id, (unsigned long long)sys_data);
out:
	bb_free(&out);
	return 0;
}

/* ----- 0x2f ACP_TYRE_COMPOUND_UPDATE -> broadcast 0x2f ---------- */

int
h_tyre_compound_update(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint16_t car_id;
	uint8_t compound;
	struct ByteBuf out;
	int rc;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u16(&r, &car_id) < 0 ||
	    rd_u8(&r, &compound) < 0) {
		log_warn("h_tyre_compound_update: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	if (check_car_owner(c, car_id) < 0) {
		log_warn("Received ACP_TYRE_COMPOUND_UPDATE for wrong car "
		    "- senderId %u, carId %u",
		    (unsigned)c->conn_id, (unsigned)car_id);
		return 0;
	}
	s->cars[c->car_id].race.current_tyres = compound;
	log_info("tyre: car=%u compound=%u",
	    (unsigned)car_id, (unsigned)compound);

	bb_init(&out);
	if (wr_u8(&out, SRV_TYRE_COMPOUND_RELAY) < 0 ||
	    wr_u16(&out, car_id) < 0 ||
	    wr_u8(&out, compound) < 0)
		goto out;
	rc = bcast_all(s, out.data, out.wpos, c->conn_id);
	log_info("Updated %d clients with new tyreCompound for car %u",
	    rc, (unsigned)car_id);
out:
	bb_free(&out);
	return 0;
}

/* ----- 0x32 ACP_CAR_LOCATION_UPDATE -> broadcast 0x32 ----------- */

int
h_car_location_update(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, location;
	uint16_t car_id;
	struct ByteBuf out;
	int rc;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u16(&r, &car_id) < 0 ||
	    rd_u8(&r, &location) < 0) {
		log_warn("h_car_location_update: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	if (check_car_owner(c, car_id) < 0) {
		log_warn("Received ACP_CAR_LOCATION_UPDATE for wrong car "
		    "- senderId %u, carId %u",
		    (unsigned)c->conn_id, (unsigned)car_id);
		return 0;
	}
	log_info("car location: car=%u loc=%u",
	    (unsigned)car_id, (unsigned)location);

	/*
	 * Phase 9 auto-penalty: pit-speeding detection.
	 *
	 * carLocation enum: NONE=0, Track=1, Pitlane=2, PitEntry=3,
	 * PitExit=4.  When the car is in the pitlane and its
	 * velocity (vec_c magnitude in m/s) exceeds the pit limit,
	 * issue a drive-through penalty.
	 *
	 * The pit limit varies per track but 80 km/h (~22.2 m/s)
	 * is a safe upper bound for every ACC track.
	 */
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
		struct CarEntry *car = &s->cars[c->car_id];
		struct CarRaceState *race = &car->race;
		uint8_t was_in_pit = race->in_pit;

		race->in_pit = (location == 2 || location == 3 ||
		    location == 4) ? 1 : 0;
		race->on_track = (location == 1) ? 1 : 0;

		/*
		 * Pit-entry timestamp for stop-and-go validation.
		 * Latch the monotonic clock the first time we see
		 * in_pit=1 after a non-pit location so the pit-exit
		 * check below can compute the dwell time the driver
		 * actually spent in the pit box.
		 */
		if (!was_in_pit && race->in_pit) {
			race->pit_entry_ms = mono_ms();
			race->pit_entry_driver_index =
			    car->current_driver_index;
		}

		/*
		 * Driver-stint tracker: accumulate on-track time per
		 * current_driver_index.  Start when transitioning from
		 * non-track to Track (location=1); stop on any other
		 * location (pit-lane traversal paused).
		 */
		if (location == 1 && was_in_pit)
			stint_start_tracking(s, c->car_id);
		else if (location != 1 && !was_in_pit && race->in_pit)
			stint_stop_tracking(s, c->car_id);
		else if (location == 0)
			stint_stop_tracking(s, c->car_id);

		/*
		 * Pitlane-speeding DQ is gated on s->session.green_fired:
		 * the penalty is only meaningful once the race is actually
		 * racing.  During the formation lap the client's own
		 * routing can briefly report location=Pitlane as the car
		 * crosses near the pit-exit boundary, and the driver has
		 * not yet been constrained by the post-green pitlane limit
		 * — killing the race there before green even fires is a
		 * false positive.  The check stays live for the whole
		 * PHASE_SESSION / OVERTIME window.
		 */
		if (location == 2 && car->rt.has_data &&
		    s->session.green_fired) {
			float vx = car->rt.vec_c[0];
			float vy = car->rt.vec_c[1];
			float vz = car->rt.vec_c[2];
			float speed = sqrtf(vx * vx + vy * vy + vz * vz);

			/*
			 * Kunos classifies pit-lane speeding as
			 * reckless driving and disqualifies the car
			 * outright; allowAutoDQ does not downgrade
			 * this since 1.8.11.  Use PEN_DQ, not PEN_DT.
			 */
			if (speed > 22.22f && !race->pit_crossing_latched) {
				char chat[128];
				log_info("PITLANE SPEEDING for car #%d "
				    "speed=%.1f m/s -> DQ",
				    car->race_number, speed);
				penalty_enqueue(s, c->car_id, EXE_DQ, 11,
				    3, 1, 0, REASON_PIT_SPEEDING);
				penalty_format_chat(chat, sizeof(chat),
				    PEN_DQ, REASON_PIT_SPEEDING, 0,
				    car->race_number);
				chat_broadcast(s, chat, 4);
				race->pit_crossing_latched = 1;
			}
		}
		if (location == 1)
			race->pit_crossing_latched = 0;

		/*
		 * Pit-lane exit: no server-side dwell verification.  Kunos
		 * has no equivalent — the AC2 client engine validates the
		 * dwell time itself and emits 0x42 when it decides the
		 * DT/SG was served, which the server then propagates via
		 * FUN_140126b50 (h_penalty_cleared on our side).  We
		 * mirror that.
		 */
		(void)was_in_pit;
	}

	bb_init(&out);
	if (wr_u8(&out, ACP_CAR_LOCATION_UPDATE) < 0 ||
	    wr_u16(&out, car_id) < 0 ||
	    wr_u8(&out, location) < 0)
		goto out;
	rc = bcast_all(s, out.data, out.wpos, c->conn_id);
	(void)rc;
out:
	bb_free(&out);
	return 0;
}

/* ----- 0x3d ACP_OUT_OF_TRACK -> broadcast 0x3c ------------------ */

int
h_out_of_track(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, force;
	int32_t ts_raw;
	struct ByteBuf out;
	int rc;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u8(&r, &force) < 0 ||
	    rd_i32(&r, &ts_raw) < 0) {
		log_warn("h_out_of_track: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS) {
		log_warn("received ACP_OUT_OF_TRACK, but no car %d found",
		    c->car_id);
		return 0;
	}
	/*
	 * accServer.exe FUN_1400142f0 case 0x3d bails on `force != 0`
	 * and gates on a per-car off-track latch (car+0x1a8 bit 0).
	 * Only the first force=0 event per latched cycle is relayed;
	 * everything else (force=1 events AND force=0 repeats within
	 * the same physical excursion) is silently dropped.
	 *
	 * Previously we filtered force=1 (opposite of exe!) and
	 * counted every one as a distinct cut — so the ACC client's
	 * per-tick force=1 spam during an extended off-track (3-6
	 * events per single excursion) over-counted to 3 and our
	 * server-side auto-DT fired after a single mistake.  The
	 * visible symptom: "got a DT while still on track" — the DT
	 * broadcast after the player had already returned to track.
	 *
	 * The exe itself does NOT auto-DT from 0x3d; it only relays
	 * 0x3c.  The ACC client self-issues its own DT via 0x41 when
	 * its internal heuristic hits 3 cuts.  Removing our auto-DT
	 * here matches the exe and fixes the false positive.  Real
	 * 3-cut DTs still arrive via h_report_penalty (0x41) which
	 * logs the self-issued penalty (future work: wire that to
	 * penalty_enqueue for wire-visible effect).
	 */
	if (force != 0)
		return 0;
	{
		struct CarRaceState *race = &s->cars[c->car_id].race;
		uint64_t now_ms = mono_ms();

		/*
		 * Latch gate: skip if we've already counted a cut within
		 * the last 2 s (approximation of the exe's +0x1a8 bit 0
		 * "off-track latch" cleared at sector boundaries — we
		 * don't have a stable sector-boundary signal to mirror
		 * that precisely).  Keeps a single physical excursion
		 * counting as one cut for best_lap invalidation while
		 * not firing spurious penalties.
		 */
		if (now_ms - race->last_cut_ms >= 2000) {
			race->out_of_track_latched = 1;
			if (race->cuts_this_lap < 255)
				race->cuts_this_lap++;
			race->last_cut_ms = now_ms;
			/*
			 * Quali "Instant Drop": invalidating the flying
			 * lap during Quali overtime ends the car's
			 * session immediately.  No-op outside Quali
			 * overtime or for a car that wasn't eligible.
			 */
			if (s->session.phase == PHASE_OVERTIME)
				session_quali_drop_eligibility(s, c->car_id);
			log_info("out-of-track: car=%d ts=%d cuts=%u",
			    c->car_id, (int)ts_raw,
			    (unsigned)race->cuts_this_lap);
		} else {
			log_debug("out-of-track: car=%d debounced "
			    "(dt=%llu ms)", c->car_id,
			    (unsigned long long)
			    (now_ms - race->last_cut_ms));
			return 0;	/* don't relay repeats either */
		}
	}

	bb_init(&out);
	if (wr_u8(&out, SRV_OUT_OF_TRACK_RELAY) < 0 ||
	    wr_u16(&out, s->cars[c->car_id].car_id) < 0 ||
	    wr_u16(&out, (uint16_t)s->cars[c->car_id]
		.race.cuts_this_lap) < 0 ||
	    wr_u32(&out, (uint32_t)ts_raw) < 0)
		goto out;
	rc = bcast_all(s, out.data, out.wpos, c->conn_id);
	(void)rc;
out:
	bb_free(&out);
	return 0;
}

/* ----- 0x41 ACP_REPORT_PENALTY ---------------------------------- */

/*
 * Map the client's 0x41 category byte to one of our REASON_* values.
 * Authoritative AC2 mapping (recovered from FUN_1434f2fb0, the
 * cat-byte → display-string translator inside the AC2 client):
 *   0  Cutting             1  Collision           2  IllegalOvertake
 *   3  PitSpeeding         4  PitEntry            5  PitExit
 *   6  IgnoredMandatoryPit 7  UnsafeRejoin        8  Trolling
 *   9  ReverseInPitlane   10  WrongWay           11  IgnoredMandatoryPit (alias of 6)
 *  12  ExceededDriverStintLimit                  13  DriverRanNoStint
 *
 * Five categories (1 Collision, 2 IllegalOvertake, 7 UnsafeRejoin,
 * 8 Trolling, 9 ReverseInPitlane) have no equivalent in our
 * penalty_reason enum AND no entry in the 0..35
 * ServerMonitorPenaltyShortcut wire mapping, so they fall through to
 * REASON_RACE_CONTROL which round-trips as one of the *_RaceControl
 * variants (values 15..19) on the wire.
 *
 * Pre-2026-05-07 this map was wrong for 13 of the 14 categories
 * (only cat=0 was correct).  The previous mapping was based on a
 * mis-reading of the DSQ_* string table at .rdata 0x143c49640
 * which is actually the widget-string keys for the SERVER-issued
 * PenaltyShortcut wire codes 1..35 — not the cat enum the client
 * uses in 0x41.
 */
static uint8_t
client_category_to_reason(uint8_t category)
{
	/*
	 * Truth table extracted from the kunos accServer.exe wire-code
	 * dispatcher FUN_1400f03b0 (438 B switch on (kind, cat) → wire
	 * 1..35), validated against the rdata penalty-shortcut keys at
	 * `notebook-a/raw/accserver-strings-all.txt` lines 18796-18831.
	 *
	 * Important quirk: AC2's cat→label table (FUN_1434f2fb0 in the
	 * client, FUN_140117330 in the server) and the wire-code
	 * dispatcher use *different* semantics for cats 11/12/13.  The
	 * label table calls cat=11 IgnoredMandatoryPit, cat=12
	 * ExceededDriverStintLimit, cat=13 DriverRanNoStint; the wire
	 * dispatcher emits wire 24..26 (IgnoredDriverStint) for cat=11,
	 * wire 28 (DriverRanNoStint) for cat=12, and wire 27
	 * (%ExceededDriverStintLimit) for cat=13+SG30.  We follow the
	 * wire dispatcher because the wire is what the AC2 widget reads.
	 *
	 * Cats 14, 15 are NEW (not in the cat→label tables but present
	 * in the wire dispatcher): 14=DamagedCar, 15=LightsOff.
	 *
	 * Cats 16, 17 come from the AC2 client's green-flag state
	 * checker (FUN_140e5ab60): 16=SpeedingOnStart (speed-driven),
	 * 17=WrongPositionOnStart (damage-driven escalation).
	 *
	 * Cats 1, 2, 7, 8, 9 have NO wire path in FUN_1400f03b0 — the
	 * kunos exe never emits a wire code for these.  Best fallback:
	 * REASON_RACE_CONTROL (wire 15-19) for those (and other unknowns).
	 *
	 * Note: penalty.c:penalty_wire_value substitutes wire 27 (% prefix,
	 * unrenderable) with 26, and wires 33-35 (! prefix, deleted from
	 * the AC2 client lookup map) with 30-32 — so REASON_EXCEEDED_*
	 * and REASON_WRONG_POSITION_ON_START are kept here for
	 * server-side semantic clarity but emit renderable wire codes.
	 */
	switch (category) {
	case 0:		return REASON_CUTTING;
	case 3:		return REASON_PIT_SPEEDING;
	case 4:		return REASON_PIT_ENTRY;
	case 5:		return REASON_PIT_EXIT;
	case 6:		return REASON_IGNORED_MANDATORY_PIT;
	case 10:	return REASON_WRONG_WAY;
	case 11:	return REASON_IGNORED_DRIVER_STINT;
	case 12:	return REASON_DRIVER_RAN_NO_STINT;
	case 13:	return REASON_EXCEEDED_DRIVER_STINT_LIMIT;
	case 14:	return REASON_DAMAGED_CAR;
	case 15:	return REASON_LIGHTS_OFF;
	case 16:	return REASON_SPEEDING_ON_START;
	case 17:	return REASON_WRONG_POSITION_ON_START;
	case 1:		/* Collision — no kunos wire path */
	case 2:		/* IllegalOvertake — no kunos wire path */
	case 7:		/* UnsafeRejoin — no kunos wire path */
	case 8:		/* Trolling — no kunos wire path; default to RaceControl */
	case 9:		/* ReverseInPitlane — no kunos wire path */
	default:	return REASON_RACE_CONTROL;
	}
}

int
h_report_penalty(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, category, kind;
	uint64_t timestamp;
	int32_t value;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u8(&r, &category) < 0 ||
	    rd_u8(&r, &kind) < 0 ||
	    rd_u64(&r, &timestamp) < 0 ||
	    rd_i32(&r, &value) < 0) {
		log_warn("h_report_penalty: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	/*
	 * Wire layout per FUN_1400142f0 case 0x41 + FUN_140125f50:
	 *   u8  msg_id    = 0x41
	 *   u8  category  → DSQ_* enum index (see client_category_to_reason)
	 *   u8  kind      → exe param_5 (1=DT, 2=SG10, 3=SG20, 4=SG30,
	 *                   5=TP, 6=DQ)
	 *   u64 timestamp → normalized via FUN_140042030 in exe
	 *   i32 value     → counter increment (0..~3 typically)
	 *
	 * The exe forwards the client's kind unmodified to FUN_140125f50
	 * (1400142f0:751).  The client uses this path to report its own
	 * 3-cut DT, escalated SG, and the four self-detected DQs that the
	 * server has no telemetry to infer (wrong-way, lights-off,
	 * speeding on start, wrong grid position).  Each conn is bound
	 * to a single car_id so a car can only report for itself — no
	 * grief vector from trusting the wire.
	 */
	log_info("report penalty: conn=%u car=%d cat=%u kind=%u "
	    "ts=%llu value=%d",
	    (unsigned)c->conn_id, c->car_id, (unsigned)category,
	    (unsigned)kind, (unsigned long long)timestamp, (int)value);
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS)
		return 0;
	if (kind < EXE_DT || kind > EXE_RBL)
		return 0;	/* out-of-enum: drop silently */
	if (kind == EXE_RBL) {
		/*
		 * RemoveBestLaptime, qualifying / hot-lap mode.  Clear the
		 * car's best lap + best sectors AND enqueue an RBL entry so
		 * the per-car tail of the next 0x36 carries the appropriate
		 * wire code (6 for cutting, 12 for pit-speed, etc.) per
		 * kunos's FUN_1400f03b0 dispatcher.  Mark the entry pending
		 * so it stays out of the active_pen prefix (matches the 0x41
		 * client-report path for other kinds).  RBL is non-DQ and
		 * kunos doesn't broadcast on it; the deep-compare in
		 * broadcast_leaderboard_if_changed skips it the same way.
		 */
		struct CarRaceState *race = &s->cars[c->car_id].race;
		int d;
		uint8_t reason = client_category_to_reason(category);

		race->best_lap_ms = 0;
		for (d = 0; d < 3; d++)
			race->best_sectors_ms[d] = 0;
		log_info("0x41 RemoveBestLaptime: car=%d best lap cleared",
		    c->car_id);
		(void)penalty_enqueue(s, c->car_id, EXE_RBL, category,
		    value, 0, 0, reason);
		{
			struct PenaltyQueue *pq = &s->cars[c->car_id].race.pen;
			if (pq->count > 0)
				pq->slots[pq->count - 1].pending = 1;
		}
		return 0;
	}
	/*
	 * Note: we used to early-exit here on `disqualified`, but kunos
	 * (FUN_140125f50) keeps processing 0x41 reports after a DQ —
	 * the per-car tail bytes update with each new penalty even on a
	 * disqualified slot.  Drop the gate for byte parity with kunos.
	 */
	/*
	 * Only accept client penalty self-reports during the live race
	 * (PHASE_SESSION / OVERTIME).  The ACC client fires a 0x41 right
	 * after green with a pseudo-DT / pseudo-DQ that is a formation-
	 * transition marker, not a real violation — materialising it
	 * leaves the car with a pending penalty and the client keeps the
	 * 70 km/h limiter engaged.  Kunos's reference capture shows the
	 * same marker getting sent (kind=DQ value=0 there) and the stock
	 * server silently drops it.  Formation-lap / pre-race transitions
	 * do their own reporting via 0x3d / 0x19 / 0x20 — 0x41 is only
	 * meaningful once cars are actually racing.
	 */
	if (s->session.phase != PHASE_SESSION &&
	    s->session.phase != PHASE_OVERTIME)
		return 0;
	/*
	 * Primer filter.  The client fires 0x41 on every lap / violation
	 * tick with value=0 as a register-severity heads-up; the exe's
	 * FUN_140125f50 fresh-branch (140125f50:147-153) stores the kind
	 * in the per-car-per-kind PenaltySheet without pushing a Penalty
	 * onto the sheet's inner vector, so primers never paint anything
	 * on the HUD there.  Our penalty_enqueue materialises on fresh for
	 * admin /dt UX, so we filter primers here.  Drop every value<=0
	 * call regardless of kind: kind=DQ value=0 from a real client
	 * (e.g. cat=10 PIT_ENTRY) used to slip through and materialise a
	 * phantom DQ.  Real DQ events for the four categories the server
	 * can't infer (wrong-way, lights-off, speeding-on-start, wrong
	 * grid) are handled by server-side detection elsewhere; we don't
	 * trust the client's self-DQ here.
	 */
	if (value <= 0)
		return 0;
	{
		uint8_t reason = client_category_to_reason(category);
		int32_t val = value > 0 ? value : 3;

		/*
		 * force = 1 only when cat == 0 (Cutting), per kunos's
		 * dispatcher (FUN_1400142f0 case 0x41): cVar10 = (server-
		 * flag && cat == 0) ? 1 : 0.  For non-cutting categories
		 * force=0, which is what gates the cat=6 (Ignored
		 * MandatoryPit) DQ-to-TP130 conversion in penalty_enqueue.
		 */
		int force = (category == 0) ? 1 : 0;
		log_info("client-reported penalty: car=%d kind=%u "
		    "category=%u -> reason=%u",
		    c->car_id, (unsigned)kind, (unsigned)category,
		    (unsigned)reason);
		{
			struct PenaltyQueue *pq = &s->cars[c->car_id].race.pen;
			int pre = pq->count;
			int pi;

			(void)penalty_enqueue(s, c->car_id, kind, category,
			    val, force, 0, reason);
			/*
			 * Mark every slot freshly enqueued by this call as
			 * pending so the 0x36 builder hides them from
			 * active_pen (kunos's car+0xc8/+0xcc are populated
			 * by a separate "server confirms" path that a 0x41
			 * alone doesn't trigger).  Whether the slot also
			 * appears in pq_emit is decided at emit time based
			 * on session type — see write_leaderboard_section.
			 *
			 * Ring eviction (penalty_materialize) may have
			 * shifted slots back so pre can be >= count.  When
			 * the queue was full pre-enqueue, the shift-then-
			 * push leaves pre == count (8 == 8 on the default
			 * ACC_MAX_PENALTIES = 8); the strict `pre > count`
			 * clamp missed that case and the freshly-enqueued
			 * slot at [count-1] was left pending=0 / admin=0.
			 * Use >= so eviction is handled.
			 */
			if (pre >= pq->count)
				pre = pq->count > 0 ? pq->count - 1 : 0;
			if (pre < 0)
				pre = 0;
			for (pi = pre; pi < pq->count; pi++)
				pq->slots[pi].pending = 1;
		}
	}
	return 0;
}

/* ----- 0x42 ACP_LAP_TICK ---------------------------------------- */

int
h_lap_tick(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint64_t ts_raw;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u64(&r, &ts_raw) < 0) {
		log_warn("h_lap_tick: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	log_info("0x42 penalty cleared: conn=%u car=%d ts=%llu",
	    (unsigned)c->conn_id, c->car_id,
	    (unsigned long long)ts_raw);
	/*
	 * Mirror exe FUN_140126b50: when the client signals it has
	 * cleared its pending DT/SG penalty (the client engine flips
	 * the penalty-pending flag from active to cleared and emits
	 * 0x42 with the supplied wallclock), mark the front pending
	 * DT/SG entry as served on the server side too.  Other paths
	 * (pit-exit dwell check, mandatory-pitstop-served, /clear
	 * admin command) already serve penalties, so this is a
	 * redundancy guard for the case where the client serves a
	 * penalty without going through one of those paths.
	 */
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
		penalty_serve_front(s, c->car_id);
		/*
		 * The 0x36 per-car tail (car+0x200/+0x201) reflects the
		 * head unserved penalty's wire_code + value; marking an
		 * entry served changes the tail bytes from (e.g.) 01 03
		 * back to 00 00, but the leaderboard cache only re-emits
		 * on a pending-flag drain.  Request an emit here so the
		 * next tick picks up the cleared tail; otherwise the
		 * stale (01 03) bytes sit in the cache for the rest of
		 * the session and run_penalty_serve_42 sees accd diverge
		 * from kunos.
		 */
		leaderboard_request_emit(s);
	}
	return 0;
}

/* ----- 0x43 ACP_DAMAGE_ZONES_UPDATE -> broadcast 0x44 ----------- */

int
h_damage_zones(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, zones[5];
	int i;
	struct ByteBuf out;
	int rc;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0)
		return -1;
	for (i = 0; i < 5; i++) {
		if (rd_u8(&r, &zones[i]) < 0) {
			log_warn("h_damage_zones: short body "
			    "from conn=%u", (unsigned)c->conn_id);
			return 0;
		}
	}
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS)
		return 0;
	/* Keep the latest state around for the welcome spawnDef so
	 * late joiners render with the same damage the live peers
	 * already see. */
	memcpy(s->cars[c->car_id].race.damage, zones, sizeof(zones));
	log_info("damage zones: car=%d [%u,%u,%u,%u,%u]",
	    c->car_id, zones[0], zones[1], zones[2], zones[3], zones[4]);

	bb_init(&out);
	if (wr_u8(&out, SRV_DAMAGE_ZONES_RELAY) < 0 ||
	    wr_u16(&out, s->cars[c->car_id].car_id) < 0)
		goto out;
	for (i = 0; i < 5; i++)
		if (wr_u8(&out, zones[i]) < 0)
			goto out;
	/* Capture confirms 0x44 is sent via UDP, not TCP. */
	rc = bcast_all_udp(s, out.data, out.wpos, c->conn_id);
	log_info("Updated %d clients with new damage zones for car %d",
	    rc, c->car_id);
	/* accweb regex: Updated \d+ clients with new damage zones for car (\d+) */
	log_kunos("Updated %d clients with new damage zones for car %d",
	    rc, ACC_CAR_ID_BASE + c->car_id);
out:
	bb_free(&out);
	return 0;
}

/* ----- 0x45 ACP_CAR_DIRT_UPDATE (store only, no relay) ---------- */

int
h_car_dirt(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, dirt[5];
	int i;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0)
		return -1;
	for (i = 0; i < 5; i++) {
		if (rd_u8(&r, &dirt[i]) < 0) {
			log_warn("h_car_dirt: short body from conn=%u",
			    (unsigned)c->conn_id);
			return 0;
		}
	}
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS)
		return 0;
	/*
	 * Kunos never relays 0x46 (per pcap) but DOES carry the
	 * latest dirt values in the welcome spawnDef tail, so late
	 * joiners see accumulated body weathering.  Store per-car;
	 * write_spawn_def reads this array.
	 */
	memcpy(s->cars[c->car_id].race.car_dirt, dirt, sizeof(dirt));
	return 0;
}

/* ----- swap state broadcast helper ------------------------------- */

/*
 * Build and broadcast 0x47 SRV_DRIVER_SWAP_STATE_BCAST to all
 * connections.  Body: u8 msg_id + u16 car_id + u8 driver_count +
 * driver_count x u8 swap_state.
 */
void
broadcast_swap_state(struct Server *s, struct CarEntry *car)
{
	struct ByteBuf bb;
	int i;

	/*
	 * settings.json doDriverSwapBroadcast (default 1): when 0,
	 * the exe suppresses the 0x47 broadcast and keeps driver-swap
	 * progress local to the swapping car.  Accept the toggle for
	 * parity with Kunos' config.
	 */
	if (!s->do_driver_swap_broadcast)
		return;
	bb_init(&bb);
	if (wr_u8(&bb, SRV_DRIVER_SWAP_STATE_BCAST) < 0 ||
	    wr_u16(&bb, car->car_id) < 0 ||
	    wr_u8(&bb, car->driver_count) < 0)
		goto done;
	for (i = 0; i < car->driver_count; i++)
		if (wr_u8(&bb, car->swap_state[i]) < 0)
			goto done;
	(void)bcast_all(s, bb.data, bb.wpos, BCAST_EXCEPT_NONE);
done:
	bb_free(&bb);
}

/* ----- 0x47 ACP_UPDATE_DRIVER_SWAP_STATE ------------------------ */

int
h_update_driver_swap_state(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, dcnt;
	uint16_t car_id;
	struct CarEntry *car;
	int slot_idx;
	int is_owner;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 || rd_u16(&r, &car_id) < 0) {
		log_warn("h_update_driver_swap_state: short body");
		return 0;
	}
	/*
	 * Match exe FUN_140012c30:62-91 dual-path semantics:
	 *   owner path (sender's car_id == target): accept any state byte,
	 *     overwrite the corresponding slot
	 *   foreign path (sender doesn't own target): only accept state
	 *     values in {2, 3, 4} (CONNECTED / REQUESTED / CONFIRMED) per
	 *     the (byte)(state - 2U) < 3 gate at FUN_140012c30:83 — slots
	 *     with any other value are silently dropped
	 *
	 * Pre-fix accd rejected the message entirely if the sender wasn't
	 * the owner, which broke the multi-driver team scenario where a
	 * non-driving teammate sends their own CONNECTED/REQUESTED state
	 * for the shared car.
	 */
	if (car_id < ACC_CAR_ID_BASE ||
	    car_id >= ACC_CAR_ID_BASE + ACC_MAX_CARS) {
		log_warn("h_update_driver_swap_state: bad car_id %u",
		    (unsigned)car_id);
		return 0;
	}
	car = &s->cars[car_id - ACC_CAR_ID_BASE];
	if (!car->used) {
		log_warn("h_update_driver_swap_state: unused car %u",
		    (unsigned)car_id);
		return 0;
	}
	is_owner = (c->car_id >= 0 && c->car_id < ACC_MAX_CARS &&
	    (uint16_t)(ACC_CAR_ID_BASE + c->car_id) == car_id);
	if (rd_u8(&r, &dcnt) < 0)
		return 0;
	if (dcnt > car->driver_count)
		dcnt = car->driver_count;
	for (slot_idx = 0; slot_idx < dcnt; slot_idx++) {
		uint8_t st;
		if (rd_u8(&r, &st) < 0)
			break;
		if (is_owner) {
			car->swap_state[slot_idx] = st;
		} else if (st >= 2 && st <= 4) {
			/* Foreign-conn filter: only CONNECTED/REQ/CONFIRM. */
			car->swap_state[slot_idx] = st;
		}
		/* Drop any foreign-conn slot whose state isn't 2/3/4. */
	}
	log_info("driver swap state update: car=%u from=%s states=[%u,%u,%u,%u]",
	    (unsigned)car_id, is_owner ? "owner" : "foreign",
	    (unsigned)car->swap_state[0], (unsigned)car->swap_state[1],
	    (unsigned)car->swap_state[2], (unsigned)car->swap_state[3]);
	/*
	 * Team-entry group propagation: when the updated car is a
	 * member of a multi-car team group (team_entry_id >= 0),
	 * kunos emits one 0x47 per car_id in the group — each car
	 * carries its own swap_state[] header.  Mirror that fan-out.
	 * Standalone cars (team_entry_id == -1) emit the single
	 * legacy broadcast.
	 */
	if (car->team_entry_id >= 0) {
		int j;
		for (j = 0; j < ACC_MAX_CARS; j++) {
			if (s->cars[j].team_entry_id == car->team_entry_id)
				broadcast_swap_state(s, &s->cars[j]);
		}
	} else {
		broadcast_swap_state(s, car);
	}
	return 0;
}

/* ----- 0x48 ACP_EXECUTE_DRIVER_SWAP -> reply 0x49, maybe 0x58 --- */

int
h_execute_driver_swap(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, swap_code, result;
	uint16_t car_id;
	struct CarEntry *car;
	struct ByteBuf out;
	int i;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u16(&r, &car_id) < 0 ||
	    rd_u8(&r, &swap_code) < 0) {
		log_warn("h_execute_driver_swap: short body");
		return 0;
	}
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS) {
		log_warn("ACP_EXECUTE_DRIVER_SWAP, but no car controlled "
		    "for connection %u", (unsigned)c->conn_id);
		result = 1;
		goto reply;
	}
	if (c->car_id < 0 ||
	    (uint16_t)(ACC_CAR_ID_BASE + c->car_id) != car_id) {
		log_warn("ACP_EXECUTE_DRIVER_SWAP, but carId mismatch: %u "
		    "(car controlled %d for connection %u)",
		    (unsigned)car_id, c->car_id, (unsigned)c->conn_id);
		result = 1;
		goto reply;
	}

	car = &s->cars[c->car_id];

	/* Validate: target driver must exist and differ from current. */
	if (swap_code >= car->driver_count) {
		log_warn("driver swap: target %u out of range (car has %u)",
		    (unsigned)swap_code, (unsigned)car->driver_count);
		result = 1;
		goto reply;
	}
	if (swap_code == car->current_driver_index) {
		log_warn("driver swap: target %u is already active",
		    (unsigned)swap_code);
		result = 1;
		goto reply;
	}

	/* Validate: car must be in pit. */
	if (!car->race.in_pit) {
		log_warn("driver swap: car %u not in pit", (unsigned)car_id);
		result = 1;
		goto reply;
	}

	/* Commit the swap.  Flush the outgoing driver's stint time
	 * into driver_stint_ms before reassigning current_driver_index
	 * so the accumulator lands on the correct driver slot. */
	stint_stop_tracking(s, c->car_id);
	car->current_driver_index = swap_code;
	for (i = 0; i < ACC_MAX_DRIVERS_PER_CAR; i++)
		car->swap_state[i] = 0;
	log_info("driver swap: car %u -> driver %u (%s %s)",
	    (unsigned)car_id, (unsigned)swap_code,
	    car->drivers[swap_code].first_name,
	    car->drivers[swap_code].last_name);
	result = 0;

	/* Broadcast 0x58 driver swap notification to all clients. */
	bb_init(&out);
	if (wr_u8(&out, SRV_DRIVER_SWAP_NOTIFY) == 0 &&
	    wr_u16(&out, car_id) == 0 &&
	    wr_u8(&out, swap_code) == 0)
		(void)bcast_all(s, out.data, out.wpos, BCAST_EXCEPT_NONE);
	bb_free(&out);

	/* Broadcast reset swap state. */
	broadcast_swap_state(s, car);

reply:
	/* Send 0x49 reply to the requester. */
	bb_init(&out);
	if (wr_u8(&out, SRV_DRIVER_SWAP_RESULT) < 0 ||
	    wr_u8(&out, result) < 0)
		goto done;
	bcast_send_one(c, out.data, out.wpos);
done:
	bb_free(&out);
	return 0;
}

/* ----- 0x4a ACP_DRIVER_SWAP_STATE_REQUEST ----------------------- */

int
h_driver_swap_state_request(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, sub_state, conn_state;
	uint16_t car_id;
	struct CarEntry *car;
	int i;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u16(&r, &car_id) < 0 ||
	    rd_u8(&r, &sub_state) < 0 ||
	    rd_u8(&r, &conn_state) < 0) {
		log_warn("h_driver_swap_state_request: short body");
		return 0;
	}
	if (check_car_owner(c, car_id) < 0) {
		log_warn("ACP_DRIVER_SWAP_STATE_REQUEST for the wrong "
		    "carId: %u (Connection owns %d)",
		    (unsigned)car_id, c->car_id);
		return 0;
	}
	car = &s->cars[c->car_id];

	switch (sub_state) {
	case 2:
		/*
		 * Initiate: set the requesting driver's swap state
		 * to the value the client sent.
		 */
		if (car->current_driver_index < car->driver_count)
			car->swap_state[car->current_driver_index] = conn_state;
		break;
	case 3:
		/*
		 * Confirm: kunos's FUN_1400142f0:1105-1194 resets every
		 * team mate's swap_state[i] in {3,4} back to 2 (per
		 * `(state - 3) < 2`), then applies the new conn_state
		 * on the requesting driver.  For non-team cars (team_
		 * entry_id == -1) the team-mate scan collapses to just
		 * this car.
		 */
		if (car->team_entry_id >= 0) {
			int g;
			for (g = 0; g < ACC_MAX_CARS; g++) {
				struct CarEntry *mate = &s->cars[g];
				int k;
				if (mate->team_entry_id != car->team_entry_id)
					continue;
				for (k = 0; k < mate->driver_count; k++)
					if (mate->swap_state[k] == 3 ||
					    mate->swap_state[k] == 4)
						mate->swap_state[k] = 2;
			}
		} else {
			for (i = 0; i < car->driver_count; i++)
				if (car->swap_state[i] == 3)
					car->swap_state[i] = 2;
		}
		if (car->current_driver_index < car->driver_count)
			car->swap_state[car->current_driver_index] = conn_state;
		break;
	case 4:
		/* Execute: set requesting driver to EXECUTING. */
		if (car->current_driver_index < car->driver_count)
			car->swap_state[car->current_driver_index] = 4;
		break;
	default:
		log_warn("DriverSwap Request for type %u is not "
		    "implemented", (unsigned)sub_state);
		return 0;
	}

	log_info("driver swap state request: car=%u sub=%u state=%u",
	    (unsigned)car_id, (unsigned)sub_state, (unsigned)conn_state);
	/*
	 * Same team-group fan-out as h_update_driver_swap_state: kunos
	 * emits one 0x47 per car in the group, each carrying its own
	 * (possibly mutated by the Confirm reset above) swap_state[].
	 */
	if (car->team_entry_id >= 0) {
		int g;
		for (g = 0; g < ACC_MAX_CARS; g++) {
			if (s->cars[g].team_entry_id == car->team_entry_id)
				broadcast_swap_state(s, &s->cars[g]);
		}
	} else {
		broadcast_swap_state(s, car);
	}
	return 0;
}

/* ----- 0x4f ACP_DRIVER_STINT_RESET ------------------------------ */

int
h_driver_stint_reset(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, force;
	uint64_t ts_raw;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u8(&r, &force) < 0 ||
	    rd_u64(&r, &ts_raw) < 0) {
		log_warn("h_driver_stint_reset: short body");
		return 0;
	}
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS)
		return 0;
	log_info("Receives driver stint reset for car %d (force=%u)",
	    c->car_id, (unsigned)force);

	/*
	 * Race "Tow Penalty" (ESC to garage during the race).  The
	 * voluntary stint reset (force == 0) arriving during a race
	 * session is the protocol signal that the driver chose to
	 * return to the pit box; the client teleports the car and
	 * cuts the engine.  Per spec the server does NOT DSQ the
	 * driver — they remain classified by laps completed before
	 * the ESC — but a mandatory wait timer is enforced before the
	 * driver can rejoin (simulated tow + repair).  We track the
	 * wait window in CarRaceState so results.json reflects
	 * which cars went through it; no server-side block on
	 * subsequent inputs since the client is the authority on
	 * "engine cut" / "wait until timer" rendering.
	 *
	 * Skip outside race phases (P/Q stint resets are routine
	 * driver swaps, not tow penalties).
	 */
	if (force == 0 && session_is_race(s) &&
	    (s->session.phase == PHASE_SESSION ||
	     s->session.phase == PHASE_OVERTIME)) {
		struct CarRaceState *r = &s->cars[c->car_id].race;
		const uint64_t TOW_WAIT_MS = 30000ull;
		r->in_tow = 1;
		r->tow_until_ms = mono_ms() + TOW_WAIT_MS;
		log_info("tow penalty: car=%d, wait %llums "
		    "(lap=%d, race_time=%dms preserved)",
		    c->car_id, (unsigned long long)TOW_WAIT_MS,
		    r->lap_count, r->race_time_ms);
	}

	/*
	 * Relay 0x4f to all other clients.  Two variants:
	 *   sub=0: 4 bytes  -- u8 id + u16 car_id + u8(0)
	 *   sub=1: 12 bytes -- u8 id + u16 car_id + u8(1) + 8 B ts
	 *
	 * For sub=1 kunos's FUN_140042030 transforms the client ts
	 * through per-conn session-relative doubles at conn+0x340 /
	 * +0x310 into a session-relative IEEE-754 double and emits
	 * the bytes verbatim.  Mirror that here via
	 * `c->session_clock_offset_ms` (updated on best-RTT pong)
	 * plus the client's ts so the value stays bounded by
	 * session_now magnitudes and the receiver sees a sensible
	 * double when interpreting the trailing 8 bytes.
	 */
	{
		struct ByteBuf out;

		bb_init(&out);
		if (wr_u8(&out, SRV_DRIVER_STINT_RELAY) == 0 &&
		    wr_u16(&out, s->cars[c->car_id].car_id) == 0 &&
		    wr_u8(&out, force) == 0) {
			if (force) {
				double ts_d =
				    (double)(int64_t)ts_raw +
				    (double)c->session_clock_offset_ms;
				uint8_t bytes[8];
				memcpy(bytes, &ts_d, sizeof(bytes));
				(void)bb_append(&out, bytes,
				    sizeof(bytes));
			}
			(void)bcast_all(s, out.data, out.wpos,
			    c->conn_id);
		}
		bb_free(&out);
	}
	return 0;
}

/* ----- 0x51 ACP_ELO_UPDATE -------------------------------------- */

int
h_elo_update(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint16_t new_elo, reserved;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u16(&r, &new_elo) < 0 ||
	    rd_u16(&r, &reserved) < 0) {
		log_warn("h_elo_update: short body");
		return 0;
	}
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
		log_info("Car %d elo update => %u",
		    c->car_id, (unsigned)new_elo);
		s->cars[c->car_id].last_elo = new_elo;
	}
	(void)reserved;
	return 0;
}

/* ----- 0x54 ACP_MANDATORY_PITSTOP_SERVED ------------------------ */

int
h_mandatory_pitstop_served(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint16_t car_id;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u16(&r, &car_id) < 0) {
		log_warn("h_mandatory_pitstop_served: short body");
		return 0;
	}
	if (check_car_owner(c, car_id) < 0) {
		log_warn("Received ACP_MANDATORY_PITSTOP_SERVED for carId "
		    "%u, but connection is %u",
		    (unsigned)car_id, (unsigned)c->conn_id);
		return 0;
	}
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
		struct CarEntry *ecar = &s->cars[c->car_id];
		struct CarRaceState *race = &ecar->race;

		if (race->mandatory_pit_served < 255)
			race->mandatory_pit_served++;
		/*
		 * Kunos's 0x54 handler (FUN_1400142f0:1265) decrements a
		 * "mandatory pits remaining" counter via FUN_14012aff0
		 * and does NOT touch the DT/SG queue.  A pending DT/SG
		 * is served only when the client engine emits 0x42 after
		 * its own dwell-time check.  Mirror that here: don't
		 * coalesce DT/SG service into the mandatory-pit completion.
		 */
		/*
		 * Mandatory driver-swap check: if eventRules demands a
		 * swap during the mandatory pit (isMandatoryPitstopSwap
		 * DriverRequired=1) and the pit entry + pit served came
		 * from the same driver, enqueue a DT so the HUD flags it
		 * as an IgnoredDriverStint.  Skipped when the car has
		 * only one registered driver (no swap target exists).
		 */
		if (s->mandatory_swap_required &&
		    ecar->driver_count > 1 &&
		    race->pit_entry_driver_index ==
			ecar->current_driver_index) {
			char chat[128];
			log_info("Car %d mandatory swap skipped (driver %u) "
			    "-> DT", c->car_id,
			    (unsigned)ecar->current_driver_index);
			(void)penalty_enqueue(s, c->car_id, EXE_DT, 24, 3, 1,
			    0, REASON_IGNORED_DRIVER_STINT);
			penalty_format_chat(chat, sizeof(chat), PEN_DT,
			    REASON_IGNORED_DRIVER_STINT, 0,
			    ecar->race_number);
			chat_broadcast(s, chat, 4);
		}
	}
	log_info("Served Mandatory Pitstop: %u", (unsigned)car_id);
	return 0;
}

/* ----- 0x55 ACP_LOAD_SETUP -> reply 0x56 ------------------------ */

/*
 * Despite the enum name, 0x55 is not a car-setup-file load — it's
 * the in-game garage's "load lap history for this car from session
 * N" request.  Wire layout (case 0x55 in FUN_1400142f0):
 *
 *     u8  0x55
 *     u8  session_index   (0 P, 4 Q, 10 R — NOT the msg.h
 *                          session_index which is slot 0/1/2)
 *     u16 car_id
 *     u32 revision        (client's cached revision — ignored here;
 *                          exe uses it for cache invalidation)
 *
 * Reply 0x56 body (see notebook-b §5.6.4a 0x56):
 *
 *     u8  0x56
 *     u8  session_index    (echoed from request)
 *     u16 car_id
 *     i16 lap_count
 *     lap_count × Lap_record:
 *         str_a track_name
 *         u32   lap_time_ms
 *         u8    split_count
 *         split_count × u32 split_time_ms
 *         u16   car_id
 *         u8    lap_quality  (0 = clean)
 *         u16   lap_number   (1-based)
 *
 * Clients tolerate the absence of the trailing full-leaderboard
 * record that the exe appends; we skip it.  We also don't
 * archive past-session histories (we only have current-session
 * data), so a request for a completed session returns count=0.
 */
int
h_load_setup(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, sess_type;
	uint16_t car_id;
	uint32_t revision;
	struct ByteBuf out;
	struct CarEntry *car = NULL;
	int slot, my_sess_type = 0, laps_emitted = 0;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u8(&r, &sess_type) < 0 ||
	    rd_u16(&r, &car_id) < 0 ||
	    rd_u32(&r, &revision) < 0) {
		log_warn("h_load_setup: short body");
		return 0;
	}

	slot = (int)car_id - ACC_CAR_ID_BASE;
	if (slot >= 0 && slot < ACC_MAX_CARS && s->cars[slot].used)
		car = &s->cars[slot];
	{
		uint8_t cur = session_cur_type(s);
		if (cur != 0xff)
			my_sess_type = cur;
	}

	/*
	 * Pick the race state to serve:
	 *  - sess_type == current session's type  -> live car->race
	 *  - else, find a session in s->sessions[] with that type and
	 *    look up the car's archived snapshot from that session's
	 *    index.  archive slot == NULL means that session was never
	 *    completed for this car; reply with lap_count=0.
	 */
	{
		const struct CarRaceState *src = NULL;

		if (car != NULL && sess_type == my_sess_type) {
			src = &car->race;
		} else if (car != NULL) {
			int k;
			for (k = 0; k < s->session_count &&
			    k < ACC_MAX_SESSIONS; k++) {
				if (s->sessions[k].session_type == sess_type
				    && car->race_archive[k] != NULL) {
					src = car->race_archive[k];
					break;
				}
			}
		}

		log_info("load setup: conn=%u car=%u sess_type=%u "
		    "cur_sess_type=%d rev=%u src=%s",
		    (unsigned)c->conn_id, (unsigned)car_id,
		    (unsigned)sess_type, my_sess_type,
		    (unsigned)revision,
		    src == NULL ? "none"
			: (src == &car->race ? "current" : "archive"));

		bb_init(&out);
		if (wr_u8(&out, SRV_SETUP_DATA_RESPONSE) < 0 ||
		    wr_u8(&out, sess_type) < 0 ||
		    wr_u16(&out, car_id) < 0)
			goto done;

		if (src != NULL && src->lap_history_count > 0) {
			int total = src->lap_history_count;
			int count = total < ACC_LAP_HISTORY
			    ? total : ACC_LAP_HISTORY;
			int start = total <= ACC_LAP_HISTORY
			    ? 0 : total % ACC_LAP_HISTORY;
			int first_lap = total <= ACC_LAP_HISTORY
			    ? 1 : total - ACC_LAP_HISTORY + 1;
			int k;

			if (wr_i16(&out, (int16_t)count) < 0)
				goto done;
			for (k = 0; k < count; k++) {
				int idx = (start + k) % ACC_LAP_HISTORY;
				int si;

				if (wr_str_a(&out, s->track) < 0) goto done;
				if (wr_u32(&out,
				    (uint32_t)src->lap_history_ms[idx]) < 0)
					goto done;
				if (wr_u8(&out, 3) < 0) goto done;
				for (si = 0; si < 3; si++)
					if (wr_u32(&out, (uint32_t)
					    src->lap_splits_ms[idx][si]) < 0)
						goto done;
				if (wr_u16(&out, car->car_id) < 0) goto done;
				if (wr_u8(&out, 0) < 0) goto done;
				if (wr_u16(&out,
				    (uint16_t)(first_lap + k)) < 0)
					goto done;
			}
			laps_emitted = count;
		} else {
			if (wr_i16(&out, 0) < 0)
				goto done;
		}
	}
	/*
	 * Trailing full leaderboard record for the target car — matches
	 * the exe where FUN_1400328f0 appends a FUN_140034210 single-
	 * car record at the tail of 0x56.  Exe passes a literal '\0'
	 * (1400328f0.c:78) for the cvar8 byte regardless of the car's
	 * formation-lap state, since 0x56 is a per-car garage reply
	 * and the cvar8 byte gates HUD-active state — irrelevant to
	 * the lap-history view.  Previously we passed
	 * formation_lap_done, which flipped the byte mid-formation
	 * and could change how the client rendered the trailing
	 * record's session-active label.
	 */
	if (car != NULL)
		(void)write_car_leaderboard_record(&out, s, car, 0, 0, NULL);
	(void)bcast_send_one(c, out.data, out.wpos);
	log_debug("0x56 reply: conn=%u car=%u sess_type=%u laps=%d "
	    "(%zu bytes)", (unsigned)c->conn_id, (unsigned)car_id,
	    (unsigned)sess_type, laps_emitted, out.wpos);
done:
	bb_free(&out);
	return 0;
}

/* ----- 0x5b ACP_CTRL_INFO --------------------------------------- */

/*
 * Parse the CtrlInfo payload and forward a compact chat summary to
 * every admin connection.  Wire layout from FUN_14002c1e0 (parser)
 * + the 0x5b case in FUN_1400142f0 (dispatcher):
 *
 *   u32  carId          (wide as u16 on the wire but read as 4)
 *   str_b car_model     (or display name)
 *   u8   gpe
 *   u8   as
 *   u8   sc_active
 *   u32  unknown_a      (scalar, possibly setup revision)
 *   str_b cam_near      (replay camera — chained output only)
 *   str_b cam_far       (replay camera — chained output only)
 *   u32  unknown_b
 *   f32  sc_scale
 *   u32  setup_id
 *
 * Chat format (matches the exe's ostream):
 *   "Ctrl Info carId N (LastName): model, gpe, as, sc X.XX, no scp"
 * If the message would exceed 250 bytes the server replaces it with
 * "Received ctrl info, but message is too long. Please check logs".
 */
int
h_ctrl_info(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, gpe = 0, as_flag = 0, sc_active = 0;
	uint32_t car_id_u32 = 0, scalar_a = 0, scalar_b = 0, setup_id = 0;
	float sc_scale = 0.0f;
	char *model = NULL, *cam_near = NULL, *cam_far = NULL;
	char chat[256];
	const char *driver_name;
	size_t off;
	int i;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 || rd_u32(&r, &car_id_u32) < 0) {
		log_warn("ctrl info: short header conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	(void)rd_str_b(&r, &model);
	(void)rd_u8(&r, &gpe);
	(void)rd_u8(&r, &as_flag);
	(void)rd_u8(&r, &sc_active);
	(void)rd_u32(&r, &scalar_a);
	(void)rd_str_b(&r, &cam_near);
	(void)rd_str_b(&r, &cam_far);
	(void)rd_u32(&r, &scalar_b);
	(void)rd_f32(&r, &sc_scale);
	(void)rd_u32(&r, &setup_id);

	driver_name = "?";
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
		struct CarEntry *car = &s->cars[c->car_id];
		if (car->driver_count > 0)
			driver_name =
			    car->drivers[car->current_driver_index
			    < car->driver_count
			    ? car->current_driver_index : 0].last_name;
	}

	/*
	 * snprintf returns "would-have-written", not bytes-actually-
	 * written.  Tracking the accumulator with the would-have-written
	 * count can overshoot sizeof(chat) on truncation; the subsequent
	 * `off < sizeof(chat)` gate prevents an out-of-bounds write but
	 * leaves a misleading invariant.  Clamp every append to the
	 * remaining space so `off` always equals bytes actually written.
	 */
#define APPEND(...) do {                                                     \
	if (off < sizeof(chat) - 1) {                                        \
		int _n = snprintf(chat + off, sizeof(chat) - off, __VA_ARGS__); \
		if (_n < 0) break;                                           \
		off += ((size_t)_n < sizeof(chat) - off)                     \
		    ? (size_t)_n : sizeof(chat) - off - 1;                   \
	}                                                                    \
} while (0)

	off = 0;
	APPEND("Ctrl Info carId %u (%s): %s",
	    (unsigned)(car_id_u32 & 0xffff), driver_name,
	    model ? model : "");
	if (gpe) APPEND(", gpe");
	if (as_flag) APPEND(", as");
	if (sc_active) APPEND(", sc %.2f", (double)sc_scale);
	if (!gpe && !as_flag && !sc_active) APPEND(", running defaults");
#undef APPEND

	log_info("ctrl info: conn=%u car=%u cam=%s-%s setup=%u",
	    (unsigned)c->conn_id, (unsigned)(car_id_u32 & 0xffff),
	    cam_near ? cam_near : "", cam_far ? cam_far : "",
	    (unsigned)setup_id);
	(void)scalar_a;
	(void)scalar_b;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *dst = s->conns[i];
		struct ByteBuf bb;

		if (dst == NULL || dst->state != CONN_AUTH || !dst->is_admin)
			continue;
		bb_init(&bb);
		if (wr_u8(&bb, SRV_CHAT_OR_STATE) == 0 &&
		    wr_str_a(&bb, RC_SENDER) == 0 &&
		    wr_str_a(&bb, off >= 250
			? "Received ctrl info, but message is too long. "
			"Please check logs" : chat) == 0 &&
		    wr_i32(&bb, 0) == 0 &&
		    wr_u8(&bb, 4) == 0)
			(void)conn_send_framed(dst, bb.data, bb.wpos);
		bb_free(&bb);
	}

	free(model);
	free(cam_near);
	free(cam_far);
	return 0;
}

/* ----- UDP 0x1e ACP_CAR_UPDATE ---------------------------------- */

int
h_udp_car_update(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, seq;
	uint16_t source_conn_id, target_car_id;
	uint32_t client_ts_ms;
	struct CarEntry *car;
	struct CarRuntime *rt;
	int i;

	if (len != 68) {
		log_warn("CarUpdate size is unexpected; did you forget "
		    "to update the megapak? (%zu byte, %d byte expected)",
		    len, 68);
		return 0;
	}
	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u16(&r, &source_conn_id) < 0 ||
	    rd_u16(&r, &target_car_id) < 0 ||
	    rd_u8(&r, &seq) < 0 ||
	    rd_u32(&r, &client_ts_ms) < 0) {
		log_warn("h_udp_car_update: short header");
		return 0;
	}

	if (c == NULL) {
		log_info("Ignoring ACP_CAR_UPDATE from unknown peer "
		    "(source_conn=%u)", (unsigned)source_conn_id);
		return 0;
	}
	if (c->car_id < 0 ||
	    s->cars[c->car_id].car_id != target_car_id) {
		log_warn("Received car update for a different car, "
		    "connectionId %u. Expected: %d Received: %u",
		    (unsigned)c->conn_id, c->car_id,
		    (unsigned)target_car_id);
		return 0;
	}
	car = &s->cars[c->car_id];
	rt = &car->rt;

	/*
	 * Drop outdated packets, mirroring exe FUN_140042900:
	 *
	 *   if source_conn_id matches the last accepted one and the
	 *   timestamp is not strictly newer, drop.
	 *
	 *   if source_conn_id changed (e.g. reconnect reusing the same
	 *   car slot), accept and reset the gate.
	 *
	 * Extension over the exe: a backwards jump > 1 s on the same
	 * conn is treated as a clock reset and accepted.  The exe drops
	 * those, which leaves a single bad timestamp able to lock the
	 * gate for the rest of the connection (reproduced 2026-05-06:
	 * stored 1050127 vs incoming 819920 blocked every car_update
	 * during a race PRE_SESSION, so the leader-pick gate never saw
	 * the car move and green never fired).
	 */
#define CAR_UPDATE_TS_RESET_GAP_MS 1000u
	if (rt->has_data) {
		int conn_changed = (rt->last_src_conn_id != source_conn_id);
		int reset_gap = (client_ts_ms < rt->last_timestamp_ms &&
		    (rt->last_timestamp_ms - client_ts_ms) >
		    CAR_UPDATE_TS_RESET_GAP_MS);

		if (conn_changed) {
			log_info("Changing lastDrivingConnectionID %u to %u "
			    "(carId %d, ts %u -> %u)",
			    (unsigned)rt->last_src_conn_id,
			    (unsigned)source_conn_id, c->car_id,
			    (unsigned)rt->last_timestamp_ms,
			    (unsigned)client_ts_ms);
		} else if (reset_gap) {
			log_info("car %d: clientTimestamp jumped backwards "
			    "%u -> %u (gap %u ms > %u ms threshold), "
			    "accepting and resetting gate",
			    c->car_id,
			    (unsigned)rt->last_timestamp_ms,
			    (unsigned)client_ts_ms,
			    (unsigned)(rt->last_timestamp_ms - client_ts_ms),
			    CAR_UPDATE_TS_RESET_GAP_MS);
		} else if (client_ts_ms <= rt->last_timestamp_ms) {
			log_info("Dropped outdated car_update paket for carId "
			    "%d, clientTimestamp %u vs lastTimeStamp %u",
			    c->car_id, (unsigned)client_ts_ms,
			    (unsigned)rt->last_timestamp_ms);
			return 0;
		}
	}

	rt->packet_seq = seq;
	rt->client_timestamp_ms = client_ts_ms;
	rt->last_timestamp_ms = client_ts_ms;
	rt->last_src_conn_id = source_conn_id;

	/*
	 * Refresh the extrapolation pivot used by write_session_mgr_state:
	 * every incoming UDP packet gives us a fresh (client_ts, server_ms)
	 * pair, so the 0x28 f32 deltas never depend on a stale 1-Hz pong.
	 * Matches the exe's FUN_1400419e0 which updates +0x280cc/+0x280ce
	 * on every 0x1e — minus the explicit drift accumulator, because
	 * the 18 Hz 0x1e cadence keeps the pivot fresh to within ~55 ms.
	 */
	c->last_udp_client_ts = client_ts_ms;
	c->last_udp_server_ms = (uint32_t)mono_ms();

	/* Three Vector3 blocks (3 * 12 = 36 bytes). */
	for (i = 0; i < 3; i++)
		if (rd_f32(&r, &rt->vec_a[i]) < 0)
			return 0;
	for (i = 0; i < 3; i++)
		if (rd_f32(&r, &rt->vec_b[i]) < 0)
			return 0;
	for (i = 0; i < 3; i++)
		if (rd_f32(&r, &rt->vec_c[i]) < 0)
			return 0;

	/* input array A (4 u8) */
	for (i = 0; i < 4; i++)
		if (rd_u8(&r, &rt->input_a[i]) < 0)
			return 0;

	if (rd_u8(&r, &rt->scalar_32) < 0 ||
	    rd_u8(&r, &rt->scalar_33) < 0 ||
	    rd_u16(&r, &rt->scalar_36) < 0 ||
	    rd_u8(&r, &rt->scalar_2c) < 0 ||
	    rd_u8(&r, &rt->scalar_34) < 0 ||
	    rd_u8(&r, &rt->scalar_35) < 0 ||
	    rd_u32(&r, &rt->scalar_44) < 0)
		return 0;

	/* input array B (4 u8) */
	for (i = 0; i < 4; i++)
		if (rd_u8(&r, &rt->input_b[i]) < 0)
			return 0;

	if (rd_u8(&r, &rt->scalar_4c) < 0 ||
	    rd_i16(&r, &rt->scalar_1ec) < 0)
		return 0;

	rt->has_data = 1;

	/*
	 * Mark the car dirty.  The periodic sweep in tick_run builds
	 * the 0x1e fan-out once per tick (333 Hz, matching exe), which
	 * mirrors FUN_14001a170 clearing the dirty byte after
	 * broadcasting every dirty car to every peer in one pass.
	 * Previous event-driven relay at ~18 Hz × peer_count flooded
	 * the client's UDP queue and caused position jitter / blinking.
	 */
	rt->dirty = 1;
	return 0;
}

/* ----- UDP 0x22 CAR_INFO_REQUEST -> reply 0x23 ------------------ */

int
h_udp_car_info_request(struct Server *s,
    const struct sockaddr_in *peer,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint16_t target_car_id, requester_conn_id;
	struct Conn *requester;
	struct ByteBuf out;
	int slot;

	/*
	 * Wire layout from 140027f80 0x22 handler:
	 *   u8  0x22 (msg id)
	 *   u16 target_car_id (the car the client wants info about)
	 *   u16 requester_conn_id
	 * The server responds with a TCP 0x23 reply containing the
	 * full spawnDef of the target car (via welcome_per_car_appender
	 * in the exe; we inline the same layout via write_spawn_def).
	 */
	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u16(&r, &target_car_id) < 0 ||
	    rd_u16(&r, &requester_conn_id) < 0) {
		log_warn("h_udp_car_info_request: short body");
		return 0;
	}
	log_info("Connection %u asks for carInfo %u",
	    (unsigned)requester_conn_id, (unsigned)target_car_id);

	requester = server_find_conn(s, requester_conn_id);
	if (requester == NULL) {
		log_warn("car info request from unknown conn %u",
		    (unsigned)requester_conn_id);
		return 0;
	}
	/*
	 * SMPR observers don't issue UDP 0x22; any frame naming an
	 * observer slot is forged, and acting on it would push a sim
	 * SRV_CAR_INFO_RESPONSE into the protobuf side-channel.
	 */
	if (requester->is_smpr)
		return 0;
	/*
	 * Require the UDP source IP to match the requester conn's
	 * accepted IP.  Without this, any UDP peer can pick another
	 * driver's conn_id and flood their TCP TX queue with ~1 KiB
	 * 0x23 spawnDef responses.
	 */
	if (requester->peer.sin_addr.s_addr != peer->sin_addr.s_addr)
		return 0;

	slot = (int)target_car_id - ACC_CAR_ID_BASE;
	if (slot < 0 || slot >= ACC_MAX_CARS || !s->cars[slot].used) {
		log_warn("car info request for unknown car %u",
		    (unsigned)target_car_id);
		return 0;
	}

	bb_init(&out);
	if (wr_u8(&out, SRV_CAR_INFO_RESPONSE) < 0)
		goto done;
	if (write_spawn_def(&out, s, slot) < 0) {
		log_warn("car info response: failed to build spawnDef "
		    "for car_id=%u slot=%d",
		    (unsigned)target_car_id, slot);
		goto done;
	}
	bcast_send_one(requester, out.data, out.wpos);
	log_info("Car Info Response sent carId=%u to conn=%u",
	    (unsigned)target_car_id, (unsigned)requester_conn_id);
done:
	bb_free(&out);
	return 0;
}
