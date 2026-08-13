/* test_cancel.c - cancellation / interrupt tests for the agent loop.
 *
 * Drives a real puci session against a forked mock OpenAI-compatible SSE server
 * and cancels the in-flight turn from a separate thread at controlled,
 * reproducible points (seeded PRNG). After every cancellation it asserts:
 *   - the turn finished with PUCI_ERR_CANCELLED,
 *   - the session message history still validates (no dangling tool_call_id,
 *     which OpenAI-compatible endpoints reject with a 400 on the next turn),
 *   - a subsequent normal turn on the SAME session succeeds,
 *   - no child processes (zombies/orphans) are left behind,
 *   - ASan records no leaks or memory errors (when built with it).
 *
 * A fixed PRNG seed is used and printed, so any failure is exactly
 * reproducible.
 */

#include "puci/puci.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
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

/* POSIX-safe sleep (usleep is not in POSIX.1-2008). */
static void msleep(long ms) {
    while (ms > 0) {
        struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
        if (nanosleep(&ts, &ts) == 0) {
            break;
        }
        ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
}

/* ---- per-run pointer so the caretaker thread can reach the session ---- */

typedef struct mevent {
    int cancelled_events;
    int turn_completes;
    char tool_result[4096];
    int tool_result_len;
} mevent;

static void on_event(void *ud, puci_session *sess, puci_event ev,
                     const void *ctx) {
    mevent *m = (mevent *)ud;
    (void)sess;
    switch (ev.kind) {
    case PUCI_EVENT_TURN_CANCELLED:
        m->cancelled_events++;
        break;
    case PUCI_EVENT_TURN_COMPLETE:
        m->turn_completes++;
        break;
    case PUCI_EVENT_TOOL_CALL_END: {
        const puci_tool_result *r = (const puci_tool_result *)ctx;
        if (r && r->content) {
            m->tool_result_len = snprintf(m->tool_result,
                                          sizeof(m->tool_result), "%s",
                                          r->content);
        }
        break;
    }
    default:
        break;
    }
}

/* ---- mock server ---------------------------------------------------------- */

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

static void write_str(int fd, const char *s) {
    write_all(fd, s, strlen(s));
}

static void write_delta(int fd, const char *content) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "data: {\"id\":\"c\",\"object\":\"chat.completion.chunk\","
                     "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"%s\"},"
                     "\"finish_reason\":null}]}\n\n",
                     content);
    if (n > 0) {
        write_all(fd, buf, (size_t)n);
    }
}

static void write_stop(int fd) {
    write_str(fd,
              "data: {\"id\":\"c\",\"object\":\"chat.completion.chunk\","
              "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":"
              "\"stop\"}]}\n\n");
    write_str(fd, "data: [DONE]\n\n");
}

/* Reads the entire HTTP request (headers + body) before returning. It is
 * essential to consume the body: if the server responds and then close()s
 * while request-body bytes are still unread in the socket receive buffer, the
 * kernel discards them and sends an RST instead of a graceful FIN. The client
 * (curl) would then report a spurious "Connection reset by peer" on its NEXT
 * request on that connection / recovery turn. We do not model anything from the
 * body; we only need to drain it so no unread data remains at close. */
