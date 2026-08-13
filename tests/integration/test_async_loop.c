/* test_async_loop.c - async-loop busy-spin regression test.
 *
 * Drives a full turn through the public non-blocking API
 * (puci_session_begin_turn / get_fds / next_timeout_ms / process / next_event)
 * from a single-threaded poll() loop against a mock server that streams many
 * chunks with a small delay between each.
 *
 * If puci_session_next_timeout_ms ever reports 0 when there is genuinely
 * nothing pending (a busy-spin), a host calls process() in a tight loop and
 * burns a core. This test counts process() calls over a full streamed turn and
 * asserts the count stays below a sane bound - far above the number of chunks,
 * but far below what an unthrottled busy-spin over the same wall time would
 * produce. It also asserts the full streamed text arrives and the turn
 * completes, so it doubles as an acceptance test for driving one turn from a
 * caller-owned event loop.
 */

#include "puci/puci.h"

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

/* Internal session headers for the buffer-capacity accessor used by the
 * no-per-token-allocation test. The test meson target adds ../src/lib to the
 * include path. libpuci is a static lib, so the headers come from source. */
#include "puci_internal.h"

/* Servers write the bound port as the first line on wfd, then exit. */
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
        close(srv);
        return -1;
    }
    socklen_t alen = sizeof(addr);
    getsockname(srv, (struct sockaddr *)&addr, &alen);
    *out_port = ntohs(addr.sin_port);
    if (listen(srv, 8) != 0) {
        close(srv);
        return -1;
    }
    return srv;
}

