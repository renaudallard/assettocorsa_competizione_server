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
#include "handlers.h"
#include "io.h"
#include "lobby.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "results.h"
#include "session.h"
#include "smpr.h"
#include "state.h"
#include "tick.h"

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
	int pit_count;		/* public pit count, exe FUN_14012c270 param_2 */
	int pit_count_priv;	/* private/CP pit count, exe FUN_14012c270 param_3 */
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
 *
 * FUN_14012c510 builds 27 TrackEntry objects by calling FUN_14012c270.
 * param_2 -> +0x20 (public pit count, written to ServerConfiguration +0xa0808).
 * param_3 -> +0x24 (private pit count, written to ServerConfiguration +0xa080c).
 * FUN_1400214b0 selects +0xa080c when registerToLobby=0 or isCPServer/isCPInvServer;
 * otherwise uses +0xa0808 (public).
 */
static const struct TrackZones track_zones[] = {
	/* name           formation_start  green_start  green_end   pub priv */
	{"monza",           0.8000000f, 0.9915550f, 0.9999990f,  29,  60},
	{"brands_hatch",    0.7299440f, 0.9765050f, 0.9999990f,  32,  50},
	{"misano",          0.7499020f, 0.9700000f, 0.9899990f,  30,  50},
	{"paul_ricard",     0.7849070f, 0.9914120f, 0.9999990f,  33,  80},
	{"zolder",          0.7776030f, 0.9916960f, 0.9999990f,  34,  50},
	{"silverstone",     0.7973160f, 0.9940350f, 0.9999990f,  36,  60},
	{"hungaroring",     0.7578200f, 0.0000000f, 0.0079990f,  27,  50},
	{"nurburgring",     0.7885620f, 0.9800000f, 0.9999990f,  30,  50},
	{"barcelona",       0.7669800f, 0.9838080f, 0.9999990f,  29,  50},
	{"zandvoort",       0.6926740f, 0.9752370f, 0.9849750f,  25,  50},
	{"imola",           0.7824390f, 0.0150000f, 0.0340000f,  30,  50},
	{"cota",            0.8814730f, 0.0454040f, 0.0605880f,  30,  70},
	{"indianapolis",    0.6999490f, 0.9571550f, 0.9922090f,  30,  60},
	{"watkins_glen",    0.7810060f, 0.9706860f, 0.9935680f,  30,  60},
	{"valencia",        0.7477980f, 0.9800000f, 0.9999990f,  29,  60},
	{"oval",            0.8600000f, 0.9709720f, 0.9900000f,  10,   2},
	{"paul_ricard_gt4", 0.7849070f, 0.9914120f, 0.9999990f,  33,  80},
	{"kyalami",         0.7251550f, 0.9999990f, 0.0173290f,  40,  50},
	{"mount_panorama",  0.8559040f, 0.0100000f, 0.0204910f,  36,  50},
	{"suzuka",          0.7824390f, 0.9856350f, 0.9999990f,  51, 105},
	{"laguna_seca",     0.6331840f, 0.9721280f, 0.9999990f,  30,  50},
	{"oulton_park",     0.7757550f, 0.9866660f, 0.9999990f,  28,  50},
	{"snetterton",      0.7477380f, 0.9866660f, 0.9999990f,  26,  50},
	{"donington",       0.7824390f, 0.0143790f, 0.0240000f,  37,  50},
	{"red_bull_ring",   0.7749770f, 0.0000000f, 0.0192060f,  28,  50},
	{"nurburgring_24h", 0.9434080f, 0.9933010f, 0.9999990f,  50, 110},
	/*
	 * "spa" is the 27th entry — Spa-Francorchamps.  The 3-char track
	 * name is stored at DAT_14016b6f8 in the exe.  Verified via
	 * accd/tmp/capture2/spa_session.pcapng (CircuitInfo bytes decode
	 * to 0.9048780 / 0.1000000 / 0.1155060).  The exe reports 82 pit
	 * boxes (FUN_14012c510 param_2=0x52), matching the extended
	 * endurance pit complex at Spa-Francorchamps.  Private count = 82 (same).
	 */
	{"spa",             0.9048780f, 0.1000000f, 0.1155060f,  82,  82},
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

/*
 * Return the pit box count for a track.  is_private selects the private
 * path (param_3 / +0xa080c) used for registerToLobby=0, isCPServer, and
 * isCPInvServer servers (FUN_1400214b0:75-81); public servers use param_2
 * / +0xa0808.  Returns 30 (public cap) or 100 (generous private default)
 * for unknown tracks so nothing is spuriously restricted.
 */
int
track_pit_count(const char *track, int is_private)
{
	size_t i;

	for (i = 0; i < sizeof(track_zones) / sizeof(track_zones[0]); i++) {
		if (strcmp(track, track_zones[i].name) == 0)
			return is_private ? track_zones[i].pit_count_priv
			    : track_zones[i].pit_count;
	}
	return is_private ? 100 : 30;
}

/* <stdlib.h> hides the prototype under _POSIX_C_SOURCE; libc has it. */
uint32_t arc4random_uniform(uint32_t);

/*
 * Pick a random track into s->track from the exe's random pool
 * (FUN_14012e710 -> FUN_14012c510): the GT3 base + red_bull_ring +
 * nurburgring_24h + spa always; the IGT / BGT DLC sets gated by the
 * use_*_dlc_tracks flags; oval and paul_ricard_gt4 are never in the
 * pool.  The exe uses a biased rand()-based pick that can fall back to
 * monza; we use a clean uniform arc4random_uniform over the pool.
 */
void
track_random_pick(struct Server *s)
{
	/* indices into track_zones[]; oval(15) + paul_ricard_gt4(16) omitted */
	static const int base[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, /* GT3 */
		24, 25, 26				/* rbr, n24h, spa */
	};
	int pool[27];
	int n = 0;
	size_t i;
	uint32_t idx;

	for (i = 0; i < sizeof(base) / sizeof(base[0]); i++)
		pool[n++] = base[i];
	if (s->use_igt_dlc_tracks) {
		pool[n++] = 17;	/* kyalami */
		pool[n++] = 18;	/* mount_panorama */
		pool[n++] = 19;	/* suzuka */
		pool[n++] = 20;	/* laguna_seca */
	}
	if (s->use_bgt_dlc_tracks) {
		pool[n++] = 21;	/* oulton_park */
		pool[n++] = 22;	/* snetterton */
		pool[n++] = 23;	/* donington */
	}
	idx = arc4random_uniform((uint32_t)n);
	snprintf(s->track, sizeof(s->track), "%s",
	    track_zones[pool[idx]].name);
	log_info("randomize: empty server -> random track %s (pool %d)",
	    s->track, n);
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
	s->post_qualy_s = 16;
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
		s->cars[i].team_entry_id = -1;	/* standalone until
						 * entrylist_load expands a
						 * multi-driver entry. */
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
	/*
	 * Drop the per-car / per-session race_archive entries.  Each
	 * holds the prior-session leaderboard line, lap history and
	 * sector splits used for 0x56 garage replies, ratings deltas
	 * and weekend-result builders; on a long-running server they
	 * grow unboundedly across weekend wraps.  session_reset
	 * clears them on a wrap, but a clean shutdown without a wrap
	 * (e.g. SIGTERM at end of session) skipped this.
	 */
	session_archive_clear(s);
	results_laps_free(s);
	free(s->session.leaderboard_cache);
	s->session.leaderboard_cache = NULL;
	s->session.leaderboard_cache_cap = 0;
	s->session.leaderboard_cache_len = 0;
	/*
	 * Tear the lobby client down too -- it owns an rx_buf alloc
	 * that grows up to ~192 KiB and would otherwise leak on a
	 * clean shutdown.  lobby_shutdown is idempotent; safe to call
	 * even when registerToLobby was off (fd stays -1).
	 */
	lobby_shutdown(&s->lobby);
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
	{
		int k;
		for (k = 0; k < RTT_RING_SLOTS; k++)
			c->rtt_ring[k] = -1;	/* -1 = empty slot */
	}
	c->rtt_ring_idx = -1;
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
	 * Notify any attached SMPR monitors that this conn is going
	 * away.  Done before the slot reset so monitor_build_connection_
	 * entry still sees the driver names.
	 */
	if (c->state == CONN_AUTH && !c->is_smpr)
		smpr_notify_conn_changed(s, c);

	/*
	 * If the connection was authenticated, broadcast 0x24 to
	 * every remaining connected client.  Kunos pcap (2026-05-09)
	 * does NOT emit a per-disconnect 0x4e to the leaving peer;
	 * the standalone 0x4e is only the periodic refresh gated by
	 * CADENCE_RATINGS_MS in tick_run.
	 */
	if (c->state == CONN_AUTH && c->car_id >= 0 &&
	    c->car_id < ACC_MAX_CARS) {
		struct ByteBuf bb;

		/* 0x24 disconnect notify to all other clients. */
		bb_init(&bb);
		if (wr_u8(&bb, SRV_CAR_DISCONNECT_NOTIFY) == 0 &&
		    wr_u16(&bb, s->cars[c->car_id].car_id) == 0)
			(void)bcast_all(s, bb.data, bb.wpos, c->conn_id);
		bb_free(&bb);
		log_info("Sent car %d disco to %d clients",
		    c->car_id, s->nconns - 1);
	}

	log_debug("conn_drop: conn=%u fd=%d car=%d state=%d",
	    (unsigned)c->conn_id, c->fd, c->car_id, (int)c->state);
	/*
	 * accweb regex: Removing dead connection (\d+)
	 * Kunos's actual output (sampled 2026-05-14) includes a
	 * trailing `(last lastUdpPaketReceived N)` field separated
	 * from the connection id by a DOUBLE space.  N is the
	 * last_udp_client_ts (the client-side timestamp of the
	 * freshest UDP packet received from this conn) reported in
	 * the units kunos already uses for its own tracking; we
	 * mirror by emitting last_udp_client_ts directly.
	 */
	if (c->state == CONN_AUTH && !c->is_smpr)
		log_kunos("Removing dead connection %u  (last lastUdpPaketReceived %u)",
		    (unsigned)c->conn_id, (unsigned)c->last_udp_client_ts);
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
		 * Mark the car slot as unused so it can be
		 * reallocated, but preserve the race state
		 * (lap times, position) so the car stays in
		 * the leaderboard session-best counters.
		 */
		s->cars[c->car_id].used = 0;
		smpr_notify_car_changed(s, c->car_id);
		car_runtime_reset_gate(&s->cars[c->car_id].rt);
		/*
		 * Kunos's per-disconnect car-reap line (FUN_14001c300);
		 * "Purging car_id" is reserved in the exe for weekend-
		 * reset entry-list reconciliation, not a single leaver.
		 */
		log_kunos("car %d has no driving connection anymore, "
		    "will remove it", ACC_CAR_ID_BASE + c->car_id);
		/*
		 * Team-entry 0x47 fan-out at conn_drop: if the leaving
		 * driver was part of a multi-car team, kunos pcap shows
		 * a 0x47 SRV_DRIVER_SWAP_STATE_BCAST per group member
		 * so the remaining teammates see the disconnected
		 * teammate's swap_state has reverted to idle.  Standalone
		 * single-driver entries skip this — kunos doesn't emit
		 * 0x47 at peer-leave for them.  Fire BEFORE clearing
		 * c->car_id so the lookup works.
		 */
		if (s->cars[c->car_id].team_entry_id >= 0) {
			int8_t group = s->cars[c->car_id].team_entry_id;
			uint8_t d_idx = s->cars[c->car_id].current_driver_index;
			int g;
			/*
			 * FUN_140011bf0:53 initialises each driver slot to 1
			 * (ready) before scanning for a live conn match.  A
			 * disconnected slot has no match, so it gets 1.
			 * Reset the leaving driver's slot here before
			 * broadcasting so teammates see the correct state.
			 */
			if (d_idx < ACC_MAX_DRIVERS_PER_CAR)
				s->cars[c->car_id].swap_state[d_idx] = 1;
			for (g = 0; g < ACC_MAX_CARS; g++) {
				if (s->cars[g].team_entry_id == group &&
				    s->cars[g].used)
					broadcast_swap_state(s, &s->cars[g]);
			}
		}
		c->car_id = -1;
		session_recompute_standings(s);
		/*
		 * Kunos emits 0x36 on every peer-leave so back-to-back
		 * disconnects produce a cascade of decreasing-car-count
		 * frames.  The standings recompute above mutated the
		 * leaderboard payload; flag it dirty so the next tick
		 * drains the pending bit and fans the smaller payload
		 * out.  Within-one-tick latency is acceptable: the
		 * outgoing 0x24 disconnect notify already preceded this
		 * point, and consecutive disconnects coalesce into a
		 * single 0x36 if they land in the same tick window.
		 */
		leaderboard_request_emit(s);
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
 * Count cars with a driver in them (used slots).  This is the
 * "drivers present" basis for the session phase machine, matching the
 * exe which counts its car-entry vector (FUN_14002f710), not the raw
 * connection count: a carless spectator (car_id = -1) is in s->nconns
 * but owns no car, so it must not start or hold a session.
 */
int
server_used_car_count(const struct Server *s)
{
	int i, n = 0;

	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++)
		if (s->cars[i].used)
			n++;
	return n;
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
	 * The requested number comes straight off the wire.  ACC car
	 * numbers are three digits, and the fallback below allocates
	 * in 1..999, so anything outside that is treated as "no
	 * preference" (-1).  This also keeps the requested + off
	 * additions below from overflowing on a hostile value.
	 */
	if (requested < 1 || requested > 999)
		requested = -1;

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
