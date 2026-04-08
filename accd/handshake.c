/*
 * handshake.c -- ACP_REQUEST_CONNECTION parser and 0x0b response.
 *
 * The 0x09 request body, after the msg id byte, contains:
 *
 *     u16          client_version    (must == ACC_PROTOCOL_VERSION)
 *     string_a     password          (Format A)
 *     ... DriverInfo + CarInfo substructures ...
 *
 * Phase 1 only validates the first two fields and ignores the
 * trailing CarInfo until later phases.  This is enough to make
 * the server respond with either accept or reject.
 *
 * The 0x0b response body is:
 *
 *     u8           msg_id = 0x0b
 *     u16          server protocol version
 *     u8           server flags        (0 for now)
 *     u16          connection_id       (0xFFFF on reject)
 *     ... welcome trailer on accept ...
 *
 * For phase 1 we send the minimum-viable trailer documented in
 * §5.6.4c: carId + trackName + eventId + 0 sessions + empty
 * sub-records + 0 cars.  This is enough for some clients to
 * proceed; if the real client demands more we'll fix it then.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "bcast.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "monitor.h"
#include "msg.h"
#include "prim.h"
#include "state.h"

static int
build_welcome_trailer(struct ByteBuf *bb, struct Server *s, struct Conn *c)
{
	int i;

	/* Trailer header: assigned car id (or 0xFFFFFFFF on reject). */
	if (wr_u32(bb, (uint32_t)c->car_id) < 0)
		return -1;

	/* trackName + eventId, both Format-A. */
	if (wr_str_a(bb, s->track) < 0)
		return -1;
	if (wr_str_a(bb, "") < 0)	/* eventId — empty for phase 1 */
		return -1;

	/*
	 * Separator + session list.  Phase 1 emits zero sessions
	 * so the trailer is short.
	 */
	if (wr_u8(bb, 1) < 0)		/* separator */
		return -1;
	if (wr_u8(bb, 0) < 0)		/* session_count */
		return -1;

	/*
	 * Sub-records (SeasonEntity, SessionManager, AssistRules,
	 * ServerConfiguration, additional state) — emit empty
	 * placeholders separated by 1-byte tags.  This is the
	 * "minimum viable accept" path documented in §5.6.4c #2.
	 */
	for (i = 0; i < 5; i++) {
		if (wr_u8(bb, 1) < 0)
			return -1;
		/* placeholder body: nothing */
	}

	/* Connected car list: count + 0 records (we send our own
	 * car later via 0x23 / 0x28 if needed). */
	if (wr_u8(bb, 1) < 0)		/* separator */
		return -1;
	if (wr_u8(bb, 0) < 0)		/* connected_car_count */
		return -1;

	return 0;
}

static int
handshake_send(struct Conn *c, enum reject_reason reason,
    struct Server *s)
{
	struct ByteBuf bb;
	uint16_t conn_id_field;
	int rc;

	bb_init(&bb);

	/* msg id + server version */
	if (wr_u8(&bb, SRV_HANDSHAKE_RESPONSE) < 0)
		goto fail;
	if (wr_u16(&bb, ACC_PROTOCOL_VERSION) < 0)
		goto fail;

	/* server flags: lobby off, accept-only currently */
	if (wr_u8(&bb, 0) < 0)
		goto fail;

	/* connection id (0xFFFF on reject) */
	conn_id_field = (reason == REJECT_OK) ? c->conn_id : 0xFFFF;
	if (wr_u16(&bb, conn_id_field) < 0)
		goto fail;

	if (reason == REJECT_OK) {
		if (build_welcome_trailer(&bb, s, c) < 0)
			goto fail;
	} else {
		/* Reject: empty trailer with the reason as a single
		 * byte (the binary uses an enum here too). */
		if (wr_u8(&bb, (uint8_t)reason) < 0)
			goto fail;
	}

	rc = tcp_send_framed(c->fd, bb.data, bb.wpos);
	bb_free(&bb);
	return rc;

fail:
	bb_free(&bb);
	return -1;
}

