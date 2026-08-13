/* test_two_agents.c - two concurrent agents driven from one host loop
 * (Stage A requirement 5, the primary gate for the async tool lifecycle).
 *
 * Two independent sessions, each dispatching a run_shell("/bin/sleep N")
 * tool call against its own mock server, are driven from a SINGLE poll()
 * loop with no threads and no global mutable state: per-turn fds are
 * aggregated into one pollfd array, per-turn timeouts folded via minimum,
 * and each session's process()/next_event() is called in turn every
 * iteration.
 *
 * What this proves:
 *  - Genuine concurrency, not sequential dispatch: session A sleeps 800ms and
 *    session B sleeps 300ms; both complete in wall-clock time close to
 *    max(800,300), not their sum (which would mean B waited for A).
 *  - Asymmetric cancellation: cancelling session A mid-tool-run does not
 *    affect session B, which completes normally in the same loop.
 */

#include "puci/puci.h"

#include <arpa/inet.h>
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
        for (ssize_t i = 0; i + 3 < n; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
                buf[i + 3] == '\n') {
                return;
            }
        }
    }
}

static void serve_sse(int fd, const char *body) {
    char head[128];
    int hn = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      strlen(body));
    write_all(fd, head, (size_t)hn);
    write_all(fd, body, strlen(body));
}

#define SSE_FINAL                                                            \
    "data: {\"id\":\"2\",\"choices\":[{\"index\":0,\"delta\":{\"role\":"     \
    "\"assistant\",\"content\":\"done\"},\"finish_reason\":null}]}\n\n"      \
    "data: {\"id\":\"2\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":" \
    "\"stop\"}]}\n\n"                                                        \
    "data: [DONE]\n\n"

/* Builds the tool-call SSE body for a run_shell("/bin/sleep", [seconds]). */
static void build_sse_tool(char *out, size_t cap, const char *seconds) {
    snprintf(out, cap,
            "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"role\":"
            "\"assistant\"},\"finish_reason\":null}]}\n\n"
            "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":"
            "[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":"
            "\"run_shell\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n"
            "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":"
            "[{\"index\":0,\"function\":{\"arguments\":\"{\\\"command\\\":\\\"/bin/sleep\\\","
            "\\\"args\\\":[\\\"%s\\\"]}\"}}]},\"finish_reason\":null}]}\n\n"
            "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":"
            "\"tool_calls\"}]}\n\n"
            "data: [DONE]\n\n",
            seconds);
}

static void server_child(int pipe_w, const char *seconds) {
    char body[1024];
    build_sse_tool(body, sizeof(body), seconds);

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
    write_all(pipe_w, pbuf, (size_t)n);
    close(pipe_w);

    int c1 = accept(srv, NULL, NULL);
    if (c1 >= 0) {
        drain_request(c1);
        serve_sse(c1, body);
        close(c1);
    }
    int c2 = accept(srv, NULL, NULL);
    if (c2 >= 0) {
        drain_request(c2);
        serve_sse(c2, SSE_FINAL);
        close(c2);
    }
    close(srv);
    _exit(0);
}

