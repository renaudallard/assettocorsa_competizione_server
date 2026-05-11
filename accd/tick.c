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
 * tick.c -- periodic server tick.
 *
 * Drives session state advancement and the periodic broadcasts
 * documented in §5.6.4a.  Phase 2 implements the per-car state
 * fan-out: every N ticks, for every car that has received an
 * ACP_CAR_UPDATE since last tick, build a SRV_PERCAR_FAST_RATE
 * (0x1e) broadcast and send it to every other connection.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "bcast.h"
#include "chat.h"
#include "ratings.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "penalty.h"
#include "prim.h"
#include "entrylist.h"
#include "results.h"
#include "session.h"
#include "state.h"
#include "tick.h"
#include "weather.h"

/*
 * Broadcast cadences, in wall-clock milliseconds.
 *
 * The exe's main() calls CreateTimerQueueTimer(Period=3 ms), i.e. a
 * 333 Hz tick, and its cadences are all expressed as integer tick
 * counts (DAT_14014bd* in the .rdata).  Our reimpl runs one
 * poll()-driven tick per main-loop iteration — but on OpenBSD
 * `poll()` rounds short timeouts up to ~20 ms, which pins the loop
 * at ~50 Hz regardless of TICK_INTERVAL_MS.  Tick-count modulo
 * gates therefore fire at ~1/6.7 of their intended wall-clock rate.
 *
 * Fix: gate every cadence on a per-cadence `last_fired_ms` counter
 * measured against mono_ms().  Rate is now a property of the
 * cadence itself, not of the underlying tick period.
 */
#define CADENCE_KEEPALIVE_MS		1000	/* 0x14 ~1 Hz */
#define CADENCE_WEATHER_MS		5000	/* 0x37 every 5 s */
#define CADENCE_LEADERBOARD_MS		75000	/* 0x36 async-coalesce */
#define MIN_LEADERBOARD_GAP_MS		2000	/* 0x36 sync rate-limit */
#define CADENCE_RATINGS_MS		81000	/* 0x4e debounce (.rdata) */

/*
 * Write the 63-byte per-car body used by both 0x1e and each
 * 0x39 batch element.  Layout from FUN_14001a170 / FUN_14001a6a0
 * in accServer.exe:
 *
 *   u16 car_id (+0x150 in the per-car struct)
 *   u8  seq (+0x2d)
 *   i32 adjusted_timestamp (+0x3c minus per-peer offset)
 *   u16 (+0x50, typically 0)
 *   3 * 12 bytes vec_a / vec_b / vec_c (positions / rotations)
 *   4 * u8 input_a (+0x2e..+0x31)
 *   u8 (+0x32)
 *   u8 (+0x33)
 *   u16 (+0x36)
 *   u8 (+0x2c)
 *   u8 (+0x34)
 *   u8 (+0x35)
 *   4 * u8 input_b (+0x48..+0x4b)
 *   u8 (+0x4c)
 *   i16 clamped (+0x1ec)
 */
/*
 * @clock_adj: per-peer clock offset (from pong RTT computation).
 *             Subtracted from car_ts so the receiver sees
 *             timestamps in its own timebase.  Pass 0 if unknown.
 */
int
build_percar_body(struct ByteBuf *bb, struct CarEntry *car,
    struct Server *s, int32_t clock_adj)
{
	int k, ok;
	int16_t clamped;
	uint32_t adj_ts;
	uint16_t sender_rtt_ms = 0;
	int j;

	/*
	 * Per-peer timestamp adjustment matching FUN_14001a170.
	 *
	 * clock_adj = sender_pong_ts - peer_pong_ts: the delta
	 * between the two clients' game clocks as observed from
	 * their most recent pong exchanges.  Subtracting it from
	 * car_ts converts from sender timebase to peer timebase,
	 * enabling correct dead-reckoning on the receiver.
	 *
	 * This uses client-to-client timestamps only, avoiding
	 * the server clock entirely, so it works regardless of
	 * the server's monotonic clock epoch.
	 */
	adj_ts = (uint32_t)((int32_t)car->rt.client_timestamp_ms
	    - clock_adj);

	/*
	 * FUN_14001a170 emits a u16 at conn+0x50 here — the sender
	 * connection's server-measured avg RTT.  Receivers read it
	 * to render the per-car ping column on the HUD timing tower.
	 * Capture-based analysis earlier said this was 0, but that
	 * was because the capture came from a loopback test where
	 * RTT collapses to 0 before the pong smoothing kicks in;
	 * a real client session needs the live value.
	 */
	for (j = 0; j < ACC_MAX_CARS; j++) {
		struct Conn *sender = s->conns[j];
		if (sender != NULL && sender->car_id ==
		    (int)(car - s->cars)) {
			if (sender->avg_rtt_ms > 65535)
				sender_rtt_ms = 65535;
			else
				sender_rtt_ms =
				    (uint16_t)sender->avg_rtt_ms;
			break;
		}
	}

	ok = 1;
	if (wr_u16(bb, car->car_id) < 0) return -1;
	if (wr_u8(bb, car->rt.packet_seq) < 0) return -1;
	if (wr_u32(bb, adj_ts) < 0) return -1;
	if (wr_u16(bb, sender_rtt_ms) < 0) return -1;

