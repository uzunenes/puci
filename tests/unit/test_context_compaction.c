/* test_context_compaction.c - 25-turn compaction with large tool outputs */
#include "puci/puci.h"
#include "puci_internal.h"
#include "../test_util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *make_large(size_t n, char c) {
    char *s = malloc(n+1);
    if (!s) return NULL;
    memset(s, c, n); s[n]='\0'; return s;
}

static void test_compaction(void) {
    puci_context *ctx=NULL;
    CHECK(puci_context_create(NULL,&ctx)==PUCI_OK);
    puci_provider_config pcfg={.name="openai", .base_url="http://127.0.0.1/v1", .model="mock-model"};
    puci_context_set_provider(ctx,&pcfg);
    puci_session_options sopts={0};
    sopts.provider=pcfg; sopts.system_prompt="system instructions: be terse";
    puci_session *sess=NULL;
    CHECK(puci_session_create(ctx,&sopts,&sess)==PUCI_OK);
    puci_session_set_context_budget(sess, 32768);

    /* Build 25 turns: user + assistant + tool result with 5k dump */
    for (int i=0;i<25;i++) {
        char ubuf[64]; snprintf(ubuf,sizeof(ubuf),"user prompt %d",i);
        CHECK(session_msg_begin(sess, PUCI_ROLE_USER, ubuf)==PUCI_OK);
        char abuf[64]; snprintf(abuf,sizeof(abuf),"assistant answer %d",i);
        CHECK(session_msg_begin(sess, PUCI_ROLE_ASSISTANT, abuf)==PUCI_OK);
        /* Simulate a tool result as TOOL message */
        char *large = make_large(5000, (char)('a'+(i%26)));
        CHECK(large!=NULL);
        CHECK(session_msg_begin(sess, PUCI_ROLE_TOOL, large)==PUCI_OK);
        CHECK(session_msg_set_tool_call_id(sess, "call_dup")==PUCI_OK);
        free(large);
    }
    /* Make duplicates for dedup test: repeat same content for 2 tools */
    /* Already covered by repeated 'a'+ pattern duplicates will be pruned */

    size_t before = puci_session_context_bytes(sess);
    CHECK(before > 32000);
    size_t pruned=0;
    CHECK(puci_session_compact_context(sess,&pruned)==PUCI_OK);
    CHECK(pruned>0);
    size_t after = puci_session_context_bytes(sess);
    CHECK(after <= puci_session_get_context_budget(sess));
    /* Tail preserved: last user prompt should still exist */
    int found_tail=0;
    for (size_t i=0;i<sess->nmessages;i++) if(sess->messages[i].content && strstr(sess->messages[i].content,"user prompt 24")) found_tail=1;
    CHECK(found_tail==1);
    /* Head preserved */
    CHECK(sess->system_prompt && strstr(sess->system_prompt,"be terse"));
    int found_head=0;
    for (size_t i=0;i<sess->nmessages;i++) if(sess->messages[i].content && strstr(sess->messages[i].content,"user prompt 0")) found_head=1;
    CHECK(found_head==1);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
}

static void test_budget_api(void) {
    puci_context *ctx=NULL;
    CHECK(puci_context_create(NULL,&ctx)==PUCI_OK);
    puci_provider_config pcfg={.name="openai", .base_url="http://127.0.0.1/v1", .model="m"};
    puci_context_set_provider(ctx,&pcfg);
    puci_session_options sopts={0}; sopts.provider=pcfg;
    puci_session *sess=NULL;
    CHECK(puci_session_create(ctx,&sopts,&sess)==PUCI_OK);
    CHECK(puci_session_get_context_budget(sess)==131072);
    CHECK(puci_session_set_context_budget(sess, 4096)==PUCI_OK);
    CHECK(puci_session_get_context_budget(sess)==4096);
    puci_session_destroy(sess);
    puci_context_destroy(ctx);
}

int main(void) {
    test_budget_api();
    test_compaction();
    TEST_END();
}
