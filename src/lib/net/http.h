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
#include <poll.h>
#include "puci/puci.h"

/* Opaque persistent HTTP connection (owns a libcurl handle). Keeping one alive
 * across turns lets libcurl reuse the underlying TCP/TLS connection (keep-alive
 * + TLS session resumption), so consecutive normal turns avoid a fresh
 * handshake. Destroy with puci_http_conn_destroy. A session owns one. */
typedef struct puci_http_conn puci_http_conn;

/* Creates a persistent connection handle, or NULL on failure. */
puci_http_conn *puci_http_conn_create(void);

/* Frees a persistent connection handle. NULL is a no-op. */
void puci_http_conn_destroy(puci_http_conn *conn);

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

    /* Optional caller-owned, session-persistent connection-dirty flag. The
     * HTTP layer reads it before starting the request: if non-zero, it forces
     * a fresh connection (CURLOPT_FRESH_CONNECT) for THIS request only and
     * then clears it, preserving keep-alive for all normal turns. After the
     * transfer, if it ended aborted / cancelled / with an incomplete body, the
     * layer sets the flag again so the next request starts clean instead of
     * silently reusing a just-aborted connection. May be NULL to disable. */
    int *conn_dirty;

    /* Optional persistent connection handle owned by the caller (the session).
     * When non-NULL the underlying libcurl handle is reused across requests,
     * so the TCP/TLS connection is kept alive and reused for normal turns.
     * When NULL a fresh libcurl handle is created for this request only. */
    puci_http_conn *conn;
} puci_http_request;

/* Performs a streaming HTTP request.
 *
 * On success returns PUCI_OK and sets *out_status to the HTTP status code.
 * On failure returns a negative puci_status with a distinguishing value:
 *  - PUCI_ERR_TIMEOUT    : either the total timeout or the idle timeout expired.
 *  - PUCI_ERR_NETWORK    : connection/TLS/transport failure.
 *  - PUCI_ERR_CANCELLED  : cancel was requested (cancel flag set / fd readable)
 *                          while the transfer was in progress.
 * A short human-readable message (which the agent loop refines further) is
 * written into err (up to errlen bytes) if err is non-NULL.
 *
 * cancel may be NULL; if provided it is polled (flag token and/or fd) so the
 * transfer aborts promptly with PUCI_ERR_CANCELLED when the caller requests
 * cancellation, at any point: during the connect/TLS handshake, mid-stream
 * between chunks, or during a silent (reasoning) period.
 *
 * The standard NO_PROXY / no_proxy environment variables are honored: we never
 * override the proxy configuration, and curl's native proxy handling respects
 * the bypass list. */
puci_status puci_http_post_stream(const puci_allocator *alloc,
                                  const puci_http_request *req,
                                  const puci_cancel_token *cancel,
                                  puci_http_stream_cb data_cb, void *userdata,
                                  int *out_status, char *err, size_t errlen);

/* ------------------------------------------------------------------ */
/* Non-blocking request handle (external event-loop driving).          */
/*                                                                     */
/* Splits puci_http_post_stream into an explicit state machine over the */
/* curl multi interface so a host program can drive the transfer from  */
/* its own poll() loop. This is the primitive poll_loop.c is built on. */
/*                                                                     */
/* Model: an in-flight request is a puci_http_req. The host calls       */
/*   puci_http_begin()  -> register + drive one bounded step            */
/*   puci_http_get_fds()       read the fds curl wants polled          */
/*   puci_http_next_timeout_ms()  how long the host may block          */
/*   puci_http_step()          do bounded work (never blocks on net)   */
/*   puci_http_end()           finalise + free                         */
/*                                                                     */
/* start / finish are optional; if provided, start() runs the first    */
/* bounded step (so the handle becomes immediately pollable) and        */
/* finish() is called once when the transfer completes or fails.       */
/*                                                                     */
/* Threading: a handle is owned by the thread that created it. All of   */
/* its functions must be called from that thread.                      */
/* ------------------------------------------------------------------ */
typedef struct puci_http_req puci_http_req;

/* Creates an in-flight request against the persistent conn (which must be
 * non-NULL). The request body/url/headers are copied into the handle's own
 * storage (allocated via alloc), so the caller may free its buffers after
 * begin() returns. Returns NULL on memory/registration failure. */
puci_http_req *puci_http_begin(const puci_allocator *alloc, puci_http_conn *conn,
                               const puci_http_request *req,
                               const puci_cancel_token *cancel,
                               puci_http_stream_cb data_cb, void *userdata);

/* Writes up to cap struct pollfd entries the host should poll into fds, and
 * returns the count written. The session surfaces these so a host loop can
 * poll libpuci's network activity alongside its own fds. Returns 0 if there
 * is currently nothing to wait on. */
size_t puci_http_get_fds(const puci_http_req *r, struct pollfd *fds, size_t cap);

/* Number of fds curl currently wants polled for this request (the count
 * puci_http_get_fds would write given enough capacity). Useful for a cap==0
 * sizing query. Returns 0 if there is nothing to wait on. */
size_t puci_http_fd_count(const puci_http_req *r);

/* How many milliseconds the host may block in poll() before it must call
 * puci_http_step again. -1 means "block indefinitely", 0 means "work is
 * pending right now". Used to bound idle/reasoning-silence windows. */
int puci_http_next_timeout_ms(const puci_http_req *r);

/* Processes poll results for a request. Given an array of pollfd entries
 * (as returned by poll after waiting on fds from puci_http_get_fds), informs
 * libcurl about which sockets are ready for I/O so it can make efficient
 * progress. This should be called BEFORE puci_http_step to feed poll results
 * into the curl multi handle. Returns PUCI_OK on success. */
puci_status puci_http_process_fds(puci_http_req *r, const struct pollfd *fds,
                                  size_t count);

/* Drives the request by one bounded step. Returns PUCI_OK while the request
 * is still in progress. When the transfer has completed (success or error) it
 * sets *done to 1 and returns a terminal puci_status:
 *   PUCI_OK                : transfer completed (HTTP status in *http_status)
 *   PUCI_ERR_CANCELLED     : cancellation observed mid-transfer
 *   PUCI_ERR_TIMEOUT       : total or idle timeout
 *   PUCI_ERR_NETWORK       : transport/connection failure
 *   PUCI_ERR_UNKNOWN       : HTTP error status returned with a body
 * On a terminal return a message is placed in err (if non-NULL, up to errlen).
 * When *done is 0 the returned status is PUCI_OK and err is untouched.
 * Various fds in the return of puci_http_get_fds may have been consumed (the
 * host should re-query fds after every step). */
puci_status puci_http_step(puci_http_req *r, int *done, int *http_status,
                           char *err, size_t errlen);

/* Finalises and frees a request handle after a terminal step. Marks the
 * connection dirty if the transfer was aborted/cancelled/incomplete so the
 * next request forces a fresh connection. NULL is a no-op. */
void puci_http_end(puci_http_req *r);

#endif /* PUCI_INTERNAL_NET_HTTP_H */