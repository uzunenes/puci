/*
 * json/json.c - JSON DOM parser.
 *
 * A defensive, hand-written recursive-descent parser. It operates on a
 * (text, len) window and never assumes NUL termination of the input. Bounds
 * are checked on every read so malformed or truncated network input yields a
 * clean parse error instead of a crash.
 */

#include "json/json.h"

#include <stdlib.h>
#include <string.h>

typedef struct parser {
    const char *text;
    size_t len;
    size_t pos;
    puci_arena *arena;
    int error;
} parser;

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_ws(parser *p) {
    while (p->pos < p->len && is_ws(p->text[p->pos])) {
        p->pos++;
    }
}

static char peek(parser *p) {
    return p->pos < p->len ? p->text[p->pos] : '\0';
}

static char advance(parser *p) {
    if (p->pos < p->len) {
        return p->text[p->pos++];
    }
    p->error = 1;
    return '\0';
}

static pj_val *new_val(parser *p) {
    return (pj_val *)puci_arena_calloc(p->arena, sizeof(pj_val));
}

/* Reads a hex digit into *out. Returns 0 on success. */
static int hex_digit(char c, int *out) {
    if (c >= '0' && c <= '9') {
        *out = (int)(c - '0');
        return 0;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (int)(c - 'a') + 10;
        return 0;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (int)(c - 'A') + 10;
        return 0;
    }
    return -1;
}

/* Decodes a single \uXXXX escape into a UTF-8 sequence appended to a buffer via
 * the append callback. Returns 0 on success. */
static int utf8_encode(unsigned codepoint, char *out, size_t *outlen) {
    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
        *outlen = 1;
        return 0;
    }
    if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        *outlen = 2;
        return 0;
    }
    if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        *outlen = 3;
        return 0;
    }
    out[0] = (char)(0xF0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char)(0x80 | (codepoint & 0x3F));
    *outlen = 4;
    return 0;
}

/* Parses a JSON string (the opening quote must have been consumed). Allocates
 * the decoded content in the arena and returns a NUL-terminated copy, or NULL
 * on error. */
static char *parse_string(parser *p) {
    size_t alloc = (p->len - p->pos) + 1;
    char *buf = (char *)puci_arena_alloc(p->arena, alloc);
    if (!buf) {
        p->error = 1;
        return NULL;
    }
    size_t o = 0;
    while (1) {
        if (p->pos >= p->len) {
            p->error = 1; /* unterminated */
            return NULL;
        }
        char c = advance(p);
        if (c == '"') {
            buf[o] = '\0';
            return buf;
        }
        if (c == '\\') {
            char e = advance(p);
            switch (e) {
            case '"':
                buf[o++] = '"';
                break;
            case '\\':
                buf[o++] = '\\';
                break;
            case '/':
                buf[o++] = '/';
                break;
            case 'b':
                buf[o++] = '\b';
                break;
            case 'f':
                buf[o++] = '\f';
                break;
            case 'n':
                buf[o++] = '\n';
                break;
            case 'r':
                buf[o++] = '\r';
                break;
            case 't':
                buf[o++] = '\t';
                break;
            case 'u': {
                unsigned int cp = 0;
                for (int i = 0; i < 4; i++) {
                    int h;
                    if (hex_digit(advance(p), &h) != 0) {
                        p->error = 1;
                        return NULL;
                    }
                    cp = (cp << 4) | (unsigned int)h;
                }
                /* Handle UTF-16 surrogate pairs. */
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (advance(p) != '\\' || advance(p) != 'u') {
                        p->error = 1;
                        return NULL;
                    }
                    unsigned int lo = 0;
                    for (int i = 0; i < 4; i++) {
                        int h;
                        if (hex_digit(advance(p), &h) != 0) {
                            p->error = 1;
                            return NULL;
                        }
                        lo = (lo << 4) | (unsigned int)h;
                    }
                    if (lo < 0xDC00 || lo > 0xDFFF) {
                        p->error = 1;
                        return NULL;
                    }
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                }
                char enc[4];
                size_t elen = 0;
                if (utf8_encode((unsigned)cp, enc, &elen) != 0) {
                    p->error = 1;
                    return NULL;
                }
                memcpy(buf + o, enc, elen);
                o += elen;
                break;
            }
            default:
                p->error = 1;
                return NULL;
            }
        } else {
            buf[o++] = c;
        }
    }
}

