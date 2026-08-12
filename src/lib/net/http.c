/*
 * net/http.c - streaming HTTP client over libcurl.
 */

#include "net/http.h"

#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util/strbuf.h"

/* Context delivered to curl's write + progress callbacks as WRITEDATA /
 * XFERINFODATA. Also carries the cancellation token so a blocked transfer can
 * abort promptly. */
typedef struct http_ctx {
    puci_http_stream_cb cb;
    void *ud;
    long long last_activity_ms;  /* monotonic ms of last received bytes */
    long long idle_timeout_ms;   /* 0 = disabled */
    int idle_timed_out;
    const puci_cancel_token *cancel; /* may be NULL */
    int cancelled;               /* latched when cancel observed */
} http_ctx;

/* Opaque persistent connection: wraps a long-lived libcurl easy handle so the
 * underlying TCP/TLS connection survives across requests (keep-alive + TLS
 * session resumption). */
struct puci_http_conn {
    CURL *curl;  /* persistent easy handle (owns connection cache for reuse) */
    CURLM *multi; /* persistent multi handle (owns the connection cache) */
};

/* An in-flight streaming request driven externally through the curl multi
 * interface. Owns copies of every string libcurl references for the duration
 * of the transfer (URL, body, auth, headers), so the caller may free its
 * buffers after puci_http_begin() returns. */
struct puci_http_req {
    puci_http_conn *conn;   /* borrowed, must outlive the request */
    CURL *curl;             /* borrowed from conn->curl */
    struct curl_slist *hdr; /* owned header list */

    /* Owned buffers referenced by curl options. */
    char *url;
    char *body;             /* NULL unless body_len > 0 */
    size_t body_len;
    char *auth_value;       /* full header value, e.g. "Bearer <key>" */
    char *content_type;
    char *accept;
    char *ca_bundle;
    char *noproxy;

    const puci_cancel_token *cancel; /* borrowed */
    int *conn_dirty;                 /* borrowed; set on abort/incomplete end */
    http_ctx hctx;
    long long deadline_total_ms;     /* -1 = no total timeout */
    int started;                     /* at least one step driven */
    int done;
    int http_status;
    int idle_timed_out;
    CURLcode rc;
    puci_status terminal_status;     /* valid once done */
    char errbuf[CURL_ERROR_SIZE];
};

puci_http_conn *puci_http_conn_create(void) {
    puci_http_conn *c = (puci_http_conn *)calloc(1, sizeof(puci_http_conn));
    if (!c) {
        return NULL;
    }
    c->curl = curl_easy_init();
    if (!c->curl) {
        free(c);
        return NULL;
    }
    c->multi = curl_multi_init();
    if (!c->multi) {
        curl_easy_cleanup(c->curl);
        free(c);
        return NULL;
    }
    return c;
}

void puci_http_conn_destroy(puci_http_conn *conn) {
    if (!conn) {
        return;
    }
    if (conn->multi) {
        curl_multi_cleanup(conn->multi);
    }
    if (conn->curl) {
        curl_easy_cleanup(conn->curl);
    }
    free(conn);
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Returns non-zero once cancellation has been requested. Safe to call from the
 * driver thread and from curl callbacks on the same thread. Latches the flag so
 * the caller can distinguish "cancelled" after the transfer returns. */
static int cancel_pending(http_ctx *h) {
    if (h->cancelled) {
        return 1;
    }
    if (h->cancel && h->cancel->flag && *h->cancel->flag) {
        h->cancelled = 1;
        return 1;
    }
    return 0;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    http_ctx *h = (http_ctx *)userdata;
    size_t total = size * nmemb;
    if (cancel_pending(h)) {
        /* Aborts the transfer with CURLE_WRITE_ERROR. */
        return 0;
    }
    if (total > 0) {
        h->last_activity_ms = now_ms();
        h->cb(h->ud, ptr, total);
    }
    return total;
}

/* Progress callback fires roughly once per second even when idle, letting us
 * detect an idle stream and abort it with a distinct cause. Returning non-zero
 * aborts the transfer (CURLE_ABORTED_BY_CALLBACK). */
static int xfer_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                   curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    http_ctx *h = (http_ctx *)clientp;
    if (cancel_pending(h)) {
        return 1; /* abort the transfer */
    }
    if (h->idle_timeout_ms > 0 &&
        now_ms() - h->last_activity_ms > h->idle_timeout_ms) {
        h->idle_timed_out = 1;
        return 1; /* abort the transfer */
    }
    return 0;
}