static void drain_request(int fd) {
    char buf[4096];
    size_t have = 0;
    long long content_length = -1; /* -1 = unknown -> read to EOF */
    size_t header_end = 0;         /* bytes of `have` that are the body */

    /* Read until the header terminator CRLFCRLF is found. */
    while (1) {
        ssize_t n = read(fd, buf + have, sizeof(buf) - have);
        if (n <= 0) {
            return; /* EOF or error; nothing more to drain */
        }
        have += (size_t)n;
        /* Look for CRLFCRLF in the accumulated bytes. */
        char *found = NULL;
        for (size_t i = 0; i + 3 < have; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
                buf[i + 3] == '\n') {
                found = &buf[i + 4];
                break;
            }
        }
        if (found) {
            header_end = (size_t)(found - buf);
            break;
        }
        if (have == sizeof(buf)) {
            /* Header blown the buffer; give up draining (should not happen). */
            while (read(fd, buf, sizeof(buf)) > 0) {
            }
            return;
        }
    }

    /* Parse Content-Length so we know how many body bytes to read. */
    for (size_t i = 0; i + 15 <= header_end; i++) {
        if ((buf[i] == 'c' || buf[i] == 'C') &&
            strncmp(&buf[i], "Content-Length:", 15) == 0) {
            long long v = 0;
            int started = 0;
            for (size_t j = i + 15; j < header_end; j++) {
                if (buf[j] >= '0' && buf[j] <= '9') {
                    started = 1;
                    v = v * 10 + (buf[j] - '0');
                } else if (started) {
                    break;
                }
            }
            content_length = started ? v : -1;
            break;
        }
    }

    /* Base is how many body bytes we already hold in the buffer. */
    size_t base = have - header_end;

    if (content_length >= 0) {
        /* Consume exactly content_length body bytes in total. */
        while (base < (size_t)content_length) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) {
                return;
            }
            base += (size_t)n;
        }
    } else {
        /* No Content-Length (e.g. chunked): drain until EOF. */
        while (read(fd, buf, sizeof(buf)) > 0) {
        }
    }
}

static void sse_head(int fd) {
    write_str(fd,
              "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
              "Connection: close\r\n\r\n");
}

/* Serves a long stream of `chunks` deltas, `delay_ms` apart, which finishes
 * normally with a stop + [DONE]. Used for the mid-stream interrupt test; each
 * connection is served identically so the client can cancel on any one of them
 * at any time without desynchronising the server. */
static void serve_loop_stream(int fd, int chunks, int delay_ms) {
    sse_head(fd);
    write_delta(fd, "first");
    for (int i = 1; i < chunks; i++) {
        if (delay_ms > 0) {
            msleep(delay_ms);
        }
        write_delta(fd, "pad");
    }
    write_stop(fd);
}

/* Servers a tool_call stream whose run_shell command is `cmd_json` (already a
 * JSON string literal). Then a follow-up connection serves the final reply. */
static void serve_tool(int fd, const char *cmd_json) {
    sse_head(fd);
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
                     "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":"
                     "{\"role\":\"assistant\"},\"finish_reason\":null}]}\n\n"
                     "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":"
                     "{\"tool_calls\":[{\"index\":0,\"id\":\"tool_1\","
                     "\"type\":\"function\",\"function\":{\"name\":\"run_shell\","
                     "\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n"
                     "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":"
                     "{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":"
                     "\"%s\"}}]},\"finish_reason\":null}]}\n\n"
                     "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{},"
                     "\"finish_reason\":\"tool_calls\"}]}\n\n"
                     "data: [DONE]\n\n",
                     cmd_json);
    if (n > 0 && (size_t)n < sizeof(buf)) {
        write_all(fd, buf, (size_t)n);
    }
}

static void serve_final(int fd) {
    sse_head(fd);
    write_delta(fd, "final");
    write_stop(fd);
}

/* Server plan, shared with the forked child via fork()-copied memory. */
typedef struct plan {
    int mode;         /* 0 = loop streams, 1 = tool then final, 2 = silent */
    int count;        /* number of loop-stream connections to serve (mode 0) */
    int chunks;       /* deltas per stream (mode 0) */
    int delay_ms;     /* per-delta delay (mode 0/2) */
    int silent_ms;    /* silence inserted after the first few deltas (mode 2) */
    const char *cmd_json; /* JSON args for the run_shell tool call (mode 1) */
} plan;

#define MODE_LOOP 0
#define MODE_TOOL 1
#define MODE_SILENT 2