/* Parses a number. Returns a PJ_NUM value or NULL on error. */
static pj_val *parse_number(parser *p) {
    size_t start = p->pos;
    if (peek(p) == '-') {
        p->pos++;
    }
    if (peek(p) == '0') {
        p->pos++;
    } else if (peek(p) >= '1' && peek(p) <= '9') {
        while (p->pos < p->len && peek(p) >= '0' && peek(p) <= '9') {
            p->pos++;
        }
    } else {
        p->error = 1;
        return NULL;
    }
    if (peek(p) == '.') {
        p->pos++;
        if (!(peek(p) >= '0' && peek(p) <= '9')) {
            p->error = 1;
            return NULL;
        }
        while (p->pos < p->len && peek(p) >= '0' && peek(p) <= '9') {
            p->pos++;
        }
    }
    if (peek(p) == 'e' || peek(p) == 'E') {
        p->pos++;
        if (peek(p) == '+' || peek(p) == '-') {
            p->pos++;
        }
        if (!(peek(p) >= '0' && peek(p) <= '9')) {
            p->error = 1;
            return NULL;
        }
        while (p->pos < p->len && peek(p) >= '0' && peek(p) <= '9') {
            p->pos++;
        }
    }
    size_t len = p->pos - start;
    if (len == 0) {
        p->error = 1;
        return NULL;
    }
    /* Copy the token into a NUL-terminated buffer to use strtod. */
    char *tmp = (char *)puci_arena_alloc(p->arena, len + 1);
    if (!tmp) {
        p->error = 1;
        return NULL;
    }
    memcpy(tmp, p->text + start, len);
    tmp[len] = '\0';
    char *end = NULL;
    double d = strtod(tmp, &end);
    if (end != tmp + len) {
        return NULL; /* partial parse: reject */
    }
    pj_val *v = new_val(p);
    if (!v) {
        p->error = 1;
        return NULL;
    }
    v->type = PJ_NUM;
    v->u.num = d;
    return v;
}

static pj_val *parse_value(parser *p);

static pj_val *parse_array(parser *p) {
    pj_val *v = new_val(p);
    if (!v) {
        p->error = 1;
        return NULL;
    }
    v->type = PJ_ARR;
    size_t cap = 4;
    v->u.arr.items = (pj_val **)puci_arena_alloc(p->arena, cap * sizeof(pj_val *));
    if (!v->u.arr.items) {
        p->error = 1;
        return NULL;
    }
    skip_ws(p);
    if (peek(p) == ']') {
        p->pos++;
        return v;
    }
    while (1) {
        skip_ws(p);
        pj_val *item = parse_value(p);
        if (p->error) {
            return NULL;
        }
        if (v->u.arr.len == cap) {
            size_t ncap = cap * 2;
            pj_val **ni = (pj_val **)puci_arena_alloc(p->arena, ncap * sizeof(pj_val *));
            if (!ni) {
                p->error = 1;
                return NULL;
            }
            memcpy(ni, v->u.arr.items, v->u.arr.len * sizeof(pj_val *));
            v->u.arr.items = ni;
            cap = ncap;
        }
        v->u.arr.items[v->u.arr.len++] = item;
        skip_ws(p);
        if (peek(p) == ',') {
            p->pos++;
            continue;
        }
        if (peek(p) == ']') {
            p->pos++;
            return v;
        }
        p->error = 1;
        return NULL;
    }
}

static pj_val *parse_object(parser *p) {
    pj_val *v = new_val(p);
    if (!v) {
        p->error = 1;
        return NULL;
    }
    v->type = PJ_OBJ;
    size_t cap = 8;
    v->u.obj.keys = (char **)puci_arena_alloc(p->arena, cap * sizeof(char *));
    v->u.obj.vals = (pj_val **)puci_arena_alloc(p->arena, cap * sizeof(pj_val *));
    if (!v->u.obj.keys || !v->u.obj.vals) {
        p->error = 1;
        return NULL;
    }
    skip_ws(p);
    if (peek(p) == '}') {
        p->pos++;
        return v;
    }
    while (1) {
        skip_ws(p);
        if (peek(p) != '"') {
            p->error = 1;
            return NULL;
        }
        p->pos++; /* consume opening quote */
        char *key = parse_string(p);
        if (p->error) {
            return NULL;
        }
        skip_ws(p);
        if (advance(p) != ':') {
            p->error = 1;
            return NULL;
        }
        skip_ws(p);
        pj_val *item = parse_value(p);
        if (p->error) {
            return NULL;
        }
        if (v->u.obj.len == cap) {
            size_t ncap = cap * 2;
            char **nk = (char **)puci_arena_alloc(p->arena, ncap * sizeof(char *));
            pj_val **nv = (pj_val **)puci_arena_alloc(p->arena, ncap * sizeof(pj_val *));
            if (!nk || !nv) {
                p->error = 1;
                return NULL;
            }
            memcpy(nk, v->u.obj.keys, v->u.obj.len * sizeof(char *));
            memcpy(nv, v->u.obj.vals, v->u.obj.len * sizeof(pj_val *));
            v->u.obj.keys = nk;
            v->u.obj.vals = nv;
            cap = ncap;
        }
        v->u.obj.keys[v->u.obj.len] = key;
        v->u.obj.vals[v->u.obj.len] = item;
        v->u.obj.len++;
        skip_ws(p);
        if (peek(p) == ',') {
            p->pos++;
            continue;
        }
        if (peek(p) == '}') {
            p->pos++;
            return v;
        }
        p->error = 1;
        return NULL;
    }
}