/* Extracts the host (lowercased, no scheme/port) from a URL into out, writing
 * at most outcap bytes. Returns the host length (<= outcap-1) or 0 if the URL
 * is not parseable. */
static size_t url_host(const char *url, char *out, size_t outcap) {
    if (!url) {
        return 0;
    }
    const char *h = url;
    const char *scheme = strstr(url, "://");
    if (scheme) {
        h = scheme + 3;
    }
    const char *end = h;
    while (*end && *end != '/' && *end != ':' && *end != '?') {
        end++;
    }
    size_t n = (size_t)(end - h);
    if (n == 0 || n >= outcap) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)tolower((unsigned char)h[i]);
    }
    out[n] = '\0';
    return n;
}

static int host_in_allowlist(const char *host, const char *const *hosts,
                             size_t nhosts) {
    if (!host) {
        return 0;
    }
    size_t hlen = strlen(host);
    for (size_t i = 0; i < nhosts; i++) {
        const char *entry = hosts[i];
        if (!entry) {
            continue;
        }
        size_t elen = strlen(entry);
        char lower_entry[256];
        if (elen >= sizeof(lower_entry)) {
            continue;
        }
        for (size_t k = 0; k < elen; k++) {
            lower_entry[k] = (char)tolower((unsigned char)entry[k]);
        }
        lower_entry[elen] = '\0';
        /* Exact host match, or subdomain of an allowlisted parent domain. */
        if (hlen == elen && memcmp(host, lower_entry, hlen) == 0) {
            return 1;
        }
        if (hlen > elen && host[hlen - elen - 1] == '.' &&
            memcmp(host + (hlen - elen), lower_entry, elen) == 0) {
            return 1;
        }
    }
    return 0;
}

