/*
 * pb.h -- minimal write-only protobuf encoder.
 *
 * Just enough to encode the seven ServerMonitor message types
 * (msg ids 0x01..0x07) without dragging in nanopb or protobuf-c.
 *
 * Wire types per protobuf spec:
 *   0  varint  (int32, int64, uint32, uint64, bool, enum)
 *   2  length-delimited (string, bytes, embedded message)
 *   5  fixed32 (sfixed32, fixed32, float)
 *   1  fixed64 (sfixed64, fixed64, double)
 *
 * Submessage encoding uses a length backpatch.  pb_sub_begin
 * reserves a fixed 5-byte varint placeholder for the length;
 * pb_sub_end fills it in.  Slightly wasteful but branch-free.
 *
 * All output goes to a ByteBuf supplied by the caller.
 */

#ifndef ACCD_PB_H
#define ACCD_PB_H

#include <stddef.h>
#include <stdint.h>

#include "io.h"

#define PB_WIRE_VARINT		0
#define PB_WIRE_FIXED64		1
#define PB_WIRE_LENGTH_DELIM	2
#define PB_WIRE_FIXED32		5

int	pb_w_varint(struct ByteBuf *bb, uint64_t v);
int	pb_w_tag(struct ByteBuf *bb, int field, int wire);

int	pb_w_int32(struct ByteBuf *bb, int field, int32_t v);
int	pb_w_int64(struct ByteBuf *bb, int field, int64_t v);
int	pb_w_uint32(struct ByteBuf *bb, int field, uint32_t v);
int	pb_w_uint64(struct ByteBuf *bb, int field, uint64_t v);
int	pb_w_bool(struct ByteBuf *bb, int field, int v);
int	pb_w_enum(struct ByteBuf *bb, int field, int v);
int	pb_w_string(struct ByteBuf *bb, int field, const char *s);
int	pb_w_bytes(struct ByteBuf *bb, int field, const void *p, size_t n);
int	pb_w_fixed32(struct ByteBuf *bb, int field, uint32_t v);
int	pb_w_float(struct ByteBuf *bb, int field, float v);

/*
 * Begin a submessage at field number `field`.  Writes the tag,
 * a 5-byte placeholder for the length, and records the position
 * in *out_start so pb_sub_end can backpatch.  Caller writes the
 * submessage body in between, then calls pb_sub_end(bb,
 * *out_start).
 */
int	pb_sub_begin(struct ByteBuf *bb, int field, size_t *out_start);
int	pb_sub_end(struct ByteBuf *bb, size_t start);

#endif /* ACCD_PB_H */
