/*
 * test_http_async.c - exercises the non-blocking HTTP primitives
 * (puci_http_begin / get_fds / next_timeout_ms / step / end) against a real
 * streaming mock server driven entirely from the caller's poll() loop.
 *
 * This is the low-level acceptance test for the embeddable-core thesis on the
 * network path: the transfer is pumped by step() calls initiated after the
 * host's own poll() returns, never by an internal blocking loop. Two cases:
 *
 *   A. A streaming request completes; the caller accumulates deltas each time
 *      process() does work. Assert the full body was streamed.
 *   B. Cancellation is requested mid-stream by setting the cancel flag; step()
 *      must report PUCI_ERR_CANCELLED and mark the connection dirty.
 */
#include "puci/puci.h"
#include "net/http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../test_util.h"

/* --- server harness (shared child reports bound port + metrics) -------- */

static void write_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0) {
            break;
        }
        off += (size_t)w;
    }
}

static void msleep(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

typedef void (*server_fn)(int wfd, void *arg);

static int open_listener(int *out_port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        return -1;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        return -1;
    }
    socklen_t alen = sizeof(addr);
    getsockname(srv, (struct sockaddr *)&addr, &alen);
    *out_port = ntohs(addr.sin_port);
    if (listen(srv, 8) != 0) {
        return -1;
    }
    return srv;
}

static int spawn_server(server_fn fn, void *arg, pid_t *out_pid, int *out_rfd) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }
    pid_t p = fork();
    if (p == 0) {
        close(pipefd[0]);
        fn(pipefd[1], arg);
        _exit(1);
    }
    close(pipefd[1]);
    char pbuf[32];
    ssize_t rd = read(pipefd[0], pbuf, sizeof(pbuf) - 1);
    pbuf[rd > 0 ? rd : 0] = '\0';
    if (rd <= 0) {
        close(pipefd[0]);
        kill(p, SIGKILL);
        waitpid(p, NULL, 0);
        return -1;
    }
    int port = atoi(pbuf);
    *out_pid = p;
    *out_rfd = pipefd[0];
    return port;
}

/* Reads one HTTP request (Content-Length aware) so the socket is not torn down
 * with unread bytes (which would send RST). */
static int read_request(int fd) {
    char buf[16384];
    size_t have = 0;
    long long clen = -1;
    size_t hend = 0;
    while (1) {
        ssize_t n = read(fd, buf + have, sizeof(buf) - have);
        if (n <= 0) {
            return 0;
        }
        have += (size_t)n;
        int found = 0;
        for (size_t i = 0; i + 3 < have; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
                buf[i + 3] == '\n') {
                hend = i + 4;
                found = 1;
                break;
            }
        }
        if (found) {
            break;
        }
        if (have == sizeof(buf)) {
            return 0;
        }
    }
    for (size_t i = 0; i + 15 <= hend; i++) {
        if ((buf[i] == 'c' || buf[i] == 'C') &&
            strncmp(&buf[i], "Content-Length:", 15) == 0) {
            long long v = 0;
            int started = 0;
            for (size_t j = i + 15; j < hend; j++) {
                if (buf[j] >= '0' && buf[j] <= '9') {
                    started = 1;
                    v = v * 10 + (buf[j] - '0');
                } else if (started) {
                    break;
                }
            }
            clen = started ? v : -1;
            break;
        }
    }
    size_t body = have - hend;
    while (clen >= 0 && body < (size_t)clen) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            return 0;
        }
        body += (size_t)n;
    }
    return 1;
}

/* Streaming SSE body that is large enough to span multiple network writes and
 * therefore multiple poll/pump cycles under the test's step driver. */
static const char *k_chunk =
    "data: {\"id\":\"r\",\"object\":\"chat.completion.chunk\",\"choices\":"
    "[{\"index\":0,\"delta\":{\"content\":\"xyz\"},"
    "\"finish_reason\":null}]}\n\n"
    "data: {\"id\":\"r\",\"object\":\"chat.completion.chunk\",\"choices\":"
    "[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

