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
 * prim.c -- primitive readers / writers.
 *
 * Hand-rolled little-endian to avoid endian.h portability issues
 * (OpenBSD has it, Linux glibc has it but musl doesn't, macOS
 * doesn't).  All operations work on byte arrays directly.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "prim.h"
#include "io.h"

void
rd_init(struct Reader *r, const void *body, size_t len)
{
	r->p = (const unsigned char *)body;
	r->end = r->p + len;
}

size_t
rd_remaining(const struct Reader *r)
{
	return (size_t)(r->end - r->p);
}

int
rd_eof(const struct Reader *r)
{
	return r->p >= r->end;
}

int
rd_u8(struct Reader *r, uint8_t *out)
{
	if (rd_remaining(r) < 1)
		return -1;
	*out = *r->p++;
	return 0;
}

int
rd_u16(struct Reader *r, uint16_t *out)
{
	if (rd_remaining(r) < 2)
		return -1;
	*out = (uint16_t)r->p[0] | ((uint16_t)r->p[1] << 8);
	r->p += 2;
	return 0;
}

int
rd_u32(struct Reader *r, uint32_t *out)
{
	if (rd_remaining(r) < 4)
		return -1;
	*out = (uint32_t)r->p[0] |
	    ((uint32_t)r->p[1] << 8) |
	    ((uint32_t)r->p[2] << 16) |
	    ((uint32_t)r->p[3] << 24);
	r->p += 4;
	return 0;
}

int
rd_u64(struct Reader *r, uint64_t *out)
{
	if (rd_remaining(r) < 8)
		return -1;
	*out = (uint64_t)r->p[0] |
	    ((uint64_t)r->p[1] << 8) |
	    ((uint64_t)r->p[2] << 16) |
	    ((uint64_t)r->p[3] << 24) |
	    ((uint64_t)r->p[4] << 32) |
	    ((uint64_t)r->p[5] << 40) |
	    ((uint64_t)r->p[6] << 48) |
	    ((uint64_t)r->p[7] << 56);
	r->p += 8;
	return 0;
}

int
rd_i32(struct Reader *r, int32_t *out)
{
	uint32_t u;

	if (rd_u32(r, &u) < 0)
		return -1;
	*out = (int32_t)u;
	return 0;
}

int
rd_i16(struct Reader *r, int16_t *out)
{
	uint16_t u;

	if (rd_u16(r, &u) < 0)
		return -1;
	*out = (int16_t)u;
	return 0;
}

int
rd_f32(struct Reader *r, float *out)
{
	uint32_t u;

	if (rd_u32(r, &u) < 0)
		return -1;
	memcpy(out, &u, 4);
	return 0;
}

int
rd_skip(struct Reader *r, size_t n)
{
	if (rd_remaining(r) < n)
		return -1;
	r->p += n;
	return 0;
}

/*
 * Encode one Unicode code point as UTF-8.  Writes 1..4 bytes to dst
 * and returns the byte count, or 0 if cp is invalid (replaced with
 * U+FFFD by the caller if desired).
 */
static size_t
utf8_encode(uint32_t cp, char dst[4])
{
	if (cp < 0x80) {
		dst[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		dst[0] = (char)(0xC0 | (cp >> 6));
		dst[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		dst[0] = (char)(0xE0 | (cp >> 12));
		dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		dst[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	if (cp < 0x110000) {
		dst[0] = (char)(0xF0 | (cp >> 18));
		dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		dst[3] = (char)(0x80 | (cp & 0x3F));
		return 4;
	}
	return 0;
}

/*
 * Decode one UTF-8 code point from src..src+srclen.  Returns the
 * byte count consumed (1..4) and sets *out_cp.  Returns 0 on
 * malformed input.
 */
static size_t
utf8_decode(const char *src, size_t srclen, uint32_t *out_cp)
{
	const unsigned char *s = (const unsigned char *)src;
	uint32_t cp;
	size_t need;

	if (srclen == 0)
		return 0;
	if ((s[0] & 0x80) == 0) {
		*out_cp = s[0];
		return 1;
	}
	if ((s[0] & 0xE0) == 0xC0) {
		need = 2;
		cp = s[0] & 0x1F;
	} else if ((s[0] & 0xF0) == 0xE0) {
		need = 3;
		cp = s[0] & 0x0F;
	} else if ((s[0] & 0xF8) == 0xF0) {
		need = 4;
		cp = s[0] & 0x07;
	} else {
		return 0;
	}
	if (srclen < need)
		return 0;
	for (size_t i = 1; i < need; i++) {
		if ((s[i] & 0xC0) != 0x80)
			return 0;
		cp = (cp << 6) | (s[i] & 0x3F);
	}
	*out_cp = cp;
	return need;
}

int
rd_str_a(struct Reader *r, char **out)
{
	uint8_t cnt;
	char *buf, *p;
	size_t alloc, used, i;

	if (rd_u8(r, &cnt) < 0)
		return -1;
	if (rd_remaining(r) < (size_t)cnt * 4)
		return -1;
	/* Worst case: 4 UTF-8 bytes per code point + NUL. */
	alloc = (size_t)cnt * 4 + 1;
	buf = malloc(alloc);
	if (buf == NULL)
		return -1;
	p = buf;
	used = 0;
	for (i = 0; i < cnt; i++) {
		uint32_t cp = 0;
		size_t n;
		char tmp[4];

		if (rd_u32(r, &cp) < 0) {
			free(buf);
			return -1;
		}
		n = utf8_encode(cp, tmp);
		if (n == 0) {
			/* Replace invalid with U+FFFD. */
			tmp[0] = (char)0xEF;
			tmp[1] = (char)0xBF;
			tmp[2] = (char)0xBD;
			n = 3;
		}
		memcpy(p + used, tmp, n);
		used += n;
	}
	p[used] = '\0';
	*out = buf;
	return 0;
}

int
rd_str_b(struct Reader *r, char **out)
{
	uint16_t units;
	char *buf;
	size_t alloc, used, i;

	if (rd_u16(r, &units) < 0)
		return -1;
	if (rd_remaining(r) < (size_t)units * 2)
		return -1;
	alloc = (size_t)units * 3 + 1;
	buf = malloc(alloc);
	if (buf == NULL)
		return -1;
	used = 0;
	for (i = 0; i < units; i++) {
		uint16_t u = 0;
		uint32_t cp;
		size_t n;
		char tmp[4];

		if (rd_u16(r, &u) < 0) {
			free(buf);
			return -1;
		}
		cp = u;
		/*
		 * Surrogate pairs are not handled (the protocol does
		 * not appear to use any non-BMP code points).  A
		 * lone surrogate is replaced with U+FFFD.
		 */
		if (cp >= 0xD800 && cp <= 0xDFFF)
			cp = 0xFFFD;
		n = utf8_encode(cp, tmp);
		if (n == 0)
			continue;
		memcpy(buf + used, tmp, n);
		used += n;
	}
	buf[used] = '\0';
	*out = buf;
	return 0;
}

/* ----- writers ---------------------------------------------------- */

int
wr_u8(struct ByteBuf *bb, uint8_t v)
{
	return bb_append_u8(bb, v);
}

int
wr_u16(struct ByteBuf *bb, uint16_t v)
{
	unsigned char b[2];

	b[0] = (unsigned char)(v & 0xff);
	b[1] = (unsigned char)((v >> 8) & 0xff);
	return bb_append(bb, b, 2);
}

int
wr_u32(struct ByteBuf *bb, uint32_t v)
{
	unsigned char b[4];

	b[0] = (unsigned char)(v & 0xff);
	b[1] = (unsigned char)((v >> 8) & 0xff);
	b[2] = (unsigned char)((v >> 16) & 0xff);
	b[3] = (unsigned char)((v >> 24) & 0xff);
	return bb_append(bb, b, 4);
}

int
wr_u64(struct ByteBuf *bb, uint64_t v)
{
	unsigned char b[8];

	b[0] = (unsigned char)(v & 0xff);
	b[1] = (unsigned char)((v >> 8) & 0xff);
	b[2] = (unsigned char)((v >> 16) & 0xff);
	b[3] = (unsigned char)((v >> 24) & 0xff);
	b[4] = (unsigned char)((v >> 32) & 0xff);
	b[5] = (unsigned char)((v >> 40) & 0xff);
	b[6] = (unsigned char)((v >> 48) & 0xff);
	b[7] = (unsigned char)((v >> 56) & 0xff);
	return bb_append(bb, b, 8);
}

int
wr_i32(struct ByteBuf *bb, int32_t v)
{
	return wr_u32(bb, (uint32_t)v);
}

int
wr_i16(struct ByteBuf *bb, int16_t v)
{
	return wr_u16(bb, (uint16_t)v);
}

int
wr_f32(struct ByteBuf *bb, float v)
{
	uint32_t u;

	memcpy(&u, &v, 4);
	return wr_u32(bb, u);
}

int
wr_str_a(struct ByteBuf *bb, const char *s)
{
	size_t len, off;
	uint32_t cps[256];
	uint8_t cnt;
	size_t i;

	if (s == NULL)
		s = "";
	len = strlen(s);

	cnt = 0;
	off = 0;
	while (off < len && cnt < 255) {
		uint32_t cp;
		size_t n;

		n = utf8_decode(s + off, len - off, &cp);
		if (n == 0)
			break;
		cps[cnt++] = cp;
		off += n;
	}

	if (wr_u8(bb, cnt) < 0)
		return -1;
	for (i = 0; i < cnt; i++)
		if (wr_u32(bb, cps[i]) < 0)
			return -1;
	return 0;
}

int
wr_str_b(struct ByteBuf *bb, const char *s)
{
	size_t len, off, units;
	uint16_t buf[1024];

	if (s == NULL)
		s = "";
	len = strlen(s);

	units = 0;
	off = 0;
	while (off < len && units < 1024) {
		uint32_t cp;
		size_t n;

		n = utf8_decode(s + off, len - off, &cp);
		if (n == 0)
			break;
		off += n;
		if (cp >= 0x10000) {
			/* Non-BMP -> replace with U+FFFD; the protocol
			 * does not appear to use surrogate pairs. */
			cp = 0xFFFD;
		}
		buf[units++] = (uint16_t)cp;
	}

	if (wr_u16(bb, (uint16_t)units) < 0)
		return -1;
	for (size_t i = 0; i < units; i++)
		if (wr_u16(bb, buf[i]) < 0)
			return -1;
	return 0;
}
