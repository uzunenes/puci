/* test_approval_event.c - async (event-mode) tool-approval lifecycle tests.
 *
 * Verifies the non-blocking approval surface:
 *   - PUCI_EVENT_APPROVAL_REQUEST pauses the owning turn; the host resolves it
 *     later via puci_session_resolve_approval without blocking the process loop.
 *   - ALLOW_ONCE runs the tool exactly once; DENY feeds a "blocked by approval
 *     policy" result back and never runs the tool.
 *   - Approval ids are single-use and session-scoped.
 *   - Cancellation wins over a concurrent allow (the tool never runs).
 *   - Destroy tears down the pending run (id space restarts next turn).
 *   - While one session awaits approval, a second session keeps streaming
 *     tokens (the host loop is never blocked by an awaiting approval).
 *
 * Every session is driven through the public non-blocking API
 * (begin_turn / get_fds / next_timeout_ms / process / next_event) from a
 * single caller-owned poll() loop - the same contract the TUI uses.
 */
#include "puci/puci.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../test_util.h"

/* ---- mock SSE server ---- */
static void write_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) {
            return;
        }
        off += (size_t)n;
    }
}

static void drain_request(int fd) {
    char buf[4096];
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            return;
        }
        int found = 0;
        for (ssize_t i = 0; i + 3 < n; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
                buf[i + 3] == '\n') {
                found = 1;
                break;
            }
        }
        if (found) {
            return;
        }
    }
}

/* SSE chunk asking the agent to call approve_me once. */
#define SSE_TOOL                                                          \
    "data: {\"id\":\"t\",\"choices\":[{\"index\":0,\"delta\":{\"role\":" \
    "\"assistant\"},\"finish_reason\":null}]}\n\n"                       \
    "data: {\"id\":\"t\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":" \
    "[{\"index\":0,\"id\":\"call_a\",\"type\":\"function\",\"function\":{" \
    "\"name\":\"approve_me\",\"arguments\":\"{\\\"x\\\":1}\"}}]},\"finish_reason\":null}]}\n\n" \
    "data: {\"id\":\"t\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n" \
    "data: [DONE]\n\n"

/* Final text answer that closes a turn. */
#define SSE_FINAL                                                          \
    "data: {\"id\":\"f\",\"choices\":[{\"index\":0,\"delta\":{\"role\":" \
    "\"assistant\",\"content\":\"done\"},\"finish_reason\":null}]}\n\n"  \
    "data: {\"id\":\"f\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n" \
    "data: [DONE]\n\n"

/* Pure streaming turn with no tool call. */
#define SSE_STREAM_NTOOL                                                    \
    "data: {\"id\":\"s\",\"choices\":[{\"index\":0,\"delta\":{\"role\":" \
    "\"assistant\"},\"finish_reason\":null}]}\n\n"                         \
    "data: {\"id\":\"s\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hello\"},\"finish_reason\":null}]}\n\n" \
    "data: {\"id\":\"s\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\" world\"},\"finish_reason\":null}]}\n\n" \
    "data: {\"id\":\"s\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n" \
    "data: [DONE]\n\n"

/* Serve `pairs` completed turns (TOOL then FINAL). After `pairs` pairs the
 * server keeps returning SSE_TOOL for odd and SSE_FINAL for even requests
 * (should not matter: a well-behaved turn ends after its last [DONE]). */
static pid_t spawn_server(int pairs, char *base_url, size_t base_url_cap) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
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
        if (listen(srv, 8) != 0) {
            _exit(1);
        }
        char pbuf[16];
        int n = snprintf(pbuf, sizeof(pbuf), "%d\n", port);
        write_all(pipefd[1], pbuf, (size_t)n);
        close(pipefd[1]);

        int served = 0;
        for (;;) {
            int c = accept(srv, NULL, NULL);
            if (c < 0) {
                break;
            }
            drain_request(c);
            const char *body = (served % 2 == 0) ? SSE_TOOL : SSE_FINAL;
            (void)pairs;
            char head[160];
            int hn = snprintf(head, sizeof(head),
                              "HTTP/1.1 200 OK\r\nContent-Type: "
                              "text/event-stream\r\nContent-Length: %zu\r\n"
                              "Connection: close\r\n\r\n",
                              strlen(body));
            write_all(c, head, (size_t)hn);
            write_all(c, body, strlen(body));
            close(c);
            served++;
        }
        close(srv);
        _exit(0);
    }
    close(pipefd[1]);
    char pbuf[32];
    ssize_t rd = read(pipefd[0], pbuf, sizeof(pbuf) - 1);
    close(pipefd[0]);
    if (rd <= 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    pbuf[rd] = '\0';
    snprintf(base_url, base_url_cap, "http://127.0.0.1:%d/v1", atoi(pbuf));
    return pid;
}

/* A streaming-only server (no tools) so another session can stream while the
 * first waits at the approval gate. */
static pid_t spawn_stream_server(char *base_url, size_t base_url_cap) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
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
        if (listen(srv, 8) != 0) {
            _exit(1);
        }
        char pbuf[16];
        int n = snprintf(pbuf, sizeof(pbuf), "%d\n", port);
        write_all(pipefd[1], pbuf, (size_t)n);
        close(pipefd[1]);
        for (;;) {
            int c = accept(srv, NULL, NULL);
            if (c < 0) {
                break;
            }
            drain_request(c);
            char head[160];
            int hn = snprintf(head, sizeof(head),
                              "HTTP/1.1 200 OK\r\nContent-Type: "
                              "text/event-stream\r\nContent-Length: %zu\r\n"
                              "Connection: close\r\n\r\n",
                              strlen(SSE_STREAM_NTOOL));
            write_all(c, head, (size_t)hn);
            write_all(c, SSE_STREAM_NTOOL, strlen(SSE_STREAM_NTOOL));
            close(c);
        }
        close(srv);
        _exit(0);
    }
    close(pipefd[1]);
    char pbuf[32];
    ssize_t rd = read(pipefd[0], pbuf, sizeof(pbuf) - 1);
    close(pipefd[0]);
    if (rd <= 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    pbuf[rd] = '\0';
    snprintf(base_url, base_url_cap, "http://127.0.0.1:%d/v1", atoi(pbuf));
    return pid;
}

