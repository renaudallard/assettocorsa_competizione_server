/*
 * Copyright (c) 2025-2026 Renaud Allard
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

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
