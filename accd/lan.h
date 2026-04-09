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
 * lan.h -- LAN discovery on the fixed UDP port 8999.
 *
 * The protocol:
 *   client -> server   ACP_LAN_DISCOVER (0x48)
 *   server -> client   ACP_LAN_RESPONSE (0xc0) carrying the
 *                       server name, capacity, current car count,
 *                       and a per-car summary.
 */

#ifndef ACCD_LAN_H
#define ACCD_LAN_H

#include <netinet/in.h>

#include "state.h"

#define ACC_LAN_DISCOVERY_PORT	8999

int	lan_open(int *out_fd);
void	lan_handle(struct Server *s, int fd);

#endif /* ACCD_LAN_H */