static void server_child(int pipe_w, const plan *pl) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        _exit(1);
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        _exit(1);
    }
    socklen_t alen = sizeof(addr);
    getsockname(srv, (struct sockaddr *)&addr, &alen);
    int port = ntohs(addr.sin_port);
    if (listen(srv, 256) != 0) {
        _exit(1);
    }
    char pbuf[16];
    int pn = snprintf(pbuf, sizeof(pbuf), "%d\n", port);
    write_all(pipe_w, pbuf, (size_t)pn);
    close(pipe_w);

    if (pl->mode == MODE_LOOP) {
        for (int i = 0; i < pl->count; i++) {
            int c = accept(srv, NULL, NULL);
            if (c < 0) {
                break;
            }
            drain_request(c);
            serve_loop_stream(c, pl->chunks, pl->delay_ms);
            close(c);
        }
    } else if (pl->mode == MODE_TOOL) {
        int c1 = accept(srv, NULL, NULL);
        if (c1 >= 0) {
            drain_request(c1);
            serve_tool(c1, pl->cmd_json);
            close(c1);
        }
        int c2 = accept(srv, NULL, NULL);
        if (c2 >= 0) {
            drain_request(c2);
            serve_final(c2);
            close(c2);
        }
    } else { /* MODE_SILENT */
        int c1 = accept(srv, NULL, NULL);
        if (c1 >= 0) {
            drain_request(c1);
            sse_head(c1);
            write_delta(c1, "leading");
            msleep(pl->silent_ms);
            for (int i = 0; i < pl->chunks; i++) {
                if (pl->delay_ms > 0) {
                    msleep(pl->delay_ms);
                }
                write_delta(c1, "pad");
            }
            write_stop(c1);
            close(c1);
        }
        int c2 = accept(srv, NULL, NULL);
        if (c2 >= 0) {
            drain_request(c2);
            serve_final(c2);
            close(c2);
        }
    }
    close(srv);
    _exit(0);
}

/* Forks a server and returns its port (and pid via *pid). */
static int spawn_server(const plan *pl, pid_t *pid) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }
    pid_t p = fork();
    if (p == 0) {
        close(pipefd[0]);
        server_child(pipefd[1], pl);
    }
    close(pipefd[1]);
    char pbuf[32];
    ssize_t rd = read(pipefd[0], pbuf, sizeof(pbuf) - 1);
    close(pipefd[0]);
    if (rd <= 0) {
        kill(p, SIGKILL);
        waitpid(p, NULL, 0);
        return -1;
    }
    pbuf[rd] = '\0';
    *pid = p;
    return atoi(pbuf);
}

/* ---- caretaker thread: cancels after a seeded delay ---- */

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

/* ---- helpers ---- */

static puci_session *make_session(puci_context *ctx, int port, mevent *m,
                                  puci_approval_cb approval, void *ap_ud) {
    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);

    puci_provider_config pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.name = "openai";
    pcfg.base_url = base_url;
    pcfg.api_key = NULL;
    pcfg.model = "mock-model";
    pcfg.timeout_ms = 15000;

    puci_session_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.provider = pcfg;
    opts.event_cb = on_event;
    opts.event_userdata = m;
    opts.system_prompt = "You are a test agent.";
    opts.max_tool_iterations = 8;
    opts.approve_cb = approval;
    opts.approve_userdata = ap_ud;

    puci_session *sess = NULL;
    if (puci_session_create(ctx, &opts, &sess) != PUCI_OK) {
        return NULL;
    }
    return sess;
}

/* Counts direct children of this process (zombies or live). Used to assert no
 * orphan/zombie processes remain after cancellation. */
static int count_children(void) {
    pid_t me = getpid();
    int n = 0;
    DIR *d = opendir("/proc");
    if (!d) {
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        char path[288];
        snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        char line[256];
        pid_t ppid = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "PPid:", 5) == 0) {
                ppid = (pid_t)atoi(line + 5);
                break;
            }
        }
        fclose(f);
        if (ppid == me) {
            n++;
        }
    }
    closedir(d);
    return n;
}

/* ---- mid-stream interrupt, 100 iterations, seeded ---- */

#define ITERS 100

