/* bench/bench_session_growth.c - measure heap footprint as a session grows.
 *
 * Builds a synthetic multi-turn transcript in a real puci_session using the
 * internal session API (no network: messages are appended directly). Reports the
 * session's resident set size (RSS) and proportional set size (PSS) at geometric
 * conversation sizes, so the per-message memory cost of long transcripts can be
 * measured directly.
 *
 * PSS is read from /proc/self/smaps_rollup when available (Linux); otherwise it
 * falls back to RSS from /proc/self/statm. This is a developer tool, not a
 * built-in test: build with `meson setup build -Dbench=true`.
 *
 * Usage: bench_session_growth [max_messages]   (default 16384, power of two)
 */

#include "puci_internal.h"
#include "puci/puci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Returns the process's proportional set size in kB (best single-number memory
 * cost accounting for shared pages), or -1 if unreadable. */
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

/* Returns the resident set size in kB (VmRSS), or -1 if unreadable. */
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

int main(int argc, char **argv) {
    long max = 16384;
    if (argc > 1) {
        max = strtol(argv[1], NULL, 10);
        if (max < 1) {
            max = 1;
        }
    }

    puci_context *ctx = NULL;
    if (puci_context_create(NULL, &ctx) != PUCI_OK) {
        fprintf(stderr, "context create failed\n");
        return 1;
    }
    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = "http://127.0.0.1/v1",
        .model = "bench-model",
    };
    puci_context_set_provider(ctx, &pcfg);

    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.system_prompt = "benchmark system prompt";
    puci_session *sess = NULL;
    if (puci_session_create(ctx, &sopts, &sess) != PUCI_OK) {
        fprintf(stderr, "session create failed\n");
        puci_context_destroy(ctx);
        return 1;
    }

    /* Warm an RSS read and touch the allocator so its baseline is mapped. */
    (void)read_rss_kb();
    (void)read_pss_kb();

    printf("%-10s %12s %12s %14s\n", "messages", "RSS(kB)", "PSS(kB)",
           "PSS+per\nmsg(kB)");
    printf("%-10s %12s %12s %14s\n", "--------", "--------", "--------",
           "--------");
    long prev_pss = -1;
    long prev_msgs = 0;
    long steps = 0;
    long target = 2; /* 2 messages per turn */
    while (target <= max) {
        /* Grow to exactly the step's message count (target). */
        while ((long)sess->nmessages < target) {
            char userbuf[64];
            snprintf(userbuf, sizeof(userbuf), "user message %zu", sess->nmessages);
            session_msg_begin(sess, PUCI_ROLE_USER, userbuf);
            session_msg_begin(sess, PUCI_ROLE_ASSISTANT, NULL);
            session_msg_finish_assistant(
                sess, "Acceptable helper response for benchmarking purposes.,",
                NULL, 0);
        }
        long msgs = (long)sess->nmessages;
        long rss = read_rss_kb();
        long pss = read_pss_kb();
        if (rss < 0 || pss < 0) {
            fprintf(stderr, "cannot read memory stats\n");
            return 1;
        }
        double delta = 0.0;
        if (steps > 0 && prev_pss >= 0 && msgs > prev_msgs) {
            delta = (double)(pss - prev_pss) / (double)(msgs - prev_msgs);
        }
        printf("%-10ld %12ld %12ld %14.3f\n", msgs, rss, pss, delta);
        prev_pss = pss;
        prev_msgs = msgs;
        steps++;
        target *= 2;
    }

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    return 0;
}
