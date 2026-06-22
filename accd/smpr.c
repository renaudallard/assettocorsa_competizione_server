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
 * smpr.c -- ServerMonitor protocol connection handler.
 *
 * Drives the protobuf side-channel that external monitoring tools
 * (accweb, accservermanager, emperorservers) connect to.  Decodes
 * the inbound ServerMonitorConnectionRequest (the monitor's hello)
 * then pushes the standard kunos initial-state burst.
 */

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>		/* inet_ntoa for the cap-reject log */

#include "bcast.h"
#include "io.h"
#include "log.h"
#include "monitor.h"
#include "pb.h"
#include "prim.h"
#include "smpr.h"
#include "state.h"

/* ServerMonitorProtocolMessage tag bytes (§12B.2 of NOTEBOOK_B.md). */
#define SMPR_MSG_REGISTRATION_RESULT	0x01
#define SMPR_MSG_SERVER_CONFIGURATION	0x02
#define SMPR_MSG_SESSION_STATE		0x03
#define SMPR_MSG_CAR_ENTRY		0x04
#define SMPR_MSG_CONNECTION_ENTRY	0x05
#define SMPR_MSG_REALTIME_UPDATE	0x06
#define SMPR_MSG_LEADERBOARD_UPDATE	0x07

/* Cadence clamps for client-supplied realtimeCarUpdateInterval. */
#define SMPR_RT_INTERVAL_MIN_MS		50
#define SMPR_RT_INTERVAL_MAX_MS		10000
#define SMPR_RT_INTERVAL_DEFAULT_MS	250

/*
 * Frame a ServerMonitor message: prepend the msg_type byte to the
 * already-built protobuf body and TCP-write to a single Conn.
 */
static int
smpr_send_msg(struct Conn *c, uint8_t msg_type, const struct ByteBuf *body)
{
	struct ByteBuf out;
	int rc = -1;

	bb_init(&out);
	if (wr_u8(&out, msg_type) == 0 &&
	    (body->wpos == 0 ||
	     bb_append(&out, body->data, body->wpos) == 0))
		rc = bcast_send_one(c, out.data, out.wpos);
	bb_free(&out);
	return rc;
}

static void
smpr_push_registration_result(struct Conn *c, int success, const char *err_txt)
{
	struct ByteBuf body;

	bb_init(&body);
	if (monitor_build_handshake_result(&body, success, c->conn_id,
	    err_txt) == 0)
		(void)smpr_send_msg(c, SMPR_MSG_REGISTRATION_RESULT, &body);
	bb_free(&body);
}

static void
smpr_push_server_configuration(struct Server *s, struct Conn *c)
{
	struct ByteBuf body;

	bb_init(&body);
	if (monitor_build_configuration_state(&body, s) == 0)
		(void)smpr_send_msg(c, SMPR_MSG_SERVER_CONFIGURATION, &body);
	bb_free(&body);
}

static void
smpr_push_session_state(struct Server *s, struct Conn *c)
{
	struct ByteBuf body;

	bb_init(&body);
	if (monitor_build_session_state(&body, s) == 0)
		(void)smpr_send_msg(c, SMPR_MSG_SESSION_STATE, &body);
	bb_free(&body);
}

static void
smpr_push_car_entries(struct Server *s, struct Conn *c)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct CarEntry *car = &s->cars[i];
		struct ByteBuf body;
		int driving_conn = -1;
		int j;

		if (!car->used)
			continue;
		for (j = 0; j < ACC_MAX_CARS; j++) {
			if (s->conns[j] != NULL &&
			    s->conns[j]->car_id == i) {
				driving_conn = s->conns[j]->conn_id;
				break;
			}
		}
		bb_init(&body);
		if (monitor_build_car_entry(&body, car, driving_conn) == 0)
			(void)smpr_send_msg(c, SMPR_MSG_CAR_ENTRY, &body);
		bb_free(&body);
	}
}

static void
smpr_push_connection_entries(struct Server *s, struct Conn *c)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *o = s->conns[i];
		struct ByteBuf body;

		if (o == NULL)
			continue;
		if (o->state != CONN_AUTH)
			continue;
		/* Exclude SMPR connections from the connection list -
		 * they are observers, not drivers. */
		if (o->is_smpr)
			continue;
		bb_init(&body);
		if (monitor_build_connection_entry(&body, s, o) == 0)
			(void)smpr_send_msg(c, SMPR_MSG_CONNECTION_ENTRY,
			    &body);
		bb_free(&body);
	}
}