static void test_mid_stream_100(void) {
    int iters = ITERS;
    const char *ov = getenv("TEST_CANCEL_ITERS");
    if (ov && ov[0]) {
        iters = atoi(ov);
        if (iters < 1) {
            iters = 1;
        }
    }
    unsigned seed = 0xC0FFEE; /* fixed for reproducible runs */
    printf("mid-stream seed = %u\n", seed);
    srand(seed);

    plan pl;
    memset(&pl, 0, sizeof(pl));
    pl.mode = MODE_LOOP;
    pl.count = iters * 2;
    pl.chunks = 150;
    pl.delay_ms = 3; /* a 150-delta stream takes ~450ms */

    pid_t spid = 0;
    int port = spawn_server(&pl, &spid);
    CHECK(port > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    mevent m;
    memset(&m, 0, sizeof(m));
    puci_session *sess = make_session(ctx, port, &m, NULL, NULL);
    CHECK(sess != NULL);

    int got_cancelled = 0;
    for (int i = 0; i < iters; i++) {
        long delay_ms = 20 + (long)(rand() % 400); /* 20-420 ms */

        caretaker ct;
        ct.sess = sess;
        ct.delay_ms = delay_ms;
        pthread_t th;
        if (pthread_create(&th, NULL, caretaker_thread, &ct) != 0) {
            CHECK(0);
            break;
        }

        puci_status st = puci_session_send(sess, "stream something long");
        pthread_join(th, NULL);

        if (st == PUCI_ERR_CANCELLED) {
            got_cancelled++;
            CHECK(m.cancelled_events >= 1);
            CHECK(puci_session_validate(sess) == PUCI_OK);

            /* The session must remain usable: a normal turn succeeds. */
            m.cancelled_events = 0;
            puci_status r2 = puci_session_send(sess, "recover");
            if (r2 != PUCI_OK) {
                fprintf(stderr, "  [recover iter %d] status=%s err=%s\n", i,
                        puci_status_string(r2),
                        puci_session_error(sess) ? puci_session_error(sess)
                                                 : "(none)");
            }
            CHECK(r2 == PUCI_OK);
            CHECK(m.turn_completes >= 1);
            CHECK(puci_session_validate(sess) == PUCI_OK);
        } else {
            /* Cancellation may occasionally land just after the stream ends; a
             * clean completion is acceptable but should not happen for the
             * majority. */
            CHECK(puci_session_validate(sess) == PUCI_OK);
        }
    }

    CHECK(got_cancelled >= iters / 2);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    kill(spid, SIGKILL);
    waitpid(spid, NULL, 0);
    CHECK(count_children() == 0);
}

/* ---- cancellation during a reasoning-silence gap (no bytes) ---- */

static void test_silent_gap(void) {
    plan pl;
    memset(&pl, 0, sizeof(pl));
    pl.mode = MODE_SILENT;
    pl.chunks = 20;
    pl.delay_ms = 5;
    pl.silent_ms = 4000; /* 4s of silence before the bulk of the stream */

    pid_t spid = 0;
    int port = spawn_server(&pl, &spid);
    CHECK(port > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    mevent m;
    memset(&m, 0, sizeof(m));
    puci_session *sess = make_session(ctx, port, &m, NULL, NULL);
    CHECK(sess != NULL);

    long delay_ms = 1500; /* cancel 1.5s in, deep inside the 4s silence */
    caretaker ct;
    ct.sess = sess;
    ct.delay_ms = delay_ms;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, caretaker_thread, &ct) == 0);

    puci_status st = puci_session_send(sess, "silent reasoning");
    pthread_join(th, NULL);

    CHECK(st == PUCI_ERR_CANCELLED);
    CHECK(m.cancelled_events >= 1);
    CHECK(puci_session_validate(sess) == PUCI_OK);

    /* Recovery turn on the same session. */
    puci_status r2 = puci_session_send(sess, "recover");
    CHECK(r2 == PUCI_OK);
    CHECK(puci_session_validate(sess) == PUCI_OK);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    waitpid(spid, NULL, 0);
    CHECK(count_children() == 0);
}

