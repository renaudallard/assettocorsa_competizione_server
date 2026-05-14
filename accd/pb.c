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
 * pb.c -- minimal protobuf encoder.
 *
 * The 5-byte fixed varint trick: when we don't yet know the
 * submessage length, we reserve five bytes (enough to encode
 * any 32-bit length).  At pb_sub_end we know the length and
 * write it as a non-canonical 5-byte varint by setting the
 * MSB on the first four bytes.  Receivers tolerate this
 * because the protobuf wire format does not require canonical
 * varint encoding.
 */

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "io.h"
#include "pb.h"

int
pb_w_varint(struct ByteBuf *bb, uint64_t v)
{
	unsigned char buf[10];
	size_t n = 0;

	while (v >= 0x80) {
		buf[n++] = (unsigned char)((v & 0x7f) | 0x80);
		v >>= 7;
	}
	buf[n++] = (unsigned char)v;
	return bb_append(bb, buf, n);
}

int
pb_w_tag(struct ByteBuf *bb, int field, int wire)
{
	uint64_t key = ((uint64_t)field << 3) | (uint64_t)wire;

	return pb_w_varint(bb, key);
}

int
pb_w_int32(struct ByteBuf *bb, int field, int32_t v)
{
	if (pb_w_tag(bb, field, PB_WIRE_VARINT) < 0)
		return -1;
	/* Negative int32 is encoded as a 10-byte varint per spec. */
	return pb_w_varint(bb, (uint64_t)(int64_t)v);
}

int
pb_w_int64(struct ByteBuf *bb, int field, int64_t v)
{
	if (pb_w_tag(bb, field, PB_WIRE_VARINT) < 0)
		return -1;
	return pb_w_varint(bb, (uint64_t)v);
}

int
pb_w_uint32(struct ByteBuf *bb, int field, uint32_t v)
{
	if (pb_w_tag(bb, field, PB_WIRE_VARINT) < 0)
		return -1;
	return pb_w_varint(bb, (uint64_t)v);
}

int
pb_w_uint64(struct ByteBuf *bb, int field, uint64_t v)
{
	if (pb_w_tag(bb, field, PB_WIRE_VARINT) < 0)
		return -1;
	return pb_w_varint(bb, v);
}

int
pb_w_bool(struct ByteBuf *bb, int field, int v)
{
	if (pb_w_tag(bb, field, PB_WIRE_VARINT) < 0)
		return -1;
	return pb_w_varint(bb, v ? 1 : 0);
}

int
pb_w_enum(struct ByteBuf *bb, int field, int v)
{
	return pb_w_int32(bb, field, v);
}

int
pb_w_string(struct ByteBuf *bb, int field, const char *s)
{
	size_t len = s != NULL ? strlen(s) : 0;

	if (pb_w_tag(bb, field, PB_WIRE_LENGTH_DELIM) < 0)
		return -1;
	if (pb_w_varint(bb, (uint64_t)len) < 0)
		return -1;
	if (len > 0)
		return bb_append(bb, s, len);
	return 0;
}

int
pb_w_bytes(struct ByteBuf *bb, int field, const void *p, size_t n)
{
	if (pb_w_tag(bb, field, PB_WIRE_LENGTH_DELIM) < 0)
		return -1;
	if (pb_w_varint(bb, (uint64_t)n) < 0)
		return -1;
	if (n > 0)
		return bb_append(bb, p, n);
	return 0;
}

int
pb_w_fixed32(struct ByteBuf *bb, int field, uint32_t v)
{
	unsigned char b[4];

	if (pb_w_tag(bb, field, PB_WIRE_FIXED32) < 0)
		return -1;
	b[0] = (unsigned char)(v & 0xff);
	b[1] = (unsigned char)((v >> 8) & 0xff);
	b[2] = (unsigned char)((v >> 16) & 0xff);
	b[3] = (unsigned char)((v >> 24) & 0xff);
	return bb_append(bb, b, 4);
}

int
pb_w_float(struct ByteBuf *bb, int field, float v)
{
	uint32_t u;

	memcpy(&u, &v, 4);
	return pb_w_fixed32(bb, field, u);
}

/*
 * Submessage with backpatched length.  Reserves a 5-byte
 * placeholder where the length varint will be written.
 */
int
pb_sub_begin(struct ByteBuf *bb, int field, size_t *out_start)
{
	unsigned char placeholder[5] = { 0x80, 0x80, 0x80, 0x80, 0x00 };

	if (pb_w_tag(bb, field, PB_WIRE_LENGTH_DELIM) < 0)
		return -1;
	*out_start = bb->wpos;
	return bb_append(bb, placeholder, 5);
}