puci_status puci_http_post_stream(const puci_allocator *alloc,
                                  const puci_http_request *req,
                                  const puci_cancel_token *cancel,
                                  puci_http_stream_cb data_cb, void *userdata,
                                  int *out_status, char *err, size_t errlen) {
    if (!req || !req->url || !data_cb) {
        return PUCI_ERR_INVALID_ARG;
    }

    int own_curl = 1;
    CURL *curl = NULL;
    if (req->conn && req->conn->curl) {
        curl = req->conn->curl; /* reuse the session's persistent handle */
        own_curl = 0;
    } else {
        curl = curl_easy_init();
        if (!curl) {
            if (err && errlen > 0) {
                snprintf(err, errlen, "failed to initialise curl");
            }
            return PUCI_ERR_UNKNOWN;
        }
    }
    curl_easy_reset(curl); /* clear any state from a previous transfer */

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "puci/" PUCI_VERSION_STRING);
    /* Never let libcurl use signals for its timeouts (e.g. DNS). Signals are
     * process-global and unsafe in a library and in threaded hosts. Required. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    /* Keep-alive is on by default so consecutive normal turns reuse the same
     * connection (no repeated TCP/TLS handshake). Only when the session's
     * connection-dirty flag is set do we force a fresh connection for THIS
     * request, then clear the flag: a transfer that was aborted, cancelled, or
     * ended with an incomplete body may have left a stale/reset connection in
     * libcurl's global cache, and silently reusing it on the next turn would
     * fail with a spurious "Connection reset by peer". Normal turns therefore
     * pay the handshake cost at most once and resume the connection after. */
    int want_fresh = 0;
    if (req->conn_dirty && *req->conn_dirty) {
        want_fresh = 1;
        *req->conn_dirty = 0; /* consumed: applies to this request only */
    }
    if (want_fresh) {
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0L);
    }

    if (req->timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)req->timeout_ms);
    }

    /* Body. */
    if (req->body && req->body_len > 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)req->body_len);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    /* Headers. */
    struct curl_slist *hdr = NULL;
    if (req->content_type) {
        puci_strbuf h;
        puci_strbuf_init(&h, alloc);
        puci_strbuf_appendz(&h, "Content-Type: ");
        puci_strbuf_appendz(&h, req->content_type);
        if (h.oom == PUCI_OK && h.data) {
            hdr = curl_slist_append(hdr, h.data);
        }
        puci_strbuf_destroy(&h);
    }
    if (req->accept) {
        puci_strbuf h;
        puci_strbuf_init(&h, alloc);
        puci_strbuf_appendz(&h, "Accept: ");
        puci_strbuf_appendz(&h, req->accept);
        if (h.oom == PUCI_OK && h.data) {
            hdr = curl_slist_append(hdr, h.data);
        }
        puci_strbuf_destroy(&h);
    }
    if (req->auth_header) {
        puci_strbuf h;
        puci_strbuf_init(&h, alloc);
        puci_strbuf_appendz(&h, "Authorization: ");
        puci_strbuf_appendz(&h, req->auth_header);
        if (h.oom == PUCI_OK && h.data) {
            hdr = curl_slist_append(hdr, h.data);
        }
        puci_strbuf_destroy(&h);
    }
    if (hdr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    }

    /* Proxy: never override the environment. Respect NO_PROXY / no_proxy so an
     * internal host can bypass a configured proxy. */
    const char *noproxy = getenv("NO_PROXY");
    if (!noproxy || !noproxy[0]) {
        noproxy = getenv("no_proxy");
    }
    if (noproxy && noproxy[0]) {
        curl_easy_setopt(curl, CURLOPT_NOPROXY, noproxy);
    }

    /* TLS. */
    if (req->ca_bundle && req->ca_bundle[0]) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, req->ca_bundle);
    }
    int relax = 0;
    if (req->tls_verify == 0 && req->insecure_hosts && req->ninsecure_hosts) {
        char host[256];
        if (url_host(req->url, host, sizeof(host)) > 0 &&
            host_in_allowlist(host, req->insecure_hosts, req->ninsecure_hosts)) {
            relax = 1;
        }
    }
    if (relax) {
        /* Opt-in, allowlist-gated relaxation: never a default or fallback. */
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        if (req->log_cb && PUCI_LOG_WARN >= req->log_level) {
            char host[256];
            if (url_host(req->url, host, sizeof(host)) > 0) {
                char msg[320];
                snprintf(msg, sizeof(msg),
                         "TLS verification disabled for host '%s' (insecure "
                         "allowlist)", host);
                req->log_cb(req->log_userdata, PUCI_LOG_WARN, msg);
            }
        }
    }

    /* Response delivery + idle timeout tracking + cancellation. */
    http_ctx hctx;
    memset(&hctx, 0, sizeof(hctx));
    hctx.cb = data_cb;
    hctx.ud = userdata;
    hctx.last_activity_ms = now_ms();
    hctx.idle_timeout_ms = req->idle_timeout_ms > 0 ? req->idle_timeout_ms : 0;
    hctx.cancel = cancel;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &hctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &hctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    /* Drive the transfer with the curl multi interface rather than the blocking
     * easy interface, so we can also wait on the caller's cancel fd and abort
     * promptly at any point (handshake, mid-stream, or silent reasoning gap),
     * and so an external event loop can adopt the transfer (curl_multi_fdset). */
    int own_multi = 0;
    CURLM *multi = NULL;
    if (req->conn && req->conn->multi) {
        multi = req->conn->multi;            /* persistent per-session multi */
        curl_multi_remove_handle(multi, curl); /* defensive: not currently added */
    } else {
        multi = curl_multi_init();
        own_multi = 1;
    }
    if (!multi) {
        curl_slist_free_all(hdr);
        if (own_curl) {
            curl_easy_cleanup(curl);
        }
        return PUCI_ERR_UNKNOWN;
    }
    if (curl_multi_add_handle(multi, curl) != CURLM_OK) {
        if (own_multi) {
            curl_multi_cleanup(multi);
        }
        curl_slist_free_all(hdr);
        if (own_curl) {
            curl_easy_cleanup(curl);
        }
        return PUCI_ERR_UNKNOWN;
    }

    /* Bound each wait so idle-timeout and cancel-flag checks stay timely and we
     * never block indefinitely on a silent (reasoning) stream. */
    const long CAP_MS = 250;
    int running = 1;
    CURLcode rc = CURLE_OK;
    while (running) {
        if (hctx.idle_timeout_ms > 0 &&
            now_ms() - hctx.last_activity_ms > hctx.idle_timeout_ms) {
            hctx.idle_timed_out = 1;
            rc = CURLE_ABORTED_BY_CALLBACK;
            break;
        }
        if (cancel_pending(&hctx)) {
            rc = CURLE_ABORTED_BY_CALLBACK;
            break;
        }

        curl_multi_perform(multi, &running);

        int msgs = 0;
        CURLMsg *info;
        while ((info = curl_multi_info_read(multi, &msgs)) != NULL) {
            if (info->msg == CURLMSG_DONE) {
                rc = info->data.result;
                running = 0;
            }
        }
        if (!running) {
            break;
        }

        if (curl_multi_wait(multi, NULL, 0, CAP_MS, NULL) != CURLM_OK) {
            rc = CURLE_READ_ERROR;
            break;
        }
        /* Retry the loop: curl_multi_wait blocks up to CAP_MS for activity on
         * the transfer's own fds, then wakes so the idle-timeout and cancel
         * flag checks at the top stay timely even on a silent (reasoning)
         * stream. Cancellation is also aborted promptly inside the write /
         * progress callbacks. EINTR is handled internally by curl_multi_wait. */
    }

    const int was_cancelled = cancel_pending(&hctx);

    /* A transfer that was cancelled, or that failed at the transport level (as
     * opposed to completing with an HTTP error status, which still delivered
     * the full body) may have aborted its connection before the body was fully
     * consumed. Mark the session's connection dirty so the NEXT request forces
     * a fresh connection instead of silently reusing the stale one. */
    {
        int aborted = was_cancelled ||
                      (rc != CURLE_OK && rc != CURLE_HTTP_RETURNED_ERROR);
        if (aborted && req->conn_dirty) {
            *req->conn_dirty = 1;
        }
    }

    curl_multi_remove_handle(multi, curl);
    if (own_multi) {
        curl_multi_cleanup(multi);
    }

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (out_status) {
        *out_status = (int)code;
    }

    curl_slist_free_all(hdr);
    if (own_curl) {
        curl_easy_cleanup(curl);
    }

    if (was_cancelled) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "cancelled");
        }
        return PUCI_ERR_CANCELLED;
    }
    if (rc != CURLE_OK) {
        const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        if (err && errlen > 0) {
            snprintf(err, errlen, "%s", msg);
        }
        if (hctx.idle_timed_out) {
            if (err && errlen > 0) {
                snprintf(err, errlen,
                         "idle timeout exceeded (no data for %lld ms)",
                         hctx.idle_timeout_ms);
            }
            return PUCI_ERR_TIMEOUT;
        }
        if (rc == CURLE_OPERATION_TIMEDOUT) {
            if (err && errlen > 0) {
                snprintf(err, errlen, "total request timeout: %s", msg);
            }
            return PUCI_ERR_TIMEOUT;
        }
        if (rc == CURLE_HTTP_RETURNED_ERROR) {
            return PUCI_ERR_UNKNOWN;
        }
        return PUCI_ERR_NETWORK;
    }
    return PUCI_OK;
}

