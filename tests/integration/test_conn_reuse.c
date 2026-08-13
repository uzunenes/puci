/* test_conn_reuse.c - connection / keep-alive regression test.
 *
 * Guards the Task-0 scope-down of CURLOPT_FRESH_CONNECT / CURLOPT_FORBID_REUSE
 * from "set unconditionally" to "set only when the session's connection-dirty
 * flag demands it":
 *
 *   Test A (keep-alive preserved):  two consecutive normal turns on one
 *   session MUST reuse a single TCP connection. A keep-alive mock server
 *   frames each response with Content-Length so curl can reuse the socket,
 *   and reports that exactly ONE connection was accepted for both turns.
 *
 *   Test B (fresh connection after cancel):  a turn is cancelled mid-stream;
 *   the very next turn on the SAME session must open a fresh connection and
 *   succeed. A streaming mock server counts accepted connections; the parent
 *   asserts the recovery consumed a NEW accepted connection and returned
 *   PUCI_OK with no dangling history.
 *
 * Both servers report their accepted-connection count back to the parent via
 * the same pipe that carried the port.
 */

#include "puci/puci.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
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

typedef struct counts {
    int completes;
    int cancelled_events;
    int errors;
} counts;

static void on_event(void *ud, puci_session *sess, puci_event ev,
                     const void *ctx) {
    counts *c = (counts *)ud;
    (void)sess;
    (void)ctx;
    switch (ev.kind) {
    case PUCI_EVENT_TURN_COMPLETE:
        c->completes++;
        break;
    case PUCI_EVENT_TURN_CANCELLED:
        c->cancelled_events++;
        break;
    case PUCI_EVENT_ERROR:
        c->errors++;
        break;
    default:
        break;
    }
}

static void write_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    (void)signal(SIGPIPE, SIG_IGN);
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) {
            return;
        }
        off += (size_t)n;
    }
}

static void msleep(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* --- shared server harness ------------------------------------------- *
 * The child reports the bound port as the first line on wfd, then later
 * reports metrics on the same fd, then exits. The parent keeps the read end. */

typedef void (*server_fn)(int wfd, void *arg);

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
    char pbuf[16];
    int n = snprintf(pbuf, sizeof(pbuf), "%d\n", *out_port);
    (void)n;
    return srv;
}

/* --- Test A: keep-alive reuse ----------------------------------------- */

/* Reads one HTTP request via Content-Length. 1 = ok, 0 = EOF. */
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

static const char *k_turn_body =
    "data: {\"id\":\"r\",\"object\":\"chat.completion.chunk\",\"choices\":"
    "[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"ok\"},"
    "\"finish_reason\":null}]}\n\n"
    "data: {\"id\":\"r\",\"object\":\"chat.completion.chunk\",\"choices\":"
    "[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

static void serve_turn(int fd) {
    char head[192];
    int hn = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                      "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
                      strlen(k_turn_body));
    write_all(fd, head, (size_t)hn);
    write_all(fd, k_turn_body, strlen(k_turn_body));
}

/* Keep-alive server. `total` = number of requests to serve across however many
 * connections. Reports "accepted=<N>" at exit. */
typedef struct ka_arg {
    int total;
} ka_arg;

static void keepalive_server(int wfd, void *arg) {
    (void)signal(SIGPIPE, SIG_IGN);
    int total = ((ka_arg *)arg)->total;
    int port = 0;
    int srv = open_listener(&port);
    if (srv < 0) {
        _exit(1);
    }
    char pbuf[16];
    int n = snprintf(pbuf, sizeof(pbuf), "%d\n", port);
    write_all(wfd, pbuf, (size_t)n);

    int accepted = 0, served = 0;
    while (served < total) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) {
            break;
        }
        accepted++;
        while (served < total && read_request(c)) {
            serve_turn(c);
            served++;
        }
        close(c);
    }
    close(srv);
    char rb[48];
    int rn = snprintf(rb, sizeof(rb), "\nACCEPTED %d %d\n", accepted, served);
    write_all(wfd, rb, (size_t)rn);
    _exit(0);
}

/* --- Test B: streaming server for cancellation ------------------------- */

static void stream_server(int wfd, void *arg) {
    int max_conns = arg ? *(int *)arg : 2;
    (void)signal(SIGPIPE, SIG_IGN);
    int port = 0;
    int srv = open_listener(&port);
    if (srv < 0) {
        _exit(1);
    }
    char pbuf[16];
    int n = snprintf(pbuf, sizeof(pbuf), "%d\n", port);
    write_all(wfd, pbuf, (size_t)n);

    static const char *k_chunk =
        "data: {\"id\":\"c\",\"object\":\"chat.completion.chunk\",\"choices\":"
        "[{\"index\":0,\"delta\":{\"content\":\"x\"},\"finish_reason\":null}]}\n\n";
    char head[160];
    int hn = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                      "Connection: close\r\n\r\n");

    for (int served = 0; served < max_conns; served++) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) {
            break;
        }
        char m[24];
        int mn = snprintf(m, sizeof(m), "\nCONN %d\n", served);
        write_all(wfd, m, (size_t)mn);
        /* Drain the full request (headers + body) so no unread bytes remain at
         * close() - otherwise the kernel sends an RST and the recovery (or any
         * turn) sees a spurious "Connection reset by peer". */
        read_request(c);
        write_all(c, head, (size_t)hn);
        for (int i = 0; i < 300; i++) {
            write_all(c, k_chunk, strlen(k_chunk));
            msleep(5); /* ~1.5 s per stream; cancellable */
        }
        close(c);
    }
    close(srv);
    _exit(0);
}

