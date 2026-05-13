/*
 * libFuzzer harness for accd's JSON parser (json.c).
 *
 * The parser is the only place where accd reads untrusted bytes off
 * disk (cfg/*.json files an operator can fat-finger or an attacker
 * could swap in if they got file-write on the cfg dir).  Bugs there
 * could surface as a crash on startup.
 *
 * Build:
 *   clang -fsanitize=fuzzer,address,undefined -O1 -g -I. \
 *     fuzz/fuzz_json.c json.c -o fuzz_json
 * Run:
 *   ./fuzz_json -max_total_time=60
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "json.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char err[256] = "";
	struct json_node *root;

	root = json_parse((const char *)data, size, err, sizeof(err));
	if (root != NULL)
		json_free(root);
	return 0;
}
