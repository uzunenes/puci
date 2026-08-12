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

/* Bundled request tuning. Zero-init for defaults. */
typedef struct puci_http_request {
    const char *url;          /* full request URL */
    const char *body;         /* request body, bytes body_len */
    size_t body_len;
    const char *auth_header;  /* full "Bearer <token>" value (no header name) */
    const char *content_type; /* value for Content-Type */
    const char *accept;       /* value for Accept, e.g. "text/event-stream" */

    /* Total request timeout in ms; 0 = library default. */
    int timeout_ms;

    /* Idle/stream timeout in ms: if no bytes arrive within this window the
     * transfer aborts with a distinct idle-timeout error. 0 = no idle timeout. */
    int idle_timeout_ms;

    /* Optional custom CA bundle (PEM) via CURLOPT_CAINFO. The preferred way to
     * trust a custom/internal CA. */
    const char *ca_bundle;

    /* TLS verification policy:
     *  - tls_verify == 1  : always verify (the safe default).
     *  - tls_verify == 0  : MAY relax verification, but ONLY when the request
     *                       host is listed in insecure_hosts. This is still an
     *                       explicit opt-in gated by the allowlist; it is never
     *                       a silent default, a fallback, or an automatic retry.
     *  - insecure_hosts   : allowlist of hostnames for which verification may
     *                       be relaxed; others are always verified.
     */
    int tls_verify;
    const char *const *insecure_hosts;
    size_t ninsecure_hosts;

    /* Logging callback used to emit a PUCI_LOG_WARN naming the host whenever
     * verification is relaxed for a request. */
    puci_log_cb log_cb;
    void *log_userdata;
    puci_log_level log_level;
} puci_http_request;

/* Performs a streaming HTTP request.
 *
 * On success returns PUCI_OK and sets *out_status to the HTTP status code.
 * On failure returns a negative puci_status with a distinguishing value:
 *  - PUCI_ERR_TIMEOUT    : either the total timeout or the idle timeout expired.
 *  - PUCI_ERR_NETWORK    : connection/TLS/transport failure.
 * A short human-readable message (which the agent loop refines further) is
 * written into err (up to errlen bytes) if err is non-NULL.
 *
 * The standard NO_PROXY / no_proxy environment variables are honored: we never
 * override the proxy configuration, and curl's native proxy handling respects
 * the bypass list. */
puci_status puci_http_post_stream(const puci_allocator *alloc,
                                  const puci_http_request *req,
                                  puci_http_stream_cb data_cb, void *userdata,
                                  int *out_status, char *err, size_t errlen);

#endif /* PUCI_INTERNAL_NET_HTTP_H */