int
smpr_handle_connect(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct PbReader r;
	char display_name[64] = { 0 };
	int32_t rt_interval = SMPR_RT_INTERVAL_DEFAULT_MS;
	int self_contained = 0;
	int extended = 0;
	int register_all = 0;
	int ok;

	pb_r_init(&r, body, len);
	while (!pb_r_eof(&r)) {
		uint32_t field, wire;

		if (pb_r_tag(&r, &field, &wire) < 0)
			break;
		ok = 0;
		switch (field) {
		case 1:	/* string displayName */
			if (wire == PB_WIRE_LENGTH_DELIM)
				ok = pb_r_string(&r, display_name,
				    sizeof(display_name)) == 0;
			break;
		case 2:	/* int32 realtimeCarUpdateInterval */
			if (wire == PB_WIRE_VARINT)
				ok = pb_r_int32(&r, &rt_interval) == 0;
			break;
		case 3:	/* bool sendSelfcontainingLeaderboards */
			if (wire == PB_WIRE_VARINT)
				ok = pb_r_bool(&r, &self_contained) == 0;
			break;
		case 4:	/* bool sendExtendedLeaderboards */
			if (wire == PB_WIRE_VARINT)
				ok = pb_r_bool(&r, &extended) == 0;
			break;
		case 5:	/* bool registerToAllEvents */
			if (wire == PB_WIRE_VARINT)
				ok = pb_r_bool(&r, &register_all) == 0;
			break;
		default:
			/* Forward-compatible: skip unknown fields. */
			break;
		}
		if (!ok && pb_r_skip(&r, wire) < 0)
			break;
	}

	if (r.error) {
		log_warn("SMPR conn %u: malformed ConnectionRequest",
		    (unsigned)c->conn_id);
		c->is_smpr = 1;
		smpr_push_registration_result(c, 0, "bad request");
		return -1;
	}

	if (rt_interval < SMPR_RT_INTERVAL_MIN_MS)
		rt_interval = SMPR_RT_INTERVAL_MIN_MS;
	if (rt_interval > SMPR_RT_INTERVAL_MAX_MS)
		rt_interval = SMPR_RT_INTERVAL_MAX_MS;

	/*
	 * Cap observers globally (maxMonitors) and per source IP
	 * (maxMonitorsPerIp).  Without these limits an unauthenticated
	 * peer can open max_connections SMPR conns and starve sim
	 * drivers out of the shared slot pool.
	 */
	{
		int total = 0, from_ip = 0, i;

		for (i = 0; i < ACC_MAX_CARS; i++) {
			struct Conn *o = s->conns[i];

			if (o == NULL || !o->is_smpr || o == c)
				continue;
			total++;
			if (o->peer.sin_addr.s_addr ==
			    c->peer.sin_addr.s_addr)
				from_ip++;
		}
		if (s->max_monitors > 0 && total >= s->max_monitors) {
			log_warn("SMPR conn %u: rejected, global cap %d "
			    "reached", (unsigned)c->conn_id,
			    s->max_monitors);
			c->is_smpr = 1;
			smpr_push_registration_result(c, 0,
			    "monitor cap reached");
			return -1;
		}
		if (s->max_monitors_per_ip > 0 &&
		    from_ip >= s->max_monitors_per_ip) {
			log_warn("SMPR conn %u: rejected, per-IP cap %d "
			    "reached for %s", (unsigned)c->conn_id,
			    s->max_monitors_per_ip,
			    inet_ntoa(c->peer.sin_addr));
			c->is_smpr = 1;
			smpr_push_registration_result(c, 0,
			    "monitor cap reached");
			return -1;
		}
	}

	c->is_smpr = 1;
	c->smpr_rt_interval_ms = (uint32_t)rt_interval;
	c->smpr_rt_last_ms = 0;
	/*
	 * sendSelfcontainingLeaderboards / sendExtendedLeaderboards are
	 * negotiated and stored but NOT yet honored: monitor_build_leaderboard
	 * always emits the full self-contained form, so an observer asking for
	 * the lighter delta form still receives the heavier one.  This is safe
	 * (more data, not less) and no current SMPR consumer requests the delta
	 * form; wiring the flags into the builder is deferred until one does.
	 */
	c->smpr_self_contained = self_contained ? 1 : 0;
	c->smpr_extended = extended ? 1 : 0;
	(void)register_all;	/* not used yet */
	/*
	 * Move out of CONN_UNAUTH so main.c's 30 s unauth-reaper
	 * doesn't kill the connection.  SMPR clients never claim a
	 * car slot (car_id stays -1) so gameplay broadcast loops
	 * that gate on `c->car_id >= 0` already skip them; the few
	 * loops that gate only on `c->state == CONN_AUTH` need an
	 * additional `!c->is_smpr` filter (audited in tick.c +
	 * bcast.c).
	 */
	c->state = CONN_AUTH;

	log_info("Received SMPR connection %u for \"%s\" (rt=%dms self=%d ext=%d)",
	    (unsigned)c->conn_id, display_name, rt_interval,
	    self_contained, extended);

	smpr_push_registration_result(c, 1, "");
	smpr_push_server_configuration(s, c);
	smpr_push_session_state(s, c);
	smpr_push_car_entries(s, c);
	smpr_push_connection_entries(s, c);

	/* Initial leaderboard push so the observer sees current standings
	 * immediately, instead of waiting on the next event-driven
	 * leaderboard_pending or the 75s async heartbeat. */
	{
		struct ByteBuf body;
		bb_init(&body);
		if (monitor_build_leaderboard(&body, s) == 0)
			(void)smpr_send_msg(c,
			    SMPR_MSG_LEADERBOARD_UPDATE, &body);
		bb_free(&body);
	}

	return 0;
}

