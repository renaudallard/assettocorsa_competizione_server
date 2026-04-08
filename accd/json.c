/*
 * json.c -- recursive-descent JSON parser.
 *
 * One slab allocation per parse holds nodes, pair arrays, item
 * arrays, and string bytes.  The slab grows on demand via
 * realloc.  All pointers into the slab are stable for the
 * lifetime of the slab (we never relocate after the parse
 * returns).
 *
 * The parser is reentrant.  No global state.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

/*
 * The slab is pre-sized so it never has to realloc during the
 * parse: a realloc would invalidate every pointer (node, key,
 * value, items array) that the recursive walker has already
 * returned.  Empirically the parse tree for ACC config files
 * fits comfortably in 16 * input_len, with a 4 KiB minimum.
 */
#define SLAB_OVERHEAD	16
#define SLAB_MIN	4096

struct slab {
	char	*data;
	size_t	 cap;
	size_t	 used;
};

struct parser {
	const char	*src;
	size_t		 pos;
	size_t		 len;
	struct slab	 slab;
	char		*err;
	size_t		 errsz;
};

/*
 * Header on every slab allocation: lets json_free() locate the
 * single slab from the root node by walking back to the slab
 * header pointer stashed at slab->data[-sizeof(struct slab)].
 *
 * Simpler approach: stash the slab pointer inside the root
 * json_node by embedding it in a wrapper.  We do this with a
 * "secret" allocation: the slab itself is malloc'd, and the
 * first sizeof(struct slab) bytes hold the slab header.  The
 * root node lives at slab+sizeof(struct slab).  json_free()
 * recovers the slab by subtracting that offset.
 */
struct slab_header {
	struct slab	 slab;
};

static void
set_err(struct parser *p, const char *msg)
{
	if (p->err == NULL || p->errsz == 0)
		return;
	snprintf(p->err, p->errsz, "json: %s at offset %zu", msg, p->pos);
}

static void *
slab_alloc(struct parser *p, size_t bytes)
{
	void *ptr;

	/* 8-byte align */
	bytes = (bytes + 7) & ~(size_t)7;
	if (p->slab.used + bytes > p->slab.cap) {
		set_err(p, "slab exhausted");
		return NULL;
	}
	ptr = p->slab.data + p->slab.used;
	p->slab.used += bytes;
	memset(ptr, 0, bytes);
	return ptr;
}

static struct json_node *
new_node(struct parser *p, enum json_kind k)
{
	struct json_node *n = slab_alloc(p, sizeof(*n));

	if (n != NULL)
		n->kind = k;
	return n;
}

static void
skip_ws(struct parser *p)
{
	while (p->pos < p->len) {
		char c = p->src[p->pos];

		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			p->pos++;
			continue;
		}
		/*
		 * Tolerate Kunos UTF-16 BOM that survives the
		 * iconv conversion or trailing zeros in the
		 * source buffer.
		 */
		if (c == '\0' || c == (char)0xfe || c == (char)0xff) {
			p->pos++;
			continue;
		}
		break;
	}
}

static int
peek(struct parser *p)
{
	skip_ws(p);
	if (p->pos >= p->len)
		return -1;
	return (unsigned char)p->src[p->pos];
}

static int
expect(struct parser *p, char c)
{
	if (peek(p) != (unsigned char)c) {
		set_err(p, "expected character");
		return -1;
	}
	p->pos++;
	return 0;
}

static struct json_node *parse_value(struct parser *p);

/*
 * Compute the upper-bound output size of a string literal so we
 * can allocate it in one shot from the slab.  Returns the byte
 * count not including the NUL terminator.  Returns SIZE_MAX on
 * malformed input (caller will detect and report).
 */
static size_t
estimate_string_bytes(struct parser *p)
{
	size_t pos = p->pos + 1;	/* skip opening quote */
	size_t out = 0;

	while (pos < p->len) {
		unsigned char c = (unsigned char)p->src[pos];

		if (c == '"')
			return out;
		if (c == '\\') {
			pos++;
			if (pos >= p->len)
				return SIZE_MAX;
			if (p->src[pos] == 'u') {
				/* worst case: 3 UTF-8 bytes for BMP */
				out += 3;
				pos += 5;
			} else {
				out += 1;
				pos += 1;
			}
			continue;
		}
		out += 1;
		pos += 1;
	}
	return SIZE_MAX;
}

/*
 * Parse a string literal into a freshly slab-allocated
 * NUL-terminated buffer.  Returns the start pointer or NULL
 * on error.  Handles \" \\ \/ \b \f \n \r \t and \uXXXX (BMP).
 *
 * Allocates the worst-case size in one shot so we never need
 * to grow mid-parse.
 */
