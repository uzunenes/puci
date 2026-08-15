/* bench/bench_swarm.c - 1000-agent swarm density benchmark
 *
 * Instantiates N concurrent puci_session instances (N=100..1000) on a single
 * poll() loop against a mock SSE fixture. Measures baseline/peak RSS, Linux
 * PSS, session delta, and event throughput.
 *
 * Target: <250 MB total RSS for 1000 sessions (<300 kB/session delta).
 *
 * Usage: bench_swarm [N]   (default 1000)
 * Build: meson setup build-bench -Dbench=true && ninja -C build-bench bench/bench_swarm
 */

#include "puci_internal.h"
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
#include <errno.h>
#include <time.h>
#include <unistd.h>

static long read_pss_kb(void) {
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Pss:", 4) == 0) {
                long kb = strtol(line + 4, NULL, 10);
                fclose(f);
                return kb;
            }
        }
        fclose(f);
    }
    return -1;
}
static long read_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                long kb = strtol(line + 6, NULL, 10);
                fclose(f);
                return kb;
            }
        }
        fclose(f);
    }
    return -1;
}

static void write_all(int fd, const char *data, size_t len) {
    (void)signal(SIGPIPE, SIG_IGN);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

static int open_listener(int *out_port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return -1;
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(srv); return -1; }
    socklen_t alen = sizeof(addr);
    getsockname(srv, (struct sockaddr *)&addr, &alen);
    *out_port = ntohs(addr.sin_port);
    if (listen(srv, 128) != 0) { close(srv); return -1; }
    return srv;
}

/* Minimal mock: serve one SSE turn per accepted connection sequentially
 * (handles up to N connections). Each turn streams 3 deltas + DONE. */
static void swarm_server(int wfd, int rfd, int expected) {
    int port = 0;
    int srv = open_listener(&port);
    if (srv < 0) _exit(1);
    char pbuf[16];
    int n = snprintf(pbuf, sizeof(pbuf), "%d\n", port);
    write_all(wfd, pbuf, (size_t)n);
    /* signal ready and wait for parent's go */
    char dummy;
    if (read(rfd, &dummy, 1) != 1) _exit(1);

    const char *body_template =
        "data: {\"id\":\"r\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"hello %d \"},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"r\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"world\"},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"r\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}\n\n"
        "data: [DONE]\n\n";

    for (int i = 0; i < expected; i++) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        /* Read request headers+body (ignore) with short timeout */
        struct pollfd pfd = {.fd=c, .events=POLLIN};
        poll(&pfd, 1, 2000);
        char tmp[4096];
        ssize_t r = recv(c, tmp, sizeof(tmp)-1, MSG_DONTWAIT);
        (void)r;
        char chunk[1024];
        int cl = snprintf(chunk, sizeof(chunk), body_template, i);
        char head[256];
        int hn = snprintf(head, sizeof(head),
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", cl);
        write_all(c, head, (size_t)hn);
        write_all(c, chunk, (size_t)cl);
        close(c);
    }
    close(srv);
    _exit(0);
}

