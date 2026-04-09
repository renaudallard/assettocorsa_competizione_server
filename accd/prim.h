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
 * prim.h -- primitive reader / writer for the ACC sim protocol.
 *
 * The wire format is little-endian throughout.  Two distinct
 * wide-string encodings exist:
 *
 *   Format A:  u8  length-in-codepoints
 *              u32 codepoints[length] (UTF-32 LE, padded if BMP)
 *
 *   Format B:  u16 length-in-units
 *              u16 units[length]      (UTF-16 LE, no surrogates assumed)
 *
 * For convenience the readers / writers operate on a (cursor, end)
 * pair held in a Reader struct.  All readers fail-soft: on short
 * input they return -1 and leave the cursor untouched.
 *
 * Strings are returned as malloc'd UTF-8 (NUL-terminated).  Caller
 * frees them.
 */

#ifndef ACCD_PRIM_H
#define ACCD_PRIM_H

#include <stddef.h>
#include <stdint.h>

#include "io.h"

struct Reader {
	const unsigned char	*p;	/* current cursor */
	const unsigned char	*end;	/* one past last valid byte */
};

void	rd_init(struct Reader *r, const void *body, size_t len);
size_t	rd_remaining(const struct Reader *r);
int	rd_eof(const struct Reader *r);

int	rd_u8(struct Reader *r, uint8_t *out);
int	rd_u16(struct Reader *r, uint16_t *out);
int	rd_u32(struct Reader *r, uint32_t *out);
int	rd_u64(struct Reader *r, uint64_t *out);
int	rd_i32(struct Reader *r, int32_t *out);
int	rd_i16(struct Reader *r, int16_t *out);
int	rd_f32(struct Reader *r, float *out);

/*
 * Read a Format-A wide string.  On success returns 0 and sets
 * *out to a freshly-malloc'd UTF-8 NUL-terminated string.  On
 * failure returns -1 and *out is untouched.
 */
int	rd_str_a(struct Reader *r, char **out);

/*
 * Read a Format-B wide string (UTF-16 LE).  Same semantics as
 * rd_str_a.
 */
int	rd_str_b(struct Reader *r, char **out);

/*
 * Skip n bytes.  Returns 0 on success, -1 if not enough data.
 */
int	rd_skip(struct Reader *r, size_t n);

/* ----- writers (append to a ByteBuf) ------------------------------ */

int	wr_u8(struct ByteBuf *bb, uint8_t v);
int	wr_u16(struct ByteBuf *bb, uint16_t v);
int	wr_u32(struct ByteBuf *bb, uint32_t v);
int	wr_u64(struct ByteBuf *bb, uint64_t v);
int	wr_i32(struct ByteBuf *bb, int32_t v);
int	wr_i16(struct ByteBuf *bb, int16_t v);
int	wr_f32(struct ByteBuf *bb, float v);

/*
 * Write a Format-A string.  s must be a NUL-terminated UTF-8
 * string.  Length is encoded as u8 codepoint count (so any string
 * longer than 255 codepoints is truncated).
 */
int	wr_str_a(struct ByteBuf *bb, const char *s);

/*
 * Write a Format-B string.  s must be a NUL-terminated UTF-8
 * string.  Length is encoded as u16 unit count.
 */
int	wr_str_b(struct ByteBuf *bb, const char *s);

/*
 * Write a u16-byte-length-prefixed raw string.  Used in the 0x0b
 * welcome trailer where the server sends plain UTF-8 bytes, not
 * the Format-A / Format-B wide encodings.
 */
int	wr_str_raw(struct ByteBuf *bb, const char *s);

#endif /* ACCD_PRIM_H */