/* ---- cancellation during tool execution (child process running) ---- */

static puci_approval allow_all(const puci_approval_request *req, void *ud) {
    (void)req;
    (void)ud;
    return PUCI_APPROVAL_ALLOW;
}

static void test_tool_execution_cancel(void) {
    plan pl;
    memset(&pl, 0, sizeof(pl));
    pl.mode = MODE_TOOL;
    pl.cmd_json = "{\\\"command\\\":\\\"/bin/sleep\\\",\\\"args\\\":[\\\"30\\\"]}";

    pid_t spid = 0;
    int port = spawn_server(&pl, &spid);
    CHECK(port > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    mevent m;
    memset(&m, 0, sizeof(m));
    puci_session *sess = make_session(ctx, port, &m, allow_all, NULL);
    CHECK(sess != NULL);

    long delay_ms = 500; /* cancel 0.5s into the 30s sleep */
    caretaker ct;
    ct.sess = sess;
    ct.delay_ms = delay_ms;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, caretaker_thread, &ct) == 0);

    puci_status st = puci_session_send(sess, "run a long tool");
    pthread_join(th, NULL);

    CHECK(st == PUCI_ERR_CANCELLED);
    CHECK(m.cancelled_events >= 1);
    /* The run_shell tool result reflects that it was cancelled (terminate_group
     * ran). */
    CHECK(m.tool_result_len > 0);
    CHECK(strstr(m.tool_result, "\"cancelled\":true") != NULL);
    CHECK(puci_session_validate(sess) == PUCI_OK);

    puci_status r2 = puci_session_send(sess, "recover");
    CHECK(r2 == PUCI_OK);
    CHECK(puci_session_validate(sess) == PUCI_OK);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    waitpid(spid, NULL, 0);
    /* No orphan `sleep 30` process group must survive. */
    CHECK(count_children() == 0);
}

/* ---- cancellation while the approval policy is waiting for input ---- */

typedef struct block_ap {
    volatile int saw_cancel;
} block_ap;

static puci_approval blocking_policy(const puci_approval_request *req, void *ud) {
    block_ap *ap = (block_ap *)ud;
    /* Simulate a host policy that blocks waiting for human input: poll the
     * in-flight turn's cancel fd so it returns the moment the caller cancels,
     * instead of hanging until the user answers. */
    if (req->cancel && req->cancel->fd >= 0) {
        struct pollfd pfd;
        pfd.fd = req->cancel->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        (void)poll(&pfd, 1, 10000); /* 10s cap so a bug still fails the test */
        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            ap->saw_cancel = 1;
        }
    }
    if (req->cancel && req->cancel->flag && *(req->cancel->flag)) {
        ap->saw_cancel = 1;
    }
    return PUCI_APPROVAL_DENY;
}

static void test_approval_cancel(void) {
    plan pl;
    memset(&pl, 0, sizeof(pl));
    pl.mode = MODE_TOOL;
    pl.cmd_json = "{\\\"command\\\":\\\"/bin/sleep\\\",\\\"args\\\":[\\\"30\\\"]}";

    pid_t spid = 0;
    int port = spawn_server(&pl, &spid);
    CHECK(port > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    mevent m;
    memset(&m, 0, sizeof(m));
    block_ap ap;
    memset(&ap, 0, sizeof(ap));
    puci_session *sess = make_session(ctx, port, &m, blocking_policy, &ap);
    CHECK(sess != NULL);

    long delay_ms = 400; /* cancel while the policy is blocking for input */
    caretaker ct;
    ct.sess = sess;
    ct.delay_ms = delay_ms;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, caretaker_thread, &ct) == 0);

    puci_status st = puci_session_send(sess, "needs approval");
    pthread_join(th, NULL);

    CHECK(st == PUCI_ERR_CANCELLED);
    CHECK(m.cancelled_events >= 1);
    CHECK(ap.saw_cancel == 1);
    CHECK(puci_session_validate(sess) == PUCI_OK);

    puci_status r2 = puci_session_send(sess, "recover");
    CHECK(r2 == PUCI_OK);
    CHECK(puci_session_validate(sess) == PUCI_OK);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    waitpid(spid, NULL, 0);
    CHECK(count_children() == 0);
}

