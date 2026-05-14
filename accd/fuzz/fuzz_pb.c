/*
 * libFuzzer harness for accd's protobuf reader primitives (pb.c).
 *
 * The pb_r_* family parses untrusted protobuf bytes off the wire for
 * the SMPR connection-request decode (smpr_handle_connect ->
 * pb_r_tag / pb_r_int32 / pb_r_bool / pb_r_string / pb_r_skip).  Bugs
 * there could let a malformed handshake crash the server or leak
 * adjacent heap (the SIZE_MAX-varint wraparound we fixed in commit
 * 5c4bfa0 is exactly the class this harness catches).
 *
 * The driver walks every field in the input with pb_r_tag + dispatch
 * on the wire type so the reader is exercised on the same code path
 * as smpr.c, including pb_r_skip for unknown / dropped fields.
 *
 * Build:
 *   clang -fsanitize=fuzzer,address,undefined -O1 -g -I. \
 *     fuzz/fuzz_pb.c pb.c -o fuzz_pb
 * Run:
 *   ./fuzz_pb -max_total_time=60
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "pb.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct PbReader r;
	char buf[256];

	pb_r_init(&r, data, size);
	while (!pb_r_eof(&r)) {
		uint32_t field, wire;
		int32_t i32;
		int b;

		if (pb_r_tag(&r, &field, &wire) < 0)
			break;
		switch (wire) {
		case PB_WIRE_VARINT:
			/*
			 * Hit both the int32-typed reader and the bool
			 * reader on alternating fields to exercise the
			 * sign-extension + truncation paths.
			 */
			if ((field & 1) == 0)
				(void)pb_r_int32(&r, &i32);
			else
				(void)pb_r_bool(&r, &b);
			break;
		case PB_WIRE_LENGTH_DELIM:
			(void)pb_r_string(&r, buf, sizeof buf);
			break;
		default:
			(void)pb_r_skip(&r, wire);
			break;
		}
		if (r.error)
			break;
	}
	return 0;
}