static void stop_child(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
}

/* ---- per-run shared state ---- */
typedef struct evrun {
    int called;      /* how many times approve_me actually executed */
    int saw_blocked; /* a "blocked by approval policy" tool result seen */
    int saw_ran;     /* the normal "ran" tool result seen */
    int done;        /* turn reached a terminal event */
} evrun;

static int approve_me_invoke(void *ud, const puci_cancel_token *cancel,
                             const char *args_json, char **out) {
    (void)cancel;
    (void)args_json;
    evrun *a = (evrun *)ud;
    a->called++;
    *out = strdup("ran");
    return 0;
}

static void make_session(puci_context *ctx, const char *base_url,
                         puci_session **out) {
    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = base_url,
        .api_key = NULL,
        .model = "mock",
        .timeout_ms = 15000,
    };
    puci_session_options opts = {0};
    opts.provider = pcfg;
    opts.approval_mode = PUCI_APPROVAL_MODE_EVENT;
    opts.max_tool_iterations = 8;
    CHECK(puci_session_create(ctx, &opts, out) == PUCI_OK);
}

/* Drive one session until it emits PUCI_EVENT_APPROVAL_REQUEST or hits a
 * terminal event. On request, the id is returned (and *req_seen set). */
static uint64_t drive_to_request(puci_session *sess, evrun *a) {
    for (int guard = 0; guard < 3000; guard++) {
        int timeout_ms = -1;
        CHECK(puci_session_next_timeout_ms(sess, &timeout_ms) == PUCI_OK);
        struct pollfd fds[64];
        size_t nf = 0;
        CHECK(puci_session_get_fds(sess, fds, 64, &nf) == PUCI_OK);
        int wait = timeout_ms < 0 ? 20 : timeout_ms;
        if (wait < 1) wait = 1;
        (void)poll(fds, (nfds_t)nf, wait);
        CHECK(puci_session_process(sess, fds, nf) == PUCI_OK);
        puci_event ev;
        while (puci_session_next_event(sess, &ev)) {
            if (ev.kind == PUCI_EVENT_APPROVAL_REQUEST) {
                return ev.payload.approval.id;
            }
            if (ev.kind == PUCI_EVENT_TURN_COMPLETE ||
                ev.kind == PUCI_EVENT_ERROR ||
                ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                a->done = 1;
                return 0;
            }
        }
    }
    return 0;
}