	for (k = 0; k < 3 && ok; k++)
		ok = wr_f32(bb, car->rt.vec_a[k]) == 0;
	for (k = 0; k < 3 && ok; k++)
		ok = wr_f32(bb, car->rt.vec_b[k]) == 0;
	for (k = 0; k < 3 && ok; k++)
		ok = wr_f32(bb, car->rt.vec_c[k]) == 0;
	for (k = 0; k < 4 && ok; k++)
		ok = wr_u8(bb, car->rt.input_a[k]) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_32) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_33) == 0;
	if (ok) ok = wr_u16(bb, car->rt.scalar_36) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_2c) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_34) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_35) == 0;
	for (k = 0; k < 4 && ok; k++)
		ok = wr_u8(bb, car->rt.input_b[k]) == 0;
	if (ok) ok = wr_u8(bb, car->rt.scalar_4c) == 0;

	clamped = car->rt.scalar_1ec;
	if (ok) ok = wr_i16(bb, clamped) == 0;
	return ok ? 0 : -1;
}

/*
 * Periodic per-car fan-out.  Walks every car with rt.dirty set,
 * emits one 0x1e (or 0x39 count=1 in legacy-netcode mode) per
 * dirty car to every other authenticated peer, then clears the
 * dirty flag.  Matches FUN_14001a170 / FUN_14001a6a0 in the exe,
 * which are gated by the legacy_netcode toggle at srv+0x22 and
 * called once per scheduler tick from FUN_14002e8d0.
 *
 * Replaces the prior event-driven relay (h_udp_car_update ->
 * per-peer sendto per incoming update).  At ~18 Hz × peer_count
 * that flooded the client's UDP queue with ~520 pps per peer and
 * surfaced as position jitter + visual blinking on real clients.
 */
static void
broadcast_percar_dirty(struct Server *s)
{
	struct ByteBuf bb;
	int i, j;
	uint8_t msg_id = s->legacy_netcode
	    ? SRV_PERCAR_SLOW_RATE : SRV_PERCAR_FAST_RATE;

	if (s->udp_fd < 0)
		return;

	bb_init(&bb);
	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct CarEntry *car = &s->cars[i];
		struct Conn *sender = NULL;

		if (!car->used || !car->rt.dirty || !car->rt.has_data)
			continue;

		for (j = 0; j < ACC_MAX_CARS; j++) {
			if (s->conns[j] != NULL &&
			    s->conns[j]->car_id == i) {
				sender = s->conns[j];
				break;
			}
		}

		for (j = 0; j < ACC_MAX_CARS; j++) {
			struct Conn *peer = s->conns[j];
			int32_t delta = 0;

			if (peer == NULL || peer->state != CONN_AUTH)
				continue;
			if (peer->car_id == i)
				continue;
			/*
			 * Per-peer client-timestamp delta.  Same pivot as
			 * write_session_mgr_state (refreshed on every UDP
			 * packet, so both endpoints are typically within
				 ~55 ms of each other at 18 Hz car updates).
			 * Before either endpoint has sent any UDP packet
			 * the pivot is 0, so leave delta at 0 rather than
			 * emit a huge synthetic offset into the client's
			 * smoothing filter.
			 */
			if (sender != NULL &&
			    sender->last_udp_server_ms != 0 &&
			    peer->last_udp_server_ms != 0)
				delta = (int32_t)(sender->last_udp_client_ts -
				    peer->last_udp_client_ts);

			bb_clear(&bb);
			if (wr_u8(&bb, msg_id) < 0)
				continue;
			if (s->legacy_netcode &&
			    wr_u8(&bb, 1) < 0)
				continue;
			if (build_percar_body(&bb, car, s, delta) < 0)
				continue;
			(void)sendto(s->udp_fd, bb.data, bb.wpos, 0,
			    (const struct sockaddr *)&peer->peer,
			    sizeof(peer->peer));
		}

		car->rt.dirty = 0;
	}
	bb_free(&bb);
}

/*
 * Send a 0x14 keepalive to each authenticated connection via
 * UDP.  The exe (FUN_140029b20) sends this per-peer over UDP;
 * the client replies with 0x16 pong only to UDP keepalives.
 *
 * Body (verified against kunos_wine_full_race.pcap):
 *   u8   0x14
 *   u32  server_ms
 *   u16  this conn's avg RTT (ms)            ← per-recipient
 *   u16  server-wide average RTT (ms)
 *   u16  server-wide max RTT (ms)
 *   u8 × 4  cpu/qos hints (2/4/100/100)
 * Total 15 bytes.  The client reads u16 #1 to render its ping
 * badge; a hardcoded 0 here makes the in-game ping display read
 * 0 ms even with measurable RTT in flight.
 */
void
compute_server_pings(const struct Server *s,
    uint16_t *avg_out, uint16_t *max_out)
{
	uint32_t sum = 0;
	int count = 0;
	uint16_t max_ping = 0;
	int i;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *cc = s->conns[i];
		if (cc == NULL || cc->avg_rtt_ms == 0)
			continue;
		sum += cc->avg_rtt_ms;
		count++;
		if (cc->avg_rtt_ms > max_ping)
			max_ping = cc->avg_rtt_ms > 65535
			    ? 65535 : (uint16_t)cc->avg_rtt_ms;
	}
	*avg_out = count > 0 ? (uint16_t)(sum / count) : 0;
	*max_out = max_ping;
}