static int match_literal(parser *p, const char *lit) {
    size_t n = strlen(lit);
    if (p->len - p->pos < n || memcmp(p->text + p->pos, lit, n) != 0) {
        return -1;
    }
    p->pos += n;
    return 0;
}

static pj_val *parse_value(parser *p) {
    skip_ws(p);
    char c = peek(p);
    if (c == '{') {
        p->pos++;
        return parse_object(p);
    }
    if (c == '[') {
        p->pos++;
        return parse_array(p);
    }
    if (c == '"') {
        p->pos++;
        char *s = parse_string(p);
        if (p->error) {
            return NULL;
        }
        pj_val *v = new_val(p);
        if (!v) {
            p->error = 1;
            return NULL;
        }
        v->type = PJ_STR;
        v->u.str.s = s;
        v->u.str.len = strlen(s);
        return v;
    }
    if (c == 't' && match_literal(p, "true") == 0) {
        pj_val *v = new_val(p);
        if (!v) {
            p->error = 1;
            return NULL;
        }
        v->type = PJ_BOOL;
        v->u.bval = 1;
        return v;
    }
    if (c == 'f' && match_literal(p, "false") == 0) {
        pj_val *v = new_val(p);
        if (!v) {
            p->error = 1;
            return NULL;
        }
        v->type = PJ_BOOL;
        v->u.bval = 0;
        return v;
    }
    if (c == 'n' && match_literal(p, "null") == 0) {
        pj_val *v = new_val(p);
        if (!v) {
            p->error = 1;
            return NULL;
        }
        v->type = PJ_NULL;
        return v;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return parse_number(p);
    }
    p->error = 1;
    return NULL;
}

int pj_parse(puci_arena *a, const char *text, size_t text_len, pj_val **out) {
    parser p;
    memset(&p, 0, sizeof(p));
    p.text = text;
    p.len = text_len;
    p.arena = a;
    skip_ws(&p);
    pj_val *root = parse_value(&p);
    if (p.error || !root) {
        return -1;
    }
    skip_ws(&p);
    if (p.pos != p.len) {
        return -1; /* trailing garbage */
    }
    *out = root;
    return 0;
}

pj_val *pj_get(pj_val *obj, const char *key) {
    if (!obj || obj->type != PJ_OBJ || !key) {
        return NULL;
    }
    for (size_t i = 0; i < obj->u.obj.len; i++) {
        if (strcmp(obj->u.obj.keys[i], key) == 0) {
            return obj->u.obj.vals[i];
        }
    }
    return NULL;
}

pj_val *pj_get_path(pj_val *root, const char **path, size_t n) {
    pj_val *cur = root;
    for (size_t i = 0; i < n && cur; i++) {
        if (cur->type == PJ_ARR) {
            /* Treat the path element as a decimal array index. */
            char *end = NULL;
            long idx = strtol(path[i], &end, 10);
            if (!end || *end != '\0' || idx < 0) {
                return NULL;
            }
            cur = pj_arr_get(cur, (size_t)idx);
        } else {
            cur = pj_get(cur, path[i]);
        }
        if (!cur) {
            return NULL;
        }
    }
    return cur;
}

const char *pj_str(const pj_val *v) {
    return (v && v->type == PJ_STR) ? v->u.str.s : NULL;
}

double pj_num(const pj_val *v) {
    return (v && v->type == PJ_NUM) ? v->u.num : 0.0;
}

int pj_bool(const pj_val *v) {
    return (v && v->type == PJ_BOOL && v->u.bval) ? 1 : 0;
}

size_t pj_arr_len(const pj_val *v) {
    return (v && v->type == PJ_ARR) ? v->u.arr.len : 0;
}

pj_val *pj_arr_get(const pj_val *v, size_t i) {
    if (!v || v->type != PJ_ARR || i >= v->u.arr.len) {
        return NULL;
    }
    return v->u.arr.items[i];
}

size_t pj_obj_len(const pj_val *v) {
    return (v && v->type == PJ_OBJ) ? v->u.obj.len : 0;
}