/*
 * handshake.h -- ACP_REQUEST_CONNECTION (0x09) parser and
 * 0x0b handshake response builder.
 */

#ifndef ACCD_HANDSHAKE_H
#define ACCD_HANDSHAKE_H

#include <stddef.h>

#include "state.h"

/*
 * Parse and act on an ACP_REQUEST_CONNECTION message body
 * (msg id byte already consumed).  Validates the version,
 * password, server-full state, and entry list.  Calls
 * handshake_send_response() with the appropriate accept or
 * reject outcome and updates the connection state.
 *
 * Returns 0 on success (whether accepted or rejected — both
 * are "successfully handled"), -1 on a fatal protocol error
 * that should drop the connection.
 */
int	handshake_handle(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

#endif /* ACCD_HANDSHAKE_H */
