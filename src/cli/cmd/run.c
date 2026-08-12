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
    switch (ev) {
    case PUCI_EVENT_TOKEN_DELTA: {
        const char *t = (const char *)ctx;
        if (t) {
            fputs(t, stdout);
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