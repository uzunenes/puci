/* test_agent_loop.c - end-to-end agent loop integration test.
 *
 * Spins up a tiny mock OpenAI-compatible HTTP/SSE server in a forked child and
 * drives a real puci session against it. This exercises the whole engine
 * without a live model: request building, SSE streaming, tool-call parsing,
 * tool execution (run_shell), feeding results back, and turn completion.
 *
 * Sequence served:
 *   request #1 -> assistant delta with a run_shell tool call, finish tool_calls
 *   request #2 -> assistant content deltas, finish stop, [DONE]
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
#include <poll.h>
#include <unistd.h>

#include "../test_util.h"

typedef struct counts {
    int deltas;
    int tool_starts;
    int tool_ends;
    int completes;
    char *final_text;
} counts;

static void on_event(void *ud, puci_session *sess, puci_event ev,
                     const void *ctx) {
    counts *c = (counts *)ud;
    (void)sess;
    switch (ev.kind) {
    case PUCI_EVENT_TOKEN_DELTA:
        c->deltas++;
        break;
    case PUCI_EVENT_TOOL_CALL_START:
        c->tool_starts++;
        break;
    case PUCI_EVENT_TOOL_CALL_END:
        c->tool_ends++;
        break;
    case PUCI_EVENT_TURN_COMPLETE:
        c->completes++;
        if (ctx) {
            c->final_text = strdup((const char *)ctx);
        }
        break;
    default:
        break;
    }
}

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

/* Reads until the end of the HTTP headers plus some body. */
static void drain_request(int fd) {
    char buf[4096];
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            return;
        }
        size_t have = 0;
        for (ssize_t i = 0; i + 3 < n; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
                buf[i + 3] == '\n') {
                have = 1;
                break;
            }
        }
        if (have) {
            return;
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

#define SSE0                                                       \
    "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"role\":" \
    "\"assistant\"},\"finish_reason\":null}]}\n\n"                  \
    "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":" \
    "[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":" \
    "\"run_shell\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n" \
    "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":" \
    "[{\"index\":0,\"function\":{\"arguments\":\"{\\\"command\\\":\\\"/bin/echo\\\"}\"}}]}," \
    "\"finish_reason\":null}]}\n\n"                              \
    "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":" \
    "\"tool_calls\"}]}\n\n"                                      \
    "data: [DONE]\n\n"

#define SSE1                                                           \
    "data: {\"id\":\"2\",\"choices\":[{\"index\":0,\"delta\":{\"role\":" \
    "\"assistant\",\"content\":\"Hello from \"},\"finish_reason\":null}]}\n\n" \
    "data: {\"id\":\"2\",\"choices\":[{\"index\":0,\"delta\":{\"content\":" \
    "\"the agent.\"},\"finish_reason\":null}]}\n\n"                 \
    "data: {\"id\":\"2\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n" \
    "data: [DONE]\n\n"

static void server_child(int pipe_w) {
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
    addr.sin_port = htons(0); /* kernel assigns */
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        _exit(1);
    }
    socklen_t alen = sizeof(addr);
    getsockname(srv, (struct sockaddr *)&addr, &alen);
    int port = ntohs(addr.sin_port);

    /* Listen before signalling the port so the parent's first connect is never
     * refused by a not-yet-listening socket. */
    if (listen(srv, 8) != 0) {
        _exit(1);
    }

    /* Send port to parent. */
    char pbuf[16];
    int n = snprintf(pbuf, sizeof(pbuf), "%d\n", port);
    write_all(pipe_w, pbuf, (size_t)n);
    close(pipe_w);

    for (int turn = 0; turn < 3; turn++) {
        int c1 = accept(srv, NULL, NULL);
        if (c1 >= 0) {
            drain_request(c1);
            serve_sse(c1, SSE0);
            close(c1);
        }
        int c2 = accept(srv, NULL, NULL);
        if (c2 >= 0) {
            drain_request(c2);
            serve_sse(c2, SSE1);
            close(c2);
        }
    }
    close(srv);
    _exit(0);
}

static void test_full_agent_loop(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        CHECK(0);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        server_child(pipefd[1]);
    }
    close(pipefd[1]);

    char pbuf[32];
    ssize_t rd = read(pipefd[0], pbuf, sizeof(pbuf) - 1);
    close(pipefd[0]);
    if (rd <= 0) {
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
        .timeout_ms = 15000,
    };
    puci_context_set_provider(ctx, &pcfg);

    counts c;
    memset(&c, 0, sizeof(c));

    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.event_cb = on_event;
    sopts.event_userdata = &c;
    sopts.system_prompt = "You are a test agent.";
    sopts.max_tool_iterations = 8;

    puci_session *sess = NULL;
    CHECK(puci_session_create(ctx, &sopts, &sess) == PUCI_OK);

    puci_status st = puci_session_send(sess, "run the echo tool then reply");
    if (st != PUCI_OK) {
        /* Prevent the forked server from blocking us if the loop aborted. */
        kill(pid, SIGKILL);
    }
    CHECK(st == PUCI_OK);

    /* One tool call was executed and fed back. */
    CHECK(c.tool_starts == 1);
    CHECK(c.tool_ends == 1);
    /* Final turn completed with the streamed text. */
    CHECK(c.completes == 1);
    CHECK(c.deltas == 2); /* "Hello from " and "the agent." */
    if (c.final_text) {
        CHECK(strcmp(c.final_text, "Hello from the agent.") == 0);
    }

    counts async_counts;
    memset(&async_counts, 0, sizeof(async_counts));
    puci_session_options async_opts = sopts;
    async_opts.event_cb = NULL;
    async_opts.event_userdata = NULL;
    puci_session *async_sess = NULL;
    CHECK(puci_session_create(ctx, &async_opts, &async_sess) == PUCI_OK);
    CHECK(puci_session_begin_turn(async_sess, "run the echo tool then reply") ==
          PUCI_OK);

    puci_status async_status = PUCI_ERR_UNKNOWN;
    int done = 0;
    for (int guard = 0; guard < 10000 && !done; guard++) {
        int timeout_ms = -1;
        struct pollfd fds[64];
        size_t fd_count = 0;
        CHECK(puci_session_next_timeout_ms(async_sess, &timeout_ms) == PUCI_OK);
        CHECK(puci_session_get_fds(async_sess, fds, 64, &fd_count) == PUCI_OK);
        int wait_ms = timeout_ms < 0 ? 1000 : timeout_ms;
        if (wait_ms > 1000) {
            wait_ms = 1000;
        }
        (void)poll(fd_count ? fds : NULL, (nfds_t)fd_count, wait_ms);
        async_status = puci_session_process(async_sess, fds, fd_count);

        puci_event ev;
        while (puci_session_next_event(async_sess, &ev)) {
            if (ev.kind == PUCI_EVENT_TOKEN_DELTA && ev.text) {
                size_t old_len = async_counts.final_text
                                     ? strlen(async_counts.final_text)
                                     : 0;
                char *next = (char *)realloc(async_counts.final_text,
                                             old_len + ev.text_len + 1);
                CHECK(next != NULL);
                async_counts.final_text = next;
                memcpy(next + old_len, ev.text, ev.text_len);
                next[old_len + ev.text_len] = '\0';
                async_counts.deltas++;
            } else if (ev.kind == PUCI_EVENT_TOOL_CALL_START) {
                CHECK(ev.payload.tool_call.id != NULL);
                CHECK(ev.payload.tool_call.name != NULL);
                CHECK(ev.payload.tool_call.args_json != NULL);
                async_counts.tool_starts++;
            } else if (ev.kind == PUCI_EVENT_TOOL_CALL_END) {
                CHECK(ev.payload.tool_result.call_id != NULL);
                CHECK(ev.payload.tool_result.content != NULL);
                async_counts.tool_ends++;
            } else if (ev.kind == PUCI_EVENT_TURN_COMPLETE) {
                CHECK(ev.payload.turn_complete.finish_reason != NULL);
                async_counts.completes++;
                done = 1;
            } else if (ev.kind == PUCI_EVENT_ERROR ||
                       ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                done = 1;
            }
        }
    }

    CHECK(async_status == PUCI_OK);
    CHECK(async_counts.tool_starts == c.tool_starts);
    CHECK(async_counts.tool_ends == c.tool_ends);
    CHECK(async_counts.deltas == c.deltas);
    CHECK(async_counts.completes == c.completes);
    CHECK(async_counts.final_text != NULL);
    if (async_counts.final_text && c.final_text) {
        CHECK(strcmp(async_counts.final_text, c.final_text) == 0);
    }

    puci_session_options limited_opts = sopts;
    limited_opts.event_cb = NULL;
    limited_opts.event_userdata = NULL;
    limited_opts.max_tool_iterations = 1;
    puci_session *limited_sess = NULL;
    CHECK(puci_session_create(ctx, &limited_opts, &limited_sess) == PUCI_OK);
    puci_status limited_status =
        puci_session_send(limited_sess, "run the echo tool then reply");
    CHECK(limited_status == PUCI_ERR_TIMEOUT);
    kill(pid, SIGKILL);

    puci_session_destroy(sess);
    puci_session_destroy(async_sess);
    puci_session_destroy(limited_sess);
    puci_context_destroy(ctx);

    free(c.final_text);
    free(async_counts.final_text);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
}

int main(void) {
    test_full_agent_loop();
    TEST_END();
}
