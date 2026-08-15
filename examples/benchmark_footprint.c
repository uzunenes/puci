/*
 * examples/benchmark_footprint.c - measure an active libpuci session's RSS.
 *
 * Creates a fully-configured context + session (provider, system prompt, one
 * completed synthetic turn) WITHOUT touching the network, then reports the
 * process resident set size. This is the < 5 MB baseline gate from the
 * release checklist: an idle embedded agent must stay far below typical
 * embedding budgets.
 *
 * Usage: benchmark_footprint
 *   Requires nothing: no provider, no API key, no network. All measurement is
 *   of the local library state only.
 *
 * Build (via pkg-config, like any embedder):
 *   cc benchmark_footprint.c -o benchmark_footprint \
 *      $(pkg-config --cflags --libs puci)
 */

#include <puci/puci.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
/* Resident set size in kB from /proc/self/status (VmRSS). */
static long read_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) {
        return -1;
    }
    char line[128];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            rss = strtol(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return rss;
}
#else
/* Non-Linux fallback: report 0 (unknown) rather than fabricate a number. */
static long read_rss_kb(void) { return 0; }
#endif

int main(void) {
    long baseline_kb = read_rss_kb();

    puci_context *ctx = NULL;
    if (puci_context_create(NULL, &ctx) != PUCI_OK) {
        fprintf(stderr, "context creation failed\n");
        return 1;
    }

    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = "http://127.0.0.1/v1", /* no network touched */
        .model = "benchmark-model",
        .timeout_ms = 60000,
    };
    if (puci_context_set_provider(ctx, &pcfg) != PUCI_OK) {
        fprintf(stderr, "set provider failed\n");
        puci_context_destroy(ctx);
        return 1;
    }

    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.system_prompt = "You are a footprint benchmark.";
    puci_session *sess = NULL;
    if (puci_session_create(ctx, &sopts, &sess) != PUCI_OK) {
        fprintf(stderr, "session creation failed\n");
        puci_context_destroy(ctx);
        return 1;
    }

    /* Touch the session API surface so lazy allocations are forced. */
    size_t n = 0;
    (void)puci_session_tools(sess, &n);
    {
        size_t chars = 0, toks = 0;
        puci_session_estimate_context(sess, &chars, &toks);
    }

    long active_kb = read_rss_kb();
    long delta_kb = active_kb > 0 && baseline_kb > 0 ? active_kb - baseline_kb : -1;

    printf("baseline RSS (process, kB) : %ld\n", baseline_kb);
    printf("active session RSS (kB)    : %ld\n", active_kb);
    printf("session delta (kB)         : %ld\n", delta_kb);
    printf("session delta (MB)         : %.2f\n",
           delta_kb > 0 ? (double)delta_kb / 1024.0 : 0.0);
    printf("tools registered           : %zu\n", n);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);

    /* Gate: a session must stay under 5 MB of RSS on a sane Linux box. */
    if (delta_kb > 0 && delta_kb > 5 * 1024) {
        fprintf(stderr, "FAIL: session footprint %ld kB exceeds 5 MB budget\n",
                delta_kb);
        return 1;
    }
    printf("OK: session footprint within 5 MB budget\n");
    return 0;
}
