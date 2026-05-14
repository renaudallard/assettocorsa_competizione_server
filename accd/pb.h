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

/*
 * Read-side primitives.  Symmetric to the writers above, used by
 * SMPR to decode the ServerMonitorConnectionRequest the monitor
 * sends as its first frame.  All readers return 0 on success and
 * -1 on bounds-overrun / malformed input; the PbReader's error
 * flag latches so callers can chain reads and check once at the
 * end.
 */
struct PbReader {
	const unsigned char	*buf;
	size_t			 pos;
	size_t			 end;
	int			 error;
};

void	pb_r_init(struct PbReader *r, const void *buf, size_t len);
int	pb_r_eof(const struct PbReader *r);
int	pb_r_varint(struct PbReader *r, uint64_t *out);
int	pb_r_tag(struct PbReader *r, uint32_t *field, uint32_t *wire);
int	pb_r_int32(struct PbReader *r, int32_t *out);
int	pb_r_bool(struct PbReader *r, int *out);

/*
 * pb_r_string copies up to outsz-1 bytes of the length-delimited
 * string and null-terminates.  Oversized strings get truncated and
 * the reader cursor still advances past the full encoded length so
 * subsequent fields stay aligned.
 */
int	pb_r_string(struct PbReader *r, char *out, size_t outsz);

/* Skip an unknown field of the given wire type (consumes the value
 * after the tag has already been read). */
int	pb_r_skip(struct PbReader *r, uint32_t wire);

#endif /* ACCD_PB_H */