/* ------------------------------------------------------------------ */
/* Non-blocking request handle (external event-loop driving).          */
/* ------------------------------------------------------------------ */

static char *dupn(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (!d) {
        return NULL;
    }
    if (n > 0) {
        memcpy(d, s, n);
    }
    d[n] = '\0';
    return d;
}

static void req_free(puci_http_req *r) {
    if (!r) {
        return;
    }
    if (r->hdr) {
        curl_slist_free_all(r->hdr);
    }
    free(r->url);
    free(r->body);
    free(r->auth_value);
    free(r->content_type);
    free(r->accept);
    free(r->ca_bundle);
    free(r->noproxy);
    free(r);
}

/* Configures r->curl from req, taking owned copies of every string libcurl
 * references so the caller may free its buffers after begin(). Returns PUCI_OK
 * on success or PUCI_ERR_NOMEM. On error r is left in a state safe for
 * req_free(). */
static puci_status req_config(puci_http_req *r, const puci_http_request *req) {
    r->url = dupn(req->url, strlen(req->url));
    if (!r->url) {
        return PUCI_ERR_NOMEM;
    }
    if (req->body && req->body_len > 0) {
        r->body = dupn(req->body, req->body_len);
        if (!r->body) {
            return PUCI_ERR_NOMEM;
        }
        r->body_len = req->body_len;
    }
    if (req->auth_header && req->auth_header[0]) {
        size_t n = strlen("Authorization: ") + strlen(req->auth_header);
        r->auth_value = (char *)malloc(n + 1);
        if (!r->auth_value) {
            return PUCI_ERR_NOMEM;
        }
        memcpy(r->auth_value, "Authorization: ", strlen("Authorization: "));
        memcpy(r->auth_value + strlen("Authorization: "), req->auth_header,
               strlen(req->auth_header) + 1);
    }
    if (req->content_type) {
        r->content_type = dupn(req->content_type, strlen(req->content_type));
        if (!r->content_type) {
            return PUCI_ERR_NOMEM;
        }
    }
    if (req->accept) {
        r->accept = dupn(req->accept, strlen(req->accept));
        if (!r->accept) {
            return PUCI_ERR_NOMEM;
        }
    }
    if (req->ca_bundle) {
        r->ca_bundle = dupn(req->ca_bundle, strlen(req->ca_bundle));
        if (!r->ca_bundle) {
            return PUCI_ERR_NOMEM;
        }
    }

    CURL *curl = r->curl;
    curl_easy_reset(curl); /* clear any state from a previous transfer */

    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, r->errbuf);
    curl_easy_setopt(curl, CURLOPT_URL, r->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "puci/" PUCI_VERSION_STRING);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    int want_fresh = 0;
    if (req->conn_dirty && *req->conn_dirty) {
        want_fresh = 1;
        *req->conn_dirty = 0;
    }
    if (want_fresh) {
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0L);
    }

    if (req->timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)req->timeout_ms);
    }

    if (r->body) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, r->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)r->body_len);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    if (r->content_type) {
        size_t n = strlen("Content-Type: ") + strlen(r->content_type);
        char *hv = (char *)malloc(n + 1);
        if (!hv) {
            return PUCI_ERR_NOMEM;
        }
        memcpy(hv, "Content-Type: ", strlen("Content-Type: "));
        memcpy(hv + strlen("Content-Type: "), r->content_type,
               strlen(r->content_type) + 1);
        r->hdr = curl_slist_append(r->hdr, hv);
        free(hv);
    }
    if (r->accept) {
        size_t n = strlen("Accept: ") + strlen(r->accept);
        char *hv = (char *)malloc(n + 1);
        if (!hv) {
            return PUCI_ERR_NOMEM;
        }
        memcpy(hv, "Accept: ", strlen("Accept: "));
        memcpy(hv + strlen("Accept: "), r->accept, strlen(r->accept) + 1);
        r->hdr = curl_slist_append(r->hdr, hv);
        free(hv);
    }
    if (r->auth_value) {
        r->hdr = curl_slist_append(r->hdr, r->auth_value);
    }
    if (r->hdr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, r->hdr);
    }

    const char *noproxy = getenv("NO_PROXY");
    if (!noproxy || !noproxy[0]) {
        noproxy = getenv("no_proxy");
    }
    if (noproxy && noproxy[0]) {
        r->noproxy = dupn(noproxy, strlen(noproxy));
        if (!r->noproxy) {
            return PUCI_ERR_NOMEM;
        }
        curl_easy_setopt(curl, CURLOPT_NOPROXY, r->noproxy);
    }

    if (r->ca_bundle) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, r->ca_bundle);
    }
    int relax = 0;
    if (req->tls_verify == 0 && req->insecure_hosts && req->ninsecure_hosts) {
        char host[256];
        if (url_host(r->url, host, sizeof(host)) > 0 &&
            host_in_allowlist(host, req->insecure_hosts,
                              req->ninsecure_hosts)) {
            relax = 1;
        }
    }
    if (relax) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        if (req->log_cb && PUCI_LOG_WARN >= req->log_level) {
            char host[256];
            if (url_host(r->url, host, sizeof(host)) > 0) {
                char msg[320];
                snprintf(msg, sizeof(msg),
                         "TLS verification disabled for host '%s' (insecure "
                         "allowlist)", host);
                req->log_cb(req->log_userdata, PUCI_LOG_WARN, msg);
            }
        }
    }

    r->hctx.last_activity_ms = now_ms();
    r->hctx.idle_timeout_ms = req->idle_timeout_ms > 0 ? req->idle_timeout_ms : 0;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r->hctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &r->hctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    r->deadline_total_ms =
        req->timeout_ms > 0 ? now_ms() + (long long)req->timeout_ms : -1;
    return PUCI_OK;
}

