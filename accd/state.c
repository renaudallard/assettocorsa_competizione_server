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
 * state.c -- per-connection and server state lifecycle.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bcast.h"
#include "ratings.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "session.h"
#include "state.h"

/*
 * Per-track formation / green trigger ranges, transcribed from exe
 * FUN_14012c510.  The exe seeds ServerConfiguration +0x14106 /
 * +0xa0834 / +0x14107 from this table at startup, overriding the ctor
 * zeros, and the same values then flow through FUN_14012f270 into
 * SessionManager +0x288 / +0x28c / +0x290 at every race session_start.
 *
 * formation_start: normalised track pos where FUN_14012f300 stamps
 *                  ts[2] the first time the leader is inside
 *                  [formation_start, green_start - 0.05].
 * green_start / green_end: range that triggers ts[3] — on most tracks
 *                  this straddles the start/finish line (values near
 *                  1.0 or wrapping to low 0.0x on tracks whose s/f
 *                  sits a few metres past the last corner).
 *
 * Tracks not in this table keep the Server-ctor defaults
 * (0.80 / 0.89 / 0.96), which are the monza-ish values the memory
 * note used to call universal.  Event.json overrides run after this
 * lookup and still win.
 */
struct TrackZones {
	const char *name;
	float formation_start;
	float green_start;
	float green_end;
};

/*
 * Values transcribed from FUN_14012c510 in accServer.exe.  Each
 * float matches the IEEE-754 hex literal the exe stores for that
 * track; tools that bit-compare the wire payload against a Kunos
 * reference will see byte parity.  Order follows the exe table.
 *
 * Earlier revisions of this table conflated several adjacent
 * entries (hungaroring used the nurburgring values; oval used
 * paul_ricard_gt4; red_bull_ring used nurburgring_24h) and three
 * tracks were missing.  Aligning fixes those.
 */
static const struct TrackZones track_zones[] = {
	{"monza",           0.8000000f, 0.9915550f, 0.9999990f},
	{"brands_hatch",    0.7299440f, 0.9765050f, 0.9999990f},
	{"misano",          0.7499020f, 0.9700000f, 0.9899990f},
	{"paul_ricard",     0.7849070f, 0.9914120f, 0.9999990f},
	{"zolder",          0.7776030f, 0.9916960f, 0.9999990f},
	{"silverstone",     0.7973160f, 0.9940350f, 0.9999990f},
	{"hungaroring",     0.7578200f, 0.0000000f, 0.0079990f},
	{"nurburgring",     0.7885620f, 0.9800000f, 0.9999990f},
	{"barcelona",       0.7669800f, 0.9838080f, 0.9999990f},
	{"zandvoort",       0.6926740f, 0.9752370f, 0.9849750f},
	{"imola",           0.7824390f, 0.0150000f, 0.0340000f},
	{"cota",            0.8814730f, 0.0454040f, 0.0605880f},
	{"indianapolis",    0.6999490f, 0.9571550f, 0.9922090f},
	{"watkins_glen",    0.7810060f, 0.9706860f, 0.9935680f},
	{"valencia",        0.7477980f, 0.9800000f, 0.9999990f},
	{"oval",            0.8600000f, 0.9709720f, 0.9900000f},
	{"paul_ricard_gt4", 0.7849070f, 0.9914120f, 0.9999990f},
	{"kyalami",         0.7251550f, 0.9999990f, 0.0173290f},
	{"mount_panorama",  0.8559040f, 0.0100000f, 0.0204910f},
	{"suzuka",          0.7824390f, 0.9856350f, 0.9999990f},
	{"laguna_seca",     0.6331840f, 0.9721280f, 0.9999990f},
	{"oulton_park",     0.7757550f, 0.9866660f, 0.9999990f},
	{"snetterton",      0.7477380f, 0.9866660f, 0.9999990f},
	{"donington",       0.7824390f, 0.0143790f, 0.0240000f},
	{"red_bull_ring",   0.7749770f, 0.0000000f, 0.0192060f},
	{"nurburgring_24h", 0.9434080f, 0.9933010f, 0.9999990f},
	/*
	 * "spa" is the 27th entry the per-track audit treated as a
	 * placeholder.  It is actually Spa-Francorchamps, with the
	 * 3-char track name stored at DAT_14016b6f8 in the exe.
	 * Verified against accd/tmp/capture2/spa_session.pcapng,
	 * whose CircuitInfo bytes decode to
	 * 0.9048780 / 0.1000000 / 0.1155060.
	 */
	{"spa",             0.9048780f, 0.1000000f, 0.1155060f},
};

