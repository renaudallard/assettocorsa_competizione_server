/*
 * json.h -- minimal recursive-descent JSON parser.
 *
 * Builds an in-memory tree from a UTF-8 byte buffer.  Supports
 * objects, arrays, strings, numbers, booleans, null.  String
 * escapes \" \\ \/ \b \f \n \r \t \uXXXX are decoded.
 *
 * Storage strategy: one slab allocation per parse.  Nodes are
 * bump-allocated from the slab; strings are unescaped in-place
 * when possible or copied into a separate slab region.  The
 * caller frees the entire tree with json_free() in O(1).
 *
 * The parser is intentionally minimal: no streaming, no UTF-16
 * surrogate pair handling beyond a basic check, no number
 * precision beyond what strtod can do.  Good enough for the
 * ACC config files which are small (entrylist.json is the
 * largest at maybe 10 KB for a full grid).
 */

#ifndef ACCD_JSON_H
#define ACCD_JSON_H

#include <stddef.h>

enum json_kind {
	JSON_NULL = 0,
	JSON_BOOL,
	JSON_NUM,
	JSON_STR,
	JSON_ARR,
	JSON_OBJ
};

struct json_node;

struct json_pair {
	const char	*key;	/* NUL-terminated, owned by the slab */
	struct json_node *val;
};

struct json_node {
	enum json_kind	kind;
	union {
		int	 b;	/* JSON_BOOL */
		double	 num;	/* JSON_NUM */
		const char *s;	/* JSON_STR (NUL-terminated) */
		struct {
			struct json_node **items;
			size_t		 count;
		} arr;
		struct {
			struct json_pair *pairs;
			size_t		 count;
		} obj;
	} u;
};

/*
 * Parse a UTF-8 JSON document.  Returns the root node on success
 * (always non-NULL), or NULL on error with err filled in.
 *
 * The returned tree must be freed with json_free().
 */
struct json_node *
	json_parse(const char *src, size_t len, char *err, size_t errsz);

/* Release the entire tree returned by json_parse. */
void	json_free(struct json_node *root);

/* ----- accessors -------------------------------------------------- */

/* Object lookups: return NULL/default if key missing or wrong kind. */
const struct json_node *
	json_obj_get(const struct json_node *obj, const char *key);
const char *
	json_obj_get_str(const struct json_node *obj, const char *key);
int	json_obj_get_int(const struct json_node *obj, const char *key, int def);
double	json_obj_get_num(const struct json_node *obj, const char *key, double def);
int	json_obj_get_bool(const struct json_node *obj, const char *key, int def);

/* Array accessors. */
size_t	json_arr_len(const struct json_node *arr);
const struct json_node *
	json_arr_at(const struct json_node *arr, size_t i);

#endif /* ACCD_JSON_H */
