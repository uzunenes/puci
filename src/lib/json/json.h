#ifndef PUCI_INTERNAL_JSON_H
#define PUCI_INTERNAL_JSON_H

/*
 * json/json.h - internal JSON DOM parser.
 *
 * Parses untrusted JSON (from the wire) into an arena-allocated value tree and
 * provides defensive navigation helpers. The parser is a hand-written
 * recursive-descent parser tuned to the exact shapes libpuci consumes. It must
 * never read out of bounds and must produce a clean parse error (return -1)
 * instead of crashing on malformed input.
 *
 * All values and strings are allocated in the supplied arena and are valid
 * until the arena is reset or destroyed.
 *
 * Not thread-safe.
 */

#include <stddef.h>
#include "util/arena.h"

typedef enum pj_type {
    PJ_NULL = 0,
    PJ_BOOL,
    PJ_NUM,
    PJ_STR,
    PJ_ARR,
    PJ_OBJ,
} pj_type;

typedef struct pj_val pj_val;

struct pj_val {
    pj_type type;
    union {
        int bval;
        double num;
        struct {
            char *s;
            size_t len;
        } str;
        struct {
            pj_val **items;
            size_t len;
            size_t cap;
        } arr;
        struct {
            char **keys;
            pj_val **vals;
            size_t len;
            size_t cap;
        } obj;
    } u;
};

/* Parses a JSON document. text_len bytes at text. On success returns 0 and
 * sets *out to the root value. On malformed input returns -1 and leaves *out
 * unchanged. The root is allocated in arena a. */
int pj_parse(puci_arena *a, const char *text, size_t text_len, pj_val **out);

/* Object member lookup. Returns the value for key, or NULL if not an object or
 * the key is absent. */
pj_val *pj_get(pj_val *obj, const char *key);

/* Looks up a dotted path, e.g. {"choices","0","delta"}. Each element except the
 * last must be an object; array indices are given as decimal strings. Returns
 * NULL if traversal fails. */
pj_val *pj_get_path(pj_val *root, const char **path, size_t n);

/* Returns a NUL-terminated copy of a string value, or NULL if v is not a
 * string. The pointer is valid for the lifetime of the arena. */
const char *pj_str(const pj_val *v);

/* Returns the numeric value of v, or 0.0 if not a number. */
double pj_num(const pj_val *v);

/* Returns 1 if v is a boolean true, 0 otherwise. */
int pj_bool(const pj_val *v);

/* Returns the length of an array value, or 0 if not an array. */
size_t pj_arr_len(const pj_val *v);

/* Returns the i-th element of an array value, or NULL if out of range. */
pj_val *pj_arr_get(const pj_val *v, size_t i);

/* Returns the number of keys in an object, or 0 if not an object. */
size_t pj_obj_len(const pj_val *v);

#endif /* PUCI_INTERNAL_JSON_H */