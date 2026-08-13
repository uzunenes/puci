#ifndef PUCI_INTERNAL_STRBUF_H
#define PUCI_INTERNAL_STRBUF_H

/*
 * util/strbuf.h - internal growable string builder.
 *
 * Appends into a dynamically-grown NUL-terminated buffer. Uses the context
 * allocator so the result can outlive per-request arenas. Strings must be
 * NUL-terminated at all times.
 *
 * Not thread-safe.
 */

#include <stddef.h>
#include "puci/puci.h"

typedef struct puci_strbuf {
    puci_allocator alloc;
    char *data;      /* NUL-terminated buffer, or NULL if empty */
    size_t len;      /* byte length excluding the NUL */
    size_t cap;      /* allocated capacity including the NUL */
    puci_status oom; /* set to PUCI_ERR_NOMEM on allocation failure */
} puci_strbuf;

/* Initializes an empty string builder. alloc may be NULL for libc defaults. */
void puci_strbuf_init(puci_strbuf *b, const puci_allocator *alloc);

/* Frees the buffer and resets to empty. */
void puci_strbuf_destroy(puci_strbuf *b);

/* Appends len bytes from s. Returns PUCI_OK or PUCI_ERR_NOMEM. */
puci_status puci_strbuf_append(puci_strbuf *b, const char *s, size_t len);

/* Appends a NUL-terminated string. */
puci_status puci_strbuf_appendz(puci_strbuf *b, const char *s);

/* Appends a single character. */
puci_status puci_strbuf_append_char(puci_strbuf *b, char c);

/* Appends a formatted string (snprintf-style). */
puci_status puci_strbuf_append_fmt(puci_strbuf *b, const char *fmt, ...);

/* Appends a quoted+escaped JSON string literal (including surrounding
 * double quotes) of s. */
puci_status puci_strbuf_append_json_string(puci_strbuf *b, const char *s);

/* Returns the NUL-terminated contents (never NULL; may be ""). The returned
 * pointer is owned by the builder and valid until the next append or destroy. */
const char *puci_strbuf_cstr(const puci_strbuf *b);

#endif /* PUCI_INTERNAL_STRBUF_H */
