/* test_serialize.c - regression test for request body serialization.
 *
 * The live endpoint caught that session_serialize_messages/tools emitted the
 * message/tool objects WITHOUT wrapping them in JSON array brackets, producing
 * an invalid request body. This test asserts the serializers always produce a
 * valid JSON array with the expected element count.
 */

#include "puci_internal.h"
#include "json/json.h"
#include "util/arena.h"
#include "util/strbuf.h"

#include <string.h>

#include "../test_util.h"

static void test_messages_and_tools_are_json_arrays(void) {
    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    puci_provider_config pcfg = {
        .name = "openai", .base_url = "http://127.0.0.1/v1", .model = "m",
    };
    CHECK(puci_context_set_provider(ctx, &pcfg) == PUCI_OK);

    puci_session_options op = {0};
    op.provider = pcfg;
    op.system_prompt = "you are a test";
    puci_session *sess = NULL;
    CHECK(puci_session_create(ctx, &op, &sess) == PUCI_OK);

    /* Session with a system prompt + one user message. */
    CHECK(session_msg_begin(sess, PUCI_ROLE_USER, "hello") == PUCI_OK);
    CHECK(sess->nmessages == 1);

    /* Messages must serialize to a JSON array of [system, user]. */
    puci_strbuf msgs;
    puci_strbuf_init(&msgs, &sess->alloc);
    CHECK(session_serialize_messages(sess, &msgs) == PUCI_OK);
    {
        puci_arena a;
        puci_arena_init(&a, NULL, 0);
        pj_val *root = NULL;
        int rc = pj_parse(&a, puci_strbuf_cstr(&msgs), msgs.len, &root);
        puci_arena_destroy(&a);
        CHECK(rc == 0);
        CHECK(root && root->type == PJ_ARR);
        CHECK(pj_arr_len(root) == 2);
    }

    /* Tools must serialize to a JSON array of the 3 built-in tools. */
    puci_strbuf tools;
    puci_strbuf_init(&tools, &sess->alloc);
    CHECK(session_serialize_tools(sess, &tools) == PUCI_OK);
    {
        puci_arena a;
        puci_arena_init(&a, NULL, 0);
        pj_val *root = NULL;
        int rc = pj_parse(&a, puci_strbuf_cstr(&tools), tools.len, &root);
        puci_arena_destroy(&a);
        CHECK(rc == 0);
        CHECK(root && root->type == PJ_ARR);
        CHECK(pj_arr_len(root) == 3);
    }

    /* An assistant tool-call message must also serialize validly. */
    CHECK(session_msg_begin(sess, PUCI_ROLE_ASSISTANT, NULL) == PUCI_OK);
    {
        /* finish_assistant transfers ownership of the array, so it must be
         * heap-allocated (not a stack local). */
        puci_internal_tool_call *tcs =
            (puci_internal_tool_call *)puci_alloc_calloc(&sess->alloc, 1,
                                                         sizeof(*tcs));
        CHECK(tcs != NULL);
        puci_strbuf_init(&tcs->name, &sess->alloc);
        puci_strbuf_init(&tcs->args, &sess->alloc);
        puci_strbuf_appendz(&tcs->name, "read_file");
        puci_strbuf_appendz(&tcs->args, "{\"path\":\"x\"}");
        tcs->id = puci_alloc_strdup(&sess->alloc, "call_1");
        CHECK(session_msg_finish_assistant(sess, NULL, tcs, 1) == PUCI_OK);
    }
    puci_strbuf msgs2;
    puci_strbuf_init(&msgs2, &sess->alloc);
    CHECK(session_serialize_messages(sess, &msgs2) == PUCI_OK);
    {
        puci_arena a;
        puci_arena_init(&a, NULL, 0);
        pj_val *root = NULL;
        int rc = pj_parse(&a, puci_strbuf_cstr(&msgs2), msgs2.len, &root);
        puci_arena_destroy(&a);
        CHECK(rc == 0);
        CHECK(root && root->type == PJ_ARR);
        CHECK(pj_arr_len(root) == 3);
    }

    puci_strbuf_destroy(&msgs);
    puci_strbuf_destroy(&tools);
    puci_strbuf_destroy(&msgs2);
    puci_session_destroy(sess);
    puci_context_destroy(ctx);
}

int main(void) {
    test_messages_and_tools_are_json_arrays();
    TEST_END();
}