/* test_session_api.c - tests for the session usage / model / clear API added
 * for the REPL (token accounting, /model, /clear).
 *
 * Verifies:
 *   - puci_session_set_model replaces the model used for subsequent turns.
 *   - puci_session_clear drops the conversation while keeping the model.
 *   - puci_session_get_usage returns the reported token totals.
 */

#include "puci_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void test_session_export_import(void) {
    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = "http://127.0.0.1/v1",
        .model = "gpt-model-x",
    };
    puci_context_set_provider(ctx, &pcfg);
    puci_session_options op = {0};
    op.provider = pcfg;
    op.system_prompt = "system instructions";
    puci_session *s1 = NULL;
    CHECK(puci_session_create(ctx, &op, &s1) == PUCI_OK);

    s1->total_prompt_tokens = 100;
    s1->total_completion_tokens = 50;
    s1->total_cached_tokens = 25;

    /* Turn 1: user -> assistant tool call -> tool result */
    CHECK(session_msg_begin(s1, PUCI_ROLE_USER, "check files") == PUCI_OK);

    puci_internal_tool_call tc;
    memset(&tc, 0, sizeof(tc));
    tc.id = puci_alloc_strdup(&s1->alloc, "call_abc");
    puci_strbuf_init(&tc.name, &s1->alloc);
    puci_strbuf_appendz(&tc.name, "glob");
    puci_strbuf_init(&tc.args, &s1->alloc);
    puci_strbuf_appendz(&tc.args, "{\"pattern\":\"*.c\"}");

    puci_internal_tool_call *tcalls = (puci_internal_tool_call *)puci_alloc_calloc(&s1->alloc, 1, sizeof(puci_internal_tool_call));
    tcalls[0] = tc;

    CHECK(session_msg_begin(s1, PUCI_ROLE_ASSISTANT, NULL) == PUCI_OK);
    CHECK(session_msg_finish_assistant(s1, "calling glob", tcalls, 1) == PUCI_OK);

    CHECK(session_msg_begin(s1, PUCI_ROLE_TOOL, "{\"ok\":true,\"count\":1}") == PUCI_OK);
    CHECK(session_msg_set_tool_call_id(s1, "call_abc") == PUCI_OK);

    /* Turn 2: assistant final message */
    CHECK(session_msg_begin(s1, PUCI_ROLE_ASSISTANT, "Found files.") == PUCI_OK);
    CHECK(session_msg_finish_assistant(s1, "Found files.", NULL, 0) == PUCI_OK);

    CHECK(puci_session_validate(s1) == PUCI_OK);

    char *json_out = NULL;
    CHECK(puci_session_export_json(s1, &json_out) == PUCI_OK);
    CHECK(json_out != NULL);
    CHECK(strstr(json_out, "gpt-model-x") != NULL);
    CHECK(strstr(json_out, "call_abc") != NULL);
    CHECK(strstr(json_out, "check files") != NULL);

    /* Import into fresh session */
    puci_session *s2 = NULL;
    CHECK(puci_session_create(ctx, &op, &s2) == PUCI_OK);
    CHECK(puci_session_import_json(s2, json_out) == PUCI_OK);
    CHECK(puci_session_validate(s2) == PUCI_OK);
    CHECK(s2->nmessages == s1->nmessages);
    CHECK(strcmp(s2->cfg.model, "gpt-model-x") == 0);

    long long p = 0, c = 0, cached = 0;
    puci_session_get_usage(s2, &p, &c, &cached);
    CHECK(p == 100 && c == 50 && cached == 25);

    free(json_out);
    puci_session_destroy(s1);
    puci_session_destroy(s2);
    puci_context_destroy(ctx);
}

static void test_session_file_save_load(void) {
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
    puci_session *s1 = NULL;
    CHECK(puci_session_create(ctx, &op, &s1) == PUCI_OK);
    CHECK(session_msg_begin(s1, PUCI_ROLE_USER, "hello persistence") == PUCI_OK);

    const char *path = "/tmp/puci_test_session_save_load.json";
    unlink(path);

    /* Save to file */
    CHECK(puci_session_save_file(s1, path) == PUCI_OK);

    /* Path guard refusal */
    CHECK(puci_session_save_file(s1, ".git/session.json") == PUCI_ERR_INVALID_ARG);
    CHECK(puci_session_save_file(s1, "build/session.json") == PUCI_ERR_INVALID_ARG);

    /* Load into fresh session */
    puci_session *s2 = NULL;
    CHECK(puci_session_create(ctx, &op, &s2) == PUCI_OK);
    CHECK(puci_session_load_file(s2, path) == PUCI_OK);
    CHECK(s2->nmessages == 1);
    CHECK(s2->messages[0].content && strcmp(s2->messages[0].content, "hello persistence") == 0);

    unlink(path);

    /* Corrupted file handling */
    const char *corrupted_path = "/tmp/puci_test_corrupted.json";
    FILE *cf = fopen(corrupted_path, "w");
    CHECK(cf != NULL);
    fputs("{\"messages\": [INVALID JSON ...", cf);
    fclose(cf);

    puci_session *s3 = NULL;
    CHECK(puci_session_create(ctx, &op, &s3) == PUCI_OK);
    CHECK(puci_session_load_file(s3, corrupted_path) == PUCI_ERR_INVALID_ARG);
    unlink(corrupted_path);

    puci_session_destroy(s1);
    puci_session_destroy(s2);
    puci_session_destroy(s3);
    puci_context_destroy(ctx);
}

static void test_context_estimation_and_options(void) {
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
    op.system_prompt = "short system";
    op.plan_mode = 1;
    puci_session *sess = NULL;
    CHECK(puci_session_create(ctx, &op, &sess) == PUCI_OK);

    CHECK(puci_session_get_plan_mode(sess) == 1);
    CHECK(puci_session_set_plan_mode(sess, 0) == PUCI_OK);
    CHECK(puci_session_get_plan_mode(sess) == 0);

    CHECK(puci_session_set_approval_feedback(sess, "custom user feedback") == PUCI_OK);
    CHECK(sess->approval_feedback && strcmp(sess->approval_feedback, "custom user feedback") == 0);

    size_t chars = 0, tokens = 0;
    puci_session_estimate_context(sess, &chars, &tokens);
    CHECK(chars > 0);
    CHECK(tokens > 0);

    CHECK(session_msg_begin(sess, PUCI_ROLE_USER, "some 20-character msg") == PUCI_OK);
    size_t chars2 = 0, tokens2 = 0;
    puci_session_estimate_context(sess, &chars2, &tokens2);
    CHECK(chars2 > chars);
    CHECK(tokens2 >= tokens);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
}

int main(void) {
    test_model_and_clear();
    test_usage();
    test_session_export_import();
    test_session_file_save_load();
    test_context_estimation_and_options();
    TEST_END();
}
