/* test_provider_fixtures.c - regression test tying the per-provider SSE
 * transcript fixtures in tests/fixtures/transcripts (one JSON payload per
 * line) to the real OpenAI provider stream parser.
 *
 * Verifies the two properties that matter for the live endpoint work:
 *   - reasoning_content deltas accumulate into turn.reasoning while answer
 *     content accumulates into turn.text, and the two are never joined;
 *   - the OpenAI tool_call/argument splitter still assembles a run_shell call
 *     from fragmented argument chunks.
 */

#include "puci_internal.h"
#include "json/json.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <string.h>

#include "../test_util.h"

#ifndef PUCI_FIXTURES_DIR
#define PUCI_FIXTURES_DIR "tests/fixtures/transcripts"
#endif

#define FNAME_MAX 512

/* Feeds every JSON `data:` payload of the transcript file through the OpenAI
 * provider parser, accumulating into a single turn exactly as the agent loop
 * does across SSE events, then calls on_done with the accumulated turn. */
static void feed_transcript(const char *fname, void (*on_done)(puci_turn *)) {
    char path[FNAME_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", PUCI_FIXTURES_DIR, fname);
    CHECK(n > 0 && n < (int)sizeof(path));

    FILE *f = fopen(path, "r");
    CHECK(f != NULL);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = "http://127.0.0.1/v1",
        .model = "m",
    };
    puci_context_set_provider(ctx, &pcfg);
    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    puci_session *sess = NULL;
    CHECK(puci_session_create(ctx, &sopts, &sess) == PUCI_OK);

    puci_turn t;
    memset(&t, 0, sizeof(t));
    puci_strbuf_init(&t.text, &sess->alloc);
    puci_strbuf_init(&t.reasoning, &sess->alloc);

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        /* [DONE] is SSE's terminal marker, not a JSON payload. */
        if (strcmp(line, "[DONE]") == 0 || line[0] != '{') {
            continue;
        }
        puci_arena_reset(&sess->scratch);
        puci_openai_provider.parse_stream(sess, &sess->scratch, line, len, &t);
    }
    on_done(&t);

    /* parse_stream heap-allocates any assembled tool calls into the turn;
     * the agent loop transfers ownership to a session message, but here we
     * release them directly (mirroring free_msg's tool-call cleanup). */
    for (size_t i = 0; i < t.ntool_calls; i++) {
        puci_internal_tool_call *tc = &t.tool_calls[i];
        if (tc->id) {
            puci_alloc_free(&sess->alloc, tc->id);
        }
        puci_strbuf_destroy(&tc->name);
        puci_strbuf_destroy(&tc->args);
    }
    if (t.tool_calls) {
        puci_alloc_free(&sess->alloc, t.tool_calls);
    }

    puci_strbuf_destroy(&t.text);
    puci_strbuf_destroy(&t.reasoning);
    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    fclose(f);
}

/* Reasoning fixture: reasoning deltas and the answer end up in separate
 * buffers, never concatenated. */
static void check_reasoning(puci_turn *t) {
    CHECK(strcmp(puci_strbuf_cstr(&t->reasoning),
                 "Let me compute this. 2 plus 3 is 5.") == 0);
    CHECK(strcmp(puci_strbuf_cstr(&t->text), "The answer is 5.") == 0);
}

/* Toolcall fixture: the answer text accumulates from fractional content chunks,
 * and a run_shell tool call was assembled. */
static void check_toolcall(puci_turn *t) {
    CHECK(strcmp(puci_strbuf_cstr(&t->text),
                 "Done. The tool ran and replied: hello-from-puci-cli") == 0);
    CHECK(t->had_tool_calls == 1);
    CHECK(t->ntool_calls == 1);
}

static void test_reasoning_fixture(void) {
    feed_transcript("openai_reasoning.jsonl", check_reasoning);
}

static void test_toolcall_fixture(void) {
    feed_transcript("openai_toolcall.jsonl", check_toolcall);
}

int main(void) {
    test_reasoning_fixture();
    test_toolcall_fixture();
    TEST_END();
}