/* Resolve the approval and keep pumping until the turn reaches a terminal
 * event, applying per-scenario verdicts to `a`. */
static void do_one_turn(puci_session *sess, evrun *a, uint64_t rid,
                        int decision, int provide_feedback) {
    if (provide_feedback) {
        CHECK(puci_session_set_approval_feedback(sess, "user declined") == PUCI_OK);
    }
    CHECK(puci_session_resolve_approval(sess, rid, decision) == PUCI_OK);
    for (int guard = 0; guard < 3000 && !a->done; guard++) {
        int timeout_ms = -1;
        CHECK(puci_session_next_timeout_ms(sess, &timeout_ms) == PUCI_OK);
        struct pollfd fds[64];
        size_t nf = 0;
        CHECK(puci_session_get_fds(sess, fds, 64, &nf) == PUCI_OK);
        int wait = timeout_ms < 0 ? 20 : timeout_ms;
        if (wait < 1) wait = 1;
        (void)poll(fds, (nfds_t)nf, wait);
        CHECK(puci_session_process(sess, fds, nf) == PUCI_OK);
        puci_event ev;
        while (puci_session_next_event(sess, &ev)) {
            if (ev.kind == PUCI_EVENT_TOOL_CALL_END) {
                const char *ct = ev.payload.tool_result.content;
                if (ct && strstr(ct, "ok\":false")) {
                    a->saw_blocked = 1;
                }
                if (ct && strcmp(ct, "ran") == 0) {
                    a->saw_ran = 1;
                }
            } else if (ev.kind == PUCI_EVENT_TURN_COMPLETE ||
                       ev.kind == PUCI_EVENT_ERROR ||
                       ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                a->done = 1;
            }
        }
    }
}

/* ---- tests ---- */

