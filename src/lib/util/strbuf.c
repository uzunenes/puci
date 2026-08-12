/*
 * util/strbuf.c - growable string builder implementation.
 */

#include "util/strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util/alloc.h"

void puci_strbuf_init(puci_strbuf *b, const puci_allocator *alloc) {
    memset(b, 0, sizeof(*b));
    puci_alloc_resolve(&b->alloc, alloc);
}

void puci_strbuf_destroy(puci_strbuf *b) {
    if (!b) {
        return;
    }
    if (b->data) {
        b->alloc.free_fn(b->alloc.userdata, b->data);
    }
    memset(b, 0, sizeof(*b));
}

static puci_status reserve(puci_strbuf *b, size_t extra) {
    if (b->oom != PUCI_OK) {
        return b->oom;
    }
    size_t need = b->len + extra + 1; /* +1 for NUL */
    if (need <= b->cap) {
        return PUCI_OK;
    }
    size_t newcap = b->cap ? b->cap : 64;
    while (newcap < need) {
        newcap *= 2;
    }
    char *nd = (char *)b->alloc.realloc_fn(b->alloc.userdata, b->data, newcap);
    if (!nd) {
        b->oom = PUCI_ERR_NOMEM;
        return b->oom;
    }
    b->data = nd;
    b->cap = newcap;
    return PUCI_OK;
}

puci_status puci_strbuf_append(puci_strbuf *b, const char *s, size_t len) {
    puci_status st = reserve(b, len);
    if (st != PUCI_OK) {
        return st;
    }
    memcpy(b->data + b->len, s, len);
    b->len += len;
    b->data[b->len] = '\0';
    return PUCI_OK;
}

puci_status puci_strbuf_appendz(puci_strbuf *b, const char *s) {
    return puci_strbuf_append(b, s, strlen(s));
}

puci_status puci_strbuf_append_char(puci_strbuf *b, char c) {
    puci_status st = reserve(b, 1);
    if (st != PUCI_OK) {
        return st;
    }
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return PUCI_OK;
}

puci_status puci_strbuf_append_fmt(puci_strbuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list aq;
    va_copy(aq, ap);
    int need = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (need < 0) {
        va_end(ap);
        return PUCI_ERR_UNKNOWN;
    }
    puci_status st = reserve(b, (size_t)need);
    if (st != PUCI_OK) {
        va_end(ap);
        return st;
    }
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)need;
    return PUCI_OK;
}

puci_status puci_strbuf_append_json_string(puci_strbuf *b, const char *s) {
    puci_status st = puci_strbuf_append_char(b, '"');
    if (st != PUCI_OK) {
        return st;
    }
    const char *p = s;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':
            st = puci_strbuf_appendz(b, "\\\"");
            break;
        case '\\':
            st = puci_strbuf_appendz(b, "\\\\");
            break;
        case '\n':
            st = puci_strbuf_appendz(b, "\\n");
            break;
        case '\r':
            st = puci_strbuf_appendz(b, "\\r");
            break;
        case '\t':
            st = puci_strbuf_appendz(b, "\\t");
            break;
        case '\b':
            st = puci_strbuf_appendz(b, "\\b");
            break;
        case '\f':
            st = puci_strbuf_appendz(b, "\\f");
            break;
        default:
            if (c < 0x20) {
                st = puci_strbuf_append_fmt(b, "\\u%04x", c);
            } else {
                st = puci_strbuf_append_char(b, (char)c);
            }
            break;
        }
        if (st != PUCI_OK) {
            return st;
        }
        p++;
    }
    return puci_strbuf_append_char(b, '"');
}

const char *puci_strbuf_cstr(const puci_strbuf *b) {
    return b->data ? b->data : "";
}