int
pb_sub_end(struct ByteBuf *bb, size_t start)
{
	size_t body_len;
	uint32_t v;

	if (start + 5 > bb->wpos)
		return -1;
	body_len = bb->wpos - (start + 5);
	v = (uint32_t)body_len;

	/*
	 * Write a non-canonical 5-byte varint over the placeholder
	 * bytes.  The receiver accepts redundant high bytes.
	 */
	bb->data[start + 0] = (unsigned char)((v        & 0x7f) | 0x80);
	bb->data[start + 1] = (unsigned char)(((v >>  7) & 0x7f) | 0x80);
	bb->data[start + 2] = (unsigned char)(((v >> 14) & 0x7f) | 0x80);
	bb->data[start + 3] = (unsigned char)(((v >> 21) & 0x7f) | 0x80);
	bb->data[start + 4] = (unsigned char)((v >> 28) & 0x0f);
	return 0;
}

/* ----- read side --------------------------------------------------- */

void
pb_r_init(struct PbReader *r, const void *buf, size_t len)
{
	r->buf = (const unsigned char *)buf;
	r->pos = 0;
	r->end = len;
	r->error = 0;
}

int
pb_r_eof(const struct PbReader *r)
{
	return r->error != 0 || r->pos >= r->end;
}

int
pb_r_varint(struct PbReader *r, uint64_t *out)
{
	uint64_t v = 0;
	int shift = 0;

	if (r->error)
		return -1;
	/* Protobuf varints are at most 10 bytes (64-bit value). */
	while (shift < 64) {
		unsigned char b;

		if (r->pos >= r->end) {
			r->error = 1;
			return -1;
		}
		b = r->buf[r->pos++];
		v |= (uint64_t)(b & 0x7f) << shift;
		if ((b & 0x80) == 0) {
			*out = v;
			return 0;
		}
		shift += 7;
	}
	r->error = 1;
	return -1;
}

int
pb_r_tag(struct PbReader *r, uint32_t *field, uint32_t *wire)
{
	uint64_t key;

	if (pb_r_varint(r, &key) < 0)
		return -1;
	*field = (uint32_t)(key >> 3);
	*wire = (uint32_t)(key & 0x07);
	return 0;
}

int
pb_r_int32(struct PbReader *r, int32_t *out)
{
	uint64_t v;

	if (pb_r_varint(r, &v) < 0)
		return -1;
	*out = (int32_t)(int64_t)v;
	return 0;
}

int
pb_r_bool(struct PbReader *r, int *out)
{
	uint64_t v;

	if (pb_r_varint(r, &v) < 0)
		return -1;
	*out = v != 0;
	return 0;
}

int
pb_r_string(struct PbReader *r, char *out, size_t outsz)
{
	uint64_t len64;
	size_t len, copy;

	if (pb_r_varint(r, &len64) < 0)
		return -1;
	len = (size_t)len64;
	/*
	 * Bounds check via subtraction: r->pos <= r->end is invariant,
	 * so `r->end - r->pos` is a non-wrapping remaining-bytes count.
	 * The earlier `r->pos + len > r->end` form wrapped modulo
	 * SIZE_MAX for an attacker-supplied UINT64_MAX varint and let
	 * the memcpy below read past the body.
	 */
	if (len64 != (uint64_t)len || len > r->end - r->pos) {
		r->error = 1;
		return -1;
	}
	if (outsz > 0) {
		copy = len < outsz - 1 ? len : outsz - 1;
		if (copy > 0)
			memcpy(out, r->buf + r->pos, copy);
		out[copy] = '\0';
	}
	r->pos += len;
	return 0;
}

int
pb_r_skip(struct PbReader *r, uint32_t wire)
{
	uint64_t v;

	if (r->error)
		return -1;
	switch (wire) {
	case PB_WIRE_VARINT:
		return pb_r_varint(r, &v);
	case PB_WIRE_FIXED64:
		if (r->pos + 8 > r->end) {
			r->error = 1;
			return -1;
		}
		r->pos += 8;
		return 0;
	case PB_WIRE_LENGTH_DELIM:
		if (pb_r_varint(r, &v) < 0)
			return -1;
		if (v != (uint64_t)(size_t)v ||
		    (size_t)v > r->end - r->pos) {
			r->error = 1;
			return -1;
		}
		r->pos += (size_t)v;
		return 0;
	case PB_WIRE_FIXED32:
		if (r->pos + 4 > r->end) {
			r->error = 1;
			return -1;
		}
		r->pos += 4;
		return 0;
	default:
		/* Unknown wire type (3/4 are deprecated groups). */
		r->error = 1;
		return -1;
	}
}
