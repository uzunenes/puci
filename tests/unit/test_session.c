/* test_session.c - public API lifecycle tests (offline; no real endpoint). */
#include "puci/puci.h"

#include <string.h>

#include "../test_util.h"

static int g_deltas = 0;
static int g_complete = 0;
static void on_event(void *ud, puci_session *s, puci_event ev, const void *ctx) {
    (void)ud;
    (void)s;
    (void)ctx;
    if (ev.kind == PUCI_EVENT_TOKEN_DELTA) {
        g_deltas++;
    } else if (ev.kind == PUCI_EVENT_TURN_COMPLETE) {
        g_complete++;
    }
}

static void test_context_lifecycle(void) {
    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);
    CHECK(ctx != NULL);
    CHECK(puci_provider_available(ctx, "openai") == 1);
    CHECK(puci_provider_available(ctx, "does-not-exist") == 0);
    CHECK(strcmp(puci_version_string(), PUCI_VERSION_STRING) == 0);
    CHECK(puci_abi_version() == PUCI_ABI_VERSION);
    CHECK(puci_tool_get(ctx, "read_file") != NULL);
    CHECK(puci_tool_get(ctx, "write_file") != NULL);
    CHECK(puci_tool_get(ctx, "run_shell") != NULL);
    CHECK(puci_tool_get(ctx, "nope") == NULL);

    puci_provider_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "openai";
    cfg.base_url = "http://127.0.0.1:1/v1";
    cfg.model = "test-model";
    cfg.api_key = "sk-test";
    cfg.timeout_ms = 3000;
    CHECK(puci_context_set_provider(ctx, &cfg) == PUCI_OK);

    puci_context_destroy(ctx);
}

static void test_session_creates_and_fails_gracefully(void) {
    puci_context *ctx = NULL;
    puci_context_create(NULL, &ctx);
    puci_provider_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "openai";
    cfg.base_url = "http://127.0.0.1:1/v1"; /* nothing listening */
    cfg.model = "test-model";
    cfg.timeout_ms = 3000;

    puci_session_options sopts;
    memset(&sopts, 0, sizeof(sopts));
    sopts.provider = cfg;
    sopts.event_cb = on_event;
    sopts.system_prompt = "you are a test";

    puci_session *sess = NULL;
    CHECK(puci_session_create(ctx, &sopts, &sess) == PUCI_OK);
    CHECK(sess != NULL);

    puci_status st = puci_session_send(sess, "hello");
    /* Either a network error or a timeout is acceptable; it must not crash. */
    CHECK(st == PUCI_ERR_NETWORK || st == PUCI_ERR_TIMEOUT ||
          st == PUCI_ERR_UNAUTHORIZED);

    puci_session_destroy(sess);
    puci_context_destroy(ctx);
}

static void test_bad_args(void) {
    CHECK(puci_context_create(NULL, NULL) == PUCI_ERR_INVALID_ARG);
    puci_session *s = NULL;
    CHECK(puci_session_create(NULL, NULL, &s) == PUCI_ERR_INVALID_ARG);
    CHECK(puci_status_string(PUCI_ERR_NOMEM) != NULL);
    CHECK(strcmp(puci_status_token(PUCI_OK), "ok") == 0);
}

int main(void) {
    test_context_lifecycle();
    test_bad_args();
    test_session_creates_and_fails_gracefully();
    TEST_END();
}