static puci_status req_terminal_status(puci_http_req *r, char *err,
                                       size_t errlen) {
    if (r->hctx.cancelled) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "cancelled");
        }
        return PUCI_ERR_CANCELLED;
    }
    if (r->rc != CURLE_OK) {
        const char *msg = r->errbuf[0] ? r->errbuf : curl_easy_strerror(r->rc);
        if (r->idle_timed_out) {
            if (err && errlen > 0) {
                snprintf(err, errlen,
                         "idle timeout exceeded (no data for %lld ms)",
                         r->hctx.idle_timeout_ms);
            }
            return PUCI_ERR_TIMEOUT;
        }
        if (r->rc == CURLE_OPERATION_TIMEDOUT) {
            if (err && errlen > 0) {
                snprintf(err, errlen, "total request timeout: %s", msg);
            }
            return PUCI_ERR_TIMEOUT;
        }
        if (r->rc == CURLE_HTTP_RETURNED_ERROR) {
            if (err && errlen > 0) {
                snprintf(err, errlen, "%s", msg);
            }
            return PUCI_ERR_UNKNOWN;
        }
        if (err && errlen > 0) {
            snprintf(err, errlen, "%s", msg);
        }
        return PUCI_ERR_NETWORK;
    }
    return PUCI_OK;
}

