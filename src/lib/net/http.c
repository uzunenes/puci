/*
 * net/http.c - streaming HTTP client over libcurl.
 */

#include "net/http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#include "util/strbuf.h"

/* Context delivered to curl's write callback as WRITEDATA. */
typedef struct http_ctx {
    puci_http_stream_cb cb;
    void *ud;
} http_ctx;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    http_ctx *h = (http_ctx *)userdata;
    size_t total = size * nmemb;
    if (total > 0) {
        h->cb(h->ud, ptr, total);
    }
    return total;
}

puci_status puci_http_post_stream(const puci_allocator *alloc, const char *url,
                                  const char *body, size_t body_len,
                                  const char *auth_header,
                                  const char *content_type, const char *accept,
                                  int timeout_ms, puci_http_stream_cb data_cb,
                                  void *userdata, int *out_status, char *err,
                                  size_t errlen) {
    if (!url || !data_cb) {
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
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "puci/" PUCI_VERSION_STRING);

    if (timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    }

    /* Body. */
    if (body && body_len > 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    /* Headers. */
    struct curl_slist *hdr = NULL;
    if (content_type) {
        puci_strbuf h;
        puci_strbuf_init(&h, alloc);
        puci_strbuf_appendz(&h, "Content-Type: ");
        puci_strbuf_appendz(&h, content_type);
        if (h.oom == PUCI_OK && h.data) {
            hdr = curl_slist_append(hdr, h.data);
        }
        puci_strbuf_destroy(&h);
    }
    if (accept) {
        puci_strbuf h;
        puci_strbuf_init(&h, alloc);
        puci_strbuf_appendz(&h, "Accept: ");
        puci_strbuf_appendz(&h, accept);
        if (h.oom == PUCI_OK && h.data) {
            hdr = curl_slist_append(hdr, h.data);
        }
        puci_strbuf_destroy(&h);
    }
    if (auth_header) {
        puci_strbuf h;
        puci_strbuf_init(&h, alloc);
        puci_strbuf_appendz(&h, "Authorization: ");
        puci_strbuf_appendz(&h, auth_header);
        if (h.oom == PUCI_OK && h.data) {
            hdr = curl_slist_append(hdr, h.data);
        }
        puci_strbuf_destroy(&h);
    }
    if (hdr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    }

    /* Response delivery. */
    http_ctx hctx;
    hctx.cb = data_cb;
    hctx.ud = userdata;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &hctx);

    CURLcode rc = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (out_status) {
        *out_status = (int)code;
    }

    curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        if (err && errlen > 0) {
            snprintf(err, errlen, "%s", msg);
        }
        if (rc == CURLE_OPERATION_TIMEDOUT) {
            return PUCI_ERR_TIMEOUT;
        }
        if (rc == CURLE_HTTP_RETURNED_ERROR) {
            return PUCI_ERR_UNKNOWN;
        }
        return PUCI_ERR_NETWORK;
    }
    return PUCI_OK;
}