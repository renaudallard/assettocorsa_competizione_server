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

#include <stdlib.h>
#include <string.h>

#include <math.h>

#include "bcast.h"
#include "chat.h"
#include "handlers.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "penalty.h"
#include "prim.h"
#include "session.h"
#include "state.h"

/*
 * Helper: verify that the given carId matches the car slot owned
 * by this connection.  Returns 0 if valid, -1 if not.
 */
static int
check_car_owner(struct Conn *c, uint16_t wire_car_id)
{
	if (c->car_id < 0)
		return -1;
	/* Compare against the wire ID (base 1001), not the slot index. */
	return ((uint16_t)(ACC_CAR_ID_BASE + c->car_id) == wire_car_id) ? 0 : -1;
}

/* ----- 0x19 ACP_LAP_COMPLETED -> mutate state, broadcast 0x1b --- */

int
h_lap_completed(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint16_t pos_a, pos_b;
	int32_t lap_time_ms;
	uint8_t quality;
	struct ByteBuf out;
	struct CarRaceState *race;
	int rc;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0)
		return -1;
	if (rd_u16(&r, &pos_a) < 0 ||
	    rd_u16(&r, &pos_b) < 0 ||
	    rd_i32(&r, &lap_time_ms) < 0 ||
	    rd_u8(&r, &quality) < 0) {
		log_warn("h_lap_completed: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	if (c->car_id < 0 || c->car_id >= ACC_MAX_CARS)
		return 0;
	race = &s->cars[c->car_id].race;

	/*
	 * 0x19 fires on sector/checkpoint crossings, not on full
	 * lap completion.  Do NOT increment lap_count here; the
	 * real lap completion arrives as 0x20 ACP_SECTOR_SPLIT bulk
	 * which carries sector times and a valid lap total.  Track
	 * current_lap_ms for display and relay the event as 0x1b.
	 */
	race->current_lap_ms = lap_time_ms;

	bb_init(&out);
	if (wr_u8(&out, SRV_LAP_BROADCAST) < 0 ||
	    wr_u16(&out, pos_a) < 0 ||
	    wr_u16(&out, pos_b) < 0 ||
	    wr_i32(&out, lap_time_ms) < 0 ||
	    wr_u8(&out, quality) < 0)
		goto out;
	rc = bcast_all(s, out.data, out.wpos, c->conn_id);
	(void)rc;
out:
	bb_free(&out);
	return 0;
}

/* ----- 0x20 ACP_SECTOR_SPLIT (bulk) -> broadcast 0x3a ------------ */

int
h_sector_split_bulk(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	int32_t field_a, clock_ms;
	uint8_t split_count;
	uint16_t car_field;
	uint32_t splits[16];
	int i;
	struct ByteBuf out;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_i32(&r, &field_a) < 0 ||
	    rd_u8(&r, &split_count) < 0 ||
	    rd_i32(&r, &clock_ms) < 0 ||
	    rd_u16(&r, &car_field) < 0) {
		log_warn("h_sector_split_bulk: short header from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	if (c->car_id < 0)
		return 0;
	if (split_count > 16) {
		log_warn("Received split with unreasonable count %u "
		    "from conn=%u",
		    (unsigned)split_count, (unsigned)c->conn_id);
		return 0;
	}
	for (i = 0; i < split_count; i++) {
		if (rd_u32(&r, &splits[i]) < 0) {
			log_warn("ACP_SECTOR_SPLIT: short split data");
			return 0;
		}
	}
	/*
	 * 0x20 is the real lap completion trigger.  Update the
	 * car's race state with the lap total and sector times.
	 */
	{
		struct CarRaceState *race = &s->cars[c->car_id].race;

		race->lap_count++;
		race->race_time_ms = clock_ms;
		if (split_count > 0) {
			int32_t lap_ms = (int32_t)splits[split_count - 1];

			race->last_lap_ms = lap_ms;
			if (race->best_lap_ms == 0 ||
			    lap_ms < race->best_lap_ms)
				race->best_lap_ms = lap_ms;
		}
		race->current_lap_ms = 0;
	}
	log_info("lap completed: car=%d lap=%d clock=%d splits=%u",
	    c->car_id, s->cars[c->car_id].race.lap_count,
	    (int)clock_ms, (unsigned)split_count);

	session_recompute_standings(s);

	/* Build the transformed 0x3a broadcast. Body:
	 *   u16 car_id + u8 split_count + u32[count] + i32 clock +
	 *   u16 car_field. */
	bb_init(&out);
	if (wr_u8(&out, SRV_SECTOR_SPLITS_RELAY) < 0 ||
	    wr_u16(&out, (uint16_t)c->car_id) < 0 ||
	    wr_u8(&out, split_count) < 0)
		goto done;
	for (i = 0; i < split_count; i++)
		if (wr_u32(&out, splits[i]) < 0)
			goto done;
	if (wr_i32(&out, clock_ms) < 0 ||
	    wr_u16(&out, car_field) < 0)
		goto done;
	(void)bcast_all(s, out.data, out.wpos, c->conn_id);
done:
	bb_free(&out);
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
	{
		struct CarRaceState *race = &s->cars[c->car_id].race;
		uint8_t sector = flag_b < 3 ? flag_b : 0;

		race->sector_ms[sector] = split_time;
		if (race->best_sectors_ms[sector] == 0 ||
		    split_time < race->best_sectors_ms[sector])
			race->best_sectors_ms[sector] = split_time;
	}
	log_info("sector split single: car=%d split=%d lap=%d",
	    c->car_id, (int)split_time, (int)lap_time);

	/* Build the transformed 0x3b broadcast. Body:
	 *   u16 car_id + u32 split_time + u8 flag + u32 lap_time +
	 *   u16 flags. */
	bb_init(&out);
	if (wr_u8(&out, SRV_SECTOR_SPLIT_RELAY) < 0 ||
	    wr_u16(&out, (uint16_t)c->car_id) < 0 ||
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
	char *sender = NULL, *text = NULL;
	int handled;
	struct ByteBuf out;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0)
		return -1;
	if (rd_str_a(&r, &sender) < 0 ||
	    rd_str_a(&r, &text) < 0) {
		log_warn("h_chat: short body from conn=%u",
		    (unsigned)c->conn_id);
		free(sender);
		free(text);
		return 0;
	}
	log_info("CHAT %s: %s", sender, text);
	/*
	 * Sanitize against printf format specifiers: the binary
	 * refuses messages containing "%%".  We don't use printf
	 * on the text but the client might.
	 */
	if (text != NULL && strstr(text, "%%") != NULL) {
		log_warn("h_chat: dropping message with format "
		    "specifier from conn=%u", (unsigned)c->conn_id);
		goto out;
	}
	handled = chat_process(s, c, text);
	if (handled == 0) {
		/*
		 * Regular chat: broadcast a 0x2b to every other
		 * client.  Body: sender name + text + i32 chat
		 * type sub id + u8 = chat type 4 (system info)
		 * per §5.6.4a.
		 */
		bb_init(&out);
		if (wr_u8(&out, SRV_CHAT_OR_STATE) == 0 &&
		    wr_str_a(&out, sender) == 0 &&
		    wr_str_a(&out, text) == 0 &&
		    wr_i32(&out, 0) == 0 &&
		    wr_u8(&out, 4) == 0)
			(void)bcast_all(s, out.data, out.wpos,
			    c->conn_id);
		bb_free(&out);
	}
out:
	free(sender);
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

		if (location == 2 && car->rt.has_data) {
			float vx = car->rt.vec_c[0];
			float vy = car->rt.vec_c[1];
			float vz = car->rt.vec_c[2];
			float speed = sqrtf(vx * vx + vy * vy + vz * vz);

			if (speed > 22.5f && !race->pit_crossing_latched) {
				log_info("PITLANE SPEEDING for car #%d "
				    "speed=%.1f m/s", car->race_number,
				    speed);
				penalty_enqueue(s, c->car_id, PEN_DT, 0);
				race->pit_crossing_latched = 1;
			}
		}
		if (location == 1)
			race->pit_crossing_latched = 0;
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
	if (c->car_id < 0) {
		log_warn("received ACP_OUT_OF_TRACK, but no car %d found",
		    c->car_id);
		return 0;
	}
	log_info("out-of-track: car=%d force=%u ts=%d",
	    c->car_id, (unsigned)force, (int)ts_raw);

	bb_init(&out);
	if (wr_u8(&out, SRV_OUT_OF_TRACK_RELAY) < 0 ||
	    wr_u16(&out, (uint16_t)c->car_id) < 0 ||
	    wr_u16(&out, 0) < 0 ||
	    wr_u32(&out, (uint32_t)ts_raw) < 0)
		goto out;
	rc = bcast_all(s, out.data, out.wpos, c->conn_id);
	(void)rc;
out:
	bb_free(&out);
	return 0;
}

/* ----- 0x41 ACP_REPORT_PENALTY ---------------------------------- */

int
h_report_penalty(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, force, ptype;
	uint64_t ts_raw;
	int32_t value;

	(void)s;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u8(&r, &force) < 0 ||
	    rd_u8(&r, &ptype) < 0 ||
	    rd_u64(&r, &ts_raw) < 0 ||
	    rd_i32(&r, &value) < 0) {
		log_warn("h_report_penalty: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	log_info("report penalty: conn=%u force=%u type=%u ts=%llu value=%d",
	    (unsigned)c->conn_id, (unsigned)force, (unsigned)ptype,
	    (unsigned long long)ts_raw, (int)value);
	/* TODO: store in per-car penalty queue. */
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

	(void)s;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u64(&r, &ts_raw) < 0) {
		log_warn("h_lap_tick: short body from conn=%u",
		    (unsigned)c->conn_id);
		return 0;
	}
	log_info("lap tick: conn=%u ts=%llu",
	    (unsigned)c->conn_id, (unsigned long long)ts_raw);
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
	if (c->car_id < 0)
		return 0;
	log_info("damage zones: car=%d [%u,%u,%u,%u,%u]",
	    c->car_id, zones[0], zones[1], zones[2], zones[3], zones[4]);

	bb_init(&out);
	if (wr_u8(&out, SRV_DAMAGE_ZONES_RELAY) < 0 ||
	    wr_u16(&out, (uint16_t)c->car_id) < 0)
		goto out;
	for (i = 0; i < 5; i++)
		if (wr_u8(&out, zones[i]) < 0)
			goto out;
	rc = bcast_all(s, out.data, out.wpos, c->conn_id);
	log_info("Updated %d clients with new damage zones for car %d",
	    rc, c->car_id);
out:
	bb_free(&out);
	return 0;
}

/* ----- 0x46 ACP_CAR_DIRT_UPDATE -> broadcast 0x46 --------------- */

int
h_car_dirt(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, dirt[5];
	int i;
	struct ByteBuf out;
	int rc;

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
	if (c->car_id < 0)
		return 0;
	log_info("car dirt: car=%d [%u,%u,%u,%u,%u]",
	    c->car_id, dirt[0], dirt[1], dirt[2], dirt[3], dirt[4]);

	bb_init(&out);
	if (wr_u8(&out, SRV_CAR_DIRT_RELAY) < 0 ||
	    wr_u16(&out, (uint16_t)c->car_id) < 0)
		goto out;
	for (i = 0; i < 5; i++)
		if (wr_u8(&out, dirt[i]) < 0)
			goto out;
	rc = bcast_all(s, out.data, out.wpos, c->conn_id);
	(void)rc;
out:
	bb_free(&out);
	return 0;
}

/* ----- swap state broadcast helper ------------------------------- */

/*
 * Build and broadcast 0x47 SRV_DRIVER_SWAP_STATE_BCAST to all
 * connections.  Body: u8 msg_id + u16 car_id + u8 driver_count +
 * driver_count x u8 swap_state.
 */
static void
broadcast_swap_state(struct Server *s, struct CarEntry *car)
{
	struct ByteBuf bb;
	int i;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_DRIVER_SWAP_STATE_BCAST) < 0 ||
	    wr_u16(&bb, car->car_id) < 0 ||
	    wr_u8(&bb, car->driver_count) < 0)
		goto done;
	for (i = 0; i < car->driver_count; i++)
		if (wr_u8(&bb, car->swap_state[i]) < 0)
			goto done;
	(void)bcast_all(s, bb.data, bb.wpos, 0xFFFF);
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
	int i;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 || rd_u16(&r, &car_id) < 0) {
		log_warn("h_update_driver_swap_state: short body");
		return 0;
	}
	if (check_car_owner(c, car_id) < 0) {
		log_warn("Received ACP_UPDATE_DRIVER_SWAP_STATE for alien "
		    "car: %u (receiver car %d, connection %u)",
		    (unsigned)car_id, c->car_id, (unsigned)c->conn_id);
		return 0;
	}
	car = &s->cars[car_id];
	if (rd_u8(&r, &dcnt) < 0)
		return 0;
	if (dcnt > car->driver_count)
		dcnt = car->driver_count;
	for (i = 0; i < dcnt; i++) {
		uint8_t st;
		if (rd_u8(&r, &st) < 0)
			break;
		car->swap_state[i] = st;
	}
	log_info("driver swap state update: car=%u states=[%u,%u,%u,%u]",
	    (unsigned)car_id,
	    (unsigned)car->swap_state[0], (unsigned)car->swap_state[1],
	    (unsigned)car->swap_state[2], (unsigned)car->swap_state[3]);
	broadcast_swap_state(s, car);
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
	if (c->car_id < 0) {
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

	car = &s->cars[car_id];

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

	/* Commit the swap. */
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
		(void)bcast_all(s, out.data, out.wpos, 0xFFFF);
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
	car = &s->cars[car_id];

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
		 * Confirm: bump any slot at state 3 (REQ_PENDING)
		 * back to 2 (FOREIGN), then apply conn_state.
		 */
		for (i = 0; i < car->driver_count; i++)
			if (car->swap_state[i] == 3)
				car->swap_state[i] = 2;
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
	broadcast_swap_state(s, car);
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

	(void)s;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u8(&r, &force) < 0 ||
	    rd_u64(&r, &ts_raw) < 0) {
		log_warn("h_driver_stint_reset: short body");
		return 0;
	}
	log_info("Receives driver stint reset for car %d", c->car_id);
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
		/* Phase 3 just logs; nothing persisted to disk yet. */
	}
	(void)s;
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
		s->cars[c->car_id].race.mandatory_pit_served = 1;
		penalty_serve_front(s, c->car_id);
	}
	log_info("Served Mandatory Pitstop: %u", (unsigned)car_id);
	return 0;
}

