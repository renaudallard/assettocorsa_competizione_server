/*
 * dispatch.c -- TCP and UDP message dispatchers.
 *
 * Most case bodies are stubs at this stage: they log the message
 * id and length so a packet capture can be cross-checked against
 * the catalog in NOTEBOOK_B.md, but they don't yet implement
 * the full state mutations.  Phase 1 only fully handles the
 * handshake (0x09).  The other cases will be filled in as
 * subsequent phases need them.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "state.h"

/* ----- one TCP message ------------------------------------------- */

static int
dispatch_one_tcp(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	uint8_t msg_id;

	if (len < 1) {
		log_warn("tcp: empty frame from fd %d", c->fd);
		return -1;
	}
	msg_id = body[0];

	/*
	 * Pre-handshake: only ACP_REQUEST_CONNECTION is accepted.
	 */
	if (c->state == CONN_UNAUTH && msg_id != ACP_REQUEST_CONNECTION) {
		log_warn("tcp: unauthenticated msg 0x%02x from fd %d "
		    "(dropping)", (unsigned)msg_id, c->fd);
		return -1;
	}

	switch (msg_id) {
	case ACP_REQUEST_CONNECTION:	/* 0x09 */
		return handshake_handle(s, c, body, len);

	case ACP_DISCONNECT:		/* 0x10 */
		log_info("tcp 0x10: clean disconnect from conn=%u",
		    (unsigned)c->conn_id);
		return -1;

	case ACP_LAP_COMPLETED:		/* 0x19 */
	case ACP_SECTOR_SPLIT_BULK:	/* 0x20 */
	case ACP_SECTOR_SPLIT_SINGLE:	/* 0x21 */
	case ACP_CHAT:			/* 0x2a */
	case ACP_CAR_SYSTEM_UPDATE:	/* 0x2e */
	case ACP_TYRE_COMPOUND_UPDATE:	/* 0x2f */
	case ACP_CAR_LOCATION_UPDATE:	/* 0x32 */
	case ACP_OUT_OF_TRACK:		/* 0x3d */
	case ACP_REPORT_PENALTY:	/* 0x41 */
	case ACP_LAP_TICK:		/* 0x42 */
	case ACP_DAMAGE_ZONES_UPDATE:	/* 0x43 */
	case ACP_CAR_DIRT_UPDATE:	/* 0x45 */
	case ACP_UPDATE_DRIVER_SWAP_STATE: /* 0x47 */
	case ACP_EXECUTE_DRIVER_SWAP:	/* 0x48 */
	case ACP_DRIVER_SWAP_STATE_REQUEST: /* 0x4a */
	case ACP_DRIVER_STINT_RESET:	/* 0x4f */
	case ACP_ELO_UPDATE:		/* 0x51 */
	case ACP_MANDATORY_PITSTOP_SERVED: /* 0x54 */
	case ACP_LOAD_SETUP:		/* 0x55 */
	case ACP_CTRL_INFO:		/* 0x5b */
		log_info("tcp 0x%02x from conn=%u (%zu bytes) — TODO",
		    (unsigned)msg_id, (unsigned)c->conn_id, len);
		return 0;

	default:
		log_warn("tcp: unknown msg 0x%02x from conn=%u (%zu bytes)",
		    (unsigned)msg_id, (unsigned)c->conn_id, len);
		return 0;
	}
}

int
dispatch_tcp(struct Server *s, struct Conn *c)
{
	const unsigned char *body;
	size_t len, consumed;
	int rc;

	for (;;) {
		rc = bb_take_frame(&c->rx, &body, &len, &consumed);
		if (rc == 0)
			return 0;
		if (rc < 0) {
			log_warn("tcp: framing error from fd %d", c->fd);
			return -1;
		}
		rc = dispatch_one_tcp(s, c, body, len);
		bb_consume(&c->rx, consumed);
		if (rc < 0)
			return -1;
	}
}

/* ----- UDP -------------------------------------------------------- */

void
dispatch_udp(struct Server *s, const struct sockaddr_in *peer,
    const unsigned char *buf, size_t len)
{
	uint8_t msg_id;

	(void)s;

	if (len < 1) {
		log_warn("udp: empty datagram from %s:%u",
		    inet_ntoa(peer->sin_addr), ntohs(peer->sin_port));
		return;
	}
	msg_id = buf[0];

	switch (msg_id) {
	case ACP_KEEPALIVE_A:		/* 0x13 */
	case ACP_KEEPALIVE_B:		/* 0x17 */
		/* Silent: drop without statistics. */
		return;

	case ACP_PONG_PHYSICS:		/* 0x16 */
	case ACP_CAR_UPDATE:		/* 0x1e */
	case ACP_CAR_INFO_REQUEST:	/* 0x22 */
	case ACP_TIME_EVENT:		/* 0x5e */
	case ACP_ADMIN_QUERY:		/* 0x5f */
		log_info("udp 0x%02x from %s:%u (%zu bytes) — TODO",
		    (unsigned)msg_id, inet_ntoa(peer->sin_addr),
		    ntohs(peer->sin_port), len);
		return;

	default:
		log_warn("udp: unknown msg 0x%02x from %s:%u (%zu bytes)",
		    (unsigned)msg_id, inet_ntoa(peer->sin_addr),
		    ntohs(peer->sin_port), len);
		return;
	}
}