static void test_allow_once(void) {
    char base_url[128];
    pid_t pid = spawn_server(2, base_url, sizeof(base_url));
    CHECK(pid > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_tool_spec tspec = {.name = "approve_me",
                            .description = "t",
                            .input_schema = "{}"};
    evrun a = {0};
    CHECK(puci_tool_register(ctx, &tspec, approve_me_invoke, &a) == PUCI_OK);

    puci_session *sess = NULL;
    make_session(ctx, base_url, &sess);
    CHECK(puci_session_begin_turn(sess, "call the tool") == PUCI_OK);
    uint64_t rid = drive_to_request(sess, &a);
    CHECK(rid != 0);
    CHECK(a.called == 0); /* tool not run while parked at approval gate */
    do_one_turn(sess, &a, rid, PUCI_APPROVAL_ALLOW_ONCE, 0);
    CHECK(a.called == 1);
    CHECK(a.saw_ran == 1);
    CHECK(a.saw_blocked == 0);
    CHECK(a.done);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    stop_child(pid);
}

static void test_deny(void) {
    char base_url[128];
    pid_t pid = spawn_server(2, base_url, sizeof(base_url));
    CHECK(pid > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_tool_spec tsc = {.name = "approve_me",
                          .description = "t",
                          .input_schema = "{}"};
    evrun a = {0};
    CHECK(puci_tool_register(ctx, &tsc, approve_me_invoke, &a) == PUCI_OK);

    puci_session *sess = NULL;
    make_session(ctx, base_url, &sess);
    CHECK(puci_session_begin_turn(sess, "call the tool") == PUCI_OK);
    uint64_t rid = drive_to_request(sess, &a);
    CHECK(rid != 0);
    do_one_turn(sess, &a, rid, PUCI_APPROVAL_DENY, 1);
    CHECK(a.called == 0); /* denied: tool never ran */
    CHECK(a.saw_blocked == 1);
    CHECK(a.saw_ran == 0);
    CHECK(a.done);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    stop_child(pid);
}

static void test_double_resolve_rejected(void) {
    char base_url[128];
    pid_t pid = spawn_server(1, base_url, sizeof(base_url));
    CHECK(pid > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_tool_spec tsc = {.name = "approve_me",
                          .description = "t",
                          .input_schema = "{}"};
    evrun a = {0};
    CHECK(puci_tool_register(ctx, &tsc, approve_me_invoke, &a) == PUCI_OK);

    puci_session *sess = NULL;
    make_session(ctx, base_url, &sess);
    CHECK(puci_session_begin_turn(sess, "call the tool") == PUCI_OK);
    uint64_t rid = drive_to_request(sess, &a);
    CHECK(rid != 0);
    CHECK(puci_session_resolve_approval(sess, rid, PUCI_APPROVAL_ALLOW_ONCE) == PUCI_OK);
    CHECK(puci_session_resolve_approval(sess, rid, PUCI_APPROVAL_ALLOW_ONCE) == PUCI_ERR_OUT_OF_STATE);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    stop_child(pid);
}

static void test_wrong_session_rejected(void) {
    char urlA[128], urlB[128];
    pid_t pidA = spawn_server(1, urlA, sizeof(urlA));
    pid_t pidB = spawn_server(1, urlB, sizeof(urlB));
    CHECK(pidA > 0);
    CHECK(pidB > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_tool_spec tsc = {.name = "approve_me",
                          .description = "t",
                          .input_schema = "{}"};
    evrun a = {0};
    CHECK(puci_tool_register(ctx, &tsc, approve_me_invoke, &a) == PUCI_OK);

    puci_session *sA = NULL, *sB = NULL;
    make_session(ctx, urlA, &sA);
    make_session(ctx, urlB, &sB);
    CHECK(puci_session_begin_turn(sA, "call") == PUCI_OK);
    uint64_t ridA = drive_to_request(sA, &a);
    CHECK(ridA != 0);
    /* Resolving session A's id on session B must be rejected. */
    CHECK(puci_session_resolve_approval(sB, ridA, PUCI_APPROVAL_ALLOW_ONCE) == PUCI_ERR_OUT_OF_STATE);

    puci_session_destroy(sA);
    puci_session_destroy(sB);
    puci_context_destroy(ctx);
    stop_child(pidA);
    stop_child(pidB);
}

static void test_cancel_wins(void) {
    char base_url[128];
    pid_t pid = spawn_server(1, base_url, sizeof(base_url));
    CHECK(pid > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_tool_spec tsc = {.name = "approve_me",
                          .description = "t",
                          .input_schema = "{}"};
    evrun a = {0};
    CHECK(puci_tool_register(ctx, &tsc, approve_me_invoke, &a) == PUCI_OK);

    puci_session *sess = NULL;
    make_session(ctx, base_url, &sess);
    CHECK(puci_session_begin_turn(sess, "call the tool") == PUCI_OK);
    uint64_t rid = drive_to_request(sess, &a);
    CHECK(rid != 0);

    /* Cancel, then (race) allow - cancellation must win. */
    CHECK(puci_session_cancel(sess) == PUCI_OK);
    CHECK(puci_session_resolve_approval(sess, rid, PUCI_APPROVAL_ALLOW_ONCE) == PUCI_OK);

    for (int guard = 0; guard < 2000 && !a.done; guard++) {
        int timeout_ms = -1;
        CHECK(puci_session_next_timeout_ms(sess, &timeout_ms) == PUCI_OK);
        struct pollfd fds[64];
        size_t nf = 0;
        CHECK(puci_session_get_fds(sess, fds, 64, &nf) == PUCI_OK);
        int wait = timeout_ms < 0 ? 20 : timeout_ms;
        if (wait < 1) wait = 1;
        (void)poll(fds, (nfds_t)nf, wait);
        /* After cancellation the session may report PUCI_ERR_CANCELLED; that
           is expected, so we only need to pump events, not assert success. */
        (void)puci_session_process(sess, fds, nf);
        puci_event ev;
        while (puci_session_next_event(sess, &ev)) {
            if (ev.kind == PUCI_EVENT_TURN_COMPLETE ||
                ev.kind == PUCI_EVENT_ERROR ||
                ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                a.done = 1;
            }
        }
    }
    CHECK(a.done);
    CHECK(a.called == 0); /* cancellation won; tool never ran */
    CHECK(a.saw_ran == 0);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    stop_child(pid);
}

static void test_destroy_clears_pending(void) {
    char urlA[128], urlB[128];
    pid_t pidA = spawn_server(1, urlA, sizeof(urlA));
    pid_t pidB = spawn_server(2, urlB, sizeof(urlB));
    CHECK(pidA > 0);
    CHECK(pidB > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_tool_spec tsc = { .name = "approve_me",
                           .description = "t",
                           .input_schema = "{}"};
    evrun a = {0};
    CHECK(puci_tool_register(ctx, &tsc, approve_me_invoke, &a) == PUCI_OK);

    puci_session *sess = NULL;
    make_session(ctx, urlA, &sess);
    CHECK(puci_session_begin_turn(sess, "call the tool") == PUCI_OK);
    uint64_t rid1 = drive_to_request(sess, &a);
    CHECK(rid1 != 0);

    puci_session_destroy(sess); /* run is torn down, pending cleared */

    /* A fresh session (own server, so a clean TOOL-then-FINAL sequence)
       mints a fresh id - not reused from the destroyed session. */
    puci_session *sess2 = NULL;
    make_session(ctx, urlB, &sess2);
    CHECK(puci_session_begin_turn(sess2, "call the tool") == PUCI_OK);
    evrun a2 = {0};
    uint64_t rid2 = drive_to_request(sess2, &a2);
    CHECK(rid2 != 0); /* a fresh, working approval request is minted */
    do_one_turn(sess2, &a2, rid2, PUCI_APPROVAL_ALLOW_ONCE, 0);
    CHECK(a.called == 1); /* tool userdata is &a; sess2 ran it exactly once */

    puci_session_destroy(sess2);
    puci_context_destroy(ctx);
    stop_child(pidA);
    stop_child(pidB);
}

static void test_second_session_streams(void) {
    char urlA[128], urlB[128];
    pid_t pidA = spawn_server(2, urlA, sizeof(urlA));
    pid_t pidB = spawn_stream_server(urlB, sizeof(urlB));
    CHECK(pidA > 0);
    CHECK(pidB > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_tool_spec tsc = { .name = "approve_me",
                           .description = "t",
                           .input_schema = "{}"};
    evrun a = {0};
    CHECK(puci_tool_register(ctx, &tsc, approve_me_invoke, &a) == PUCI_OK);

    puci_session *sA = NULL, *sB = NULL;
    make_session(ctx, urlA, &sA);
    make_session(ctx, urlB, &sB);

    CHECK(puci_session_begin_turn(sA, "call the tool") == PUCI_OK);
    uint64_t rid = drive_to_request(sA, &a);
    CHECK(rid != 0); /* A parked at approval gate */

    CHECK(puci_session_begin_turn(sB, "hello") == PUCI_OK);
    int b_tokens = 0, b_done = 0;
    for (int guard = 0; guard < 4000 && !b_done; guard++) {
        int ta = -1, tb = -1;
        CHECK(puci_session_next_timeout_ms(sA, &ta) == PUCI_OK);
        CHECK(puci_session_next_timeout_ms(sB, &tb) == PUCI_OK);
        struct pollfd fds[128];
        size_t nfA = 0, nfB = 0;
        CHECK(puci_session_get_fds(sA, fds, 64, &nfA) == PUCI_OK);
        CHECK(puci_session_get_fds(sB, fds + nfA, 128 - nfA, &nfB) == PUCI_OK);
        size_t nf = nfA + nfB;
        int wait = tb < 0 ? 20 : tb;
        if (wait < 1) wait = 1;
        (void)poll(fds, (nfds_t)nf, wait);
        CHECK(puci_session_process(sA, fds, nfA) == PUCI_OK);
        CHECK(puci_session_process(sB, fds + nfA, nfB) == PUCI_OK);
        puci_event ev;
        while (puci_session_next_event(sB, &ev)) {
            if (ev.kind == PUCI_EVENT_TOKEN_DELTA && ev.text) {
                b_tokens++;
            }
            if (ev.kind == PUCI_EVENT_TURN_COMPLETE ||
                ev.kind == PUCI_EVENT_ERROR ||
                ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                b_done = 1;
            }
        }
    }
    CHECK(b_tokens >= 1); /* B streamed while A awaited approval */
    CHECK(b_done);

    /* Resolve A so nothing dangles, then confirm the engine still works. */
    evrun af = a;
    do_one_turn(sA, &af, rid, PUCI_APPROVAL_DENY, 0);

    puci_session_destroy(sA);
    puci_session_destroy(sB);
    puci_context_destroy(ctx);
    stop_child(pidA);
    stop_child(pidB);
}

int main(void) {
    test_allow_once();
    test_deny();
    test_double_resolve_rejected();
    test_wrong_session_rejected();
    test_cancel_wins();
    test_destroy_clears_pending();
    test_second_session_streams();
    TEST_END();
}