void
build_keepalive_pkt(unsigned char pkt[15], uint8_t msg_id,
    uint32_t srv_ms, uint16_t conn_rtt, uint16_t avg_ping,
    uint16_t max_ping)
{
	pkt[0]  = msg_id;
	pkt[1]  = (unsigned char)(srv_ms & 0xff);
	pkt[2]  = (unsigned char)((srv_ms >> 8) & 0xff);
	pkt[3]  = (unsigned char)((srv_ms >> 16) & 0xff);
	pkt[4]  = (unsigned char)((srv_ms >> 24) & 0xff);
	pkt[5]  = (unsigned char)(conn_rtt & 0xff);
	pkt[6]  = (unsigned char)((conn_rtt >> 8) & 0xff);
	pkt[7]  = (unsigned char)(avg_ping & 0xff);
	pkt[8]  = (unsigned char)((avg_ping >> 8) & 0xff);
	pkt[9]  = (unsigned char)(max_ping & 0xff);
	pkt[10] = (unsigned char)((max_ping >> 8) & 0xff);
	pkt[11] = 2;
	pkt[12] = 4;
	pkt[13] = 100;
	pkt[14] = 100;
}

static void
broadcast_keepalive(struct Server *s, uint8_t msg_id)
{
	unsigned char pkt[15];
	int i;
	uint32_t srv_ms;
	uint16_t avg_ping, max_ping;

	if (s->udp_fd < 0)
		return;

	compute_server_pings(s, &avg_ping, &max_ping);
	srv_ms = (uint32_t)mono_ms();

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];
		uint16_t per_conn_ping;

		if (c == NULL || c->state != CONN_AUTH)
			continue;
		/* Clamp like build_percar_body does — a stalled link can
		 * push avg_rtt_ms past 65535 and the silent cast wraps to
		 * a small value, which the HUD renders as a misleadingly
		 * low ping. */
		per_conn_ping = c->avg_rtt_ms > 65535
		    ? 65535 : (uint16_t)c->avg_rtt_ms;
		build_keepalive_pkt(pkt, msg_id, srv_ms, per_conn_ping,
		    avg_ping, max_ping);
		(void)sendto(s->udp_fd, pkt, sizeof(pkt), 0,
		    (const struct sockaddr *)&c->peer,
		    sizeof(c->peer));
	}
}

/*
 * Build and emit the SRV_LEADERBOARD_BCAST (0x36) when the
 * standings have changed.  Matches FUN_14002f710 in accServer.exe:
 * the body is `u8 0x36 + FUN_140034a40 output` (the same leaderboard
 * section embedded in the welcome trailer), so we reuse the shared
 * write_leaderboard_section helper from handshake.c instead of
 * hand-rolling a simplified record.
 */
void
broadcast_leaderboard(struct Server *s)
{
	struct ByteBuf bb;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_LEADERBOARD_BCAST) < 0)
		goto done;
	if (write_leaderboard_section(&bb, s) < 0)
		goto done;
	(void)bcast_all(s, bb.data, bb.wpos, BCAST_EXCEPT_NONE);
	log_info("Updated leaderboard for %d clients", s->nconns);
done:
	bb_free(&bb);
}

/*
 * Build and emit the SRV_GRID_POSITIONS (0x3f) at the start of
 * the RACE phase.  Body: u8 grid_count + per-car { u16 carId +
 * u8 flag_a + u32 grid_position + u8 flag_b }.
 */
static void
broadcast_grid(struct Server *s)
{
	struct ByteBuf bb;
	int i, g, n = 0, emitted = 0;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_GRID_POSITIONS) < 0)
		goto done;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++)
		if (s->cars[i].used)
			n++;
	if (wr_u8(&bb, (uint8_t)n) < 0)
		goto done;
	/*
	 * Emit in grid order — FUN_140032400 in the exe assigns grid
	 * slots and walks them in ascending order when building the
	 * 0x3f payload, and the client infers each car's starting
	 * position from the record sequence.  Cars without a valid
	 * grid slot trail in car_id order.
	 */
	for (g = 0; g <= ACC_MAX_CARS && emitted < n; g++) {
		for (i = 0; i < ACC_MAX_CARS && i < s->max_connections;
		    i++) {
			struct CarEntry *car = &s->cars[i];
			if (!car->used)
				continue;
			if (car->race.grid_position != g)
				continue;
			if (wr_u16(&bb, car->car_id) < 0 ||
			    wr_u8(&bb, 0) < 0 ||
			    wr_u32(&bb, (uint32_t)g) < 0 ||
			    wr_u8(&bb, 0) < 0)
				goto done;
			emitted++;
		}
	}
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections &&
	    emitted < n; i++) {
		struct CarEntry *car = &s->cars[i];
		if (!car->used)
			continue;
		if (car->race.grid_position >= 0 &&
		    car->race.grid_position <= ACC_MAX_CARS)
			continue;
		if (wr_u16(&bb, car->car_id) < 0 ||
		    wr_u8(&bb, 0) < 0 ||
		    wr_u32(&bb, (uint32_t)i) < 0 ||
		    wr_u8(&bb, 0) < 0)
			goto done;
		emitted++;
	}
	(void)bcast_all(s, bb.data, bb.wpos, BCAST_EXCEPT_NONE);
	log_info("Sending grid positions: %d cars", n);
done:
	bb_free(&bb);
}

/*
 * Build and emit SRV_SESSION_RESULTS (0x3e) at end of session.
 *
 * Wire format per accServer.exe FUN_1400197b0 + FUN_1400351f0:
 *   u8 0x3e
 *   u8 result_count
 *   result_count × (23-byte result_header + per-car leaderboard_section)
 *
 * Per-session result header is now write_session_result_header in
 * handshake.c — kunos's 0x3e header carries session metadata (hour,
 * day-of-weekend, durations) not per-car results.  Our old port of
 * FUN_1400351f0 emitted per-car bytes that the AC2 result widget
 * never used.
 */