/* ----- 0x55 ACP_LOAD_SETUP -> reply 0x56 ------------------------ */

int
h_load_setup(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id, setup_index;
	uint16_t car_id;
	uint32_t revision;
	struct ByteBuf out;

	(void)s;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u8(&r, &setup_index) < 0 ||
	    rd_u16(&r, &car_id) < 0 ||
	    rd_u32(&r, &revision) < 0) {
		log_warn("h_load_setup: short body");
		return 0;
	}
	log_info("load setup: conn=%u car=%u index=%u rev=%u",
	    (unsigned)c->conn_id, (unsigned)car_id,
	    (unsigned)setup_index, (unsigned)revision);

	/*
	 * Phase 11 minimum-viable 0x56 reply: setup_index + carId
	 * + lap_count = 0 + an empty per-car leaderboard record
	 * stub.  We don't actually have a setup file library on
	 * disk yet so the response is empty.
	 */
	bb_init(&out);
	if (wr_u8(&out, SRV_SETUP_DATA_RESPONSE) < 0 ||
	    wr_u8(&out, setup_index) < 0 ||
	    wr_u16(&out, car_id) < 0 ||
	    wr_i16(&out, 0) < 0)
		goto done;
	(void)bcast_send_one(c, out.data, out.wpos);
done:
	bb_free(&out);
	return 0;
}