/* Reads one HTTP request (headers + Content-Length body). 1 = ok, 0 = EOF. */
static int read_request(int fd) {
    char buf[16384];
    size_t have = 0;
    long long clen = -1;
    size_t hend = 0;
    for (;;) {
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

/* One text-producing SSE delta chunk; provider parses it into the answer. */
static const char *k_chunk =
    "data: {\"id\":\"r\",\"object\":\"chat.completion.chunk\",\"choices\":"
    "[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"t\"},"
    "\"finish_reason\":null}]}\n\n";

static const char *chunk_template =
    "data: {\"id\":\"r\",\"object\":\"chat.completion.chunk\",\"choices\":"
    "[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"%c\"},"
    "\"finish_reason\":null}]}\n\n";

/* Builds one SSE delta whose answer-content is a single char `c`. Same byte
 * length as k_chunk for any single-char c, so the overall Content-Length is
 * unchanged. */
static size_t make_chunk(char c, char *dst, size_t cap) {
    int n = snprintf(dst, cap, chunk_template, c);
    return n > 0 ? (size_t)n : 0;
}

static void stream_server(int wfd, int count, int delay_ms, int distinct) {
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
    size_t cl = strlen(k_chunk);
    size_t total = (size_t)count * cl;
    char head[192];
    int hn = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                      "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
                      total);
    write_all(c, head, (size_t)hn);
    char chunk[256];
    for (int i = 0; i < count; i++) {
        if (delay_ms > 0) {
            msleep(delay_ms); /* force incremental streaming / poll wakes */
        }
        if (distinct) {
            size_t clen = make_chunk((char)('a' + (i % 26)), chunk,
                                     sizeof(chunk));
            /* chunk_template has the same byte length as k_chunk for any
             * single-char content, so Content-Length stays exact. */
            write_all(c, chunk, clen);
        } else {
            write_all(c, k_chunk, cl);
        }
    }
    close(c);
    close(srv);
    _exit(0);
}

typedef struct counts {
    int deltas;
    int completes;
    int errors;
    char *text;
    size_t text_used;
    size_t text_cap;
} counts;

static void test_bounded_process_calls(void) {
    int NCHUNKS = 300;
    int DELAY_MS = 2; /* ~600 ms of dribbled streaming */

    int pf[2];
    if (pipe(pf) != 0) {
        CHECK(0);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(pf[0]);
        stream_server(pf[1], NCHUNKS, DELAY_MS, 0);
    }
    close(pf[1]);
    char pbuf[32];
    ssize_t rd = read(pf[0], pbuf, sizeof(pbuf) - 1);
    close(pf[0]);
    if (rd <= 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        CHECK(0);
        return;
    }
    pbuf[rd] = '\0';
    int port = atoi(pbuf);
    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = base_url,
        .api_key = NULL,
        .model = "mock-model",
        .timeout_ms = 30000,
    };
    CHECK(puci_context_set_provider(ctx, &pcfg) == PUCI_OK);

    counts c;
    memset(&c, 0, sizeof(c));
    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.event_userdata = &c;

    puci_session *sess = NULL;
    CHECK(puci_session_create(ctx, &sopts, &sess) == PUCI_OK);
    CHECK(sess != NULL);

    CHECK(puci_session_begin_turn(sess, "say visit") == PUCI_OK);

    long process_calls = 0;
    int completed = 0;
    int errored = 0;
    int guard = 0;
    /* Guard is far above the honest cost (~NCHUNKS flow) but finite, so a
     * busy-spin hits it and fails the bounded-call assert instead of hanging. */
    while (guard++ < 1000000) {
        int ms = -1;
        CHECK(puci_session_next_timeout_ms(sess, &ms) == PUCI_OK);
        struct pollfd fds[16];
        size_t n = 0;
        CHECK(puci_session_get_fds(sess, fds, 16, &n) == PUCI_OK);

        int pt;
        if (n > 0) {
            pt = ms < 0 ? 1000 : (ms > 1000 ? 1000 : ms);
            if (pt <= 0) {
                pt = 0; /* poll(0): return immediately; work is pending */
            }
            (void)poll(fds, (nfds_t)n, pt);
        } else {
            pt = ms < 0 ? 1000 : (ms > 1000 ? 1000 : ms);
            (void)poll(NULL, 0, pt);
        }

        puci_status st = puci_session_process(sess, fds, n);
        process_calls++;
        (void)st;

        /* Drain produced events. For the non-blocking API, events are
         * delivered via next_event, not callbacks. Accumulate the streamed
         * text from the text-delta events into a caller-owned buffer. */
        puci_event ev;
        while (puci_session_next_event(sess, &ev)) {
            switch (ev.kind) {
            case PUCI_EVENT_TOKEN_DELTA: {
                c.deltas++;
                /* ev.text is owned by the session and valid only until the
                 * next next_event()/process() call; copy it out now. */
                if (ev.text) {
                    size_t add = ev.text_len;
                    size_t nlen = c.text_used + add;
                    if (nlen + 1 > c.text_cap) {
                        size_t ncap = c.text_cap ? c.text_cap * 2 : 64;
                        while (ncap < nlen + 1) {
                            ncap *= 2;
                        }
                        char *nb = (char *)realloc(c.text, ncap);
                        CHECK(nb != NULL);
                        c.text = nb;
                        c.text_cap = ncap;
                    }
                    memcpy(c.text + c.text_used, ev.text, add);
                    c.text_used += add;
                    c.text[c.text_used] = '\0';
                }
                break;
            }
            case PUCI_EVENT_TURN_COMPLETE:
                completed = 1;
                break;
            case PUCI_EVENT_ERROR:
                errored = 1;
                break;
            default:
                break;
            }
        }
        if (completed || errored) {
            break;
        }
    }

    CHECK(completed == 1);
    CHECK(errored == 0);
    /* The real, throttled cost is ~NCHUNKS-ish process() calls (plus setup).
     * A busy-spin over ~600 ms would be ~hundreds of thousands. 20k cleanly
     * separates honest throttling from a spin. */
    CHECK(process_calls < 20000);
    CHECK(c.deltas == NCHUNKS);
    /* Slice 1 test 1: text must arrive on the async path. All 300 chunks are
     * single 't' deltas; assert the accumulated text is exactly 300 't's. */
    CHECK(c.text != NULL);
    CHECK(c.text_used == (size_t)NCHUNKS);
    int all_ts = 1;
    for (size_t i = 0; i < c.text_used; i++) {
        if (c.text[i] != 't') {
            all_ts = 0;
            break;
        }
    }
    CHECK(all_ts == 1);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    free(c.text);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Slice 1 tests: async-path text delivery, invalidation, no-alloc.    */
/* ------------------------------------------------------------------ */

/* Visit callback for drive_streaming_turn; return 1 to continue draining. */
typedef int (*ev_visit)(void *ud, puci_session *sess, puci_event *ev);

/* Drives one full streaming turn against a forked mock server streaming
 * `nchunks` deltas (single char each), invoking `visit` for every drained
 * event. Returns 0 on success, -1 on setup failure. */
static int drive_streaming_turn(int nchunks, int delay_ms, int distinct,
                                ev_visit visit, void *ud) {
    int pf[2];
    if (pipe(pf) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(pf[0]);
        stream_server(pf[1], nchunks, delay_ms, distinct);
    }
    close(pf[1]);
    char pbuf[32];
    ssize_t rd = read(pf[0], pbuf, sizeof(pbuf) - 1);
    close(pf[0]);
    if (rd <= 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    pbuf[rd] = '\0';
    int port = atoi(pbuf);
    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);

    puci_context *ctx = NULL;
    if (puci_context_create(NULL, &ctx) != PUCI_OK) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = base_url,
        .api_key = NULL,
        .model = "mock-model",
        .timeout_ms = 30000,
    };
    puci_context_set_provider(ctx, &pcfg);
    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    puci_session *sess = NULL;
    if (puci_session_create(ctx, &sopts, &sess) != PUCI_OK) {
        puci_context_destroy(ctx);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    int bad = 0;
    if (puci_session_begin_turn(sess, "say visit") != PUCI_OK) {
        bad = 1;
    }
    int done = 0;
    int guard = 0;
    while (!bad && !done && guard++ < 1000000) {
        int ms = -1;
        puci_session_next_timeout_ms(sess, &ms);
        struct pollfd fds[16];
        size_t n = 0;
        puci_session_get_fds(sess, fds, 16, &n);
        int pt = ms < 0 ? 1000 : (ms > 1000 ? 1000 : ms);
        if (n > 0) {
            (void)poll(fds, (nfds_t)n, pt);
        } else {
            (void)poll(NULL, 0, pt);
        }
        (void)puci_session_process(sess, fds, n);
        puci_event ev;
        while (puci_session_next_event(sess, &ev)) {
            if (visit && !visit(ud, sess, &ev)) {
                bad = 1;
            }
            if (ev.kind == PUCI_EVENT_TURN_COMPLETE ||
                ev.kind == PUCI_EVENT_ERROR ||
                ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                done = 1;
            }
        }
    }

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return bad ? -1 : 0;
}

/* ---- Slice 1 test 3: no per-token allocation growth ---- */

typedef struct cap_hist {
    size_t caps[400];
    int n;
} cap_hist;

static int visit_cap(void *ud, puci_session *sess, puci_event *ev) {
    cap_hist *h = (cap_hist *)ud;
    /* Capacity is only meaningful for text-delivering events; record it for
     * the token deltas so the terminal event cannot perturb the count. */
    if (ev->kind == PUCI_EVENT_TOKEN_DELTA && h->n < 400) {
        h->caps[h->n++] = puci_session_event_text_cap(sess);
    }
    return 1;
}

static void test_no_per_token_alloc(void) {
    cap_hist h;
    memset(&h, 0, sizeof(h));
    CHECK(drive_streaming_turn(300, 2, 0, visit_cap, &h) == 0);
    CHECK(h.n == 300); /* one capacity sample per token delta */
    /* The reusable buffer is allocated once on the first delivery and must not
     * grow across the remaining ~299 deltas: steady state is allocation-free. */
    CHECK(h.n > 0);
    CHECK(h.caps[0] > 0);
    int settled = 1;
    for (int i = 1; i < h.n; i++) {
        if (h.caps[i] != h.caps[0]) {
            settled = 0;
            break;
        }
    }
    CHECK(settled == 1);
}

/* ---- Slice 1 test 2: documented invalidation holds ---- */

typedef struct inval_state {
    const char *p_first;
    char first_char;
    char second_char;
    size_t cap_first;
    size_t cap_second;
    int saw_first;
    int saw_second;
    int reused_same_storage;
    int overwritten_to_new;
} inval_state;

static int visit_inval(void *ud, puci_session *sess, puci_event *ev) {
    inval_state *s = (inval_state *)ud;
    if (ev->kind == PUCI_EVENT_TOKEN_DELTA && !s->saw_second) {
        if (!s->saw_first) {
            s->saw_first = 1;
            s->p_first = ev->text;
            s->first_char = ev->text_len ? ev->text[0] : '\0';
            s->cap_first = puci_session_event_text_cap(sess);
        } else {
            s->saw_second = 1;
            s->second_char = ev->text_len ? ev->text[0] : '\0';
            s->cap_second = puci_session_event_text_cap(sess);
            /* No capacity growth between the two deltas means the second delta
             * reused the first delta's storage: the reusable buffer is not
             * per-token. */
            s->reused_same_storage = (s->cap_first == s->cap_second);
            /* With the storage reused in place, the pointer captured from the
             * first delta must no longer hold the first delta's data: the
             * library reset and rewrote the buffer, so a stale caller pointer
             * now reads the second delta's payload. Dereference only when reuse
             * held, so we never read memory that was reallocated away. */
            if (s->reused_same_storage && s->p_first != NULL) {
                s->overwritten_to_new =
                    (*s->p_first == s->second_char &&
                     s->first_char != s->second_char);
            }
        }
    }
    return 1;
}

static void test_invalidation(void) {
    inval_state s;
    memset(&s, 0, sizeof(s));
    /* Distinct per-delta chars ('a','b',...) so consecutive deltas differ. */
    CHECK(drive_streaming_turn(8, 1, 1, visit_inval, &s) == 0);
    CHECK(s.saw_first == 1);
    CHECK(s.saw_second == 1);
    CHECK(s.first_char == 'a');
    CHECK(s.second_char == 'b');
    /* The documented contract: a text pointer is invalidated by the next
     * next_event()/process() call, and the library must not keep a stale
     * reference to caller state nor allocate per token. */
    CHECK(s.reused_same_storage == 1);
    CHECK(s.overwritten_to_new == 1);
}

int main(void) {
    test_bounded_process_calls();
    test_invalidation();
    test_no_per_token_alloc();
    
    /* Test get_fds sizing query: cap==0 should return required count. */
    {
        puci_context *ctx = NULL;
        CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
        puci_provider_config pcfg = {
            .name = "openai",
            .base_url = "http://127.0.0.1:9999/v1",
            .api_key = NULL,
            .model = "test-model",
            .timeout_ms = 5000,
        };
        CHECK(puci_context_set_provider(ctx, &pcfg) == PUCI_OK);
        
        puci_session_options sopts = {0};
        sopts.provider = pcfg;
        puci_session *sess = NULL;
        CHECK(puci_session_create(ctx, &sopts, &sess) == PUCI_OK);
        
        /* Before turn starts, no fds needed. */
        size_t count = 0;
        CHECK(puci_session_get_fds(sess, NULL, 0, &count) == PUCI_OK);
        CHECK(count == 0);
        
        /* Start a turn. */
        CHECK(puci_session_begin_turn(sess, "test") == PUCI_OK);
        
        /* Now query sizing: cap==0 with NULL buffer should return required count. */
        count = 0;
        CHECK(puci_session_get_fds(sess, NULL, 0, &count) == PUCI_OK);
        /* Exact count varies (depends on connection state), but should be > 0
         * since we have a pending HTTP request. */
        CHECK(count > 0);
        
        /* Query with adequate capacity should return same count and fill buffer. */
        struct pollfd fds[16];
        size_t actual = 0;
        CHECK(puci_session_get_fds(sess, fds, 16, &actual) == PUCI_OK);
        CHECK(actual == count);
        CHECK(actual > 0);
        
        puci_session_destroy(sess);
        puci_context_destroy(ctx);
    }
    
    TEST_END();
}