void
track_zones_apply(struct Server *s)
{
	size_t i;

	for (i = 0; i < sizeof(track_zones) / sizeof(track_zones[0]); i++) {
		if (strcmp(s->track, track_zones[i].name) == 0) {
			s->formation_trigger_start =
			    track_zones[i].formation_start;
			s->green_trigger_start = track_zones[i].green_start;
			s->green_trigger_end = track_zones[i].green_end;
			log_info("track zones: %s formation=%.4f "
			    "green=[%.4f, %.4f]", s->track,
			    (double)s->formation_trigger_start,
			    (double)s->green_trigger_start,
			    (double)s->green_trigger_end);
			return;
		}
	}
	log_info("track zones: %s not in per-track table — using "
	    "defaults %.3f / %.3f / %.3f", s->track,
	    (double)s->formation_trigger_start,
	    (double)s->green_trigger_start,
	    (double)s->green_trigger_end);
}

void
server_init(struct Server *s)
{
	memset(s, 0, sizeof(*s));
	s->tcp_fd = -1;
	s->udp_fd = -1;
	s->lan_fd = -1;
	s->allow_auto_dq = 1;
	s->use_async_leaderboard = 0;
	s->unsafe_rejoin = 1;
	s->legacy_netcode = 1;
	s->formation_trigger_start = 0.80f;
	s->green_trigger_start = 0.89f;
	s->green_trigger_end = 0.96f;
	s->formation_lap_type = 3;
	s->short_formation_lap = 0;
	s->write_latency_dumps = 0;
	s->do_driver_swap_broadcast = 1;
	s->session_overtime_s = 120;
	s->post_qualy_s = 10;
	s->post_race_s = 15;
	s->config_version = 0;
	/* eventRules.json defaults — match the handbook III.2.4 spec. */
	s->qualify_standing_type = 1;	/* superpole */
	s->pit_window_length_s = -1;	/* unset sentinel */
	s->max_total_driving_time_s = -1;
	s->max_drivers_count = 1;
	s->refuelling_allowed = 1;
	s->refuelling_time_fixed = 0;
	s->pit_refuelling_required = 0;
	s->pit_tyre_change_required = 0;
	s->tyre_set_count = 1;
	snprintf(s->car_group, sizeof(s->car_group), "FreeForAll");
	lobby_init(&s->lobby);
	for (int i = 0; i < ACC_MAX_CARS; i++) {
		s->cars[i].car_id = (uint16_t)(ACC_CAR_ID_BASE + i);
		s->cars[i].last_elo = 0xff;	/* unrated sentinel */
		car_runtime_reset_gate(&s->cars[i].rt);
	}
	/*
	 * Anchor the 0x4e debounce to server start so the periodic
	 * gate measures elapsed-since-startup rather than absolute
	 * monotonic ms (which would fire on the first tick because
	 * mono_ms - 0 >= 81000 is trivially true at boot).
	 */
	s->ratings_last_emit_ms = mono_ms();
}

void
car_runtime_reset_gate(struct CarRuntime *rt)
{
	rt->has_data = 0;
	rt->last_timestamp_ms = 0;
	rt->client_timestamp_ms = 0;
	rt->packet_seq = 0;
	rt->last_src_conn_id = 0xffff;
}

void
server_free(struct Server *s)
{
	for (int i = 0; i < ACC_MAX_CARS; i++) {
		if (s->conns[i] != NULL) {
			conn_drop(s, s->conns[i]);
			s->conns[i] = NULL;
		}
	}
	if (s->latency_dump_fp != NULL) {
		fclose((FILE *)s->latency_dump_fp);
		s->latency_dump_fp = NULL;
	}
}

struct Conn *
conn_new(struct Server *s, int fd, const struct sockaddr_in *peer)
{
	int slot;
	struct Conn *c;

	for (slot = 0; slot < s->max_connections && slot < ACC_MAX_CARS; slot++) {
		if (s->conns[slot] == NULL)
			break;
	}
	if (slot >= s->max_connections || slot >= ACC_MAX_CARS)
		return NULL;

	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->accepted_mono_ms = mono_ms();
	c->fd = fd;
	c->peer = *peer;
	c->state = CONN_UNAUTH;
	c->conn_id = (uint16_t)slot;
	c->car_id = -1;
	c->hs_echo = NULL;
	c->hs_echo_len = 0;
	bb_init(&c->rx);
	bb_init(&c->tx);

	s->conns[slot] = c;
	s->nconns++;
	return c;
}