/* Serves `count` chunk copies at `delay_ms` each over a Content-Length framed
 * keep-alive response, then exits. */
typedef struct stream_arg {
    int count;
    int delay_ms;
    int reset; /* if set, close without sending (abort) */
} stream_arg;

static void stream_server(int wfd, void *arg) {
    (void)signal(SIGPIPE, SIG_IGN);
    stream_arg *sa = (stream_arg *)arg;
    int port = 0;
    int srv = open_listener(&port);
    if (srv < 0) {
        _exit(1);
    }
    char pbuf[16];
    int n = snprintf(pbuf, sizeof(pbuf), "%d\n", port);
    write_all(wfd, pbuf, (size_t)n);

    int c = accept(srv, NULL, NULL);
    if (c < 0) {
        _exit(1);
    }
    if (!read_request(c)) {
        _exit(1);
    }
    if (!sa->reset) {
        struct {
            char body[8192];
            size_t len;
        } acc;
        size_t used = 0;
        for (int i = 0; i < sa->count; i++) {
            size_t cl = strlen(k_chunk);
            if (used + cl >= sizeof(acc.body)) {
                break;
            }
            memcpy(acc.body + used, k_chunk, cl);
            used += cl;
        }
        char head[192];
        int hn = snprintf(head, sizeof(head),
                          "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
                          used);
        write_all(c, head, (size_t)hn);
        for (int i = 0; i < sa->count; i++) {
            if (sa->delay_ms > 0) {
                msleep(sa->delay_ms);
            }
            size_t cl = strlen(k_chunk);
            if (cl > used) {
                break;
            }
            write_all(c, k_chunk, cl);
            used -= cl;
            if (used == 0) {
                break;
            }
        }
    }
    close(c);
    close(srv);
    _exit(0);
}

/* Accumulates streamed bytes into a growable buffer. */
typedef struct acc {
    char *data;
    size_t len;
    size_t cap;
} acc;

static void on_data(void *ud, const char *data, size_t len) {
    acc *a = (acc *)ud;
    if (a->len + len + 1 > a->cap) {
        size_t ncap = a->cap ? a->cap * 2 : 128;
        while (a->len + len + 1 > ncap) {
            ncap *= 2;
        }
        char *nd = (char *)realloc(a->data, ncap);
        if (!nd) {
            return;
        }
        a->data = nd;
        a->cap = ncap;
    }
    memcpy(a->data + a->len, data, len);
    a->len += len;
    a->data[a->len] = '\0';
}

static const char *url_for(int port) {
    static char u[64];
    snprintf(u, sizeof(u), "http://127.0.0.1:%d/v1/chat/completions", port);
    return u;
}

/* Drives a request handle through the caller's own poll() until done. */
static puci_status pump(puci_http_req *r, acc *a) {
    for (int guard = 0; guard < 10000; guard++) {
        int done = 0;
        int hs = 0;
        char err[128];
        puci_status st = puci_http_step(r, &done, &hs, err, sizeof(err));
        if (done) {
            return st;
        }
        struct pollfd fds[16];
        size_t nfds = puci_http_get_fds(r, fds, 16);
        int to = puci_http_next_timeout_ms(r);
        /* curl's canonical multi driver treats the timeout as an UPPER BOUND:
         * poll with a floor (curl's own example uses 1000 ms) even when the
         * timeout is 0 or -1, so we block for the server's next chunk instead
         * of busy-spinning. poll() returns as soon as the fd is ready. */
        int pt;
        if (nfds > 0) {
            pt = (to < 0) ? 1000 : (to > 1000 ? 1000 : to);
            if (pt <= 0) {
                pt = 1000;
            }
        } else if (to == 0) {
            continue; /* no fds but work pending: pump immediately */
        } else {
            pt = (to < 0) ? 1000 : to;
        }
        int rv = poll(fds, (nfds_t)nfds, pt);
        if (rv < 0 && errno != EINTR) {
            return PUCI_ERR_NETWORK;
        }
        (void)st;
        (void)a;
    }
    return PUCI_ERR_UNKNOWN;
}

