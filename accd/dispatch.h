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
 * dispatch.h -- per-message dispatch on TCP and UDP.
 *
 * The TCP dispatcher pulls framed messages out of a connection's
 * rx buffer and dispatches by msg id.  The UDP dispatcher takes a
 * single datagram.
 */

#ifndef ACCD_DISPATCH_H
#define ACCD_DISPATCH_H

#include <stddef.h>
#include <netinet/in.h>

#include "state.h"

/*
 * Drain the rx buffer of c, dispatching every complete framed
 * message.  Returns 0 on success, -1 if the connection should be
 * dropped.
 */
int	dispatch_tcp(struct Server *s, struct Conn *c);

/*
 * Process one UDP datagram from peer.
 */
void	dispatch_udp(struct Server *s, const struct sockaddr_in *peer,
		const unsigned char *buf, size_t len);

#endif /* ACCD_DISPATCH_H */
