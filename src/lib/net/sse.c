/*
 * net/sse.c - SSE stream parser implementation.
 */

#include "net/sse.h"

#include <stdlib.h>
#include <string.h>

#include "util/strbuf.h"

#define PUCI_SSE_MAX_BUFFER ((size_t)8 * 1024 * 1024)

struct puci_sse_parser {
    puci_strbuf buf;       /* unprocessed input bytes */
    puci_strbuf pending;   /* current event payload */
    int finished;          /* [DONE] seen */
    puci_status oom;
};

puci_sse_parser *puci_sse_parser_create(const puci_allocator *alloc) {
    puci_sse_parser *p = (puci_sse_parser *)calloc(1, sizeof(puci_sse_parser));
    if (!p) {
        return NULL;
    }
    puci_strbuf_init(&p->buf, alloc);
    puci_strbuf_init(&p->pending, alloc);
    return p;
}

void puci_sse_parser_destroy(puci_sse_parser *p) {
    if (!p) {
        return;
    }
    puci_strbuf_destroy(&p->buf);
    puci_strbuf_destroy(&p->pending);
    free(p);
}

static void emit_event(puci_sse_parser *p, puci_sse_event_cb cb, void *ud) {
    if (p->pending.len > 0) {
        cb(ud, p->pending.data, p->pending.len);
    }
    p->pending.len = 0;
    if (p->pending.data) {
        p->pending.data[0] = '\0';
    }
}

/* Processes a single line (without the trailing LF). Returns 0 on success. */
static void process_line(puci_sse_parser *p, const char *line, size_t len,
                         puci_sse_event_cb cb, void *ud) {
    /* Trim a trailing CR. */
    size_t n = len;
    if (n > 0 && line[n - 1] == '\r') {
        n--;
    }
    if (n == 0) {
        /* Blank line: terminate the current event. */
        emit_event(p, cb, ud);
        return;
    }
    /* Ignore comment lines (start with ':'). */
    if (line[0] == ':') {
        return;
    }
    /* data: field */
    if (n >= 5 && memcmp(line, "data:", 5) == 0) {
        const char *payload = line + 5;
        size_t plen = n - 5;
        if (plen > 0 && payload[0] == ' ') {
            payload++;
            plen--;
        }
        if (plen == 6 && memcmp(payload, "[DONE]", 6) == 0) {
            p->finished = 1;
            return;
        }
        if (p->pending.len > 0) {
            p->oom = puci_strbuf_append_char(&p->pending, '\n');
        }
        if (p->oom == PUCI_OK) {
            p->oom = puci_strbuf_append(&p->pending, payload, plen);
        }
        return;
    }
    /* Other fields (event:, id:, retry:) are ignored for our shapes. */
}

static puci_status process_buffer(puci_sse_parser *p, puci_sse_event_cb cb,
                                  void *ud) {
    const char *s = p->buf.data;
    size_t remain = p->buf.len;
    size_t start = 0;
    while (start < remain) {
        const char *nl = memchr(s + start, '\n', remain - start);
        if (!nl) {
            break;
        }
        size_t line_len = (size_t)(nl - (s + start));
        process_line(p, s + start, line_len, cb, ud);
        start += line_len + 1;
    }
    /* Keep the trailing partial line. */
    if (start > 0) {
        memmove(p->buf.data, s + start, remain - start);
        p->buf.len = remain - start;
        if (p->buf.data) {
            p->buf.data[p->buf.len] = '\0';
        }
    }
    return p->oom != PUCI_OK ? p->oom : PUCI_OK;
}

puci_status puci_sse_parser_feed(puci_sse_parser *p, const char *data,
                                 size_t len, puci_sse_event_cb cb, void *ud) {
    if (!p || (!data && len > 0)) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (p->buf.len + len > PUCI_SSE_MAX_BUFFER ||
        p->pending.len > PUCI_SSE_MAX_BUFFER) {
        return PUCI_ERR_LIMIT_EXCEEDED;
    }
    p->oom = puci_strbuf_append(&p->buf, data, len);
    if (p->oom != PUCI_OK) {
        return p->oom;
    }
    if (p->buf.len > PUCI_SSE_MAX_BUFFER || p->pending.len > PUCI_SSE_MAX_BUFFER) {
        return PUCI_ERR_LIMIT_EXCEEDED;
    }
    return process_buffer(p, cb, ud);
}

puci_status puci_sse_parser_flush(puci_sse_parser *p, puci_sse_event_cb cb,
                                  void *ud) {
    if (!p) {
        return PUCI_ERR_INVALID_ARG;
    }
    /* Any remaining line without a trailing newline forms an event. */
    if (p->buf.len > 0) {
        process_line(p, p->buf.data, p->buf.len, cb, ud);
        p->buf.len = 0;
        if (p->buf.data) {
            p->buf.data[0] = '\0';
        }
    }
    emit_event(p, cb, ud);
    return p->oom != PUCI_OK ? p->oom : PUCI_OK;
}

int puci_sse_parser_finished(const puci_sse_parser *p) {
    return p ? p->finished : 0;
}