void
conn_drop(struct Server *s, struct Conn *c)
{
	if (c == NULL)
		return;

	/*
	 * If the connection was authenticated, send a 0x4e rating
	 * summary to the disconnecting client, then emit a 0x24
	 * disconnect notify to every remaining connected client.
	 */
	if (c->state == CONN_AUTH && c->car_id >= 0 &&
	    c->car_id < ACC_MAX_CARS) {
		struct ByteBuf bb;
		struct DriverInfo *drv;

		/*
		 * 0x4e rating summary to the disconnecting client.
		 * See handshake.c welcome path for the per-entry
		 * field layout (must match byte-for-byte).
		 */
		drv = &s->cars[c->car_id].drivers[0];
		{
			uint16_t sa = 5000, tr = 5000;
			ratings_get(s, drv->steam_id, &sa, &tr);
			bb_init(&bb);
			/*
			 * Per-entry wire shape verified against AC2 reader
			 * 143526030.c:786-832 (case 0x4e): u16 car_id, u8 0,
			 * u16 sa, u16 tr, u32 (discarded by AC2),
			 * kson_string steam_id.  The discard u32 is
			 * consumed verbatim by the two i16(-1) below
			 * (bit pattern 0xFFFFFFFF), so AC2 sees the
			 * canonical 11-byte pre-string layout.  Earlier
			 * code added a separate u32 0xFFFFFFFF after the
			 * sentinels which made AC2 parse our u32 as the
			 * str_a length-prefix and over-read 1020 bytes
			 * past the steam_id.
			 */
			if (wr_u8(&bb, SRV_RATING_SUMMARY) == 0 &&
			    wr_u8(&bb, 1) == 0 &&
			    wr_u16(&bb, s->cars[c->car_id].car_id) == 0 &&
			    wr_u8(&bb, 0) == 0 &&
			    wr_u16(&bb, sa) == 0 &&
			    wr_u16(&bb, tr) == 0 &&
			    wr_i16(&bb, -1) == 0 &&
			    wr_i16(&bb, -1) == 0 &&
			    wr_str_a(&bb, drv->steam_id) == 0)
				(void)conn_send_framed(c, bb.data, bb.wpos);
			bb_free(&bb);
		}

		/* 0x24 disconnect notify to all other clients. */
		bb_init(&bb);
		if (wr_u8(&bb, SRV_CAR_DISCONNECT_NOTIFY) == 0 &&
		    wr_u16(&bb, s->cars[c->car_id].car_id) == 0)
			(void)bcast_all(s, bb.data, bb.wpos, c->conn_id);
		bb_free(&bb);
		log_info("Sent car %d disco to %d clients",
		    c->car_id, s->nconns - 1);

		/*
		 * ignorePrematureDisconnects (handbook III.2.2): when 0
		 * (default) penalize a driver who quits the race before
		 * it ends.  Apply a TR rating delta only — SA stays
		 * untouched since pulling the plug isn't an unsafe
		 * driving event per se.  Skip when the setting is 1, in
		 * P/Q sessions, or if we're already past the active
		 * phase (COMPLETED / RESULTS — race already over).
		 */
		if (!s->ignore_premature_disconnects &&
		    session_is_race(s) &&
		    (s->session.phase == PHASE_SESSION ||
		     s->session.phase == PHASE_OVERTIME) &&
		    drv->steam_id[0] != '\0') {
			/*
			 * No public ratings_apply_delta; reuse
			 * ratings_on_race_end's "0% completion" path
			 * which applies a -30 TR delta when the
			 * disqualified flag is set.  We don't actually
			 * DSQ the slot — the delta is what matters here.
			 */
			ratings_on_race_end(s, drv->steam_id, 0, 1);
			log_info("premature disconnect: %s, TR penalty applied",
			    drv->steam_id);
		}
	}

	log_debug("conn_drop: conn=%u fd=%d car=%d state=%d",
	    (unsigned)c->conn_id, c->fd, c->car_id, (int)c->state);
	/*
	 * Flush pending TX before closing so kick/ban notify + 0x24
	 * disconnect broadcast reach the wire.  EAGAIN is accepted as
	 * "best effort" — we don't block on a slow client since we're
	 * already tearing down the connection.
	 */
	if (c->fd >= 0) {
		(void)conn_drain_tx(c);
		close(c->fd);
	}
	bb_free(&c->rx);
	bb_free(&c->tx);
	free(c->hs_echo);
	if (c->conn_id < ACC_MAX_CARS && s->conns[c->conn_id] == c) {
		s->conns[c->conn_id] = NULL;
		s->nconns--;
	}
	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
		/*
		 * Flush any in-progress stint into driver_stint_ms
		 * before the slot goes idle.  Without this a driver
		 * who disconnects mid-lap keeps stint_start_ms latched
		 * and the stint counter keeps accruing wall-clock until
		 * session end — if they reconnect the reclaim path
		 * would not reset stint_start_ms (the guard short-
		 * circuits a second stint_start_tracking) and the
		 * stint-violation check would over-count their total.
		 */
		stint_stop_tracking(s, c->car_id);
		/*
		 * Clear pit_entry_ms so a reconnecting driver who was
		 * inside the pit box at disconnect doesn't auto-serve
		 * an SG penalty via a huge `now - pit_entry_ms` dwell
		 * on the first post-reclaim pit-exit event.  DT still
		 * serves on any exit (required_s == 0).
		 */
		s->cars[c->car_id].race.pit_entry_ms = 0;
		/*
		 * Mark the car slot as unused so it can be
		 * reallocated, but preserve the race state
		 * (lap times, position) so the car stays in
		 * the leaderboard session-best counters.
		 */
		s->cars[c->car_id].used = 0;
		car_runtime_reset_gate(&s->cars[c->car_id].rt);
		c->car_id = -1;
		session_recompute_standings(s);
		s->session.standings_seq++;
	}
	free(c);
	{
		int j, n = 0;
		for (j = 0; j < ACC_MAX_CARS; j++)
			if (s->cars[j].used)
				n++;
		lobby_notify_drivers_changed(&s->lobby, (uint8_t)n);
	}
}

