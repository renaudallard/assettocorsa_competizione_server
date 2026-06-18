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
#include "results.h"
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
 *   i32 timestamp            (client clock; see normalisation note below)
 *   u8  quality              (signed: <0 -> -1.0f "invalid contact",
 *                             non-negative -> byte/10.0f rating quality)
 *
 * AC2 client emit sites: 1410be570.c:40,109 (wall / car-to-car
 * contact) and 140e3f9c0.c:960 (SA-contact handler with the
 * "SA contact: %f obwp" log).  Server case 0x19 at exe
 * 1400142f0.c:174-205 builds a queued-broadcast lambda.  The 0x1b
 * wire builder FUN_1400179b0 emits the SAME 9-byte body we do:
 * u16 carA + u16 carB + i32 timestamp + u8 quality.  The double and
 * float seen in the decomp are only in-memory promotions; the builder
 * truncates them to i32/u8 (cvttsd2si at 1400179b0.c:69 / *10 then
 * cvttsd2si for the byte), and the AC2 client receiver 143526030.c
 * reads them back as i32(+4) + u8(+1).  So our i32+u8 relay is
 * wire-width correct.  Two value notes:
 *   - timestamp: the exe normalises it PER RECIPIENT in the fan-out
 *     (FUN_140041fc0 = raw - that conn's session base).  bcast_all
 *     cannot carry a per-recipient value, so we apply the sender's
 *     session_clock_offset_ms to reach the shared server-session frame
 *     (the same simplification our 0x3a/0x3b/0x4f relays use).
 *   - quality: the exe round-trips the byte (i8 -> float byte/10 or
 *     -1.0, then float -> u8 >=0 ? (u8)(val*10) : 0xff), we pass the
 *     raw byte through.  Client-visible identical: both receivers
 *     re-divide by 10 and read the byte signed, so a >127 byte is
 *     "invalid" either way.
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
	int i, ts_off;

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
	 *
	 * The timestamp is per-recipient: the exe applies FUN_140041fc0
	 * (each receiving conn's clock offset) inside the fan-out loop
	 * FUN_14001ae20.  We replicate this by building one shared
	 * buffer, then patching the 4-byte timestamp field for each peer
	 * before sending.
	 */
	bb_init(&out);
	if (wr_u8(&out, SRV_LAP_BROADCAST) < 0 ||
	    wr_u16(&out, reporter_car_id) < 0 ||
	    wr_u16(&out, target_car_id) < 0)
		goto out;
	ts_off = (int)out.wpos;
	if (wr_i32(&out, 0) < 0 ||	/* placeholder, patched per peer */
	    wr_u8(&out, quality) < 0)
		goto out;
	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *peer = s->conns[i];
		int32_t ts_peer;

		if (peer == NULL || peer->state != CONN_AUTH || peer->is_smpr)
			continue;
		ts_peer = (int32_t)((int64_t)timestamp +
		    conn_clock_offset(s, peer));
		out.data[ts_off + 0] = (uint8_t)ts_peer;
		out.data[ts_off + 1] = (uint8_t)(ts_peer >> 8);
		out.data[ts_off + 2] = (uint8_t)(ts_peer >> 16);
		out.data[ts_off + 3] = (uint8_t)(ts_peer >> 24);
		(void)bcast_send_one(peer, out.data, out.wpos);
	}
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
 * Per notebook-b §5.6 (binary reverse of FUN_140126450), the
 * kunos exe DOES relay each inbound 0x20 as a transformed
 * SRV_SECTOR_SPLITS_RELAY (0x3a) broadcast to every other
 * client.  Without the relay, remote drivers' per-sector
 * splits never reach the AC2 client's in-race timetable for
 * any car the local driver isn't currently sitting in — the
 * timetable's "last split" column stays empty for all peers.
 * The earlier "exe never relays 0x3a" note in this file was
 * an unverified hypothesis that the pcap evidence contradicts.
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
	/*
	 * Ignore a split the CLIENT flags as IsSessionOver (lap-states bit
	 * 0x0400), mirroring the exe's wire-bit guard (FUN_1400142f0 case
	 * 0x20: `(word >> 8 & 4)` -> "Received split with isSessionOver
	 * flag; will ignore it").  The exe trusts the client's per-message
	 * bit, not the server phase.
	 */
	if ((car_field & 0x0400) != 0) {
		log_info("sector split ignored: isSessionOver (car=%d)",
		    c->car_id);
		return 0;
	}
	race = &s->cars[c->car_id].race;

	/*
	 * Drop a car's very first sector split.  Per FUN_1400142f0 case
	 * 0x20, a car that has not yet finished the formation lap
	 * (car+0x200 == 0) has its first split discarded: the exe logs
	 * "did not finish the formation lap", sets the flag and returns
	 * without recording or relaying.  Subsequent splits count.  The
	 * flag is managed here in the 0x20 path, not in the 0x21 lap-
	 * complete handler.
	 */
	if (!race->formation_lap_done) {
		race->formation_lap_done = 1;
		log_info("sector split: car=%d did not finish the formation "
		    "lap, first split dropped", c->car_id);
		return 0;
	}

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
	/* Append to the arrival-ordered per-lap split buffer (exe car+0x1d0
	 * vector) for the 0x3a relay below. */
	if (race->lap_split_n < 3)
		race->lap_split_buf[race->lap_split_n++] = sector_time_ms;
	/* Persist the lap-states word (exe car+0x54 / +0x1e8) for the 0x36
	 * status field and the 0x3c relay. */
	race->car_field = car_field;
	race->sectors_in_lap++;
	log_info("sector split: car=%d sector=%u time=%dms clock=%d",
	    c->car_id, (unsigned)sector_index, (int)sector_time_ms,
	    (int)clock_ms);

	/*
	 * 0x3a relay to every OTHER client (kunos exe queued-broadcast
	 * tier sends to all peers except the sender; sender already has
	 * the split locally).  Body: u8 0x3a + u16 car_id + u8 split_count
	 * + split_count x u32 split_time + i32 session_relative_ts +
	 * u16 car_field.
	 *
	 * The exe builder FUN_140011590 emits ALL cumulative splits of the
	 * open lap (count = vector size at car+0x1d0..+0x1d8), not just the
	 * latest one; accd previously hardcoded count=1 and the peer's live
	 * timetable only ever saw sector 0.  Emit the arrival-ordered
	 * per-lap buffer.
	 *
	 * The trailing i32 timestamp is normalised per recipient, mirroring
	 * the exe's use of FUN_14001ae20 (per-recipient fan-out) in the 0x20
	 * dispatcher (FUN_1400142f0:316) -- the same pattern as the 0x19 and
	 * 0x3c fixes.  Build the static parts once, then patch the 4-byte
	 * timestamp for each peer before sending.
	 * The trailing u16 is the lap-states/car_field word (exe car+0x1e8).
	 */
	{
		struct ByteBuf out;
		uint8_t n = race->lap_split_n;
		int ok, si, ts_off, i;

		bb_init(&out);
		ok = wr_u8(&out, SRV_SECTOR_SPLITS_RELAY) == 0 &&
		    wr_u16(&out, s->cars[c->car_id].car_id) == 0 &&
		    wr_u8(&out, n) == 0;
		for (si = 0; ok && si < n; si++) {
			uint32_t sw = race->lap_split_buf[si] < 0
			    ? LAP_TIME_INVALID
			    : (uint32_t)race->lap_split_buf[si];
			ok = wr_u32(&out, sw) == 0;
		}
		ts_off = (int)out.wpos;
		ok = ok && wr_i32(&out, 0) == 0 &&   /* placeholder, patched per peer */
		    wr_u16(&out, car_field) == 0;
		if (ok) {
			for (i = 0; i < ACC_MAX_CARS; i++) {
				struct Conn *peer = s->conns[i];
				int32_t ts_peer;
				if (peer == NULL || peer->state != CONN_AUTH ||
				    peer->is_smpr)
					continue;
				if (peer->conn_id == c->conn_id)
					continue;
				ts_peer = (int32_t)((int64_t)clock_ms +
				    conn_clock_offset(s, peer));
				out.data[ts_off + 0] = (uint8_t)ts_peer;
				out.data[ts_off + 1] = (uint8_t)(ts_peer >> 8);
				out.data[ts_off + 2] = (uint8_t)(ts_peer >> 16);
				out.data[ts_off + 3] = (uint8_t)(ts_peer >> 24);
				(void)bcast_send_one(peer, out.data, out.wpos);
			}
		}
		bb_free(&out);
	}
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
	/*
	 * Ignore a lap the CLIENT flags as IsSessionOver (lap-states bit
	 * 0x0400), mirroring the exe's wire-bit guard (FUN_1400142f0 case
	 * 0x21: `word & 0x400` -> "Received lap with isSessionOver flag;
	 * will ignore it").  The exe trusts the client's per-lap bit, not
	 * the server phase, so a final lap completing as the server crosses
	 * into its end-detection phase is still scored.
	 */
	if ((car_field & 0x0400) != 0)
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
		/*
		 * Trust the client's car_field flags for lap validity.
		 * Previously accd OR'd in race->out_of_track_latched, which
		 * meant that ANY 0x3d OUT_OF_TRACK during the lap (typical
		 * on tracks like paul_ricard with wide lateral runoff)
		 * invalidated the whole lap — even when the AC2 client
		 * itself considered the lap clean (no advantage gained, no
		 * cut bit set in car_field).  Kunos's exe doesn't do this:
		 * cut bookkeeping vs lap validity are separate concerns.
		 * Keep race->out_of_track_latched as a debounce for the
		 * per-lap cuts counter (handlers.c h_out_of_track) without
		 * leaking it into best_lap_ms / last_lap_ms.
		 */
		int invalid = has_cut || is_out_lap;

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

		/*
		 * Kunos pcap (admin_clear, Practice): after an out-lap
		 * the 0x36 shows lap_count=0 and status=0x0000, proving
		 * the exe skips both updates for out-laps.  Cut laps and
		 * other non-out-lap invalids still update car_field and
		 * lap_count (only is_out_lap gates them).
		 */
		if (!is_out_lap) {
			/* Persist the lap-states word for the 0x36 status
			 * field and the 0x3c relay. */
			race->car_field = car_field;
			race->lap_count++;
		}
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
		 * against the exe.  lap_history_count tracks valid laps;
		 * lap_count tracks all non-out-lap crossings.
		 */
		if (!invalid) {
			uint32_t slot = race->lap_history_count
			    % ACC_LAP_HISTORY;
			int si;
			race->lap_history_ms[slot] = lap_ms;
			for (si = 0; si < 3; si++)
				race->lap_splits_ms[slot][si] =
				    race->sector_ms[si];
			race->lap_history_driver[slot] =
			    s->cars[c->car_id].current_driver_index;
			race->lap_history_count++;
		}

		/*
		 * Results-log append: record EVERY completed lap (valid and
		 * invalid) in global completion order for the results.json
		 * laps[] array, independent of the valid-only 16-slot ring
		 * above.  isValidForBest mirrors the exe's results verdict
		 * (FUN_140129b10): invalid if any lap-states bit in 0x100f is
		 * set or the laptime is outside [1, 0x7ffffffe].  This is
		 * stricter than the live best_lap mask (cut + out-lap only),
		 * which is left untouched so 0x36 byte-parity is preserved.
		 */
		{
			int valid_for_best = (car_field & 0x100f) == 0 &&
			    lap_ms >= 1 && lap_ms <= 0x7ffffffe;

			results_laps_append(s, s->cars[c->car_id].car_id,
			    s->cars[c->car_id].current_driver_index,
			    lap_ms, race->sector_ms, valid_for_best);
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

		/* Per-lap state reset.  The exe (FUN_1400142f0:464) zeroes
		 * car+0x1e8 at lap-end before the 0x3b relay fan-out, so the
		 * 0x3c relay on the next out-of-track event always starts with
		 * a clean car_field; that is why the pcap shows car_field=0
		 * across inbound off-track reports on the new lap. */
		{
			race->current_lap_ms = 0;
			race->out_of_track_latched = 0;
			race->cuts_this_lap = 0;
			/*
			 * Snapshot the just-completed lap's splits BEFORE the
			 * per-lap sector_ms reset, so results.c can emit them
			 * in the per-car "lastSplits" field (which would
			 * otherwise read back as [0,0,0]).  Captures both
			 * valid and cut completions — matches kunos semantics
			 * (the last lap shown in the live timetable is the
			 * last lap completed, regardless of validity).
			 */
			race->last_lap_splits_ms[0] = race->sector_ms[0];
			race->last_lap_splits_ms[1] = race->sector_ms[1];
			race->last_lap_splits_ms[2] = race->sector_ms[2];
			race->sector_ms[0] = 0;
			race->sector_ms[1] = 0;
			race->sector_ms[2] = 0;
			/* Reset the 0x3a arrival-ordered split buffer for the
			 * new lap (mirrors the exe vector reset at lap-end). */
			race->lap_split_n = 0;
			/* Mirror exe line 464: zero car+0x1e8 for new lap. */
			race->car_field = 0;
			race->sectors_in_lap = 0;
			/*
			 * Record the S/F crossing timestamp as the start time
			 * of the new open lap (mirrors exe lap_start_time at
			 * car+0x1b8, set in FUN_1400142f0 at the 0x21 branch).
			 * Used as tiebreaker in cmp_cars after sectors_in_lap:
			 * earlier crossing = further through the lap = ahead.
			 */
			race->race_time_ms = lap_time;
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
		 * serve, else auto-DQ.  The exe always DQs a serve miss
		 * (allowAutoDQ does not soften it). */
		if (race->pen.count > 0 && !race->disqualified) {
			int pi = penalty_first_unserved_dtsg(&race->pen);
			struct PenaltyEntry *front = pi >= 0
			    ? &race->pen.slots[pi] : NULL;
			/*
			 * Only count laps the driver actually races: the exe
			 * (FUN_140125c60) decrements the serve countdown only
			 * when the lap-state in-lap bit (0x08) is clear, so an
			 * in-lap (including the lap the penalty is served on)
			 * doesn't burn the deadline and auto-DQ a lap early.
			 */
			if (front != NULL && front->laps_remaining > 0 &&
			    (car_field & 0x0008) == 0) {
				front->laps_remaining--;
				if (front->laps_remaining == 0) {
					uint8_t inherited = front->reason;
					uint8_t inherited_cat = front->category;
					char chat[128];
					/*
					 * The exe (FUN_140125c60:109) force-DQs the serve-deadline
					 * miss unconditionally; allowAutoDQ gates only the fresh
					 * cutting force, not the serve miss.  The DQ inherits the
					 * unserved entry's category (the exe passes entry+0x58),
					 * so the results label matches the original violation;
					 * value 0 matches the exe (param_7=0).
					 */
					log_info("Car %d failed to serve %s -> DQ",
					    c->car_id, penalty_name(front->kind));
					penalty_enqueue(s, c->car_id, EXE_DQ, inherited_cat, 0, 1, 0,
					    inherited);
					penalty_format_chat(chat, sizeof(chat), PEN_DQ,
					    inherited, 0,
					    s->cars[c->car_id].race_number);
					chat_broadcast(s, chat, 4);
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
			/*
			 * sector_ms[] was reset above (per-lap reset).  Read
			 * the splits from the freshly-taken last_lap_splits_ms
			 * snapshot so the Kunos "Lap" log line carries the
			 * real S1/S2/S3 times for accweb-style scrapers.
			 */
			int s1 = race->last_lap_splits_ms[0] < 0
			    ? 0 : race->last_lap_splits_ms[0];
			int s2 = race->last_lap_splits_ms[1] < 0
			    ? 0 : race->last_lap_splits_ms[1];
			int s3 = race->last_lap_splits_ms[2] < 0
			    ? 0 : race->last_lap_splits_ms[2];
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

		/*
		 * Re-broadcast the leaderboard: a lap completion bumped
		 * race->lap_count (handlers.c above), which is carried to the
		 * client ONLY in the 0x36 record (+0x1f4, handshake.c) and
		 * stored client-side only by the 0x36 receiver (FUN_14352c0c0
		 * +0x1f4); no realtime message (0x28/0x39/0x3b) carries it.
		 * Without this trigger the HUD timing-tower lap column freezes
		 * at its last-emitted value until an unrelated event (penalty,
		 * join/leave, phase change) fires a 0x36.  The stock server
		 * re-emits 0x36 every tick whenever its per-car leaderboard
		 * deep-compare detects a change (FUN_14002f710 -> FUN_140115f60
		 * -> FUN_140126f10 compares offset 0x1f4 = lap count); this is
		 * the minimal event-scoped equivalent.  The memcmp gate inside
		 * broadcast_leaderboard_if_changed keeps it a once-per-real-
		 * change emit, and the per-server leaderboard_pending boolean
		 * coalesces simultaneous lap completions into one 0x36 per tick,
		 * so it cannot flood even a 30-car pack crossing the line.
		 */
		leaderboard_request_emit(s);

		if (s->session.phase == PHASE_OVERTIME) {
			/*
			 * Crossing S/F during overtime is a race finish: mark
			 * the car so the phase-6 end-detection hold no longer
			 * waits on it (exe car+0x1d1 bit 0x04).
			 */
			if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS)
				s->cars[c->car_id].race.finished = 1;
			session_overtime_car_finished(s);
			session_quali_drop_eligibility(s, c->car_id);
		}
	}

	/*
	 * Build the transformed 0x3b broadcast per-recipient, mirroring
	 * the exe's FUN_14001ae20 call in the 0x21 dispatcher
	 * (FUN_1400142f0:475) -- same fan-out as 0x19, 0x3a, 0x3c.
	 * Field 2 keeps the negative -> LAP_TIME_INVALID guard so a crafted
	 * 0x21 can't sign-extend into a ~4-billion-ms time on the client.
	 * Field 4 is a session-relative timestamp normalised per recipient.
	 * FUN_14001ae20 excludes the sender (conn != param_4 check at
	 * FUN_14001ae20:29), so we skip the sender in the loop.
	 */
	{
		uint32_t split_wire = split_time < 0
		    ? LAP_TIME_INVALID : (uint32_t)split_time;
		int ts_off, i;
		bb_init(&out);
		if (wr_u8(&out, SRV_SECTOR_SPLIT_RELAY) < 0 ||
		    wr_u16(&out, s->cars[c->car_id].car_id) < 0 ||
		    wr_u32(&out, split_wire) < 0 ||
		    wr_u8(&out, flag_b) < 0)
			goto done;
		ts_off = (int)out.wpos;
		if (wr_i32(&out, 0) < 0 ||   /* placeholder, patched per peer */
		    wr_u16(&out, car_field) < 0)
			goto done;
		for (i = 0; i < ACC_MAX_CARS; i++) {
			struct Conn *peer = s->conns[i];
			int32_t ts_peer;
			if (peer == NULL || peer->state != CONN_AUTH ||
			    peer->is_smpr)
				continue;
			if (peer->conn_id == c->conn_id)
				continue;
			ts_peer = (int32_t)((int64_t)lap_time +
			    conn_clock_offset(s, peer));
			out.data[ts_off + 0] = (uint8_t)ts_peer;
			out.data[ts_off + 1] = (uint8_t)(ts_peer >> 8);
			out.data[ts_off + 2] = (uint8_t)(ts_peer >> 16);
			out.data[ts_off + 3] = (uint8_t)(ts_peer >> 24);
			(void)bcast_send_one(peer, out.data, out.wpos);
		}
	}
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
		/*
		 * Cap the relayed message at 35 code points, matching the
		 * exe's chat ceiling: FUN_1400142f0 passes 0x23 to
		 * FUN_140020700, which on overflow keeps the first 32 code
		 * points and appends "...".  The stock AC2 client already
		 * limits outbound chat to 35 code points, so this only bites
		 * a non-conforming client; without it accd would relay up to
		 * 255.  The exe caps its combined "name: message" string;
		 * accd's 0x2b carries sender and message as separate fields,
		 * so the cap lands on the message field.  Done on the relay
		 * path only, so chat commands (e.g. a long /report) keep
		 * their full argument.  The rd_str_a buffer holds 4 bytes per
		 * code point, so the three dots fit within the allocation.
		 */
		if (text != NULL) {
			size_t i, cps = 0, cut = 0;

			for (i = 0; text[i] != '\0'; i++) {
				if (((unsigned char)text[i] & 0xc0) == 0x80)
					continue;	/* continuation byte */
				if (cps == 32)
					cut = i;	/* 33rd code point */
				cps++;
			}
			if (cps > 35) {
				text[cut] = '.';
				text[cut + 1] = '.';
				text[cut + 2] = '.';
				text[cut + 3] = '\0';
			}
		}
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
			    BCAST_EXCEPT_NONE);
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

		race->in_pit = (location == 2 || location == 3 ||
		    location == 4) ? 1 : 0;
		race->on_track = (location == 1) ? 1 : 0;
		race->car_location = location;

		/*
		 * Driver-stint timing is NOT driven by track location.  The
		 * exe's 0x32 handler makes no stint call: a stint runs
		 * continuously from driver take-control (green / handshake /
		 * swap) through pit stops until a swap, disconnect, or session
		 * end.  accd previously started the stint on pit-exit and
		 * paused it on pit-entry, which never counted the grid->first-
		 * pit stint or pit time and under-enforced the driver-stint
		 * limit.  Stints now start at green (session.c), at handshake
		 * for mid-race joiners (below), and on the swap commit; they
		 * stop on swap, conn_drop, and stint_check_violations.
		 */

		/*
		 * No server-side pit-lane speeding penalty.  The exe has no
		 * server-side cat=3 origination: FUN_1400142f0 only relays
		 * the client's graduated PitSpeedingDetector 0x41 report
		 * (DT -> SG30 -> DQ).  Removed for strict wire parity.
		 */

		/*
		 * Pit-lane exit: no server-side dwell verification.  Kunos
		 * has no equivalent — the AC2 client engine validates the
		 * dwell time itself and emits 0x42 when it decides the
		 * DT/SG was served, which the server then propagates via
		 * FUN_140126b50 (h_penalty_cleared on our side).  We
		 * mirror that.
		 */
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
	int i, ts_off;

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
	 * and gates on a per-car off-track latch: bit 0 of the lap-states
	 * word at car+0x1e8 (the same word relayed as the 0x3c car_field;
	 * the struct path 0x180+0x40+0x28 resolves to 0x1e8).  Only the
	 * first force=0 event per latched cycle is relayed; everything else
	 * (force=1 events AND force=0 repeats within the same physical
	 * excursion) is silently dropped.
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

		/*
		 * Latch gate: exe (FUN_1400142f0 case 0x3d) sets bit 0 of
		 * car+0x1e8 on the first cut, suppresses all further 0x3c
		 * relays for the rest of the lap, and clears the bit at
		 * lap-complete (0x21 zeroes the word).  out_of_track_latched
		 * mirrors this exactly: set here, cleared in h_sector_split
		 * _single's per-lap reset block.
		 */
		if (race->out_of_track_latched) {
			log_debug("out-of-track: car=%d suppressed "
			    "(latch set)", c->car_id);
			return 0;
		}
		race->out_of_track_latched = 1;
		if (race->cuts_this_lap < 255)
			race->cuts_this_lap++;
		/*
		 * No quali instant-drop here: the exe 0x3d handler has no
		 * phase check and no eligibility mutation - it only sets the
		 * +0x1e8 latch and relays 0x3c.  Quali lap validity is
		 * decided by the client lap-states word at lap completion
		 * (h_sector_split_single -> session_quali_drop_eligibility
		 * during overtime), matching how the exe drives it.
		 */
		log_info("out-of-track: car=%d ts=%d cuts=%u",
		    c->car_id, (int)ts_raw,
		    (unsigned)race->cuts_this_lap);
	}

	/*
	 * 2nd u16 is the per-car lap-states / car_field word (exe builder
	 * FUN_140018210 emits car+0x1e8, the same value carried in the 0x3a
	 * trailing field), NOT a cut counter.  accd previously sent the
	 * internal cuts_this_lap counter here; emit the persisted lap-states
	 * word so the client's out-of-track widget reads the same flags it
	 * gets from 0x3a/0x3b.
	 *
	 * The timestamp is per-recipient: the exe applies FUN_140041fc0
	 * (each receiving conn's clock offset) inside FUN_14001ae20.
	 * Build one shared buffer and patch the 4-byte timestamp field for
	 * each peer, excluding the sender.
	 */
	bb_init(&out);
	if (wr_u8(&out, SRV_OUT_OF_TRACK_RELAY) < 0 ||
	    wr_u16(&out, s->cars[c->car_id].car_id) < 0 ||
	    wr_u16(&out, s->cars[c->car_id].race.car_field) < 0)
		goto out;
	ts_off = (int)out.wpos;
	if (wr_i32(&out, 0) < 0)	/* placeholder, patched per peer */
		goto out;
	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *peer = s->conns[i];
		int32_t ts_peer;

		if (peer == NULL || peer->state != CONN_AUTH || peer->is_smpr)
			continue;
		if (peer->conn_id == c->conn_id)
			continue;
		ts_peer = (int32_t)((int64_t)ts_raw +
		    conn_clock_offset(s, peer));
		out.data[ts_off + 0] = (uint8_t)ts_peer;
		out.data[ts_off + 1] = (uint8_t)(ts_peer >> 8);
		out.data[ts_off + 2] = (uint8_t)(ts_peer >> 16);
		out.data[ts_off + 3] = (uint8_t)(ts_peer >> 24);
		(void)bcast_send_one(peer, out.data, out.wpos);
	}
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
	 * Cats 1, 2, 7, 9 have NO wire path in FUN_1400f03b0.  Cat 8
	 * (RaceControl) DOES have wire paths (wires 15-19 for DT/SG/DQ).
	 * Best fallback: REASON_RACE_CONTROL (wire 15-19) for unknown cats.
	 *
	 * Note: penalty.c:penalty_wire_value emits these wire codes
	 * verbatim, matching the kunos dispatcher FUN_1400f03b0 byte for
	 * byte, including wire 27 (% prefix) and wires 33-35 (! prefix,
	 * deleted from the AC2 client lookup map).  There is no
	 * substitution to a renderable code: the unrenderable ones go out
	 * exactly as kunos sends them.
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
	case 8:		/* Trolling — has cat-8 RaceControl wire paths (15-19) */
		return REASON_RACE_CONTROL;
	case 1:		/* Collision */
	case 2:		/* IllegalOvertake */
	case 7:		/* UnsafeRejoin */
	case 9:		/* ReverseInPitlane */
	default:
		/*
		 * FUN_1400f03b0 has no inner-switch case for these cats under
		 * any kind, so the exe emits wire 0 (No_Penalty).  REASON_NONE
		 * -> penalty_wire_value 0, which the present-gate surfaces as
		 * no penalty, matching the exe.  Stock clients never report
		 * 1/2/7/9; reachable only via a modified client.
		 */
		return REASON_NONE;
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
	 * (PHASE_SESSION / OVERTIME).  Real client-detected violations
	 * (cuts, start violations, wrong-way, lights-off) only occur once
	 * cars are racing; formation / pre-race transitions report via
	 * 0x3d / 0x19 / 0x20, not 0x41.  The stock server does NOT phase-
	 * gate 0x41 (FUN_1400142f0 case 0x41 forwards straight to
	 * FUN_140125f50), but accd drops out-of-race reports defensively so
	 * a stray pre-green 0x41 can't materialise a penalty before the race
	 * has started.  This loses no real in-race violation.
	 */
	if (s->session.phase != PHASE_SESSION &&
	    s->session.phase != PHASE_OVERTIME)
		return 0;
	/*
	 * Primer filter.  The client fires 0x41 on every lap / violation
	 * tick with value=0 as a register-severity heads-up; the exe's
	 * FUN_140125f50 fresh-branch (140125f50:147-153) stores the kind
	 * in the per-car-per-kind PenaltySheet without pushing a Penalty
	 * onto the sheet's inner vector, so a value=0 DT/SG primer paints
	 * nothing.  Our penalty_enqueue materialises on fresh, so drop those
	 * primers here.
	 *
	 * EXCEPTION: a client DQ self-report arrives as kind=EXE_DQ with
	 * value=0 (the value field is meaningless for a DQ; every internal
	 * exe DQ passes 0 too).  The exe MATERIALISES it — FUN_140125f50
	 * sets the +0x59=6 DQ scalar consumed by the results/standings
	 * builder FUN_140129b10, it does not drop it.  These are exactly the
	 * four categories the server has no telemetry to infer: wrong-way
	 * (10), lights-off (15), speeding-on-start (16), wrong-grid (17),
	 * all already mapped in client_category_to_reason.  Let kind=EXE_DQ
	 * through so accd applies them; the pending latch below keeps the DQ
	 * tail-only on the wire like every other client report.
	 */
	if (value <= 0 && kind != EXE_DQ)
		return 0;
	{
		uint8_t reason = client_category_to_reason(category);
		/*
		 * DT/SG value is the report's lap/cut count (always > 0 after
		 * the primer drop).  A DQ carries value 0 (the exe ignores it),
		 * so pass it verbatim — clamping negatives to 0 — to keep the
		 * 0x36 tail b1 at 0 like the exe.
		 */
		int32_t val = value > 0 ? value : 0;

		/*
		 * force = 1 only when cat == 0 (Cutting) AND auto-DQ is
		 * allowed.  FUN_1400142f0 case 0x41 gates the cutting force
		 * on a RaceControl byte (+0x100) that defaults to set, so
		 * kunos auto-DQs repeated cutting out of the box (verified:
		 * the 4-bot ladder run DQs cutting with a default config).
		 * allowAutoDQ is the operator control that clears it; with
		 * allowAutoDQ=0 the ladder caps repeated cutting at SG30
		 * instead of DQ, consistent with how accd already gates its
		 * other auto-DQ paths (lap-end serve, pit speeding).
		 * Non-cutting categories always force=0, which also gates the
		 * cat=6 (IgnoredMandatoryPit) DQ-to-TP130 conversion.
		 */
		int force = (category == 0 && s->allow_auto_dq) ? 1 : 0;
		log_info("client-reported penalty: car=%d kind=%u "
		    "category=%u -> reason=%u",
		    c->car_id, (unsigned)kind, (unsigned)category,
		    (unsigned)reason);
		/*
		 * Mark the freshly-materialised entry pending. A client 0x41
		 * report surfaces only in the per-car tail bytes (car+0x200/
		 * +0x201), never in the active_pen prefix (present stays 0) or
		 * as a wire code in the pq array. VM pcap run_race_end
		 * (2026-05-30, a forced cutting DT cat=0:1:3 landing mid-race
		 * in PHASE_SESSION) shows kunos emit present=0 plus pq sentinel
		 * [0] plus tail 01 03 for the whole live-DT window, never
		 * present=1, even with force=1. The 0x36 builder's active
		 * prefix and pq wire loop both skip pending entries, so the
		 * entry is tail-only in Practice (pq empty) and tail plus pq
		 * sentinel in Race, byte-matching kunos in both phases. The
		 * escalation ladder still updates the tail wire code because the
		 * tail's DQ-priority back-scan keeps pending entries visible
		 * (handshake.c). Guard on a count delta: register-only ladder
		 * steps don't materialise a slot, so blindly latching
		 * slots[count-1] would mark a pre-existing entry pending.
		 *
		 * This restores the pre-regression behaviour: the latch was
		 * dropped earlier for a "visible immediately" UX that diverged
		 * from the stock server (which is itself tail-only here, so the
		 * client HUD renders the DT identically from the tail).
		 */
		{
			struct PenaltyQueue *pq = &s->cars[c->car_id].race.pen;
			int before = pq->count;
			(void)penalty_enqueue(s, c->car_id, kind, category,
			    val, force, 0, reason);
			if (pq->count > before)
				pq->slots[pq->count - 1].pending = 1;
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

/* ----- 0x45 ACP_CAR_DIRT_UPDATE -> store + relay 0x46 (TCP) ----- */

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
	 * Store per-car (write_spawn_def reads this array so late joiners
	 * see accumulated weathering in the welcome spawnDef tail) AND
	 * relay live to every other client.  The exe (FUN_1400142f0 case
	 * 0x45) stores the dirt then unconditionally broadcasts a 0x46
	 * frame = u8 0x46 + u16 car_id + 5 x u8 dirt (FUN_1400327a0) to all
	 * peers except the sender via FUN_14001ada0.  Without the relay,
	 * already-connected peers never see dirt accumulate on this car.
	 */
	memcpy(s->cars[c->car_id].race.car_dirt, dirt, sizeof(dirt));
	{
		struct ByteBuf out;
		int ok;

		bb_init(&out);
		ok = wr_u8(&out, SRV_CAR_DIRT_RELAY) == 0 &&
		    wr_u16(&out, s->cars[c->car_id].car_id) == 0;
		for (i = 0; ok && i < 5; i++)
			ok = wr_u8(&out, dirt[i]) == 0;
		if (ok)
			(void)bcast_all(s, out.data, out.wpos, c->conn_id);
		bb_free(&out);
	}
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
	/*
	 * The exe only builds 0x47 for multi-driver entries (FUN_140011bf0:47
	 * gates on driver_count > 1); a single-driver car never emits a swap-
	 * state broadcast.  Match that so single-driver slots stay silent.
	 */
	if (car->driver_count <= 1)
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
			if (s->cars[j].team_entry_id == car->team_entry_id &&
			    s->cars[j].used)
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

	/*
	 * No in-pit gate: the exe's 0x48 EXECUTE path (FUN_1400142f0 case
	 * 0x48) has no pit or session-phase check — that gate lives only in
	 * the chat %swap path (FUN_140027990).  accd previously rejected a
	 * not-in-pit 0x48 the exe would have accepted, so the gate is dropped
	 * here to match.  The chat &swap path (chat.c) keeps its own pit gate.
	 */

	/* Commit the swap.  Flush the outgoing driver's stint time
	 * into driver_stint_ms before reassigning current_driver_index
	 * so the accumulator lands on the correct driver slot, then start
	 * the incoming driver's stint (the exe restarts on swap via
	 * FUN_14012b230 -> FUN_14011ab60; accd previously only stopped). */
	i = car->current_driver_index;   /* outgoing slot */
	stint_stop_tracking(s, c->car_id);
	car->current_driver_index = swap_code;
	stint_start_tracking(s, c->car_id);
	car->swap_state[i] = 5;          /* outgoing: exe FUN_140012830:106 */
	car->swap_state[swap_code] = 2;  /* incoming: exe FUN_140012830:105 */
	log_info("driver swap: car %u -> driver %u (%s %s)",
	    (unsigned)car_id, (unsigned)swap_code,
	    car->drivers[swap_code].first_name,
	    car->drivers[swap_code].last_name);
	result = 0;

	/*
	 * Send 0x49 reply before the 0x58 broadcast.  Exe FUN_140012c30:1061
	 * sends the reply to the requester first, then lines 1063-1094 do the
	 * swap-notify broadcast.
	 */
	bb_init(&out);
	if (wr_u8(&out, SRV_DRIVER_SWAP_RESULT) == 0 &&
	    wr_u8(&out, result) == 0)
		bcast_send_one(c, out.data, out.wpos);
	bb_free(&out);

	/*
	 * Broadcast 0x58 driver swap notification.  exe FUN_1400142f0:1063
	 * gates this on the doDriverSwapBroadcast config flag (the same flag
	 * that gates the 0x47 state broadcast), so suppress it when disabled.
	 */
	if (s->do_driver_swap_broadcast) {
		bb_init(&out);
		if (wr_u8(&out, SRV_DRIVER_SWAP_NOTIFY) == 0 &&
		    wr_u16(&out, car_id) == 0 &&
		    wr_u8(&out, swap_code) == 0)
			(void)bcast_all(s, out.data, out.wpos,
			    BCAST_EXCEPT_NONE);
		bb_free(&out);
	}

	/* Broadcast reset swap state. */
	broadcast_swap_state(s, car);

	/*
	 * Re-sync this car's BoP to the swapping connection.  The exe swap
	 * commit (FUN_140012830) re-sends 0x53 to the swap conns even though
	 * a swap leaves ballast / restrictor unchanged; mirror it.
	 */
	bb_init(&out);
	if (wr_u8(&out, SRV_BOP_UPDATE) == 0 &&
	    wr_u16(&out, car->car_id) == 0 &&
	    wr_u16(&out, (uint16_t)car->ballast_kg) == 0 &&
	    wr_f32(&out, car->restrictor) == 0)
		bcast_send_one(c, out.data, out.wpos);
	bb_free(&out);
	return 0;

reply:
	/* Send 0x49 reply to the requester (error path). */
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
	uint8_t msg_id, wire_b2, state;
	uint16_t car_id;
	struct CarEntry *car;
	int i;

	/*
	 * Wire body = u16 car_id + u8 + u8.  Per exe FUN_1400142f0:1107-1110
	 * the SECOND data byte is read but ignored for dispatch, and the
	 * THIRD byte is the swap state that drives the switch and is stored.
	 * accd previously dispatched on the second byte (the ignored one),
	 * so a real client's request fell through to "not implemented" and
	 * the state was never applied.  Read the ignored byte into wire_b2
	 * and dispatch/store on state (the third byte).
	 */
	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u16(&r, &car_id) < 0 ||
	    rd_u8(&r, &wire_b2) < 0 ||
	    rd_u8(&r, &state) < 0) {
		log_warn("h_driver_swap_state_request: short body");
		return 0;
	}
	(void)wire_b2;
	if (check_car_owner(c, car_id) < 0) {
		log_warn("ACP_DRIVER_SWAP_STATE_REQUEST for the wrong "
		    "carId: %u (Connection owns %d)",
		    (unsigned)car_id, c->car_id);
		return 0;
	}
	car = &s->cars[c->car_id];

	switch (state) {
	case 2:
		/*
		 * Initiate: set the requesting driver's swap state
		 * to the value the client sent.
		 */
		if (car->current_driver_index < car->driver_count)
			car->swap_state[car->current_driver_index] = state;
		break;
	case 3:
		/*
		 * Confirm: kunos's FUN_1400142f0:1105-1194 resets every
		 * team mate's swap_state[i] in {3,4} back to 2 (per
		 * `(state - 3) < 2`), then applies the new state
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
				if (car->swap_state[i] == 3 ||
				    car->swap_state[i] == 4)
					car->swap_state[i] = 2;
		}
		if (car->current_driver_index < car->driver_count)
			car->swap_state[car->current_driver_index] = state;
		break;
	case 4:
		/* Execute: set requesting driver to EXECUTING. */
		if (car->current_driver_index < car->driver_count)
			car->swap_state[car->current_driver_index] = 4;
		break;
	default:
		log_warn("DriverSwap Request for type %u is not "
		    "implemented", (unsigned)state);
		return 0;
	}

	log_info("driver swap state request: car=%u b2=%u state=%u",
	    (unsigned)car_id, (unsigned)wire_b2, (unsigned)state);
	/*
	 * Same team-group fan-out as h_update_driver_swap_state: kunos
	 * emits one 0x47 per car in the group, each carrying its own
	 * (possibly mutated by the Confirm reset above) swap_state[].
	 */
	if (car->team_entry_id >= 0) {
		int g;
		for (g = 0; g < ACC_MAX_CARS; g++) {
			if (s->cars[g].team_entry_id == car->team_entry_id &&
			    s->cars[g].used)
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
	 * Stint accounting, matching the exe 0x4f case (FUN_1400142f0):
	 * force == 0 ends the current driver's stint (FUN_140126eb0),
	 * force != 0 restarts it (FUN_14012b230).  accd previously did
	 * neither here and instead invented a 30s "tow penalty" with no
	 * exe analogue, whose in_tow/tow_until_ms state was write-only.
	 */
	stint_stop_tracking(s, c->car_id);
	if (force != 0)
		stint_start_tracking(s, c->car_id);

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
				double ts_d, ts_adj;
				uint8_t bytes[8];
				/*
				 * Wire ts is an IEEE-754 double, not an
				 * integer.  Reinterpret the bits directly
				 * (memcpy avoids UB) before adding the
				 * session clock offset.
				 */
				memcpy(&ts_d, &ts_raw, sizeof(ts_d));
				ts_adj = ts_d +
				    (double)conn_clock_offset(s, c);
				memcpy(bytes, &ts_adj, sizeof(bytes));
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
		leaderboard_request_emit(s);
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

		if (race->mandatory_pit_served < 255) {
			race->mandatory_pit_served++;
			/*
			 * 0x36 leaderboard byte at +0x204 carries
			 * max(0, mandatory_pit_count - mandatory_pit_served);
			 * trigger a rebroadcast so peers' OBLIGATOIRE widget
			 * decrements within one tick instead of waiting for
			 * the next leaderboard-pending event (or the 75 s
			 * async heartbeat, off by default).
			 */
			leaderboard_request_emit(s);
		}
		/*
		 * Kunos's 0x54 handler (FUN_1400142f0:1265) decrements a
		 * "mandatory pits remaining" counter via FUN_14012aff0
		 * and does NOT touch the DT/SG queue.  A pending DT/SG
		 * is served only when the client engine emits 0x42 after
		 * its own dwell-time check.  Mirror that here: don't
		 * coalesce DT/SG service into the mandatory-pit completion.
		 */
		/*
		 * No server-side mandatory-swap-skip penalty.  The exe's
		 * 0x54 path (FUN_1400142f0:1265) only decrements the
		 * mandatory-pit counter; the IgnoredDriverStint penalty is
		 * originated by the client (0x41 cat=11), not the server.
		 * Removed for strict wire parity.
		 */
	}
	log_info("Served Mandatory Pitstop: %u", (unsigned)car_id);
	return 0;
}

/* ----- 0x55 ACP_LOAD_SETUP -> reply 0x56 ------------------------ */

/*
 * Despite the enum name, 0x55 is not a car-setup-file load — it's
 * the in-game garage's "load lap history for this car from session N"
 * request.  Wire layout (case 0x55 in FUN_1400142f0):
 *
 *     u8  0x55
 *     u8  session_index   (0-based: 0 P, 1 Q, 2 R.  The exe compares
 *                          it to the session position at +0x268 and
 *                          uses it as a raw archive-vector index, NOT
 *                          the session type 0/4/10.)
 *     u16 car_id
 *     u32 revision        (read and discarded in both the exe and accd)
 *
 * Reply 0x56 body (FUN_1400328f0):
 *
 *     u8  0x56
 *     u8  session_index    (echoed from request)
 *     u16 car_id
 *     i16 lap_count
 *     lap_count × Lap_record:
 *         u32   lap_time_ms       (NO leading track name — the exe
 *                                  copies but never serialises it)
 *         u8    split_count
 *         split_count × u32 split_time_ms
 *         u16   car_id
 *         u8    lap_quality       (0 = clean)
 *         u16   lap_number        (1-based)
 *     then a trailing single-car leaderboard record (FUN_140034210)
 *     with the cvar8 byte forced to 0.
 *
 * The source race state is the live car->race for the current session
 * index, else the car's archived snapshot race_archive[session_index].
 * An unknown car or an out-of-range index gets no reply (the exe
 * returns silently); a valid-but-empty session replies lap_count 0.
 */
int
h_load_setup(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, sess_index;
	uint16_t car_id;
	uint32_t revision;
	struct ByteBuf out;
	struct CarEntry *car;
	int slot, laps_emitted = 0;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u8(&r, &sess_index) < 0 ||
	    rd_u16(&r, &car_id) < 0 ||
	    rd_u32(&r, &revision) < 0) {
		log_warn("h_load_setup: short body");
		return 0;
	}

	/*
	 * An unknown car or an out-of-range session index gets no reply,
	 * matching the exe (FUN_1400142f0 cleans up and returns without
	 * sending 0x56).
	 */
	slot = (int)car_id - ACC_CAR_ID_BASE;
	if (slot < 0 || slot >= ACC_MAX_CARS || !s->cars[slot].used) {
		log_info("load setup: unknown car %u, no reply",
		    (unsigned)car_id);
		return 0;
	}
	car = &s->cars[slot];
	if (sess_index >= s->session_count ||
	    sess_index >= ACC_MAX_SESSIONS) {
		log_info("load setup: session index %u out of range "
		    "(have %u), no reply", (unsigned)sess_index,
		    (unsigned)s->session_count);
		return 0;
	}

	/*
	 * Pick the race state to serve: the live car->race for the
	 * current session index, else the car's archived snapshot for
	 * that 0-based session index.  A NULL archive slot means the
	 * session was never completed for this car -> lap_count 0.
	 */
	{
		const struct CarRaceState *src =
		    (sess_index == s->session.session_index)
		    ? &car->race : car->race_archive[sess_index];

		log_info("load setup: conn=%u car=%u sess_index=%u rev=%u "
		    "src=%s", (unsigned)c->conn_id, (unsigned)car_id,
		    (unsigned)sess_index, (unsigned)revision,
		    src == NULL ? "none"
			: (src == &car->race ? "current" : "archive"));

		bb_init(&out);
		if (wr_u8(&out, SRV_SETUP_DATA_RESPONSE) < 0 ||
		    wr_u8(&out, sess_index) < 0 ||
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

				/*
				 * Per-lap record starts directly with the u32
				 * lap time, matching FUN_1400328f0 (which copies
				 * but never serialises the track name) and the
				 * client parser FUN_143528910 (reads u32 first).
				 * An earlier leading str_a(track) desynced the
				 * client: it read the string's length byte as the
				 * low byte of lap_time and mis-parsed the rest of
				 * the Previous-Laps panel.
				 */
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
	(void)write_car_leaderboard_record(&out, s, car, 0, 0, NULL, 0);
	(void)bcast_send_one(c, out.data, out.wpos);
	log_debug("0x56 reply: conn=%u car=%u sess_index=%u laps=%d "
	    "(%zu bytes)", (unsigned)c->conn_id, (unsigned)car_id,
	    (unsigned)sess_index, laps_emitted, out.wpos);
done:
	bb_free(&out);
	return 0;
}

/* ----- 0x5b ACP_CTRL_INFO --------------------------------------- */

/*
 * Parse the CtrlInfo payload (the client's controller / assist report)
 * and forward a one-line summary to every admin connection.  The client
 * sends 0x5b unsolicited and in reply to the 1-byte SRV_CTRL_INFO_REQUEST
 * probe the /controllers and /controller admin commands emit.
 *
 * Wire layout (parser FUN_14002c1e0, struct offsets in parens).  The
 * three strings are str_a (u8 count + N x u32 codepoints), NOT str_b:
 *   u32   car_id        (+0x00)
 *   str_a model         (+0x08)
 *   u8    f_as          (+0x28)   drives ", as"
 *   u8    f_scp         (+0x30)   drives ", no scp" when this byte is 0
 *   u8    f_gpe         (+0x31)   drives ", gpe"
 *   f32   sc            (+0x2c)   drives ", sc <value>" when > 0
 *   str_a cam_near      (+0x38)
 *   str_a cam_far       (+0x58)
 *   u32   scalar_b      (+0x34)
 *   f32   wear          (+0x78)
 *   u32   setup_id      (+0x7c)
 *
 * Chat summary, ported verbatim from the dispatcher (FUN_1400142f0 case
 * 0x5b lines 1382-1418): "Ctrl Info carId<id> (<driver>): <model>" then
 * an assist branch emitting ", gpe" / ", as" / ", sc <f>" / ", no scp" /
 * ", running defaults".  The ", sc"/", no scp" tail runs in every case
 * except the lone "running defaults" path.  At 251 rendered bytes the
 * text is replaced with "Received ctrl info, but message is too long.
 * Please check logs".  Sent as 0x2b with an EMPTY sender name and
 * chat_type 0, to admin connections only.
 */
int
h_ctrl_info(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, f_as = 0, f_scp = 0, f_gpe = 0;
	uint32_t car_id_u32 = 0, scalar_b = 0, setup_id = 0;
	float sc = 0.0f, wear = 0.0f;
	char *model = NULL, *cam_near = NULL, *cam_far = NULL;
	char chat[256];
	const char *driver_name;
	size_t off;
	int run_sc, i;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 || rd_u32(&r, &car_id_u32) < 0) {
		log_warn("ctrl info: short header conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	(void)rd_str_a(&r, &model);
	(void)rd_u8(&r, &f_as);
	(void)rd_u8(&r, &f_scp);
	(void)rd_u8(&r, &f_gpe);
	(void)rd_f32(&r, &sc);
	(void)rd_str_a(&r, &cam_near);
	(void)rd_str_a(&r, &cam_far);
	(void)rd_u32(&r, &scalar_b);
	(void)rd_f32(&r, &wear);
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
	APPEND("Ctrl Info carId%d (%s): %s",
	    (int)car_id_u32, driver_name, model ? model : "");
	/*
	 * Assist-token branch, ported from FUN_1400142f0 case 0x5b
	 * (lines 1395-1418).  f_gpe (+0x31) selects the leading token;
	 * the ", sc"/", no scp" tail (run_sc) runs in every case except
	 * the lone "running defaults" path.  The exe prints the +0x2c
	 * float via ostream (default precision), so %g matches.
	 */
	run_sc = 1;
	if (f_gpe == 0) {
		if (f_as != 0)
			APPEND(", as");
		else if (sc == 0.0f && f_scp != 0) {
			APPEND(", running defaults");
			run_sc = 0;
		}
		/* else (sc != 0 || f_scp == 0): straight to the tail */
	} else {
		APPEND(", gpe");
		if (f_as != 0)
			APPEND(", as");
	}
	if (run_sc) {
		if (sc > 0.0f)
			APPEND(", sc %g", (double)sc);
		if (f_scp == 0)
			APPEND(", no scp");
	}
#undef APPEND

	log_info("ctrl info: conn=%u car=%d cam=%s-%s wear=%g setup=%u",
	    (unsigned)c->conn_id, (int)car_id_u32,
	    cam_near ? cam_near : "", cam_far ? cam_far : "",
	    (double)wear, (unsigned)setup_id);
	(void)scalar_b;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *dst = s->conns[i];
		struct ByteBuf bb;

		if (dst == NULL || dst->state != CONN_AUTH || !dst->is_admin)
			continue;
		bb_init(&bb);
		/*
		 * Exe framing for this message: EMPTY sender name and
		 * chat_type 0 (FUN_1400142f0:1438/1456), unlike the
		 * RC_SENDER/type-4 used by other server-originated chat.
		 */
		if (wr_u8(&bb, SRV_CHAT_OR_STATE) == 0 &&
		    wr_str_a(&bb, "") == 0 &&
		    wr_str_a(&bb, off >= 251
			? "Received ctrl info, but message is too long. "
			"Please check logs" : chat) == 0 &&
		    wr_i32(&bb, 0) == 0 &&
		    wr_u8(&bb, 0) == 0)
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
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS ||
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
	 * on every 0x1e.
	 *
	 * Also update the clock drift accumulator (FUN_1400419e0:20-25).
	 * drift += (server_delta - client_delta) on each new-seq packet.
	 * Only runs after first pong (session_clock_seen).  drift_valid=0
	 * after a best-pong reset — first packet after reset stores prev
	 * timestamps without touching drift (mirrors exe's prev_seq=-1
	 * sentinel path).  Car+0x50 (wire +0x07) = (int)(best_rtt + drift).
	 */
	if (c->session_clock_seen) {
		double srv = (double)(int)(uint32_t)mono_ms();
		double cli = (double)(int)(uint32_t)client_ts_ms;

		if (c->drift_valid)
			c->drift_ms += (srv - c->drift_prev_server) -
			    (cli - c->drift_prev_client);
		c->drift_prev_server = srv;
		c->drift_prev_client = cli;
		c->drift_valid = 1;
	}
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
	 * Record a "last moved" timestamp whenever the car's confirmed
	 * velocity (vec_c, m/s) exceeds 5 km/h, mirroring exe FUN_140027f80
	 * (which writes car+0x158 from FUN_1400427c0's magnitude when
	 * speed*3.6 > 5).  The phase-6 end-detection hold reads it.  Compare
	 * squared magnitudes to avoid a sqrt: (5 km/h / 3.6)^2 = 1.929,
	 * (350 km/h / 3.6)^2 = 9452 is the exe's upper sanity clamp (a
	 * garbage velocity reads as stationary).
	 */
	{
		float sq = rt->vec_c[0] * rt->vec_c[0] +
		    rt->vec_c[1] * rt->vec_c[1] +
		    rt->vec_c[2] * rt->vec_c[2];
		if (sq > 1.929f && sq <= 9452.0f)
			rt->last_moved_ms = mono_ms();
	}

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
	/* Mirror FUN_140027f80:246 -- exe gates 0x22 on CONN_AUTH. */
	if (requester->state != CONN_AUTH)
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