static const char *
parse_string(struct parser *p)
{
	char *out;
	size_t bound, used;

	/* Skip whitespace so estimate_string_bytes sees the
	 * opening quote at p->pos. */
	skip_ws(p);
	bound = estimate_string_bytes(p);
	if (bound == SIZE_MAX) {
		set_err(p, "unterminated string");
		return NULL;
	}

	if (expect(p, '"') < 0)
		return NULL;

	out = slab_alloc(p, bound + 1);
	if (out == NULL)
		return NULL;
	used = 0;

	while (p->pos < p->len) {
		unsigned char c = (unsigned char)p->src[p->pos];

		if (c == '"') {
			p->pos++;
			out[used] = '\0';
			return out;
		}
		if (c == '\\') {
			p->pos++;
			if (p->pos >= p->len) {
				set_err(p, "trailing backslash");
				return NULL;
			}
			c = (unsigned char)p->src[p->pos++];
			switch (c) {
			case '"':  c = '"'; break;
			case '\\': c = '\\'; break;
			case '/':  c = '/'; break;
			case 'b':  c = '\b'; break;
			case 'f':  c = '\f'; break;
			case 'n':  c = '\n'; break;
			case 'r':  c = '\r'; break;
			case 't':  c = '\t'; break;
			case 'u': {
				unsigned int cp = 0;
				int i;

				if (p->pos + 4 > p->len) {
					set_err(p, "short unicode escape");
					return NULL;
				}
				for (i = 0; i < 4; i++) {
					char hc = p->src[p->pos++];
					unsigned int v;
					if (hc >= '0' && hc <= '9')
						v = (unsigned int)(hc - '0');
					else if (hc >= 'a' && hc <= 'f')
						v = (unsigned int)(hc - 'a' + 10);
					else if (hc >= 'A' && hc <= 'F')
						v = (unsigned int)(hc - 'A' + 10);
					else {
						set_err(p, "bad hex digit");
						return NULL;
					}
					cp = (cp << 4) | v;
				}
				if (cp < 0x80) {
					out[used++] = (char)cp;
				} else if (cp < 0x800) {
					out[used++] = (char)(0xC0 | (cp >> 6));
					out[used++] = (char)(0x80 | (cp & 0x3F));
				} else {
					out[used++] = (char)(0xE0 | (cp >> 12));
					out[used++] = (char)(0x80 | ((cp >> 6) & 0x3F));
					out[used++] = (char)(0x80 | (cp & 0x3F));
				}
				continue;
			}
			default:
				set_err(p, "bad escape");
				return NULL;
			}
			out[used++] = (char)c;
			continue;
		}
		if (c < 0x20) {
			set_err(p, "control char in string");
			return NULL;
		}
		p->pos++;
		out[used++] = (char)c;
	}
	set_err(p, "unterminated string");
	return NULL;
}

static struct json_node *
parse_number(struct parser *p)
{
	struct json_node *n;
	char *end;
	double v;
	const char *start = p->src + p->pos;

	v = strtod(start, &end);
	if (end == start) {
		set_err(p, "bad number");
		return NULL;
	}
	p->pos += (size_t)(end - start);
	n = new_node(p, JSON_NUM);
	if (n != NULL)
		n->u.num = v;
	return n;
}

static struct json_node *
parse_object(struct parser *p)
{
	struct json_node *obj;
	struct json_pair *pairs = NULL;
	size_t cap = 0, count = 0;

	if (expect(p, '{') < 0)
		return NULL;
	obj = new_node(p, JSON_OBJ);
	if (obj == NULL)
		return NULL;
	if (peek(p) == '}') {
		p->pos++;
		return obj;
	}
	for (;;) {
		const char *key;
		struct json_node *val;

		key = parse_string(p);
		if (key == NULL)
			return NULL;
		if (expect(p, ':') < 0)
			return NULL;
		val = parse_value(p);
		if (val == NULL)
			return NULL;

		if (count == cap) {
			size_t newcap = cap == 0 ? 8 : cap * 2;
			struct json_pair *np;

			np = slab_alloc(p, newcap * sizeof(*np));
			if (np == NULL)
				return NULL;
			if (pairs != NULL)
				memcpy(np, pairs, count * sizeof(*np));
			pairs = np;
			cap = newcap;
		}
		pairs[count].key = key;
		pairs[count].val = val;
		count++;

		if (peek(p) == ',') {
			p->pos++;
			continue;
		}
		if (expect(p, '}') < 0)
			return NULL;
		break;
	}
	obj->u.obj.pairs = pairs;
	obj->u.obj.count = count;
	return obj;
}

static struct json_node *
parse_array(struct parser *p)
{
	struct json_node *arr;
	struct json_node **items = NULL;
	size_t cap = 0, count = 0;

	if (expect(p, '[') < 0)
		return NULL;
	arr = new_node(p, JSON_ARR);
	if (arr == NULL)
		return NULL;
	if (peek(p) == ']') {
		p->pos++;
		return arr;
	}
	for (;;) {
		struct json_node *val = parse_value(p);

		if (val == NULL)
			return NULL;
		if (count == cap) {
			size_t newcap = cap == 0 ? 8 : cap * 2;
			struct json_node **ni;

			ni = slab_alloc(p, newcap * sizeof(*ni));
			if (ni == NULL)
				return NULL;
			if (items != NULL)
				memcpy(ni, items, count * sizeof(*ni));
			items = ni;
			cap = newcap;
		}
		items[count++] = val;
		if (peek(p) == ',') {
			p->pos++;
			continue;
		}
		if (expect(p, ']') < 0)
			return NULL;
		break;
	}
	arr->u.arr.items = items;
	arr->u.arr.count = count;
	return arr;
}