static void
broadcast_session_results(struct Server *s)
{
	int i, n;
	uint8_t result_count;
	struct ByteBuf bb;
	int ok = 1;
	int j, used = 0;

	if (s->session_count == 0)
		return;
	result_count = (uint8_t)(s->session.session_index + 1);
	if (result_count > s->session_count)
		result_count = s->session_count;

	for (j = 0; j < ACC_MAX_CARS; j++)
		if (s->cars[j].used)
			used++;
	if (used == 0)
		return;

	/*
	 * The body is identical for every recipient (the leader
	 * resolution and write_leaderboard_section don't depend on
	 * the destination conn).  Build it once and fan out, instead
	 * of malloc/free per auth conn.
	 */
	bb_init(&bb);
	ok = ok && wr_u8(&bb, SRV_SESSION_RESULTS) == 0;
	ok = ok && wr_u8(&bb, result_count) == 0;
	/*
	 * FUN_1400351f0 walks a per-session-results vector and
	 * emits one (header + leaderboard) pair per completed
	 * session, each describing THAT session's leading car.
	 * We used to pick the first used car for every iteration,
	 * so the end-of-session screen for sessions 1+ showed the
	 * wrong driver and times.  Resolve the session leader from
	 * the archive slot (or live state for the current
	 * session).
	 */
	for (n = 0; n < result_count && ok; n++) {
		int leader = -1;
		const struct CarRaceState *src = NULL;
		int cur = s->session.session_index;

		for (j = 0; j < ACC_MAX_CARS; j++) {
			if (!s->cars[j].used)
				continue;
			if (n == cur) {
				if (s->cars[j].race.position == 1) {
					leader = j;
					src = &s->cars[j].race;
					break;
				}
			} else if (n < cur &&
			    n < ACC_MAX_SESSIONS &&
			    s->cars[j].race_archive[n] != NULL &&
			    s->cars[j].race_archive[n]->position
				== 1) {
				leader = j;
				src = s->cars[j].race_archive[n];
				break;
			}
		}
		if (leader < 0) {
			/* Fallback: first used car, archived state
			 * if available, else live. */
			for (j = 0; j < ACC_MAX_CARS; j++)
				if (s->cars[j].used) {
					leader = j;
					break;
				}
			if (leader < 0) {
				ok = 0;
				break;
			}
			if (n < cur && n < ACC_MAX_SESSIONS &&
			    s->cars[leader].race_archive[n] != NULL)
				src = s->cars[leader]
				    .race_archive[n];
			else
				src = &s->cars[leader].race;
		}
		/*
		 * Per-session result header — session metadata, NOT
		 * per-car data.  Pcap-verified (2026-05-11 race-end
		 * test): kunos's 23 B header carries hour_of_day,
		 * dayOfWeekend-1, session-type AC2 enum, durations.
		 * Our previous write_result_header (port of
		 * FUN_1400351f0 from accServer.exe) emitted per-car
		 * data that AC2's result widget never used.
		 */
		{
			const struct SessionDef *sd =
			    (n < ACC_MAX_SESSIONS && n < s->session_count)
			    ? &s->sessions[n] : &s->sessions[0];
			ok = ok && write_session_result_header(&bb,
			    sd, s->session_overtime_s) == 0;
			/*
			 * Per-session leaderboard — pass the entry's
			 * session_type so cvar8 + pq_emit policy matches
			 * kunos (in a race-end results frame, the practice
			 * entry emits cvar8=0 even though the current
			 * server phase is the race that just completed).
			 */
			ok = ok && write_session_leaderboard_section(&bb, s,
			    sd->session_type) == 0;
		}
		(void)leader; (void)src;
	}
	if (ok) {
		for (i = 0; i < ACC_MAX_CARS; i++) {
			struct Conn *c = s->conns[i];
			if (c == NULL || c->state != CONN_AUTH)
				continue;
			(void)conn_send_framed(c, bb.data, bb.wpos);
		}
	}
	bb_free(&bb);
	log_info("Send session results to %d clients (count=%u)",
	    s->nconns, (unsigned)result_count);
}

/*
 * Optional 0xbe periodic telemetry push to 127.0.0.1:<stats_udp_port>.
 *
 * Mirrors FUN_14002e8d0 + FUN_140034c70 in accServer.exe: a 1 Hz UDP
 * datagram carrying a snapshot of the server state (weekend time, phase,
 * session manager, weather, connection and car lists).  The exe sends
 * this only when a stats port is configured (short at +0x112 in its
 * server struct); we replicate the gating via s->stats_udp_port.
 *
 * Byte layout matches the exe structurally but the two opaque internal
 * serializers (FUN_140033890 session_mgr_state, FUN_1400330e0 additional
 * state) are substituted with our canonical writers from handshake.c /
 * weather.c, which cover the same field surface as the exe's 0x28 /
 * 0x37 payloads.  Since 0xbe is localhost-only telemetry with no known
 * external consumer, exact parity with the exe is not required — any
 * monitoring tool can match our format by reading the fields in order.
 *
 * Intentionally reuses s->udp_fd (the main game UDP socket).  Kunos
 * uses a dedicated stats socket at offset +0x78 but a shared loopback
 * send is equivalent in practice.
 */
