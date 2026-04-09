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