static int spawn_server(const char *seconds, pid_t *pid) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }
    pid_t p = fork();
    if (p == 0) {
        close(pipefd[0]);
        server_child(pipefd[1], seconds);
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

static puci_session *make_session(puci_context *ctx, int port) {
    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = base_url,
        .api_key = NULL,
        .model = "mock-model",
        .timeout_ms = 15000,
    };
    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.system_prompt = "test";
    sopts.max_tool_iterations = 8;
    puci_session *sess = NULL;
    puci_context_set_provider(ctx, &pcfg);
    if (puci_session_create(ctx, &sopts, &sess) != PUCI_OK) {
        return NULL;
    }
    return sess;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Aggregates fds/timeout for one session into the shared arrays, returning
 * the number of fds appended. *min_ms is folded down (never up). */
static size_t fold_session(puci_session *sess, int done,
                           struct pollfd *fds, size_t cap, size_t offset,
                           int *min_ms) {
    if (done) {
        return 0;
    }
    int ms = -1;
    CHECK(puci_session_next_timeout_ms(sess, &ms) == PUCI_OK);
    if (ms >= 0 && (*min_ms < 0 || ms < *min_ms)) {
        *min_ms = ms;
    }
    size_t n = 0;
    CHECK(puci_session_get_fds(sess, fds + offset, cap - offset, &n) ==
          PUCI_OK);
    return n;
}

static void test_two_concurrent_agents(void) {
    pid_t pidA = 0, pidB = 0;
    int portA = spawn_server("1", &pidA);   /* ~1s tool call */
    int portB = spawn_server("0.3", &pidB); /* ~0.3s tool call */
    CHECK(portA > 0 && portB > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    puci_session *sessA = make_session(ctx, portA);
    puci_session *sessB = make_session(ctx, portB);
    CHECK(sessA != NULL && sessB != NULL);

    CHECK(puci_session_begin_turn(sessA, "run a slow tool") == PUCI_OK);
    CHECK(puci_session_begin_turn(sessB, "run a fast tool") == PUCI_OK);

    long long t0 = now_ms();
    int done_a = 0, done_b = 0;
    puci_status status_a = PUCI_ERR_UNKNOWN, status_b = PUCI_ERR_UNKNOWN;

    /* One loop, one poll() call per iteration, no threads: this is the whole
     * point of the non-blocking API. Neither session knows about the other. */
    for (int guard = 0; guard < 40000 && !(done_a && done_b); guard++) {
        struct pollfd fds[128];
        int min_ms = -1;
        size_t na = fold_session(sessA, done_a, fds, 128, 0, &min_ms);
        size_t nb = fold_session(sessB, done_b, fds, 128, na, &min_ms);

        int wait_ms = min_ms < 0 ? 1000 : min_ms;
        if (wait_ms > 1000) {
            wait_ms = 1000;
        }
        (void)poll(fds, (nfds_t)(na + nb), wait_ms);

        if (!done_a) {
            status_a = puci_session_process(sessA, fds, na);
            puci_event ev;
            while (puci_session_next_event(sessA, &ev)) {
                if (ev.kind == PUCI_EVENT_TURN_COMPLETE ||
                    ev.kind == PUCI_EVENT_ERROR ||
                    ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                    done_a = 1;
                }
            }
        }
        if (!done_b) {
            status_b = puci_session_process(sessB, fds + na, nb);
            puci_event ev;
            while (puci_session_next_event(sessB, &ev)) {
                if (ev.kind == PUCI_EVENT_TURN_COMPLETE ||
                    ev.kind == PUCI_EVENT_ERROR ||
                    ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                    done_b = 1;
                }
            }
        }
    }
    long long elapsed = now_ms() - t0;

    CHECK(done_a && done_b);
    CHECK(status_a == PUCI_OK);
    CHECK(status_b == PUCI_OK);
    /* Sequential dispatch would take >= 1000 + 300 = 1300ms; true concurrency
     * finishes close to max(1000, 300) = 1000ms. Generous margin for CI. */
    CHECK(elapsed < 1250);

    puci_session_destroy(sessA);
    puci_session_destroy(sessB);
    puci_context_destroy(ctx);
    waitpid(pidA, NULL, 0);
    waitpid(pidB, NULL, 0);
}

static void test_asymmetric_cancel(void) {
    pid_t pidA = 0, pidB = 0;
    int portA = spawn_server("5", &pidA);   /* long enough to cancel mid-run */
    int portB = spawn_server("0.3", &pidB);
    CHECK(portA > 0 && portB > 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    puci_session *sessA = make_session(ctx, portA);
    puci_session *sessB = make_session(ctx, portB);
    CHECK(sessA != NULL && sessB != NULL);

    CHECK(puci_session_begin_turn(sessA, "run a slow tool") == PUCI_OK);
    CHECK(puci_session_begin_turn(sessB, "run a fast tool") == PUCI_OK);

    int done_a = 0, done_b = 0;
    puci_status status_a = PUCI_ERR_UNKNOWN, status_b = PUCI_ERR_UNKNOWN;
    int cancelled_a = 0;

    for (int guard = 0; guard < 40000 && !(done_a && done_b); guard++) {
        struct pollfd fds[128];
        int min_ms = -1;
        size_t na = fold_session(sessA, done_a, fds, 128, 0, &min_ms);
        size_t nb = fold_session(sessB, done_b, fds, 128, na, &min_ms);

        int wait_ms = min_ms < 0 ? 1000 : min_ms;
        if (wait_ms > 1000) {
            wait_ms = 1000;
        }
        (void)poll(fds, (nfds_t)(na + nb), wait_ms);

        if (!done_a) {
            status_a = puci_session_process(sessA, fds, na);
            puci_event ev;
            while (puci_session_next_event(sessA, &ev)) {
                if (ev.kind == PUCI_EVENT_TOOL_CALL_START && !cancelled_a) {
                    /* Cancel A the moment its (5s) tool call has visibly
                     * started, leaving B alone. */
                    CHECK(puci_session_cancel(sessA) == PUCI_OK);
                    cancelled_a = 1;
                } else if (ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                    done_a = 1;
                } else if (ev.kind == PUCI_EVENT_TURN_COMPLETE ||
                          ev.kind == PUCI_EVENT_ERROR) {
                    done_a = 1;
                }
            }
        }
        if (!done_b) {
            status_b = puci_session_process(sessB, fds + na, nb);
            puci_event ev;
            while (puci_session_next_event(sessB, &ev)) {
                if (ev.kind == PUCI_EVENT_TURN_COMPLETE ||
                    ev.kind == PUCI_EVENT_ERROR ||
                    ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                    done_b = 1;
                }
            }
        }
    }

    CHECK(done_a && done_b);
    CHECK(status_a == PUCI_ERR_CANCELLED);
    CHECK(status_b == PUCI_OK); /* B unaffected by A's cancellation */
    CHECK(puci_session_validate(sessA) == PUCI_OK);
    CHECK(puci_session_validate(sessB) == PUCI_OK);

    puci_session_destroy(sessA);
    puci_session_destroy(sessB);
    puci_context_destroy(ctx);
    /* A's mock server is still blocked in accept() waiting for the second
     * (never-sent) request, since the turn was cancelled mid-tool-call; kill
     * it rather than waiting for a connection that will never arrive. */
    kill(pidA, SIGKILL);
    waitpid(pidA, NULL, 0);
    waitpid(pidB, NULL, 0);
}

int main(void) {
    test_two_concurrent_agents();
    test_asymmetric_cancel();
    TEST_END();
}