struct Conn *
server_find_conn(struct Server *s, uint16_t conn_id)
{
	if (conn_id >= ACC_MAX_CARS)
		return NULL;
	return s->conns[conn_id];
}

int
server_alloc_car(struct Server *s)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		if (!s->cars[i].used) {
			s->cars[i].used = 1;
			s->cars[i].car_id = (uint16_t)(ACC_CAR_ID_BASE + i);
			s->cars[i].last_elo = 0xff;
			return i;
		}
	}
	return -1;
}

/*
 * Active grid-position assignment per FUN_140021090.  Scan every
 * used car, find the maximum assigned grid number, and return
 * max+1 if it still fits under max_connections.  Otherwise walk
 * downward looking for an unoccupied slot; return -1 if the grid
 * is full.
 *
 * Entrylist.json may pre-assign a grid position via
 * `defaultGridPosition`.  If so the caller should honor that
 * instead of calling this helper.
 */
int
server_find_grid_slot(struct Server *s)
{
	int i, max_pits, used_count, max_assigned;
	uint8_t occupied[ACC_MAX_CARS];

	max_pits = s->max_connections > 0 &&
	    s->max_connections <= ACC_MAX_CARS
	    ? s->max_connections : ACC_MAX_CARS;
	max_assigned = -1;
	used_count = 0;
	for (i = 0; i < ACC_MAX_CARS; i++)
		occupied[i] = 0;
	for (i = 0; i < ACC_MAX_CARS; i++) {
		int g;
		if (!s->cars[i].used)
			continue;
		used_count++;
		g = s->cars[i].race.grid_position;
		if (g >= 0 && g < ACC_MAX_CARS) {
			occupied[g] = 1;
			if (g > max_assigned)
				max_assigned = g;
		}
	}
	if (max_assigned + 1 < max_pits)
		return max_assigned + 1;
	/* Fall back: walk down from max_pits-1 looking for unoccupied. */
	for (i = max_pits - 1; i >= 0; i--)
		if (!occupied[i])
			return i;
	(void)used_count;
	return -1;
}

int
server_alloc_race_number(struct Server *s, int my_slot, int requested)
{
	int i, n, off;

	/*
	 * Try requested, requested+1, ..., requested+9.  The exe
	 * skips uVar34 <= 0 in this offset loop (signed comparison),
	 * so a request of 0 lands on offset 1 first, and a -1
	 * request (no preference from the client) skips offsets 0
	 * and 1 and tries 1 first.
	 */
	for (off = 0; off < 10; off++) {
		int cand = requested + off;
		int taken = 0;

		if (cand <= 0)
			continue;
		for (i = 0; i < ACC_MAX_CARS; i++) {
			const struct CarEntry *ec = &s->cars[i];

			if (i == my_slot)
				continue;
			if (!ec->used && ec->driver_count == 0)
				continue;
			if (ec->race_number == cand) {
				taken = 1;
				break;
			}
		}
		if (!taken)
			return cand;
	}

	/* Fallback: smallest free in 1..999. */
	for (n = 1; n < 1000; n++) {
		int taken = 0;

		for (i = 0; i < ACC_MAX_CARS; i++) {
			const struct CarEntry *ec = &s->cars[i];

			if (i == my_slot)
				continue;
			if (!ec->used && ec->driver_count == 0)
				continue;
			if (ec->race_number == n) {
				taken = 1;
				break;
			}
		}
		if (!taken) {
			log_info("Used fallback race number %d", n);
			return n;
		}
	}

	log_info("Server ran out of racing numbers to use, "
	    "defaulting to 999");
	return 999;
}
