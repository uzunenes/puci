/*
 * examples/minimal_agent.c - the smallest possible libpuci consumer.
 *
 * Uses only the public headers in include/puci/. Builds a context, configures
 * the OpenAI-compatible provider, creates a session, sends one message, and
 * streams the completion (including any tool calls the model makes) to stdout.
 *
 * Configuration (all optional):
 *   PUCI_BASE_URL   provider base URL  (default https://api.openai.com/v1)
 *   PUCI_API_KEY    API key            (required for hosted providers)
 *   PUCI_MODEL      model id           (default gpt-4o-mini)
 *
 * Usage: ./minimal_agent "your prompt here"
 */

#include <puci/puci.h>

#include <stdio.h>
#include <stdlib.h>

/* Streams output and echoes tool calls / results to stderr. */
static void on_event(void *ud, puci_session *sess, puci_event ev,
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
        fprintf(stderr, "\n[tool] %s(%s)\n", tc->name,
                tc->args_json ? tc->args_json : "");
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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s \"<prompt>\"\n"
                "  set PUCI_BASE_URL, PUCI_API_KEY, PUCI_MODEL as needed\n",
                argv[0]);
        return 2;
    }
    const char *prompt = argv[1];
    const char *base = getenv("PUCI_BASE_URL");
    const char *key = getenv("PUCI_API_KEY");
    const char *model = getenv("PUCI_MODEL");
    if (!base) {
        base = "https://api.openai.com/v1";
    }
    if (!model) {
        model = "gpt-4o-mini";
    }
    if (!key) {
        fprintf(stderr, "warning: PUCI_API_KEY not set; "
                        "an Ollama/vLLM/openai endpoint may not require one\n");
    }

    puci_context *ctx = NULL;
    if (puci_context_create(NULL, &ctx) != PUCI_OK) {
        fprintf(stderr, "failed to create context\n");
        return 1;
    }

    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = base,
        .api_key = key,
        .model = model,
        .timeout_ms = 60000,
    };
    if (puci_context_set_provider(ctx, &pcfg) != PUCI_OK) {
        fprintf(stderr, "failed to set provider config\n");
        puci_context_destroy(ctx);
        return 1;
    }

    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.event_cb = on_event;
    sopts.system_prompt = "You are a helpful coding agent.";

    puci_session *sess = NULL;
    if (puci_session_create(ctx, &sopts, &sess) != PUCI_OK) {
        fprintf(stderr, "failed to create session\n");
        puci_context_destroy(ctx);
        return 1;
    }

    printf("> %s\n", prompt);
    puci_status st = puci_session_send(sess, prompt);
    if (st != PUCI_OK) {
        fprintf(stderr, "\nerror: %s", puci_status_string(st));
        const char *msg = puci_session_error(sess);
        if (msg) {
            fprintf(stderr, " (%s)", msg);
        }
        fprintf(stderr, "\n");
        puci_session_destroy(sess);
        puci_context_destroy(ctx);
        return 1;
    }

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    return 0;
}