int main(int argc, char **argv) {
    int N = 1000;
    const char *json_path = NULL;
    int json_enabled = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json_enabled = 1;
            if (i + 1 < argc && argv[i+1][0] != '-' && strchr(argv[i+1], '.')) {
                json_path = argv[++i];
            } else {
                json_path = "docs/benchmarks.json";
            }
        } else if (strncmp(argv[i], "--json=", 7) == 0) {
            json_enabled = 1;
            json_path = argv[i] + 7;
            if (!*json_path) json_path = "docs/benchmarks.json";
        } else if (argv[i][0] != '-') {
            int v = atoi(argv[i]);
            if (v > 0) { N = v; if (N < 1) N = 1; if (N > 5000) N = 5000; }
        } else if (strcmp(argv[i], "--help")==0 || strcmp(argv[i],"-h")==0) {
            printf("Usage: %s [N] [--json [path]]\n", argv[0]);
            printf("  N         number of sessions (1..5000, default 1000)\n");
            printf("  --json    export structured JSON (default docs/benchmarks.json)\n");
            return 0;
        }
    }

    long base_rss = read_rss_kb();
    long base_pss = read_pss_kb();
    printf("=== PUCI 1000-Agent Swarm Density Benchmark ===\n");
    printf("Target N=%d | baseline RSS=%ld kB PSS=%ld kB\n", N, base_rss, base_pss);

    int pf[2], rf[2];
    if (pipe(pf)!=0 || pipe(rf)!=0) { perror("pipe"); return 1; }
    pid_t pid = fork();
    if (pid==0) {
        close(pf[0]); close(rf[1]);
        swarm_server(pf[1], rf[0], N);
    }
    close(pf[1]); close(rf[0]);
    char pbuf[32];
    ssize_t rd = read(pf[0], pbuf, sizeof(pbuf)-1);
    if (rd <= 0) { kill(pid,SIGKILL); waitpid(pid,NULL,0); fprintf(stderr,"server failed\n"); return 1; }
    pbuf[rd]='\0';
    int port = atoi(pbuf);
    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    printf("mock server on port %d\n", port);

    puci_context *ctx = NULL;
    if (puci_context_create(NULL, &ctx)!=PUCI_OK) { fprintf(stderr,"ctx fail\n"); return 1;}
    puci_provider_config pcfg = {.name="openai", .base_url=base_url, .api_key=NULL, .model="mock-model", .timeout_ms=30000};
    puci_context_set_provider(ctx, &pcfg);

    long after_ctx_rss = read_rss_kb();
    long after_ctx_pss = read_pss_kb();

    puci_session **sessions = calloc((size_t)N, sizeof(puci_session*));
    if (!sessions) return 1;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i=0;i<N;i++) {
        puci_session_options sopts={0};
        sopts.provider=pcfg;
        if (puci_session_create(ctx, &sopts, &sessions[i])!=PUCI_OK) {
            fprintf(stderr,"session %d create failed\n",i); return 1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long create_ms = (t1.tv_sec - t0.tv_sec)*1000 + (t1.tv_nsec - t0.tv_nsec)/1000000;

    long after_create_rss = read_rss_kb();
    long after_create_pss = read_pss_kb();
    long delta_rss = after_create_rss - base_rss;
    long delta_pss = after_create_pss - base_pss;
    double per_sess_rss = N ? (double)delta_rss / N : 0;
    double per_sess_pss = N ? (double)delta_pss / N : 0;

    printf("\n--- Creation ---\n");
    printf("ctx RSS %ld kB (+%ld), PSS %ld kB (+%ld)\n", after_ctx_rss, after_ctx_rss-base_rss, after_ctx_pss, after_ctx_pss-base_pss);
    printf("after %d sessions: RSS %ld kB (+%ld), PSS %ld kB (+%ld) in %ld ms\n", N, after_create_rss, delta_rss, after_create_pss, delta_pss, create_ms);
    printf("per-session delta: RSS %.2f kB, PSS %.2f kB\n", per_sess_rss, per_sess_pss);

    /* Start turns and drive poll loop */
    for (int i=0;i<N;i++) {
        char prompt[32];
        snprintf(prompt,sizeof(prompt),"hello %d",i);
        puci_session_begin_turn(sessions[i], prompt);
    }
    /* signal server to start accepting */
    {
        ssize_t _w = write(rf[1], "G", 1);
        (void)_w;
    }
    close(rf[1]);

    struct timespec t_poll0;
    clock_gettime(CLOCK_MONOTONIC, &t_poll0);
    int done = 0;
    long events = 0;
    long wall_ms = 0;
    for (int guard=0; guard<20000 && done < N; guard++) {
        // aggregate fds
        struct pollfd fds[8192];
        size_t total = 0;
        int min_timeout = -1;
        for (int i=0;i<N;i++) {
            if (sessions[i]==NULL) continue;
            puci_status ls = puci_session_last_status(sessions[i]);
            // check if turn still active via get_fds count
            size_t cnt=0;
            puci_session_get_fds(sessions[i], NULL, 0, &cnt);
            int tm=-1;
            puci_session_next_timeout_ms(sessions[i], &tm);
            if (tm>=0 && (min_timeout<0 || tm < min_timeout)) min_timeout=tm;
            if (cnt>0) {
                if (total + cnt > sizeof(fds)/sizeof(fds[0])) cnt = sizeof(fds)/sizeof(fds[0]) - total;
                size_t got=0;
                puci_session_get_fds(sessions[i], fds+total, cnt, &got);
                total += got;
                (void)ls;
            }
        }
        int wait_ms = min_timeout<0 ? 50 : min_timeout;
        if (wait_ms > 50) wait_ms = 50;
        if (total>0) poll(fds, (nfds_t)total, wait_ms);
        else poll(NULL,0,wait_ms);

        for (int i=0;i<N;i++) {
            if (!sessions[i]) continue;
            puci_status st = puci_session_process(sessions[i], fds, total);
            (void)st;
            puci_event ev;
            while (puci_session_next_event(sessions[i], &ev)) {
                events++;
                if (ev.kind == PUCI_EVENT_TURN_COMPLETE || ev.kind == PUCI_EVENT_ERROR || ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                    // count completion once per session: check turn_complete
                    if (ev.kind == PUCI_EVENT_TURN_COMPLETE) done++;
                }
            }
            // also check terminal via get_fds 0 and last_status?
            // If done detection missed (events drained earlier), check if no fds and last_status ok
            size_t cnt2=0;
            puci_session_get_fds(sessions[i], NULL,0,&cnt2);
            if (cnt2==0) {
                puci_status ls2 = puci_session_last_status(sessions[i]);
                // if session became idle without event (should not), count
                if (ls2==PUCI_OK && done < N) {
                    // heuristic: if we haven't counted this session yet, peek if it had completed
                    // Use session validate as proxy: idle sessions have 0 fds
                    // We track per-session done flag array to avoid double count
                }
            }
        }
        // recompute done as sessions with no active turn (0 fds and last_status valid)
        int idle=0;
        for (int i=0;i<N;i++) if(sessions[i]){ size_t c=0; puci_session_get_fds(sessions[i],NULL,0,&c); if(c==0) idle++; }
        // break if all idle
        if (idle==N) { done=N; }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        wall_ms = (now.tv_sec - t_poll0.tv_sec)*1000 + (now.tv_nsec - t_poll0.tv_nsec)/1000000;
        if (wall_ms > 30000) break;
    }
    struct timespec t_poll1;
    clock_gettime(CLOCK_MONOTONIC, &t_poll1);
    long poll_ms = (t_poll1.tv_sec - t_poll0.tv_sec)*1000 + (t_poll1.tv_nsec - t_poll0.tv_nsec)/1000000;

    long peak_rss = read_rss_kb();
    long peak_pss = read_pss_kb();

    printf("\n--- Swarm poll ---\n");
    printf("sessions=%d events=%ld done=%d wall=%ld ms poll_ms=%ld\n", N, events, done, wall_ms, poll_ms);
    printf("peak RSS %ld kB (+%ld vs baseline), PSS %ld kB (+%ld)\n", peak_rss, peak_rss-base_rss, peak_pss, peak_pss-base_pss);
    {
        double ev_rate = poll_ms ? (double)events * 1000.0 / (double)poll_ms : 0;
        double turn_rate = poll_ms ? (double)N * 1000.0 / (double)poll_ms : 0;
        printf("throughput: %.1f events/s, %.1f turns/s\n", ev_rate, turn_rate);
    }
    if (peak_rss - base_rss < 250*1024) printf("PASS: total RSS <250 MB for %d sessions\n", N);
    else printf("WARN: RSS exceeds 250 MB target\n");
    if (per_sess_pss < 300) printf("PASS: per-session PSS %.1f kB <300 kB\n", per_sess_pss);
    else printf("WARN: per-session PSS %.1f kB exceeds 300 kB\n", per_sess_pss);

    if (json_enabled && json_path) {
        time_t now_t = time(NULL);
        struct tm tm_now;
        gmtime_r(&now_t, &tm_now);
        char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm_now);
        double ev_rate = poll_ms ? (double)events * 1000.0 / (double)poll_ms : 0;
        double turn_rate = poll_ms ? (double)N * 1000.0 / (double)poll_ms : 0;
        FILE *jf = fopen(json_path, "w");
        if (jf) {
            fprintf(jf, "{\n");
            fprintf(jf, "  \"timestamp\": \"%s\",\n", ts);
            fprintf(jf, "  \"N\": %d,\n", N);
            fprintf(jf, "  \"baseline_rss_kb\": %ld,\n", base_rss);
            fprintf(jf, "  \"baseline_pss_kb\": %ld,\n", base_pss);
            fprintf(jf, "  \"after_ctx_rss_kb\": %ld,\n", after_ctx_rss);
            fprintf(jf, "  \"after_ctx_pss_kb\": %ld,\n", after_ctx_pss);
            fprintf(jf, "  \"after_create_rss_kb\": %ld,\n", after_create_rss);
            fprintf(jf, "  \"after_create_pss_kb\": %ld,\n", after_create_pss);
            fprintf(jf, "  \"peak_rss_kb\": %ld,\n", peak_rss);
            fprintf(jf, "  \"peak_pss_kb\": %ld,\n", peak_pss);
            fprintf(jf, "  \"delta_rss_kb\": %ld,\n", delta_rss);
            fprintf(jf, "  \"delta_pss_kb\": %ld,\n", delta_pss);
            fprintf(jf, "  \"per_session_rss_kb\": %.2f,\n", per_sess_rss);
            fprintf(jf, "  \"per_session_pss_kb\": %.2f,\n", per_sess_pss);
            fprintf(jf, "  \"wall_ms\": %ld,\n", wall_ms);
            fprintf(jf, "  \"poll_ms\": %ld,\n", poll_ms);
            fprintf(jf, "  \"create_ms\": %ld,\n", create_ms);
            fprintf(jf, "  \"events\": %ld,\n", events);
            fprintf(jf, "  \"events_per_sec\": %.1f,\n", ev_rate);
            fprintf(jf, "  \"turns_per_sec\": %.1f,\n", turn_rate);
            fprintf(jf, "  \"done\": %d,\n", done);
            fprintf(jf, "  \"pass_rss_250mb\": %s,\n", (peak_rss - base_rss < 250*1024) ? "true" : "false");
            fprintf(jf, "  \"pass_pss_300kb\": %s\n", (per_sess_pss < 300) ? "true" : "false");
            fprintf(jf, "}\n");
            fclose(jf);
            printf("JSON exported to %s\n", json_path);
        } else {
            fprintf(stderr, "Failed to write JSON to %s: %s\n", json_path, strerror(errno));
        }
    }

    for (int i=0;i<N;i++) if(sessions[i]) puci_session_destroy(sessions[i]);
    free(sessions);
    puci_context_destroy(ctx);
    close(pf[0]);
    kill(pid, SIGKILL);
    waitpid(pid,NULL,0);

    printf("\nReproduce: bench/bench_swarm %d\n", N);
    return 0;
}
