#ifndef PUCI_INTERNAL_NET_HTTP_H
#define PUCI_INTERNAL_NET_HTTP_H

/*
 * net/http.h - internal HTTP client.
 *
 * A thin streaming HTTP layer used by the provider backends. libcurl is
 * abstracted behind this single header so it can be replaced later with raw
 * sockets plus mbedTLS for ultra-minimal embedded builds.
 *
 * Synchronous: the agent loop call drives the transfer to completion, invoking
 * the data callback with each body chunk as it arrives. This keeps the call
 * stack simple; the non-blocking pollable API the public headers advertise is
 * built on top of this via a dedicated pumping thread / curl multi handle in a
 * later phase.
 */

#include <stddef.h>
#include "puci/puci.h"

/* Called with each chunk of the response body. May be called any number of
 * times including zero. The pointer is only valid for the duration of the
 * callback. */
typedef void (*puci_http_stream_cb)(void *userdata, const char *data,
                                    size_t len);

/* Performs a streaming HTTP request.
 *
 * On success returns PUCI_OK and sets *out_status to the HTTP status code.
 * On transport / TLS / timeout failure returns a negative puci_status
 * (PUCI_ERR_NETWORK, PUCI_ERR_TIMEOUT, PUCI_ERR_NOMEM, ...) and writes a short
 * human-readable message into err (up to errlen bytes) if err is non-NULL.
 *
 * url: full request URL.
 * body/body_len: request body (may be NULL if body_len==0).
 * auth_header: full "Bearer <token>" value (without "Authorization: "), or
 *              NULL for none. Set to an empty string to send "none".
 * content_type: value for the Content-Type header.
 * accept: value for the Accept header (e.g. "text/event-stream").
 * timeout_ms: per-transfer timeout; 0 uses the library default.
 * The allocator is used only for internal temporary buffers.
 */
puci_status puci_http_post_stream(const puci_allocator *alloc, const char *url,
                                  const char *body, size_t body_len,
                                  const char *auth_header,
                                  const char *content_type, const char *accept,
                                  int timeout_ms, puci_http_stream_cb data_cb,
                                  void *userdata, int *out_status, char *err,
                                  size_t errlen);

#endif /* PUCI_INTERNAL_NET_HTTP_H */