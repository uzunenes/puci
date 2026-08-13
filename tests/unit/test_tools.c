/* test_tools.c - tool registry and invocation tests. */
#include "puci_internal.h"
#include "json/json.h"
#include "util/arena.h"

#include <string.h>

#include "../test_util.h"

static int invoke_echo(void *ud, const puci_cancel_token *cancel,
                       const char *args_json, char **out) {
    (void)ud;
    (void)cancel;
    (void)args_json;
    *out = strdup("echo-reply");
    return 0;
}

static int invoke_failing(void *ud, const puci_cancel_token *cancel,
                          const char *args_json, char **out) {
    (void)ud;
    (void)cancel;
    (void)args_json;
    (void)out;
    return 1;
}

static void test_builtins_registered(void) {
    puci_context *ctx = NULL;
    puci_context_create(NULL, &ctx);
    const char *names[] = {"read_file", "write_file", "run_shell"};
    for (size_t i = 0; i < 3; i++) {
        const puci_tool_spec *spec = puci_tool_get(ctx, names[i]);
        CHECK(spec != NULL);
        CHECK(spec->name && strcmp(spec->name, names[i]) == 0);
        CHECK(spec->description != NULL);
        /* The schema must be valid JSON. */
        if (spec->input_schema) {
            puci_arena a;
            puci_arena_init(&a, NULL, 0);
            pj_val *root = NULL;
            CHECK(pj_parse(&a, spec->input_schema, strlen(spec->input_schema),
                           &root) == 0);
            CHECK(root && root->type == PJ_OBJ);
            puci_arena_destroy(&a);
        }
    }
    CHECK(puci_tool_get(ctx, "bogus") == NULL);
    puci_context_destroy(ctx);
}

static void test_custom_tool_via_registry_path(void) {
    puci_context *ctx = NULL;
    puci_context_create(NULL, &ctx);

    puci_tool_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = "echo_tool";
    spec.description = "echo tool";
    spec.input_schema = "{\"type\":\"object\"}";
    CHECK(puci_tool_register(ctx, &spec, invoke_echo, NULL) == PUCI_OK);

    puci_tool *t = NULL;
    CHECK(puci_tools_lookup(ctx, "echo_tool", &t) == 1);
    CHECK(t && t->invoke);
    char *res = NULL;
    int rc = t->invoke(t->userdata, NULL, "{}", &res);
    CHECK(rc == 0);
    CHECK(res && strcmp(res, "echo-reply") == 0);
    free(res);

    /* Replacing a tool updates the callback. */
    CHECK(puci_tool_register(ctx, &spec, invoke_failing, NULL) == PUCI_OK);
    CHECK(puci_tools_lookup(ctx, "echo_tool", &t) == 1);
    res = NULL;
    rc = t->invoke(t->userdata, NULL, "{}", &res);
    CHECK(rc == 1);
    if (res) {
        free(res);
    }

    puci_context_destroy(ctx);
}

static void test_async_tool_registration_validation(void) {
    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    puci_tool_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = "async_mock";
    spec.description = "mock async tool";
    spec.input_schema = "{\"type\":\"object\"}";

    /* Incomplete vtable (e.g. NULL step) must be rejected */
    puci_tool_async_vtable vt;
    memset(&vt, 0, sizeof(vt));
    CHECK(puci_tool_register_async(ctx, &spec, &vt, NULL) == PUCI_ERR_INVALID_ARG);
    CHECK(puci_tool_register_async(ctx, NULL, &vt, NULL) == PUCI_ERR_INVALID_ARG);
    CHECK(puci_tool_register_async(ctx, &spec, NULL, NULL) == PUCI_ERR_INVALID_ARG);

    puci_context_destroy(ctx);
}

int main(void) {
    test_builtins_registered();
    test_custom_tool_via_registry_path();
    test_async_tool_registration_validation();
    TEST_END();
}