static void
broadcast_stats_udp(struct Server *s)
{
	struct ByteBuf bb, wb;
	struct sockaddr_in dst;
	int i, ok, n_conn, n_car;

	if (s->stats_udp_port <= 0 || s->stats_udp_port > 65535)
		return;
	if (s->udp_fd < 0)
		return;

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons((uint16_t)s->stats_udp_port);
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);	/* 127.0.0.1 */

	bb_init(&bb);
	bb_init(&wb);
	ok = wr_u8(&bb, SRV_PERIODIC_UDP) == 0;
	ok = ok && wr_f32(&bb, (float)s->session.weekend_time_s) == 0;
	ok = ok && wr_u8(&bb, s->session.phase) == 0;
	ok = ok && write_session_mgr_state(&bb, s, 0, 0) == 0;

	/*
	 * Weather block: reuse weather_build_broadcast (which writes the
	 * 0x37 opcode + 17 floats) into a scratch buffer and append
	 * everything after the leading opcode byte.
	 */
	if (ok && weather_build_broadcast(s, &wb) == 0 && wb.wpos > 1) {
		if (bb_append(&bb, wb.data + 1, wb.wpos - 1) < 0)
			ok = 0;
	}

	/* Connection list. */
	n_conn = 0;
	for (i = 0; i < ACC_MAX_CARS; i++)
		if (s->conns[i] != NULL &&
		    s->conns[i]->state == CONN_AUTH)
			n_conn++;
	ok = ok && wr_u8(&bb, (uint8_t)(n_conn > 255 ? 255 : n_conn)) == 0;
	for (i = 0; i < ACC_MAX_CARS && ok; i++) {
		struct Conn *c = s->conns[i];
		struct CarEntry *car;
		const char *name = "";

		if (c == NULL || c->state != CONN_AUTH)
			continue;
		car = (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) ?
		    &s->cars[c->car_id] : NULL;
		if (car != NULL && car->driver_count > 0)
			name = car->drivers[car->current_driver_index
			    < car->driver_count
			    ? car->current_driver_index : 0].last_name;
		ok = ok && wr_u16(&bb, car ? car->car_id : 0) == 0;
		ok = ok && wr_u16(&bb, car ? (uint16_t)car->race_number : 0)
		    == 0;
		ok = ok && wr_u8(&bb,
		    car ? car->current_driver_index : 0) == 0;
		ok = ok && wr_str_b(&bb, name) == 0;
		ok = ok && wr_u16(&bb, (uint16_t)c->avg_rtt_ms) == 0;
	}

	/* Car list. */
	n_car = 0;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++)
		if (s->cars[i].used)
			n_car++;
	ok = ok && wr_u8(&bb, (uint8_t)(n_car > 255 ? 255 : n_car)) == 0;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections && ok; i++) {
		if (!s->cars[i].used)
			continue;
		ok = ok && wr_u16(&bb, s->cars[i].car_id) == 0;
		ok = ok && wr_u16(&bb,
		    (uint16_t)s->cars[i].race.position) == 0;
	}

	if (ok)
		(void)sendto(s->udp_fd, bb.data, bb.wpos, 0,
		    (const struct sockaddr *)&dst, sizeof(dst));
	bb_free(&bb);
	bb_free(&wb);
}

