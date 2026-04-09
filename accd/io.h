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
 * io.h -- byte buffer and TCP framing layer.
 *
 * The ACC sim protocol uses a simple 16-bit length prefix on each
 * TCP message body, with an escape into a 32-bit extended length:
 *
 *     u16 length;                       // host length, little-endian
 *     if length == 0xFFFF:
 *         u32 extended_length;          // little-endian
 *     u8 body[length];
 *
 * The length does NOT include the 2-byte (or 6-byte) prefix itself.
 * UDP datagrams are unframed: the datagram itself is the message.
 *
 * A ByteBuf is a growable byte buffer used both for accumulating
 * incoming bytes from a TCP socket and for building outgoing
 * messages.  Read and write cursors are tracked separately.
 */

#ifndef ACCD_IO_H
#define ACCD_IO_H

#include <stddef.h>
#include <stdint.h>

/*
 * ByteBuf -- growable byte buffer.
 *
 * Layout:
 *     [data ............ wpos] [wpos ............ cap]
 *      ^read cursor (rpos)
 *
 * Bytes in [data .. wpos) are valid; rpos is the read cursor.
 */
struct ByteBuf {
	unsigned char	*data;
	size_t		 cap;
	size_t		 wpos;	/* write cursor (== valid byte count) */
	size_t		 rpos;	/* read cursor */
};

void	bb_init(struct ByteBuf *bb);
void	bb_free(struct ByteBuf *bb);
int	bb_reserve(struct ByteBuf *bb, size_t need);
void	bb_clear(struct ByteBuf *bb);
void	bb_consume(struct ByteBuf *bb, size_t n);

int	bb_append(struct ByteBuf *bb, const void *src, size_t n);
int	bb_append_u8(struct ByteBuf *bb, uint8_t v);

/*
 * Try to extract one framed TCP message from the buffer.
 *
 * On success returns 1 and sets *out_body and *out_len to point at
 * the body bytes (which are inside bb->data, NOT copied).  The
 * caller must call bb_consume() to advance past the message after
 * processing it.
 *
 * Returns 0 if not enough bytes for a complete frame yet.
 * Returns -1 on protocol error (caller should drop the connection).
 */
int	bb_take_frame(struct ByteBuf *bb,
		const unsigned char **out_body, size_t *out_len,
		size_t *out_consumed);

/*
 * Send a TCP-framed message.  Prepends the 2-byte (or 6-byte for
 * lengths >= 0xFFFF) length header to the body and sends it on fd.
 * Returns 0 on success, -1 on error (errno set).
 */
int	tcp_send_framed(int fd, const void *body, size_t len);

#endif /* ACCD_IO_H */