void
smpr_tick_realtime(struct Server *s)
{
	uint32_t now = (uint32_t)mono_ms();
	int i;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];
		struct ByteBuf body;

		if (c == NULL || !c->is_smpr)
			continue;
		if (c->state != CONN_AUTH)
			continue;
		if (c->smpr_rt_last_ms != 0 &&
		    now - c->smpr_rt_last_ms < c->smpr_rt_interval_ms)
			continue;
		bb_init(&body);
		if (monitor_build_realtime_update(&body, s) == 0)
			(void)smpr_send_msg(c, SMPR_MSG_REALTIME_UPDATE, &body);
		bb_free(&body);
		c->smpr_rt_last_ms = now;
	}
}

void
smpr_broadcast_leaderboard(struct Server *s)
{
	int i;
	int any = 0;

	for (i = 0; i < ACC_MAX_CARS; i++)
		if (s->conns[i] != NULL && s->conns[i]->is_smpr)
			any = 1;
	if (!any)
		return;
	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];
		struct ByteBuf body;

		if (c == NULL || !c->is_smpr || c->state != CONN_AUTH)
			continue;
		bb_init(&body);
		if (monitor_build_leaderboard(&body, s) == 0)
			(void)smpr_send_msg(c,
			    SMPR_MSG_LEADERBOARD_UPDATE, &body);
		bb_free(&body);
	}
}

/*
 * Push SESSION_STATE to all connected SMPR monitors.  Called on session
 * advance / phase change (exe FUN_14002aca0 sends 0x03 on event change).
 */
void
smpr_broadcast_session_state(struct Server *s)
{
	int i;

	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];
		if (c == NULL || !c->is_smpr || c->state != CONN_AUTH)
			continue;
		smpr_push_session_state(s, c);
	}
}

/*
 * Fan out a single CONNECTION_ENTRY for `changed` to every SMPR
 * conn currently attached.  Skips when changed itself is an SMPR
 * conn (observers don't show up in the connection list).
 */
void
smpr_notify_conn_changed(struct Server *s, struct Conn *changed)
{
	int i;

	if (changed == NULL || changed->is_smpr)
		return;
	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];
		struct ByteBuf body;

		if (c == NULL || !c->is_smpr || c->state != CONN_AUTH)
			continue;
		bb_init(&body);
		if (monitor_build_connection_entry(&body, s, changed) == 0)
			(void)smpr_send_msg(c, SMPR_MSG_CONNECTION_ENTRY,
			    &body);
		bb_free(&body);
	}
}

void
smpr_notify_car_changed(struct Server *s, int car_id)
{
	int i, driving_conn = -1;

	if (car_id < 0 || car_id >= ACC_MAX_CARS)
		return;
	for (i = 0; i < ACC_MAX_CARS; i++) {
		if (s->conns[i] != NULL &&
		    s->conns[i]->car_id == car_id) {
			driving_conn = s->conns[i]->conn_id;
			break;
		}
	}
	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];
		struct ByteBuf body;

		if (c == NULL || !c->is_smpr || c->state != CONN_AUTH)
			continue;
		bb_init(&body);
		if (monitor_build_car_entry(&body, &s->cars[car_id],
		    driving_conn) == 0)
			(void)smpr_send_msg(c, SMPR_MSG_CAR_ENTRY, &body);
		bb_free(&body);
	}
}
