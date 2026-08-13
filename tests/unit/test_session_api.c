/* test_session_api.c - tests for the session usage / model / clear API added
 * for the REPL (token accounting, /model, /clear).
 *
 * Verifies:
 *   - puci_session_set_model replaces the model used for subsequent turns.
 *   - puci_session_clear drops the conversation while keeping the model.
 *   - puci_session_get_usage returns the reported token totals.
 */

#include "puci_internal.h"

#include <string.h>

#include "../test_util.h"

static void test_model_and_clear(void) {
    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = "http://127.0.0.1/v1",
        .model = "gpt-a",
    };
    puci_context_set_provider(ctx, &pcfg);
    puci_session_options op = {0};
    op.provider = pcfg;
    op.system_prompt = "sys";
    puci_session *sess = NULL;
    CHECK(puci_session_create(ctx, &op, &sess) == PUCI_OK);

    CHECK(puci_session_set_model(sess, "gpt-b") == PUCI_OK);
    CHECK(strcmp(sess->cfg.model, "gpt-b") == 0);

    /* Add messages, then clear. */
    CHECK(session_msg_begin(sess, PUCI_ROLE_USER, "hi") == PUCI_OK);
    CHECK(sess->nmessages == 1);
    CHECK(puci_session_clear(sess) == PUCI_OK);
    CHECK(sess->nmessages == 0);
    /* model survives clear */
    CHECK(strcmp(sess->cfg.model, "gpt-b") == 0);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
}

static void test_usage(void) {
    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = "http://127.0.0.1/v1",
        .model = "m",
    };
    puci_context_set_provider(ctx, &pcfg);
    puci_session_options op = {0};
    op.provider = pcfg;
    puci_session *sess = NULL;
    CHECK(puci_session_create(ctx, &op, &sess) == PUCI_OK);

    long long p = 0, c = 0, cached = 0;
    puci_session_get_usage(sess, &p, &c, &cached);
    CHECK(p == 0 && c == 0 && cached == 0);

    /* Simulate a parse that reports usage (as the OpenAI provider would). */
    puci_turn t;
    memset(&t, 0, sizeof(t));
    puci_strbuf_init(&t.text, &sess->alloc);
    puci_strbuf_init(&t.reasoning, &sess->alloc);
    const char *chunk =
        "{\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":7,"
        "\"prompt_tokens_details\":{\"cached_tokens\":4}}}";
    puci_openai_provider.parse_stream(sess, &sess->scratch, chunk,
                                      strlen(chunk), &t);
    CHECK(t.prompt_tokens == 11);
    CHECK(t.completion_tokens == 7);
    CHECK(t.cached_tokens == 4);

    /* Fold into the session (as the agent loop does) and read back. */
    sess->total_prompt_tokens += t.prompt_tokens;
    sess->total_completion_tokens += t.completion_tokens;
    sess->total_cached_tokens += t.cached_tokens;
    puci_session_get_usage(sess, &p, &c, &cached);
    CHECK(p == 11 && c == 7 && cached == 4);

    puci_strbuf_destroy(&t.text);
    puci_strbuf_destroy(&t.reasoning);
    puci_session_destroy(sess);
    puci_context_destroy(ctx);
}

int main(void) {
    test_model_and_clear();
    test_usage();
    TEST_END();
}