/* --- client helpers ---------------------------------------------------- */

static puci_session *make_session(puci_context *ctx, int port, counts *c) {
    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = base_url,
        .api_key = NULL,
        .model = "mock-model",
        .timeout_ms = 15000,
    };
    puci_context_set_provider(ctx, &pcfg);
    puci_session_options op = {0};
    op.provider = pcfg;
    op.event_cb = on_event;
    op.event_userdata = c;
    op.system_prompt = "You are a connection test.";
    puci_session *sess = NULL;
    if (puci_session_create(ctx, &op, &sess) != PUCI_OK) {
        return NULL;
    }
    return sess;
}

typedef struct caretaker {
    puci_session *sess;
    long delay_ms;
} caretaker;

static void *caretaker_thread(void *arg) {
    caretaker *ct = (caretaker *)arg;
    msleep(ct->delay_ms);
    puci_session_cancel(ct->sess);
    return NULL;
}

/* Counts "CONN n" lines in a pipe buffer to determine how many connections the
 * streaming server accepted. */
static int count_conns(char *buf, size_t len) {
    (void)len;
    int k = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save)) {
        if (strncmp(tok, "CONN ", 5) == 0) {
            k++;
        }
    }
    return k;
}

int main(void) {
    /* ---- Test A: keep-alive reuse ---- */
    {
        ka_arg arg;
        arg.total = 2;
        pid_t spid = 0;
        int rfd = -1;
        int port = spawn_server(keepalive_server, &arg, &spid, &rfd);
        CHECK(port > 0);

        puci_context *ctx = NULL;
        counts c;
        memset(&c, 0, sizeof(c));
        CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
        puci_session *sess = make_session(ctx, port, &c);
        CHECK(sess != NULL);

        CHECK(puci_session_send(sess, "turn one") == PUCI_OK);
        CHECK(puci_session_send(sess, "turn two") == PUCI_OK);
        CHECK(c.completes == 2);
        CHECK(c.errors == 0);

        puci_session_destroy(sess);
        puci_context_destroy(ctx);

        int ws = 0;
        waitpid(spid, &ws, 0);
        char rb[64];
        ssize_t rn = read(rfd, rb, sizeof(rb) - 1);
        rb[rn > 0 ? rn : 0] = '\0';
        close(rfd);
        /* Keep-alive must have served both turns over ONE accepted connection. */
        fprintf(stderr, "  [reuse report] %s\n", rb);
        CHECK(strstr(rb, "ACCEPTED 1 2") != NULL);
    }

    /* ---- Test B: fresh connection after cancellation ---- */
    {
        int max_conns = 2;
        pid_t spid = 0;
        int rfd = -1;
        int port = spawn_server(stream_server, &max_conns, &spid, &rfd);
        CHECK(port > 0);

        puci_context *ctx = NULL;
        counts c;
        memset(&c, 0, sizeof(c));
        CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
        puci_session *sess = make_session(ctx, port, &c);
        CHECK(sess != NULL);

        /* Turn 1: cancel mid-stream. */
        caretaker ct;
        ct.sess = sess;
        ct.delay_ms = 150;
        pthread_t th;
        CHECK(pthread_create(&th, NULL, caretaker_thread, &ct) == 0);
        puci_status st = puci_session_send(sess, "long stream");
        pthread_join(th, NULL);
        CHECK(st == PUCI_ERR_CANCELLED);
        CHECK(c.cancelled_events >= 1);

        /* Turn 2: recovery, must succeed and consume a new connection. */
        puci_status r2 = puci_session_send(sess, "recover");
        CHECK(r2 == PUCI_OK);
        CHECK(puci_session_validate(sess) == PUCI_OK);

        puci_session_destroy(sess);
        puci_context_destroy(ctx);
        int ws = 0;
        waitpid(spid, &ws, 0);
        char rb[128];
        ssize_t rn = read(rfd, rb, sizeof(rb) - 1);
        rb[rn > 0 ? rn : 0] = '\0';
        close(rfd);
        /* The recovery turn opened a fresh connection: >= 2 accepted. */
        CHECK(count_conns(rb, (size_t)rn) >= 2);
    }

    TEST_END();
}
