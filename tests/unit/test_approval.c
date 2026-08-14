/* test_approval.c - approval-policy hook and diff-computation tests.
 *
 * Verifies the embeddable safety surface:
 *   - puci_compute_diff produces correct unified diffs (add/remove/replace),
 *     returns NULL for identical input, and is freed with puci_diff_free.
 *   - The approval callback fires before a tool runs; a DENY prevents the tool
 *     from executing and feeds a "blocked by approval policy" result back to
 *     the model; an ALLOW lets it run.
 *
 * The approval scenarios run a real agent loop against an embedded mock SSE
 * server (same pattern as test_agent_loop).
 */

#include "puci/puci.h"
#include "puci/diff.h"

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

/* state shared with the approval policy and event callback for one run */
typedef struct arun {
    int ran;          /* 1 if the custom tool actually executed */
    int saw_blocked;  /* 1 if a "blocked by approval policy" result was seen */
    int saw_ran;      /* 1 if the tool result was the normal "ran" output */
    puci_approval decision;
} arun;

static int approve_me_invoke(void *ud, const puci_cancel_token *cancel,
                             const char *args_json, char **out) {
    (void)cancel;
    (void)args_json;
    arun *a = (arun *)ud;
    a->ran = 1;
    *out = strdup("ran");
    return 0;
}

static puci_approval policy(const puci_approval_request *req, void *ud) {
    (void)req;
    return ((arun *)ud)->decision;
}

static void on_event(void *ud, puci_session *sess, puci_event ev,
                     const void *ctx) {
    arun *a = (arun *)ud;
    (void)sess;
    if (ev.kind == PUCI_EVENT_TOOL_CALL_END) {
        const puci_tool_result *res = (const puci_tool_result *)ctx;
        if (res && res->content && strstr(res->content, "blocked by approval policy")) {
            a->saw_blocked = 1;
        }
        if (res && res->content && strcmp(res->content, "ran") == 0) {
            a->saw_ran = 1;
        }
    }
}

/* ---- mock server serving two requests: tool call, then final text ---- */

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

#define SSE_TOOL                                                          \
    "data: {\"id\":\"t\",\"choices\":[{\"index\":0,\"delta\":{\"role\":" \
    "\"assistant\"},\"finish_reason\":null}]}\n\n"                       \
    "data: {\"id\":\"t\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":" \
    "[{\"index\":0,\"id\":\"call_a\",\"type\":\"function\",\"function\":{" \
    "\"name\":\"approve_me\",\"arguments\":\"{\\\"x\\\":1}\"}}]},\"finish_reason\":null}]}\n\n" \
    "data: {\"id\":\"t\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n" \
    "data: [DONE]\n\n"

#define SSE_FINAL                                                          \
    "data: {\"id\":\"f\",\"choices\":[{\"index\":0,\"delta\":{\"role\":" \
    "\"assistant\",\"content\":\"done\"},\"finish_reason\":null}]}\n\n" \
    "data: {\"id\":\"f\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n" \
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
    addr.sin_port = htons(0);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        _exit(1);
    }
    socklen_t alen = sizeof(addr);
    getsockname(srv, (struct sockaddr *)&addr, &alen);
    int port = ntohs(addr.sin_port);
    /* Listen before signalling the port, so the parent's first connect is never
     * refused by a not-yet-listening socket. */
    if (listen(srv, 8) != 0) {
        _exit(1);
    }
    char pbuf[16];
    int n = snprintf(pbuf, sizeof(pbuf), "%d\n", port);
    write_all(pipe_w, pbuf, (size_t)n);
    close(pipe_w);

    for (int r = 0; r < 2; r++) {
        int c = accept(srv, NULL, NULL);
        if (c >= 0) {
            drain_request(c);
            const char *b = (r == 0) ? SSE_TOOL : SSE_FINAL;
            char head[160];
            int hn = snprintf(head, sizeof(head),
                              "HTTP/1.1 200 OK\r\nContent-Type: "
                              "text/event-stream\r\nContent-Length: %zu\r\n"
                              "Connection: close\r\n\r\n",
                              strlen(b));
            write_all(c, head, (size_t)hn);
            write_all(c, b, strlen(b));
            close(c);
        }
    }
    close(srv);
    _exit(0);
}

