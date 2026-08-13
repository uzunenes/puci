/*
 * examples/custom_tool.c - registering a tool from outside the library.
 *
 * Shows how to add a custom tool using only the public API. The tool is
 * advertised to the model and its callback runs in the host process when the
 * model calls it.
 *
 * Usage: ./custom_tool "<prompt>"   (config via PUCI_* env vars as in
 *                                    minimal_agent)
 */

#include <puci/puci.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Custom tool callback: returns the current Unix time. args_json is parsed but
 * not required here. The result string must be allocated with malloc(). */
static int invoke_greeting(void *userdata, const puci_cancel_token *cancel,
                           const char *args_json, char **out_result) {
    (void)userdata;
    (void)cancel;
    (void)args_json;
    /* A plausible tool result we might otherwise compose from real state. */
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"greeting\":\"hello from custom_tool\"}");
    *out_result = strdup(buf);
    return 0;
}

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
        fprintf(stderr, "\n[custom tool invoked] %s(%s)\n", tc->name,
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
        fprintf(stderr, "usage: %s \"<prompt>\"\n", argv[0]);
        return 2;
    }
    const char *base = getenv("PUCI_BASE_URL") ? getenv("PUCI_BASE_URL")
                                               : "https://api.openai.com/v1";
    const char *key = getenv("PUCI_API_KEY");
    const char *model = getenv("PUCI_MODEL") ? getenv("PUCI_MODEL")
                                             : "gpt-4o-mini";

    puci_context *ctx = NULL;
    if (puci_context_create(NULL, &ctx) != PUCI_OK) {
        return 1;
    }

    /* Register the custom tool -> available to every session on this context. */
    puci_tool_spec spec = {
        .name = "greeting_tool",
        .description =
            "Return a hello-world greeting. Call it when asked to greet.",
        .input_schema =
            "{\"type\":\"object\",\"properties\":{},"
            "\"additionalProperties\":false}",
    };
    if (puci_tool_register(ctx, &spec, invoke_greeting, NULL) != PUCI_OK) {
        fprintf(stderr, "failed to register custom tool\n");
        puci_context_destroy(ctx);
        return 1;
    }

    puci_provider_config pcfg = {
        .name = "openai", .base_url = base, .api_key = key, .model = model,
        .timeout_ms = 60000,
    };
    puci_context_set_provider(ctx, &pcfg);

    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.event_cb = on_event;

    puci_session *sess = NULL;
    if (puci_session_create(ctx, &sopts, &sess) != PUCI_OK) {
        puci_context_destroy(ctx);
        return 1;
    }

    printf("> %s\n", argv[1]);
    puci_status st = puci_session_send(sess, argv[1]);
    if (st != PUCI_OK) {
        fprintf(stderr, "error: %s\n", puci_status_string(st));
    }

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    return st == PUCI_OK ? 0 : 1;
}