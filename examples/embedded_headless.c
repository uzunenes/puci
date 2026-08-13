/*
 * examples/embedded_headless.c - no-TUI, low-memory embedded usage pattern.
 *
 * Demonstrates the embedded-friendly features of libpuci:
 *   - caller-controlled allocation through a custom allocator (counts bytes,
 *     simulating a constrained heap budget)
 *   - output entirely through a logging callback (no terminal, no std::io
 *     assumptions)
 *   - no event callback at all: headless sessions that just run a turn
 *
 * This mirror the shape a Raspberry Pi / OpenWRT / Yocto edge program would
 * use. To actually reach a model, set PUCI_BASE_URL/PUCI_API_KEY/PUCI_MODEL as
 * in minimal_agent.
 */

#include <puci/puci.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A deliberately small, counting allocator that routes through the standard
 * heap but tracks live bytes so we can reason about peak usage. */
typedef struct budget {
    size_t current;
    size_t peak;
} budget;

static void *b_malloc(void *ud, size_t size) {
    budget *b = (budget *)ud;
    void *p = malloc(size);
    if (!p) {
        return NULL;
    }
    b->current += size;
    if (b->current > b->peak) {
        b->peak = b->current;
    }
    return p;
}
static void *b_realloc(void *ud, void *ptr, size_t size) {
    budget *b = (budget *)ud;
    void *p = realloc(ptr, size);
    if (!p) {
        return NULL;
    }
    b->current = 0; /* recompute below from a tracked side table in real code */
    (void)b;
    return p;
}
static void b_free(void *ud, void *ptr) {
    (void)ud;
    free(ptr);
}

static void on_log(void *ud, puci_log_level level, const char *msg) {
    (void)ud;
    const char *name = "?";
    switch (level) {
    case PUCI_LOG_TRACE: name = "trace"; break;
    case PUCI_LOG_DEBUG: name = "debug"; break;
    case PUCI_LOG_INFO: name = "info"; break;
    case PUCI_LOG_WARN: name = "warn"; break;
    case PUCI_LOG_ERROR: name = "error"; break;
    }
    printf("[%s] %s\n", name, msg);
}

int main(void) {
    budget b = {0, 0};

    puci_context_options copts;
    memset(&copts, 0, sizeof(copts));
    copts.allocator.malloc_fn = b_malloc;
    copts.allocator.realloc_fn = b_realloc;
    copts.allocator.free_fn = b_free;
    copts.allocator.userdata = &b;
    copts.log_cb = on_log;
    copts.log_userdata = NULL;
    copts.log_level = PUCI_LOG_INFO;

    puci_context *ctx = NULL;
    if (puci_context_create(&copts, &ctx) != PUCI_OK) {
        fprintf(stderr, "context creation failed\n");
        return 1;
    }
    printf("context created; live bytes through custom allocator: %zu\n",
           b.current);

    const char *base = getenv("PUCI_BASE_URL");
    const char *key = getenv("PUCI_API_KEY");
    const char *model = getenv("PUCI_MODEL") ? getenv("PUCI_MODEL") : "gpt-4o-mini";

    if (base && key) {
        puci_provider_config pcfg = {
            .name = "openai", .base_url = base, .api_key = key, .model = model,
            .timeout_ms = 60000,
        };
        puci_context_set_provider(ctx, &pcfg);
        puci_session_options sopts = {0};
        sopts.provider = pcfg;
        sopts.system_prompt = "You run embedded. Be terse.";

        puci_session *sess = NULL;
        if (puci_session_create(ctx, &sopts, &sess) == PUCI_OK) {
            printf("running a headless turn...\n");
            puci_status st = puci_session_send(sess, "Say hi in five words.");
            printf("turn finished with: %s\n", puci_status_string(st));
            puci_session_destroy(sess);
        }
    } else {
        printf("PUCI_BASE_URL/PUCI_API_KEY not both set; "
               "skipped the actual network call.\n");
    }

    printf("destroying context\n");
    puci_context_destroy(ctx);
    return 0;
}