static struct json_node *
parse_keyword(struct parser *p, const char *kw, size_t klen,
    enum json_kind k, int boolval)
{
	struct json_node *n;

	if (p->pos + klen > p->len ||
	    memcmp(p->src + p->pos, kw, klen) != 0) {
		set_err(p, "bad keyword");
		return NULL;
	}
	p->pos += klen;
	n = new_node(p, k);
	if (n != NULL && k == JSON_BOOL)
		n->u.b = boolval;
	return n;
}

static struct json_node *
parse_value(struct parser *p)
{
	int c = peek(p);

	if (c < 0) {
		set_err(p, "unexpected end of input");
		return NULL;
	}
	switch (c) {
	case '{':
		return parse_object(p);
	case '[':
		return parse_array(p);
	case '"': {
		const char *s = parse_string(p);
		struct json_node *n;

		if (s == NULL)
			return NULL;
		n = new_node(p, JSON_STR);
		if (n != NULL)
			n->u.s = s;
		return n;
	}
	case 't':
		return parse_keyword(p, "true", 4, JSON_BOOL, 1);
	case 'f':
		return parse_keyword(p, "false", 5, JSON_BOOL, 0);
	case 'n':
		return parse_keyword(p, "null", 4, JSON_NULL, 0);
	default:
		if (c == '-' || (c >= '0' && c <= '9'))
			return parse_number(p);
		set_err(p, "unexpected character");
		return NULL;
	}
}

struct json_node *
json_parse(const char *src, size_t len, char *err, size_t errsz)
{
	struct parser p;
	struct slab_header *hdr;
	struct json_node *root;
	size_t cap;

	memset(&p, 0, sizeof(p));
	p.src = src;
	p.len = len;
	p.err = err;
	p.errsz = errsz;

	cap = len * SLAB_OVERHEAD;
	if (cap < SLAB_MIN)
		cap = SLAB_MIN;
	p.slab.cap = cap;
	p.slab.data = malloc(p.slab.cap);
	if (p.slab.data == NULL) {
		if (err != NULL && errsz > 0)
			snprintf(err, errsz, "json: out of memory");
		return NULL;
	}
	p.slab.used = 0;

	/* Reserve the slab header at the start of the slab so
	 * json_free() can find it from the root pointer. */
	hdr = (struct slab_header *)p.slab.data;
	p.slab.used = sizeof(*hdr);

	root = parse_value(&p);
	if (root == NULL) {
		free(p.slab.data);
		return NULL;
	}
	skip_ws(&p);
	/* Trailing junk is tolerated -- many configs have a final
	 * newline or BOM. */

	hdr->slab = p.slab;
	return root;
}

void
json_free(struct json_node *root)
{
	struct slab_header *hdr;
	char *base;

	if (root == NULL)
		return;
	/*
	 * The root node is bump-allocated immediately after the
	 * slab header.  Walk back to find the header.
	 */
	base = (char *)root - sizeof(struct slab_header);
	hdr = (struct slab_header *)base;
	free(hdr->slab.data);
}

/* ----- accessors -------------------------------------------------- */

const struct json_node *
json_obj_get(const struct json_node *obj, const char *key)
{
	size_t i;

	if (obj == NULL || obj->kind != JSON_OBJ || key == NULL)
		return NULL;
	for (i = 0; i < obj->u.obj.count; i++)
		if (strcmp(obj->u.obj.pairs[i].key, key) == 0)
			return obj->u.obj.pairs[i].val;
	return NULL;
}

const char *
json_obj_get_str(const struct json_node *obj, const char *key)
{
	const struct json_node *n = json_obj_get(obj, key);

	if (n == NULL || n->kind != JSON_STR)
		return NULL;
	return n->u.s;
}

int
json_obj_get_int(const struct json_node *obj, const char *key, int def)
{
	const struct json_node *n = json_obj_get(obj, key);

	if (n == NULL)
		return def;
	if (n->kind == JSON_NUM)
		return (int)n->u.num;
	if (n->kind == JSON_BOOL)
		return n->u.b;
	return def;
}

double
json_obj_get_num(const struct json_node *obj, const char *key, double def)
{
	const struct json_node *n = json_obj_get(obj, key);

	if (n == NULL || n->kind != JSON_NUM)
		return def;
	return n->u.num;
}

int
json_obj_get_bool(const struct json_node *obj, const char *key, int def)
{
	const struct json_node *n = json_obj_get(obj, key);

	if (n == NULL)
		return def;
	if (n->kind == JSON_BOOL)
		return n->u.b;
	if (n->kind == JSON_NUM)
		return n->u.num != 0;
	return def;
}

size_t
json_arr_len(const struct json_node *arr)
{
	if (arr == NULL || arr->kind != JSON_ARR)
		return 0;
	return arr->u.arr.count;
}

const struct json_node *
json_arr_at(const struct json_node *arr, size_t i)
{
	if (arr == NULL || arr->kind != JSON_ARR || i >= arr->u.arr.count)
		return NULL;
	return arr->u.arr.items[i];
}