/* ----- 0x5b ACP_CTRL_INFO --------------------------------------- */

int
h_ctrl_info(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	(void)s;
	log_info("ctrl info: conn=%u len=%zu (TODO)",
	    (unsigned)c->conn_id, len);
	(void)body;
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
	 * Drop outdated packets: the server tracks a monotonically
	 * increasing client timestamp and discards anything not
	 * strictly newer than the last one seen.
	 */
	if (rt->has_data && client_ts_ms <= rt->last_timestamp_ms) {
		log_info("Dropped outdated car_update paket for carId %d,"
		    " clientTimestamp %u vs lastTimeStamp %u",
		    c->car_id, (unsigned)client_ts_ms,
		    (unsigned)rt->last_timestamp_ms);
		return 0;
	}

	rt->packet_seq = seq;
	rt->client_timestamp_ms = client_ts_ms;
	rt->last_timestamp_ms = client_ts_ms;

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
	return 0;
}

/* ----- UDP 0x22 CAR_INFO_REQUEST -> reply 0x23 ------------------ */

/*
 * Build a per-car entry list record for the welcome trailer or
 * for the 0x23 CAR_INFO_RESPONSE.  The 24-byte layout from
 * §5.6.4c.  Returns 0 on success.
 */
static int
build_per_car_record(struct ByteBuf *bb, struct CarEntry *car)
{
	if (wr_u8(bb, car->car_model) < 0 ||
	    wr_u8(bb, car->cup_category) < 0 ||
	    wr_u8(bb, (uint8_t)(car->driver_count ? car->driver_count : 1)) < 0 ||
	    wr_u32(bb, (uint32_t)car->race_number) < 0 ||
	    wr_u16(bb, car->nationality) < 0 ||
	    wr_u32(bb, (uint32_t)car->car_id) < 0 ||
	    wr_u32(bb, (uint32_t)(car->default_grid_position < 0 ?
		0 : car->default_grid_position)) < 0 ||
	    wr_u8(bb, car->ballast_kg) < 0 ||
	    wr_u8(bb, car->current_driver_index) < 0 ||
	    wr_u32(bb, 0) < 0 ||
	    wr_u8(bb, 0) < 0)
		return -1;
	return 0;
}