int
handshake_handle(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint16_t client_version;
	char *password = NULL;
	enum reject_reason reason = REJECT_OK;

	rd_init(&r, body, len);

	if (rd_u8(&r, &msg_id) < 0 || msg_id != ACP_REQUEST_CONNECTION) {
		log_warn("handshake: bad first byte 0x%02x from fd %d",
		    msg_id, c->fd);
		return -1;
	}
	if (rd_u16(&r, &client_version) < 0) {
		log_warn("handshake: short version from fd %d", c->fd);
		return -1;
	}
	if (client_version != ACC_PROTOCOL_VERSION) {
		log_info("rejecting new connection with wrong client "
		    "version %u (server runs %u)",
		    (unsigned)client_version,
		    (unsigned)ACC_PROTOCOL_VERSION);
		reason = REJECT_VERSION;
		goto reply;
	}
	if (rd_str_a(&r, &password) < 0) {
		log_warn("handshake: short password from fd %d", c->fd);
		return -1;
	}
	if (strcmp(password, s->password) != 0) {
		log_info("rejecting connection: bad password from fd %d",
		    c->fd);
		reason = REJECT_PASSWORD;
		goto reply;
	}
	if (s->nconns >= s->max_connections) {
		log_info("rejecting connection: server full");
		reason = REJECT_FULL;
		goto reply;
	}

	/*
	 * For phase 1 we trust the rest of the body (DriverInfo,
	 * CarInfo) and skip past it.  Allocate a car slot and
	 * mark the connection as authenticated.
	 */
	c->car_id = server_alloc_car(s);
	if (c->car_id < 0) {
		reason = REJECT_FULL;
		goto reply;
	}
	c->state = CONN_AUTH;
	log_info("handshake accepted: fd=%d conn_id=%u car_id=%d",
	    c->fd, (unsigned)c->conn_id, c->car_id);

reply:
	free(password);
	if (handshake_send(c, reason, s) < 0)
		return -1;

	/*
	 * After a successful accept, fan out 0x2e new-client-
	 * joined notify to every OTHER already-connected client.
	 * This lets them add the joining car to their local entry
	 * list and display it in the lobby.  The binary also emits
	 * a paired 0x4f sub-opcode 1 message right after; we do
	 * the same.
	 */
	if (reason == REJECT_OK) {
		struct ByteBuf notify;
		uint64_t timestamp_ms;
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		timestamp_ms = (uint64_t)ts.tv_sec * 1000ull +
		    (uint64_t)ts.tv_nsec / 1000000ull;

		bb_init(&notify);
		if (wr_u8(&notify, SRV_CAR_SYSTEM_RELAY) == 0 &&
		    wr_u16(&notify, (uint16_t)c->car_id) == 0 &&
		    wr_u64(&notify, timestamp_ms) == 0)
			(void)bcast_all(s, notify.data, notify.wpos,
			    c->conn_id);
		bb_free(&notify);

		/* Paired 0x4f sub-opcode 1: u8 msg_id + u16 carId +
		 * u8 sub=1 + u64 timestamp (12 bytes). */
		bb_init(&notify);
		if (wr_u8(&notify, SRV_DRIVER_STINT_RELAY) == 0 &&
		    wr_u16(&notify, (uint16_t)c->car_id) == 0 &&
		    wr_u8(&notify, 1) == 0 &&
		    wr_u64(&notify, timestamp_ms) == 0)
			(void)bcast_all(s, notify.data, notify.wpos,
			    c->conn_id);
		bb_free(&notify);

		/*
		 * Push the post-handshake welcome state to the
		 * joining client: 0x04 CAR_ENTRY, 0x05 CONNECTION_
		 * ENTRY, 0x03 SESSION_STATE, 0x07 LEADERBOARD_UPDATE.
		 * Without this the real client almost certainly
		 * disconnects after the 0x0b response.
		 */
		(void)monitor_push_welcome_sequence(s, c);
	}
	return 0;
}
