/*
 * bcast.h -- broadcast helpers.
 *
 * The binary uses a two-tier broadcast architecture (see §5.6.4b):
 *
 *   Tier 1 -- direct relay: server receives a message, validates,
 *     and forwards the same wire body (with maybe a leading msg
 *     id byte swap) to every other connected client.  Used for
 *     0x2a chat, 0x2e car system, 0x2f tyre compound, 0x32 car
 *     location, 0x43 damage, 0x45 dirt, 0x3d out of track.
 *
 *   Tier 2 -- queued lambda with per-recipient transformation:
 *     server updates its own state, then builds a PER-RECIPIENT
 *     message with (potentially) different bytes for each client.
 *     Used for 0x19 lap completed -> 0x1b and 0x20/0x21 sector
 *     splits -> 0x3a/0x3b.
 *
 * For a clean-room reimplementation we can treat both tiers as
 * "build one message, send to N recipients" as long as the per-
 * recipient customizations (e.g. relative-to-my-best-delta) can
 * be computed in a single pass.  Phase 2 doesn't do any of that
 * yet -- it just implements the simple broadcast path.
 */

#ifndef ACCD_BCAST_H
#define ACCD_BCAST_H

#include <stddef.h>

#include "state.h"

/*
 * Send a framed TCP message to one connection.  The body must
 * start with the msg id byte.  Returns 0 on success, -1 on send
 * error.
 */
int	bcast_send_one(struct Conn *c, const void *body, size_t len);

/*
 * Send a framed TCP message to every authenticated connection
 * except the one identified by except_conn_id (use 0xFFFF for
 * "exclude nobody").  Returns the number of clients that
 * received the message.
 */
int	bcast_all(struct Server *s, const void *body, size_t len,
		uint16_t except_conn_id);

#endif /* ACCD_BCAST_H */