int
h_udp_car_info_request(struct Server *s,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint16_t conn_id, car_index;
	struct Conn *requester;
	struct ByteBuf out;

	rd_init(&r, body, len);
	if (rd_u8(&r, &msg_id) < 0 ||
	    rd_u16(&r, &conn_id) < 0 ||
	    rd_u16(&r, &car_index) < 0) {
		log_warn("h_udp_car_info_request: short body");
		return 0;
	}
	log_info("Connection %u asks for carInfo %u",
	    (unsigned)conn_id, (unsigned)car_index);

	requester = server_find_conn(s, conn_id);
	if (requester == NULL) {
		log_warn("car info request from unknown conn %u",
		    (unsigned)conn_id);
		return 0;
	}
	if (car_index >= ACC_MAX_CARS || !s->cars[car_index].used) {
		log_warn("car info request for unknown car %u",
		    (unsigned)car_index);
		return 0;
	}

	bb_init(&out);
	if (wr_u8(&out, SRV_CAR_INFO_RESPONSE) < 0)
		goto done;
	if (build_per_car_record(&out, &s->cars[car_index]) < 0)
		goto done;
	bcast_send_one(requester, out.data, out.wpos);
	log_info("Car Info Response sent carId=%u to conn=%u",
	    (unsigned)car_index, (unsigned)conn_id);
done:
	bb_free(&out);
	return 0;
}
