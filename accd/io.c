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
 * io.c -- byte buffer and TCP framing implementation.
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "io.h"
#include "log.h"

#define BB_INITIAL_CAP	256
/*
 * Safety cap on ByteBuf growth.  Legitimate ACC traffic tops out
 * at the welcome-trailer-sized frame (~1.3 KB) plus the largest
 * 0x36 leaderboard (~20 KB) and 0x3e session results (~10 KB).
 * 256 KiB is ~10× the largest real frame and well below any OS
 * memory pressure; a malicious peer streaming partial frames
 * forever to exhaust rx buffers fails cleanly at this cap.
 */
#define BB_MAX_CAP	(256u * 1024u)

void
bb_init(struct ByteBuf *bb)
{
	bb->data = NULL;
	bb->cap = 0;
	bb->wpos = 0;
	bb->rpos = 0;
}

void
bb_free(struct ByteBuf *bb)
{
	free(bb->data);
	bb->data = NULL;
	bb->cap = 0;
	bb->wpos = 0;
	bb->rpos = 0;
}

void
bb_clear(struct ByteBuf *bb)
{
	bb->wpos = 0;
	bb->rpos = 0;
}

int
bb_reserve(struct ByteBuf *bb, size_t need)
{
	size_t newcap;
	unsigned char *p;

	if (need <= bb->cap - bb->wpos)
		return 0;

	newcap = bb->cap == 0 ? BB_INITIAL_CAP : bb->cap;
	while (newcap - bb->wpos < need) {
		if (newcap >= BB_MAX_CAP) {
			errno = ENOMEM;
			return -1;
		}
		newcap *= 2;
		if (newcap > BB_MAX_CAP)
			newcap = BB_MAX_CAP;
	}
	p = realloc(bb->data, newcap);
	if (p == NULL)
		return -1;
	bb->data = p;
	bb->cap = newcap;
	return 0;
}

void
bb_consume(struct ByteBuf *bb, size_t n)
{
	if (n >= bb->wpos) {
		bb->wpos = 0;
		bb->rpos = 0;
		return;
	}
	memmove(bb->data, bb->data + n, bb->wpos - n);
	bb->wpos -= n;
	if (bb->rpos > n)
		bb->rpos -= n;
	else
		bb->rpos = 0;
}

int
bb_append(struct ByteBuf *bb, const void *src, size_t n)
{
	if (bb_reserve(bb, n) < 0)
		return -1;
	memcpy(bb->data + bb->wpos, src, n);
	bb->wpos += n;
	return 0;
}

int
bb_append_u8(struct ByteBuf *bb, uint8_t v)
{
	if (bb_reserve(bb, 1) < 0)
		return -1;
	bb->data[bb->wpos++] = v;
	return 0;
}

int
bb_take_frame(struct ByteBuf *bb,
    const unsigned char **out_body, size_t *out_len,
    size_t *out_consumed)
{
	size_t avail, hdr, len;
	const unsigned char *p;

	avail = bb->wpos;
	if (avail < 2)
		return 0;

	p = bb->data;
	len = (size_t)p[0] | ((size_t)p[1] << 8);
	hdr = 2;
	if (len == 0xFFFF) {
		if (avail < 6)
			return 0;
		len = (size_t)p[2] | ((size_t)p[3] << 8) |
		    ((size_t)p[4] << 16) | ((size_t)p[5] << 24);
		hdr = 6;
		/* Sanity: a 4 GiB frame is impossible on this protocol. */
		if (len > (size_t)(16u * 1024u * 1024u))
			return -1;
	}
	if (avail - hdr < len)
		return 0;

	*out_body = p + hdr;
	*out_len = len;
	*out_consumed = hdr + len;
	return 1;
}

int
tcp_send_framed(int fd, const void *body, size_t len)
{
	unsigned char hdr[6];
	size_t hdrlen;
	struct iovec iov[2];
	const unsigned char *src;
	ssize_t n;
	size_t total, sent;
	const unsigned char *b = (const unsigned char *)body;

	if (len >= 1) {
		log_debug("tcp tx fd=%d msg=0x%02x len=%zu",
		    fd, (unsigned)b[0], len);
		if (g_debug && len > 1)
			log_hexdump("  tx", body, len);
	}

	if (len < 0xFFFF) {
		hdr[0] = (unsigned char)(len & 0xff);
		hdr[1] = (unsigned char)((len >> 8) & 0xff);
		hdrlen = 2;
	} else {
		hdr[0] = 0xff;
		hdr[1] = 0xff;
		hdr[2] = (unsigned char)(len & 0xff);
		hdr[3] = (unsigned char)((len >> 8) & 0xff);
		hdr[4] = (unsigned char)((len >> 16) & 0xff);
		hdr[5] = (unsigned char)((len >> 24) & 0xff);
		hdrlen = 6;
	}

	/*
	 * Use writev for the common single-call case, fall back to a
	 * partial-write loop on EINTR / partial sends.
	 */
	iov[0].iov_base = hdr;
	iov[0].iov_len = hdrlen;
	iov[1].iov_base = (void *)(uintptr_t)body;
	iov[1].iov_len = len;
	total = hdrlen + len;
	sent = 0;
	src = (const unsigned char *)body;
	for (;;) {
		n = writev(fd, iov, 2);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		sent += (size_t)n;
		if (sent >= total)
			return 0;
		/* Partial: switch to plain write loop on the body. */
		break;
	}
	while (sent < total) {
		size_t off = sent;
		size_t remaining;
		const void *p;

		if (off < hdrlen) {
			p = hdr + off;
			remaining = hdrlen - off;
		} else {
			p = src + (off - hdrlen);
			remaining = total - off;
		}
		n = write(fd, p, remaining);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		sent += (size_t)n;
	}
	return 0;
}
