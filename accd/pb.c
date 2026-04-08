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