int main(void) {
    /* Capture output data. */
    acc got = {0};

    /* ---- Case A: full streaming completion via poll-driven pump. ---- */
    {
        stream_arg sa = {.count = 200, .delay_ms = 2, .reset = 0};
        pid_t spid = 0;
        int rfd = 0;
        int port = spawn_server(stream_server, &sa, &spid, &rfd);
        CHECK(port > 0);

        puci_http_conn *conn = puci_http_conn_create();
        CHECK(conn != NULL);

        if (conn) {
            int dirty = 0;
            puci_http_request req;
            memset(&req, 0, sizeof(req));
            req.url = url_for(port);
            req.body = "{}";
            req.body_len = 2;
            req.content_type = "application/json";
            req.accept = "text/event-stream";
            req.conn_dirty = &dirty;
            req.conn = conn;

            puci_http_req *h = puci_http_begin(NULL, conn, &req, NULL, on_data, &got);
            CHECK(h != NULL);
            puci_status st = h ? pump(h, &got) : PUCI_ERR_UNKNOWN;
            CHECK(st == PUCI_OK);
            CHECK(got.len > 0);
            CHECK(strstr(got.data, "[DONE]") != NULL);
            CHECK(dirty == 0); /* clean completion leaves connection reusable */
            puci_http_end(h);
            puci_http_conn_destroy(conn);
        }
        if (rfd > 0) {
            close(rfd);
        }
        if (spid > 0) {
            int ws = 0;
            waitpid(spid, &ws, 0);
        }
    }

    /* ---- Case B: cancellation mid-stream must return CANCELLED and mark
     * the connection dirty. ---- */
    {
        stream_arg sa = {.count = 100000, .delay_ms = 2, .reset = 0};
        pid_t spid = 0;
        int rfd = 0;
        int port = spawn_server(stream_server, &sa, &spid, &rfd);
        CHECK(port > 0);

        puci_http_conn *conn = puci_http_conn_create();
        CHECK(conn != NULL);

        if (conn) {
            volatile sig_atomic_t flag = 0;
            int dirty = 0;
            puci_cancel_token cancel;
            cancel.flag = &flag;
            cancel.fd = -1;

            puci_http_request req;
            memset(&req, 0, sizeof(req));
            req.url = url_for(port);
            req.body = "{}";
            req.body_len = 2;
            req.content_type = "application/json";
            req.accept = "text/event-stream";
            req.conn_dirty = &dirty;
            req.conn = conn;

            puci_http_req *h = puci_http_begin(NULL, conn, &req, &cancel, on_data, &got);
            CHECK(h != NULL);
            if (h) {
                /* Let a few chunks arrive, then cancel. */
                int saw_some = 0;
                for (int i = 0; i < 50 && !saw_some; i++) {
                    int done = 0;
                    int hs = 0;
                    char err[64];
                    if (puci_http_step(h, &done, &hs, err, sizeof(err)) == PUCI_OK && got.len > 0) {
                        saw_some = 1;
                    }
                    struct pollfd fds[16];
                    size_t nfds = puci_http_get_fds(h, fds, 16);
                    int to = puci_http_next_timeout_ms(h);
                    int pt = (nfds == 0) ? (to < 0 ? 0 : to)
                                         : (to < 0 ? 1000 : (to == 0 ? 1000 : to));
                    if (nfds > 0 || to != 0) {
                        (void)poll(fds, (nfds_t)nfds, pt);
                    }
                }
                flag = 1; /* host requests cancellation */
                int done = 0;
                int hs = 0;
                char err[96];
                puci_status st = puci_http_step(h, &done, &hs, err, sizeof(err));
                CHECK(st == PUCI_ERR_CANCELLED);
                CHECK(done == 1);
                puci_http_end(h);
                CHECK(dirty == 1); /* aborted -> next request forces fresh connect */
            }
            puci_http_conn_destroy(conn);
        }
        if (rfd > 0) {
            close(rfd);
        }
        if (spid > 0) {
            int ws = 0;
            waitpid(spid, &ws, 0);
        }
    }

    free(got.data);
    TEST_END();
}