static void test_cancellation_equivalence(void) {
    plan pl;
    memset(&pl, 0, sizeof(pl));
    pl.mode = MODE_LOOP;
    pl.count = 4;
    pl.chunks = 200;
    pl.delay_ms = 5; /* ~1000ms stream */

    pid_t spid = 0;
    int port = spawn_server(&pl, &spid);
    CHECK(port > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    /* 1. Cancellation on Blocking Path */
    mevent m_block;
    memset(&m_block, 0, sizeof(m_block));
    puci_session *sess_block = make_session(ctx, port, &m_block, NULL, NULL);
    CHECK(sess_block != NULL);

    caretaker ct_block;
    ct_block.sess = sess_block;
    ct_block.delay_ms = 100;
    pthread_t th_block;
    CHECK(pthread_create(&th_block, NULL, caretaker_thread, &ct_block) == 0);
    puci_status st_block = puci_session_send(sess_block, "stream to cancel");
    pthread_join(th_block, NULL);

    CHECK(st_block == PUCI_ERR_CANCELLED);
    CHECK(m_block.cancelled_events == 1);
    CHECK(puci_session_last_status(sess_block) == PUCI_ERR_CANCELLED);

    /* 2. Cancellation on Async Path */
    puci_session *sess_async = make_session(ctx, port, NULL, NULL, NULL);
    CHECK(sess_async != NULL);
    CHECK(puci_session_begin_turn(sess_async, "stream to cancel") == PUCI_OK);

    puci_status st_async = PUCI_ERR_UNKNOWN;
    int async_cancelled_event = 0;
    int async_done = 0;
    int cancel_triggered = 0;

    for (int guard = 0; guard < 20000 && !async_done; guard++) {
        int timeout_ms = -1;
        struct pollfd fds[16];
        size_t fd_count = 0;
        CHECK(puci_session_next_timeout_ms(sess_async, &timeout_ms) == PUCI_OK);
        CHECK(puci_session_get_fds(sess_async, fds, 16, &fd_count) == PUCI_OK);
        int wait_ms = timeout_ms < 0 ? 1000 : timeout_ms;
        if (wait_ms > 1000) {
            wait_ms = 1000;
        }
        (void)poll(fd_count ? fds : NULL, (nfds_t)fd_count, wait_ms);

        if (!cancel_triggered && guard > 2) {
            puci_session_cancel(sess_async);
            cancel_triggered = 1;
        }

        st_async = puci_session_process(sess_async, fds, fd_count);

        puci_event ev;
        while (puci_session_next_event(sess_async, &ev)) {
            if (ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                async_cancelled_event++;
                async_done = 1;
            } else if (ev.kind == PUCI_EVENT_TURN_COMPLETE || ev.kind == PUCI_EVENT_ERROR) {
                async_done = 1;
            }
        }
    }

    CHECK(st_async == PUCI_ERR_CANCELLED);
    CHECK(puci_session_last_status(sess_async) == PUCI_ERR_CANCELLED);
    CHECK(async_cancelled_event == 1);

    /* Both blocking and async path produced identical terminal status and cancellation event */
    CHECK(st_block == st_async);
    CHECK(m_block.cancelled_events == async_cancelled_event);

    puci_session_destroy(sess_block);
    puci_session_destroy(sess_async);
    puci_context_destroy(ctx);
    kill(spid, SIGKILL);
    waitpid(spid, NULL, 0);
    CHECK(count_children() == 0);
}

int main(void) {
    test_mid_stream_100();
    test_silent_gap();
    test_tool_execution_cancel();
    test_approval_cancel();
    test_cancellation_equivalence();
    TEST_END();
}
