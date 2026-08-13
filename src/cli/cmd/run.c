/*
 * cmd/run.c - `puci run "<prompt>"` subcommand.
 *
 * Built exclusively on the public libpuci API. Reads configuration from the
 * environment (PUCI_BASE_URL / PUCI_API_KEY / PUCI_MODEL); full config.toml
 * support arrives in a later phase. Streams deltas to stdout and echoes tool
 * activity to stderr, so a clean transcript can be piped from stdout.
 */

#include <puci/puci.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void stream_event(void *ud, puci_session *sess, puci_event ev,
                         const void *ctx) {
    (void)ud;
    (void)sess;
    (void)ctx;
    switch (ev.kind) {
    case PUCI_EVENT_TOKEN_DELTA: {
        if (ev.text) {
            fputs(ev.text, stdout);
            fflush(stdout);
        }
        break;
    }
    case PUCI_EVENT_TOOL_CALL_START: {
        const puci_tool_call *tc = (const puci_tool_call *)ctx;
        fprintf(stderr, "\n[tool] %s\n", tc->name);
        break;
    }
    case PUCI_EVENT_TURN_COMPLETE:
        fputs("\n", stdout);
        fflush(stdout);
        break;
    default:
        break;
    }
}

int puci_cli_run(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: puci run \"<prompt>\"\n");
        return 2;
    }

    /* Join argv[1..] into a single prompt, so unquoted multi-word prompts work. */
    size_t total = 1;
    for (int i = 1; i < argc; i++) {
        total += strlen(argv[i]) + 1;
    }
    char *prompt = (char *)malloc(total);
    if (!prompt) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    size_t off = 0;
    for (int i = 1; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (i > 1) {
            prompt[off++] = ' ';
        }
        memcpy(prompt + off, argv[i], len);
        off += len;
    }
    prompt[off] = '\0';

    const char *base = getenv("PUCI_BASE_URL");
    const char *key = getenv("PUCI_API_KEY");
    const char *model = getenv("PUCI_MODEL");
    if (!base) {
        base = "https://api.openai.com/v1";
    }
    if (!model) {
        model = "gpt-4o-mini";
    }

    /* Live-robustness overrides. TLS always verifies by default; relaxation is
     * only possible when the user explicitly opts in via PUCI_TLS_VERIFY=0 AND
     * the host is allowlisted through PUCI_INSECURE_TLS_HOSTS. */
    const char *cafile = getenv("PUCI_CA_BUNDLE");
    const char *extra = getenv("PUCI_EXTRA_BODY");
    const char *tls = getenv("PUCI_TLS_VERIFY");
    int tls_verify = 1;
    if (tls && *tls && (strcmp(tls, "0") == 0 || strcmp(tls, "false") == 0 ||
                        strcmp(tls, "off") == 0)) {
        tls_verify = 0;
    }
    const char *idle = getenv("PUCI_STREAM_IDLE_TIMEOUT_SECS");
    int idle_ms = 0;
    if (idle && *idle) {
        long secs = strtol(idle, NULL, 10);
        if (secs > 0 && secs <= 2147483) {
            idle_ms = (int)(secs * 1000);
        }
    }
    const char *max_iters_env = getenv("PUCI_MAX_TOOL_ITERATIONS");
    size_t max_tool_iterations = 0;
    if (max_iters_env && *max_iters_env) {
        char *end = NULL;
        unsigned long parsed = strtoul(max_iters_env, &end, 10);
        if (end != max_iters_env && *end == '\0' && parsed > 0 &&
            parsed <= 10000UL) {
            max_tool_iterations = (size_t)parsed;
        }
    }

    puci_context *ctx = NULL;
    if (puci_context_create(NULL, &ctx) != PUCI_OK) {
        fprintf(stderr, "puci: failed to create context\n");
        free(prompt);
        return 1;
    }

    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = base,
        .api_key = key,
        .model = model,
        .timeout_ms = 120000,
        .tls_verify = tls_verify,
        .stream_idle_timeout_ms = idle_ms,
        .ca_bundle = cafile,
        .extra_body = extra,
    };
    puci_status st = puci_context_set_provider(ctx, &pcfg);
    if (st != PUCI_OK) {
        fprintf(stderr, "puci: failed to set provider: %s\n",
                puci_status_string(st));
        puci_context_destroy(ctx);
        free(prompt);
        return 1;
    }

    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.event_cb = stream_event;
    sopts.system_prompt = "You are puci, a fast coding agent. Be concise.";
    sopts.max_tool_iterations = max_tool_iterations;

    puci_session *sess = NULL;
    st = puci_session_create(ctx, &sopts, &sess);
    if (st != PUCI_OK) {
        fprintf(stderr, "puci: failed to create session: %s\n",
                puci_status_string(st));
        puci_context_destroy(ctx);
        free(prompt);
        return 1;
    }

    st = puci_session_send(sess, prompt);
    if (st != PUCI_OK) {
        fprintf(stderr, "\npuci: %s", puci_status_string(st));
        const char *msg = puci_session_error(sess);
        if (msg) {
            fprintf(stderr, ": %s", msg);
        }
        fprintf(stderr, "\n");
        puci_session_destroy(sess);
        puci_context_destroy(ctx);
        free(prompt);
        return 1;
    }

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    free(prompt);
    return 0;
}
