#ifndef PUCI_INTERNAL_NET_SSE_H
#define PUCI_INTERNAL_NET_SSE_H

/*
 * net/sse.h - internal Server-Sent Events stream parser.
 *
 * Parses an SSE byte stream fed in arbitrary-sized chunks. Each complete event
 * triggers a callback with the raw event data (the payload after "data: ").
 * The parser is defensive: malformed or truncated input never crashes; it
 * simply yields the events that can be reconstructed.
 *
 * Not thread-safe.
 */

#include <stddef.h>
#include "puci/puci.h"

typedef struct puci_sse_parser puci_sse_parser;

/* Called with one complete event's data payload. Borrowed pointer: only valid
 * for the duration of the callback. */
typedef void (*puci_sse_event_cb)(void *userdata, const char *data, size_t len);

/* Creates a parser. alloc may be NULL for libc defaults. */
puci_sse_parser *puci_sse_parser_create(const puci_allocator *alloc);

/* Feeds bytes into the parser. Fields/events are emitted via cb as they become
 * complete. Returns PUCI_OK or PUCI_ERR_NOMEM. */
puci_status puci_sse_parser_feed(puci_sse_parser *p, const char *data,
                                 size_t len, puci_sse_event_cb cb, void *ud);

/* Flushes any partial trailing event (e.g. stream ended without a blank line).
 * Call once after the last feed. Events buffered are emitted via cb. */
puci_status puci_sse_parser_flush(puci_sse_parser *p, puci_sse_event_cb cb,
                                  void *ud);

/* Returns 1 if the stream contained the terminal "[DONE]" marker. */
int puci_sse_parser_finished(const puci_sse_parser *p);

/* Destroys the parser and frees all resources. NULL is a no-op. */
void puci_sse_parser_destroy(puci_sse_parser *p);

#endif /* PUCI_INTERNAL_NET_SSE_H */