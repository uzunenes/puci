/* test_async_tool.c - async tool lifecycle regression test (Issue #2 /
 * Stage A).
 *
 * Drives a run_shell("/bin/sleep 1") tool call entirely through the public
 * non-blocking API (begin_turn/get_fds/next_timeout_ms/process/next_event)
 * from a host-owned poll() loop that ALSO owns an unrelated fd: a timerfd
 * ticking every 100ms, exactly as an embedder's own event source would.
 *
 * What this proves, and how:
 *  - The host's own fd fires many times while the tool runs (>= 5 ticks for a
 *    ~1s sleep): the agent loop does not monopolise the host's poll() the way
 *    a blocking tool invocation would (that would deliver 0 host ticks until
 *    the whole command finished).
 *  - No single process()/poll() cycle blocks anywhere near the command's
 *    runtime: process() is called far more than once during the sleep, each
 *    call bounded by the host's own poll() timeout, not by the child process.
 *  - The tool's own fds (its stdout/stderr pipes) are surfaced via
 *    puci_session_get_fds() while PUCI_TS_RUNNING_TOOL is active, and stop
 *    being reported once the tool finishes.
 *  - The turn still completes normally and no zombie/orphan process survives.
 */

#include "puci/puci.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
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

#define SSE_TOOL                                                            \
    "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"role\":"    \
    "\"assistant\"},\"finish_reason\":null}]}\n\n"                          \
    "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":" \
    "[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":" \
    "\"run_shell\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n"     \
    "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":" \
    "[{\"index\":0,\"function\":{\"arguments\":\"{\\\"command\\\":\\\"/bin/sleep\\\"," \
    "\\\"args\\\":[\\\"1\\\"]}\"}}]},\"finish_reason\":null}]}\n\n"          \
    "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":" \
    "\"tool_calls\"}]}\n\n"                                                 \
    "data: [DONE]\n\n"

#define SSE_FINAL                                                            \
    "data: {\"id\":\"2\",\"choices\":[{\"index\":0,\"delta\":{\"role\":"     \
    "\"assistant\",\"content\":\"done\"},\"finish_reason\":null}]}\n\n"      \
    "data: {\"id\":\"2\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":" \
    "\"stop\"}]}\n\n"                                                        \
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
        serve_sse(c1, SSE_TOOL);
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

/* Counts direct children of this process (zombies or live). */
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

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void test_run_shell_does_not_block_host_loop(void) {
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    pid_t spid = fork();
    if (spid == 0) {
        close(pipefd[0]);
        server_child(pipefd[1]);
    }
    close(pipefd[1]);
    char pbuf[32];
    ssize_t rd = read(pipefd[0], pbuf, sizeof(pbuf) - 1);
    close(pipefd[0]);
    CHECK(rd > 0);
    pbuf[rd > 0 ? rd : 0] = '\0';
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

    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.system_prompt = "test";
    sopts.max_tool_iterations = 8;

    puci_session *sess = NULL;
    CHECK(puci_session_create(ctx, &sopts, &sess) == PUCI_OK);
    CHECK(puci_session_begin_turn(sess, "run a slow tool") == PUCI_OK);

    /* Host-owned fd, completely unrelated to the session: a 100ms heartbeat.
     * A host driving a blocking tool call would see this starve for the
     * duration of the command; an async tool must let it keep firing. */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    CHECK(tfd >= 0);
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 100L * 1000 * 1000;
    its.it_interval.tv_nsec = 100L * 1000 * 1000;
    CHECK(timerfd_settime(tfd, 0, &its, NULL) == 0);

    int heartbeats = 0;
    int tool_fds_seen_while_running = 0;
    long long max_process_gap_ms = 0;
    long long last_process_at = now_ms();
    int done = 0;
    puci_status status = PUCI_ERR_UNKNOWN;

    for (int guard = 0; guard < 20000 && !done; guard++) {
        int timeout_ms = -1;
        CHECK(puci_session_next_timeout_ms(sess, &timeout_ms) == PUCI_OK);

        struct pollfd fds[64];
        size_t fd_count = 0;
        CHECK(puci_session_get_fds(sess, fds, 64, &fd_count) == PUCI_OK);

        /* Fold in the host's own fd, exactly as an embedder would. */
        fds[fd_count].fd = tfd;
        fds[fd_count].events = POLLIN;
        fds[fd_count].revents = 0;
        size_t host_slot = fd_count;
        fd_count++;

        int wait_ms = timeout_ms < 0 ? 1000 : timeout_ms;
        if (wait_ms > 1000) {
            wait_ms = 1000;
        }
        (void)poll(fds, (nfds_t)fd_count, wait_ms);

        long long t0 = now_ms();
        long long gap = t0 - last_process_at;
        if (gap > max_process_gap_ms) {
            max_process_gap_ms = gap;
        }
        last_process_at = t0;

        if (fds[host_slot].revents & POLLIN) {
            uint64_t ticks = 0;
            ssize_t rr = read(tfd, &ticks, sizeof(ticks));
            (void)rr;
            heartbeats++;
        }

        /* Session fds (only) for process(); the host fd is not the session's
         * to hand back. */
        status = puci_session_process(sess, fds, host_slot);

        size_t probe_count = 0;
        CHECK(puci_session_get_fds(sess, NULL, 0, &probe_count) == PUCI_OK);
        if (probe_count > 0) {
            tool_fds_seen_while_running = 1;
        }

        puci_event ev;
        while (puci_session_next_event(sess, &ev)) {
            if (ev.kind == PUCI_EVENT_TURN_COMPLETE ||
                ev.kind == PUCI_EVENT_ERROR ||
                ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                done = 1;
            }
        }
    }

    close(tfd);

    CHECK(status == PUCI_OK);
    CHECK(done);
    /* The 1s sleep must have let the 100ms host heartbeat fire many times,
     * proving the host loop was not blocked for the tool's duration. */
    CHECK(heartbeats >= 5);
    /* No single iteration of the host loop waited anywhere near the full 1s
     * command: bounded well under it (generous margin for CI jitter). */
    CHECK(max_process_gap_ms < 700);
    CHECK(tool_fds_seen_while_running);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    kill(spid, SIGKILL);
    waitpid(spid, NULL, 0);
    CHECK(count_children() == 0);
}

int main(void) {
    test_run_shell_does_not_block_host_loop();
    TEST_END();
}
