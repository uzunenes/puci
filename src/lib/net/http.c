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

    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "failed to initialise curl");
        }
        return PUCI_ERR_UNKNOWN;
    }

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "puci/" PUCI_VERSION_STRING);
    /* Never let libcurl use signals for its timeouts (e.g. DNS). Signals are
     * process-global and unsafe in a library and in threaded hosts. Required. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

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
    CURLM *multi = curl_multi_init();
    if (!multi) {
        curl_slist_free_all(hdr);
        curl_easy_cleanup(curl);
        return PUCI_ERR_UNKNOWN;
    }
    if (curl_multi_add_handle(multi, curl) != CURLM_OK) {
        curl_multi_cleanup(multi);
        curl_slist_free_all(hdr);
        curl_easy_cleanup(curl);
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

    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (out_status) {
        *out_status = (int)code;
    }

    curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);

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