/*
 * cmd/run.c - `puci run "<prompt>"` subcommand.
 *
 * Built exclusively on the public libpuci API. Reads configuration from the
 * environment (PUCI_BASE_URL / PUCI_API_KEY / PUCI_MODEL); full config.toml
 * support arrives in a later phase. Streams deltas to stdout and echoes tool
 * activity to stderr, so a clean transcript can be piped from stdout.
 */

#include <puci/puci.h>
#include "config.h"
#include "session_mgr.h"
#include "cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

int puci_cli_run_with_params(const puci_cli_params *params) {
    const char *prompt = params ? params->prompt : NULL;
    if (!prompt || !prompt[0]) {
        fprintf(stderr, "usage: puci run \"<prompt>\"\n");
        return 2;
    }

    puci_provider_config *pcfg = puci_cli_provider("openai");
    if (!pcfg) {
        fprintf(stderr, "puci: failed to resolve provider config\n");
        return 1;
    }

    if (!pcfg->api_key || !pcfg->api_key[0]) {
        fprintf(stderr, "puci: missing API key. Set PUCI_API_KEY environment variable or configure 'api_key' in ~/.config/puci/config.toml\n");
        return 1;
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
        return 1;
    }

    puci_status st = puci_context_set_provider(ctx, pcfg);
    if (st != PUCI_OK) {
        fprintf(stderr, "puci: failed to set provider: %s\n",
                puci_status_string(st));
        puci_context_destroy(ctx);
        return 1;
    }

    puci_session_options sopts = {0};
    sopts.provider = *pcfg;
    sopts.event_cb = stream_event;
    sopts.system_prompt = "You are puci, a fast coding agent. Be concise.";
    sopts.max_tool_iterations = max_tool_iterations;
    sopts.plan_mode = params ? params->plan_mode : 0;
    sopts.tool_timeout_ms = 60000;

    puci_session *sess = NULL;
    st = puci_session_create(ctx, &sopts, &sess);
    if (st != PUCI_OK) {
        fprintf(stderr, "puci: failed to create session: %s\n",
                puci_status_string(st));
        puci_context_destroy(ctx);
        return 1;
    }

    char active_id[256] = "";
    if (params && params->resume_id) {
        if (puci_cli_load_session(sess, params->resume_id, active_id, sizeof(active_id)) == 0) {
            fprintf(stderr, "[resumed session: %s]\n", active_id);
        } else {
            fprintf(stderr, "puci: could not resume session '%s'\n", params->resume_id);
        }
    } else if (params && params->session_id) {
        (void)puci_cli_load_session(sess, params->session_id, active_id, sizeof(active_id));
        snprintf(active_id, sizeof(active_id), "%s", params->session_id);
    }

    if (params && params->plan_mode) {
        fprintf(stderr, "[plan mode: read-only dry run; no disk modifications will be made]\n");
    }

    size_t chars = 0, tokens = 0;
    puci_session_estimate_context(sess, &chars, &tokens);
    if (tokens > 50000) {
        fprintf(stderr, "[notice] transcript context size is ~%zu tokens, nearing context limit\n", tokens);
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
        return 1;
    }

    if (active_id[0]) {
        puci_cli_save_session(sess, active_id);
    } else {
        char auto_id[64];
        snprintf(auto_id, sizeof(auto_id), "session_%lld", (long long)time(NULL));
        puci_cli_save_session(sess, auto_id);
    }

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    return 0;
}

int puci_cli_run(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: puci run \"<prompt>\"\n");
        return 2;
    }

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

    puci_cli_params params = {0};
    params.prompt = prompt;
    int rc = puci_cli_run_with_params(&params);
    free(prompt);
    return rc;
}