void
tick_run(struct Server *s)
{
	uint32_t *last_standings_seq = &s->session.last_standings_seq;
	uint8_t *last_phase = &s->session.last_phase;
	/*
	 * Wall-clock cadence state.  Initialized to 0 so every gate
	 * fires on the first tick after startup (matches the prior
	 * tick-modulo behavior that always fires at tick 0).
	 */
	static uint64_t last_keepalive_ms = 0;
	uint64_t *last_leaderboard_ms = &s->session.last_leaderboard_ms;
	static uint64_t last_weather_ms = 0;
	static uint64_t last_state28_ms = 0;
	/*
	 * Tick-rate probe.  Every 60 s of wall-clock, log the observed
	 * tick rate so we can confirm the main-loop busy-wait is
	 * hitting the intended 333 Hz.  Costs one log line per minute.
	 */
	static uint64_t tickprobe_start_ms = 0;
	static uint32_t tickprobe_start_count = 0;
	uint64_t now_ms = mono_ms();

	s->tick_count++;
	if (tickprobe_start_ms == 0) {
		tickprobe_start_ms = now_ms;
		tickprobe_start_count = s->tick_count;
	} else if (now_ms - tickprobe_start_ms >= 60000) {
		uint32_t dt_ticks = s->tick_count - tickprobe_start_count;
		uint64_t dt_ms = now_ms - tickprobe_start_ms;
		log_info("tick rate: %u ticks in %llu ms = %.1f Hz",
		    (unsigned)dt_ticks,
		    (unsigned long long)dt_ms,
		    (double)dt_ticks * 1000.0 / (double)dt_ms);
		tickprobe_start_ms = now_ms;
		tickprobe_start_count = s->tick_count;
	}

	/* Drive the session phase machine. */
	session_tick(s);

	/*
	 * Race green-flag position gate (FUN_14012f4a0).  While a race
	 * session is in its formation lap, find the current leader and
	 * feed its normalized track position into the trigger check.
	 * When green fires, broadcast the "Race start initialized"
	 * system chat — the exe emits the exact same 0x2b.
	 */
	if (s->session.phase == PHASE_PRE_SESSION &&
	    s->session_count > 0 &&
	    s->sessions[s->session.session_index].session_type == 10 &&
	    !s->session.green_fired) {
		int i, leader = -1;

		/*
		 * Per-car formation-mid latch (exe FUN_1400431e0).  One-shot
		 * set when the car's normalized track position passes through
		 * [0.6, 0.7] during the formation lap.  A car that never rolls
		 * out of its paddock slot thus cannot gate the green flag even
		 * if its reported norm_pos happens to fall in the trigger
		 * window.
		 */
		for (i = 0; i < ACC_MAX_CARS; i++) {
			struct CarEntry *car = &s->cars[i];
			float pos;

			if (!car->used || !car->rt.has_data)
				continue;
			if (car->race.formation_mid_passed)
				continue;
			memcpy(&pos, &car->rt.scalar_44, sizeof(pos));
			if (pos > 0.6f && pos < 0.7f)
				car->race.formation_mid_passed = 1;
		}

		/*
		 * Leader pick — phase-split gate matching exe
		 * FUN_1400428d0:
		 *   bVar8==3 (formation phase, !formation_ended):
		 *     car+0x153 == 1 (on track) AND (car+0x1b0 & 0x20) == 0
		 *   bVar8 != 3 (post-formation, formation_ended/green_fired):
		 *     above AND car+0x204 != 0 (fmp required)
		 * The exe sorts conns and picks the first one passing the
		 * gate (FUN_14002f710:273-276); off-track / disconnected
		 * cars sort to the end and don't fire the trigger.
		 *
		 * We mirror that by walking cars in race-position order
		 * (= what the exe's sort settles on for normal racing) and
		 * stopping on the first that passes the gate, rather than
		 * hard-pinning to position 1.  Without this, an in-pit or
		 * off-track P1 in a multi-driver session blocks the
		 * formation_end trigger forever — the windows server would
		 * have already moved on to P2.
		 *
		 * Applying fmp as a blanket gate deadlocks solo races: the
		 * driver spawns inside the formation-end range (typical
		 * grid norm_pos = 0.83-0.95), the client holds brakes until
		 * phase 3 opens, phase 3 opens via formation_end firing,
		 * formation_end firing needs the gate to pass, and the gate
		 * fails on fmp which only latches when the car drives
		 * through 0.6-0.7 — which it can't because brakes are held.
		 * Kunos's 81-min solo capture shows formation_end firing the
		 * instant pre-race-waiting elapses (s2 stamps at ts[1]+1000
		 * with the car still on the grid), confirming that fmp is
		 * not checked during the formation-end trigger.
		 */
		{
			int16_t best_pos = INT16_MAX;

			for (i = 0; i < ACC_MAX_CARS; i++) {
				const struct CarEntry *car = &s->cars[i];

				if (!car->used || !car->rt.has_data)
					continue;
				if (!car->race.on_track)
					continue;
				if ((car->last_sys_data & 0x20) != 0)
					continue;
				if (s->session.formation_ended &&
				    !car->race.formation_mid_passed)
					continue;
				if (car->race.position < 1)
					continue;
				if (car->race.position < best_pos) {
					best_pos = car->race.position;
					leader = i;
				}
			}
		}
		if (leader >= 0) {
			float pos;

			memcpy(&pos, &s->cars[leader].rt.scalar_44,
			    sizeof(pos));
			if (pos >= 0.0f && pos <= 1.0f &&
			    session_advance_race_triggers(s, pos))
				chat_broadcast(s,
				    "Race start initialized", 4);
		}
	}

	/*
	 * Per-car fan-out: send 0x1e (or 0x39 in legacy-netcode mode)
	 * for every car marked dirty since the last sweep.  Matches
	 * FUN_14001a170 / FUN_14001a6a0 being called once per scheduler
	 * tick from FUN_14002e8d0.
	 */
	broadcast_percar_dirty(s);

	/*
	 * Keepalive 0x14 + 0xbe localhost telemetry + optional latency
	 * CSV row, all sharing the 1 s wall-clock cadence.  See
	 * CADENCE_KEEPALIVE_MS — driven off now_ms so the cadence is
	 * honest regardless of how many tick iterations the OS schedules
	 * per second.
	 */
	if (now_ms - last_keepalive_ms >= CADENCE_KEEPALIVE_MS) {
		broadcast_keepalive(s, SRV_KEEPALIVE_14);
		broadcast_stats_udp(s);

		if (s->write_latency_dumps &&
		    s->latency_dump_fp != NULL && s->nconns > 0) {
			FILE *fp = (FILE *)s->latency_dump_fp;
			int i;

			for (i = 0; i < ACC_MAX_CARS; i++) {
				struct Conn *c = s->conns[i];
				const char *sid = "";

				if (c == NULL || c->state != CONN_AUTH)
					continue;
				if (c->car_id >= 0 &&
				    c->car_id < ACC_MAX_CARS &&
				    s->cars[c->car_id].driver_count > 0)
					sid = s->cars[c->car_id]
					    .drivers[0].steam_id;
				fprintf(fp, "%llu,%u,%s,%u,%d\n",
				    (unsigned long long)now_ms,
				    (unsigned)c->conn_id, sid,
				    (unsigned)c->avg_rtt_ms,
				    (int)c->clock_offset_ms);
			}
			fflush(fp);
		}
		last_keepalive_ms = now_ms;
	}

	/*
	 * Leaderboard rebroadcast.  useAsyncLeaderboard (settings.json,
	 * default 0 = sync) fires on standings_seq change subject to a
	 * 2 s minimum gap.  Without the gap, accd over-emitted 3-4×
	 * compared to kunos in penalty-heavy scenarios (4-bot test,
	 * 2026-05-10): 53 broadcasts vs kunos's 14.  Kunos coalesces
	 * close penalty events into a single broadcast — the gap
	 * reproduces the same coalescing.  Async mode (=1) keeps the
	 * older 75 s coarse cadence for ops who want minimum fan-out.
	 */
	{
		int changed = s->session.standings_seq !=
		    *last_standings_seq;
		int cadence = now_ms - *last_leaderboard_ms >=
		    (s->use_async_leaderboard ? CADENCE_LEADERBOARD_MS
		                              : MIN_LEADERBOARD_GAP_MS);
		int fire = changed && cadence;

		if (fire) {
			*last_standings_seq = s->session.standings_seq;
			broadcast_leaderboard(s);
			*last_leaderboard_ms = now_ms;
		}
	}

	/*
	 * 0x28 session state broadcast.  Two triggers:
	 *
	 *   1. Phase or descriptor change (event-driven), so phase
	 *      transitions reach the client with ~3 ms latency.
	 *   2. 1 Hz cadence — even with frozen ts[] deadlines, the body
	 *      carries per-conn projected client timestamps that advance
	 *      with server time.  Without the cadence, the client's
	 *      countdown widgets stop refreshing during steady-state
	 *      session play.  Verified against a 583 s 2-bot kunos pcap
	 *      where 0x28 fired 599 times (1.03 Hz / conn) with body
	 *      bytes changing every emit.
	 */
	{
		uint8_t cur_phase = s->session.phase;
		int changed = !s->session.last_emit_valid;
		int cadence_28 = now_ms - last_state28_ms >= 1000;
		int k;

		if (!changed && cur_phase != s->session.last_emit_phase)
			changed = 1;
		for (k = 0; k < 7 && !changed; k++)
			if (s->session.ts[k] != s->session.last_emit_ts[k])
				changed = 1;

		if (changed || cadence_28) {
			if (s->nconns > 0) {
				struct ByteBuf bb;
				int i;

				/*
				 * Per-peer body (each conn gets its own
				 * clock-base projected timestamps); the
				 * scratch buffer is reused across all peers
				 * to avoid one malloc+free per conn per emit.
				 */
				bb_init(&bb);
				for (i = 0; i < ACC_MAX_CARS; i++) {
					struct Conn *c = s->conns[i];
					uint32_t client_ts_est;

					if (c == NULL ||
					    c->state != CONN_AUTH)
						continue;
					/*
					 * Extrapolate the client's clock from
					 * the freshest UDP pivot (refreshed on
					 * every pong AND car update, so at
					 * 18 Hz it's never more than ~55 ms
					 * stale).  Tracks the client's live
					 * clock regardless of client↔server
					 * tick-rate skew.
					 */
					if (c->last_udp_server_ms != 0)
						client_ts_est =
						    c->last_udp_client_ts +
						    ((uint32_t)now_ms -
						    c->last_udp_server_ms);
					else
						client_ts_est =
						    c->last_pong_client_ts;
					bb_clear(&bb);
					if (wr_u8(&bb,
					    SRV_LARGE_STATE_RESPONSE) == 0 &&
					    write_session_mgr_state(&bb, s,
						client_ts_est,
						c->avg_rtt_ms) == 0)
						(void)conn_send_framed(c,
						    bb.data, bb.wpos);
				}
				bb_free(&bb);
			}
			/*
			 * Refresh the snapshot regardless of whether we sent
			 * to anyone — keeps the change detector from
			 * re-firing forever when no clients are connected.
			 */
			s->session.last_emit_phase = cur_phase;
			for (k = 0; k < 7; k++)
				s->session.last_emit_ts[k] =
				    s->session.ts[k];
			s->session.last_emit_valid = 1;
			last_state28_ms = now_ms;
		}
	}

	/*
	 * One-shot actions on phase transitions.
	 */
	if (s->session.phase != *last_phase) {
		/*
		 * 0x3f grid positions fire once per race, at the
		 * PRE_SESSION (countdown) transition.  Per
		 * FUN_14002f710 the exe gates the emit on
		 * `(iVar11 != 0) && (phase == 0x04)`, i.e. grid
		 * results ready and phase == PRE_SESSION.  We had been
		 * firing at FORMATION which is one level too early.
		 */
		if (s->session.phase == PHASE_PRE_SESSION &&
		    s->session_count > 0 &&
		    s->sessions[s->session.session_index]
			.session_type == 10)
			broadcast_grid(s);
		if (s->session.phase == PHASE_COMPLETED) {
			/*
			 * Flush + check driver-stint violations before
			 * results serialize so any ExceededDriver
			 * StintLimit DQ shows up in the result record.
			 */
			stint_check_violations(s);
			/*
			 * Stint checks may have DQ'd cars; re-sort so
			 * 0x3e session results broadcast + results.json
			 * emit positions with DQ'd cars at the bottom.
			 */
			session_recompute_standings(s);
			/*
			 * Race-only: convert every car's unserved DT/SG
			 * to the equivalent post-race time penalty
			 * (DT->30 s, SG10->40 s, SG20->50 s, SG30->60 s),
			 * matching exe FUN_140127440 invoked from
			 * FUN_14012b380's session-over branch.  Run before
			 * broadcast_session_results / results_write so the
			 * converted-TP entries appear in both the 0x3e
			 * broadcast and the per-session results.json.
			 * Only meaningful in races; qualy / practice don't
			 * have lap-bound DT/SG penalties to convert.
			 * Gated on !results_written so we run exactly once
			 * even if PHASE_COMPLETED ticks repeat.
			 */
			if (!s->session.results_written &&
			    s->session_count > 0 &&
			    s->sessions[s->session.session_index]
				.session_type == 10) {
				int j;
				for (j = 0; j < ACC_MAX_CARS; j++)
					penalty_convert_race_end(
					    &s->cars[j].race.pen);
				/*
				 * No standings_seq bump here — kunos doesn't
				 * emit a 0x36 in PHASE_COMPLETED.  The next
				 * 0x36 fires after session_advance wraps the
				 * weekend back to session 0 (P), with an empty
				 * queue and cvar8=0; the converted entries
				 * are consumed by broadcast_session_results
				 * (0x3e) and results.json instead.
				 */
			}
			/*
			 * Update Trust rating based on race outcome.
			 * Runs only once per session-complete (before
			 * archive) so we don't inflate TR on repeated
			 * ticks.  Uses the session results written below.
			 */
			if (!s->session.results_written &&
			    s->session_count > 0 &&
			    s->sessions[s->session.session_index]
				.session_type == 10) {
				int leader_laps = 0, i;
				for (i = 0; i < ACC_MAX_CARS; i++) {
					int lc = s->cars[i].race.lap_count;
					/*
					 * Include disconnected drivers in
					 * the leader scan — a driver who was
					 * leading and disconnected still set
					 * the benchmark lap count.
					 */
					if (s->cars[i].driver_count > 0 &&
					    lc > leader_laps)
						leader_laps = lc;
				}
				for (i = 0; i < ACC_MAX_CARS; i++) {
					struct CarEntry *car = &s->cars[i];
					int pct;
					if (car->driver_count == 0)
						continue;
					/*
					 * Skip slots that already received a
					 * rating delta in conn_drop's premature-
					 * DC branch (cars[].used cleared but
					 * driver_count and steam_id preserved).
					 * Without this guard the same steam_id
					 * gets a -30 TR from conn_drop and then
					 * another +3/+10 here at COMPLETED.
					 */
					if (!car->used)
						continue;
					pct = leader_laps > 0
					    ? (car->race.lap_count * 100)
					      / leader_laps : 0;
					ratings_on_race_end(s,
					    car->drivers[0].steam_id, pct,
					    car->race.disqualified);
				}
			}
			broadcast_session_results(s);
			if (!s->session.results_written) {
				/*
				 * dumpLeaderboards = 1 in settings.json:
				 * write the per-session results.json.
				 * Operators who explicitly disable the
				 * dump (set 0) skip the file but still get
				 * the broadcast / archive paths.
				 */
				if (s->dump_leaderboards)
					(void)results_write(s);
				/*
				 * dumpEntryList = 1: snapshot the entry
				 * list with each car's final position
				 * baked into defaultGridPosition.  Useful
				 * after Quali so operators can use the
				 * file to seed the next race's grid via an
				 * external workflow; harmless after P/R.
				 */
				if (s->dump_entry_list) {
					int ci;
					for (ci = 0; ci < ACC_MAX_CARS; ci++) {
						struct CarEntry *cc =
						    &s->cars[ci];
						if (cc->driver_count == 0)
							continue;
						if (cc->race.position >= 1)
							cc->default_grid_position
							    = cc->race.position;
					}
					(void)entrylist_save(s, s->cfg_dir);
				}
				s->session.results_written = 1;
			}
			/*
			 * Snapshot per-car race state so future 0x56
			 * garage requests for this session's laps can
			 * serve them after we've moved on.
			 */
			session_archive_snapshot(s);
			/* Persist the local rating ledger at session end. */
			ratings_save(s);
		}
		*last_phase = s->session.phase;
	}

	/*
	 * Periodic 0x4e rating summary.  The exe gates this on
	 * DAT_14014bd58 = 81000 ms (verified in .rdata) — a deliberate
	 * 81 s debounce that keeps the rating fan-out out of the
	 * per-second broadcast cost.  Previously debounced at 10 s
	 * which was 8× too fast.
	 */
	if (ratings_is_dirty(s) &&
	    now_ms - s->ratings_last_emit_ms >= CADENCE_RATINGS_MS) {
		struct ByteBuf wb;
		bb_init(&wb);
		if (build_rating_summary(&wb, s) == 0)
			(void)bcast_all(s, wb.data, wb.wpos, BCAST_EXCEPT_NONE);
		bb_free(&wb);
		ratings_clear_dirty(s);
		s->ratings_last_emit_ms = now_ms;
	}

	/*
	 * Weather: step the simulator and broadcast 0x37 every
	 * cadence.  The broadcast carries weekend_time_s which
	 * drives the client's in-game clock, so it must be sent
	 * unconditionally (matching the Kunos 5-second cadence).
	 */
	if (now_ms - last_weather_ms >= CADENCE_WEATHER_MS) {
		struct ByteBuf bb;

		(void)weather_step(s);
		bb_init(&bb);
		if (weather_build_broadcast(s, &bb) == 0)
			(void)bcast_all(s, bb.data, bb.wpos, BCAST_EXCEPT_NONE);
		bb_free(&bb);
		last_weather_ms = now_ms;
	}
}