static void run_one(puci_approval decision, int expect_ran, int expect_blocked) {
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        server_child(pipefd[1]);
    }
    close(pipefd[1]);
    char pbuf[32];
    ssize_t rd = read(pipefd[0], pbuf, sizeof(pbuf) - 1);
    close(pipefd[0]);
    CHECK(rd > 0);
    pbuf[rd] = '\0';
    int port = atoi(pbuf);
    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    puci_tool_spec spec = {
        .name = "approve_me",
        .description = "test tool",
        .input_schema = "{}",
    };
    arun a;
    memset(&a, 0, sizeof(a));
    a.decision = decision;
    CHECK(puci_tool_register(ctx, &spec, approve_me_invoke, &a) == PUCI_OK);

    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = base_url,
        .api_key = NULL,
        .model = "mock",
        .timeout_ms = 15000,
    };
    puci_context_set_provider(ctx, &pcfg);

    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.event_cb = on_event;
    sopts.event_userdata = &a;
    sopts.approve_cb = policy;
    sopts.approve_userdata = &a;
    sopts.max_tool_iterations = 8;

    puci_session *sess = NULL;
    CHECK(puci_session_create(ctx, &sopts, &sess) == PUCI_OK);
    puci_status st = puci_session_send(sess, "call the tool");
    if (st != PUCI_OK) {
        kill(pid, SIGKILL);
    }
    CHECK(st == PUCI_OK);
    CHECK(a.ran == expect_ran);
    CHECK(a.saw_blocked == expect_blocked);
    if (expect_blocked) {
        CHECK(a.saw_ran == 0);
    } else {
        CHECK(a.saw_ran == 1);
    }

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
}

/* ---- diff unit checks ---- */

static void test_diff(void) {
    /* replacement */
    const char *d = puci_compute_diff("a\nb\nc\n", 6, "a\nB\nc\n", 6);
    CHECK(d != NULL);
    if (d) {
        CHECK(strstr(d, "-b") != NULL);
        CHECK(strstr(d, "+B") != NULL);
    }
    puci_diff_free(d);

    /* identical -> NULL */
    d = puci_compute_diff("x\ny\n", 4, "x\ny\n", 4);
    CHECK(d == NULL);
    puci_diff_free(d);

    /* new file: all additions */
    d = puci_compute_diff(NULL, 0, "hello\n", 6);
    CHECK(d != NULL);
    if (d) {
        CHECK(strstr(d, "+hello") != NULL);
    }
    puci_diff_free(d);

    /* deleted file: all removals */
    d = puci_compute_diff("bye\n", 4, "", 0);
    CHECK(d != NULL);
    if (d) {
        CHECK(strstr(d, "-bye") != NULL);
    }
    puci_diff_free(d);
}

#define SSE_WRITE_FILE                                                     \
    "data: {\"id\":\"t\",\"choices\":[{\"index\":0,\"delta\":{\"role\":" \
    "\"assistant\"},\"finish_reason\":null}]}\n\n"                       \
    "data: {\"id\":\"t\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":" \
    "[{\"index\":0,\"id\":\"call_w\",\"type\":\"function\",\"function\":{" \
    "\"name\":\"write_file\",\"arguments\":\"{\\\"path\\\":\\\"/tmp/puci_plan_test.txt\\\",\\\"content\\\":\\\"dry\\\"}\"}}]},\"finish_reason\":null}]}\n\n" \
    "data: {\"id\":\"t\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n" \
    "data: [DONE]\n\n"

static void test_plan_mode_dry_run(void) {
    const char *target_file = "/tmp/puci_plan_test.txt";
    unlink(target_file);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(srv >= 0);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    CHECK(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t alen = sizeof(addr);
    getsockname(srv, (struct sockaddr *)&addr, &alen);
    int port = ntohs(addr.sin_port);
    CHECK(listen(srv, 8) == 0);

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        char pbuf[16];
        int n = snprintf(pbuf, sizeof(pbuf), "%d\n", port);
        write_all(pipefd[1], pbuf, (size_t)n);
        close(pipefd[1]);

        for (int r = 0; r < 2; r++) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) {
                drain_request(c);
                const char *b = (r == 0) ? SSE_WRITE_FILE : SSE_FINAL;
                char head[160];
                int hn = snprintf(head, sizeof(head),
                                  "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                  "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                  strlen(b));
                write_all(c, head, (size_t)hn);
                write_all(c, b, strlen(b));
                close(c);
            }
        }
        close(srv);
        _exit(0);
    }
    close(pipefd[1]);
    close(srv);
    char pbuf[32];
    ssize_t rd = read(pipefd[0], pbuf, sizeof(pbuf) - 1);
    close(pipefd[0]);
    CHECK(rd > 0);

    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = base_url,
        .api_key = NULL,
        .model = "mock",
        .timeout_ms = 15000,
    };
    puci_context_set_provider(ctx, &pcfg);

    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.plan_mode = 1;
    sopts.max_tool_iterations = 8;

    puci_session *sess = NULL;
    CHECK(puci_session_create(ctx, &sopts, &sess) == PUCI_OK);
    CHECK(puci_session_get_plan_mode(sess) == 1);

    puci_status st = puci_session_send(sess, "write file");
    CHECK(st == PUCI_OK);

    /* File must NOT have been written to disk */
    FILE *check_f = fopen(target_file, "r");
    CHECK(check_f == NULL);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    int ws = 0;
    waitpid(pid, &ws, 0);
}

int main(void) {
    test_diff();
    run_one(PUCI_APPROVAL_DENY, /*expect_ran*/ 0, /*expect_blocked*/ 1);
    run_one(PUCI_APPROVAL_ALLOW, /*expect_ran*/ 1, /*expect_blocked*/ 0);
    test_plan_mode_dry_run();
    TEST_END();
}