puci_http_req *puci_http_begin(const puci_allocator *alloc, puci_http_conn *conn,
                               const puci_http_request *req,
                               const puci_cancel_token *cancel,
                               puci_http_stream_cb data_cb, void *userdata) {
    (void)alloc;
    if (!conn || !conn->curl || !conn->multi || !req || !req->url) {
        return NULL;
    }
    puci_http_req *r = (puci_http_req *)calloc(1, sizeof(puci_http_req));
    if (!r) {
        return NULL;
    }
    r->conn = conn;
    r->curl = conn->curl;
    r->cancel = cancel;
    r->conn_dirty = req->conn_dirty;
    r->hctx.idle_timed_out = 0;
    r->hctx.cancelled = 0;

    curl_multi_remove_handle(conn->multi, r->curl); /* defensive */

    if (req_config(r, req) != PUCI_OK) {
        req_free(r);
        return NULL;
    }
    r->hctx.cb = data_cb;
    r->hctx.ud = userdata;
    r->hctx.cancel = cancel;

    if (curl_multi_add_handle(conn->multi, r->curl) != CURLM_OK) {
        curl_easy_reset(r->curl);
        req_free(r);
        return NULL;
    }
    r->started = 1;
    return r;
}

size_t puci_http_get_fds(const puci_http_req *r, struct pollfd *fds, size_t cap) {
    if (!r || !r->started || r->done) {
        return 0;
    }
    fd_set rd, wr, ex;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    int maxfd = -1;
    (void)curl_multi_fdset(r->conn->multi, &rd, &wr, &ex, &maxfd);
    size_t n = 0;
    for (int fd = 0; fd <= maxfd && n < cap; fd++) {
        short ev = 0;
        if (FD_ISSET(fd, &rd)) {
            ev |= POLLIN;
        }
        if (FD_ISSET(fd, &wr)) {
            ev |= POLLOUT;
        }
        if (FD_ISSET(fd, &ex)) {
            ev |= POLLERR;
        }
        if (ev) {
            fds[n].fd = fd;
            fds[n].events = ev;
            fds[n].revents = 0;
            n++;
        }
    }
    return n;
}

int puci_http_next_timeout_ms(const puci_http_req *r) {
    if (!r || !r->started || r->done) {
        return -1;
    }
    long curl_ms = 0;
    if (r->conn->multi && curl_multi_timeout(r->conn->multi, &curl_ms) != CURLM_OK) {
        curl_ms = -1;
    }
    if (curl_ms < 0) {
        curl_ms = 0;
    }
    if (r->deadline_total_ms > 0) {
        long long rem = r->deadline_total_ms - now_ms();
        if (rem <= 0) {
            return 0;
        }
        if ((long long)curl_ms > rem) {
            curl_ms = (long)rem;
        }
    }
    return (int)curl_ms;
}

puci_status puci_http_step(puci_http_req *r, int *done, int *http_status,
                           char *err, size_t errlen) {
    if (done) {
        *done = 0;
    }
    if (!r || !r->started) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (r->done) {
        if (done) {
            *done = 1;
        }
        if (http_status) {
            *http_status = r->http_status;
        }
        return r->terminal_status;
    }

    if (r->hctx.idle_timeout_ms > 0 &&
        now_ms() - r->hctx.last_activity_ms > r->hctx.idle_timeout_ms) {
        r->idle_timed_out = 1;
        r->rc = CURLE_ABORTED_BY_CALLBACK;
        r->done = 1;
        curl_multi_remove_handle(r->conn->multi, r->curl);
        if (http_status) {
            *http_status = 0;
        }
        r->terminal_status = req_terminal_status(r, err, errlen);
        if (done) {
            *done = 1;
        }
        return r->terminal_status;
    }
    if (cancel_pending(&r->hctx)) {
        r->rc = CURLE_ABORTED_BY_CALLBACK;
        r->done = 1;
        curl_multi_remove_handle(r->conn->multi, r->curl);
        if (http_status) {
            *http_status = 0;
        }
        r->terminal_status = req_terminal_status(r, err, errlen);
        if (done) {
            *done = 1;
        }
        return r->terminal_status;
    }

    int running = 0;
    (void)curl_multi_perform(r->conn->multi, &running);

    int msgs = 0;
    CURLMsg *info;
    while ((info = curl_multi_info_read(r->conn->multi, &msgs)) != NULL) {
        if (info->msg == CURLMSG_DONE && info->easy_handle == r->curl) {
            r->rc = info->data.result;
            r->done = 1;
        }
    }
    if (r->done) {
        curl_multi_remove_handle(r->conn->multi, r->curl);
        long code = 0;
        curl_easy_getinfo(r->curl, CURLINFO_RESPONSE_CODE, &code);
        r->http_status = (int)code;
        if (http_status) {
            *http_status = r->http_status;
        }
        r->terminal_status = req_terminal_status(r, err, errlen);
        if (done) {
            *done = 1;
        }
        return r->terminal_status;
    }
    return PUCI_OK; /* still in progress */
}

void puci_http_end(puci_http_req *r) {
    if (!r) {
        return;
    }
    /* A transfer that was cancelled, or that failed at the transport level
     * (as opposed to completing with an HTTP error status, which still
     * delivered the full body) may leave the connection unusable; force a
     * fresh connect on the next request. Aborted = the transfer did not run
     * to a clean transport-level completion. */
    int aborted = (r->done && (r->hctx.cancelled ||
                               (r->rc != CURLE_OK &&
                                r->rc != CURLE_HTTP_RETURNED_ERROR)));
    if (aborted && r->conn_dirty) {
        *(r->conn_dirty) = 1;
    }
    if (r->started && !r->done) {
        curl_multi_remove_handle(r->conn->multi, r->curl);
    }
    req_free(r);
}