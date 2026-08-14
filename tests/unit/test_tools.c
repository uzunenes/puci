/* test_tools.c - tool registry and invocation tests. */
#include "puci_internal.h"
#include "json/json.h"
#include "util/arena.h"
#include "tools/path_guard.h"
#include "../../src/cli/cmd/config.h"

#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

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
    const char *names[] = {"read_file", "write_file", "edit_file", "grep", "glob", "run_shell"};
    for (size_t i = 0; i < 6; i++) {
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

static void test_path_guards(void) {
    CHECK(puci_path_is_protected(".git") == 1);
    CHECK(puci_path_is_protected(".git/config") == 1);
    CHECK(puci_path_is_protected("./.git/HEAD") == 1);
    CHECK(puci_path_is_protected("build/output.txt") == 1);
    CHECK(puci_path_is_protected("build-gcc/src/lib/libpuci.a") == 1);
    CHECK(puci_path_is_protected("/tmp/puci-good") == 1);
    CHECK(puci_path_is_protected("safe_dir/file.txt") == 0);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    puci_tool *tw = NULL;
    CHECK(puci_tools_lookup(ctx, "write_file", &tw) == 1);
    char *res_w = NULL;
    int rc_w = tw->invoke(tw->userdata, NULL, "{\"path\":\".git/test.txt\",\"content\":\"bad\"}", &res_w);
    CHECK(rc_w == 1);
    CHECK(res_w != NULL);
    CHECK(strstr(res_w, "refused by path guard") != NULL);
    free(res_w);

    puci_tool *te = NULL;
    CHECK(puci_tools_lookup(ctx, "edit_file", &te) == 1);
    char *res_e = NULL;
    int rc_e = te->invoke(te->userdata, NULL, "{\"path\":\"build/test.txt\",\"old_str\":\"a\",\"new_str\":\"b\"}", &res_e);
    CHECK(rc_e == 1);
    CHECK(res_e != NULL);
    CHECK(strstr(res_e, "refused by path guard") != NULL);
    free(res_e);

    puci_context_destroy(ctx);
}

static void test_edit_file_tool(void) {
    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    char test_path[128];
    snprintf(test_path, sizeof(test_path), "/tmp/puci_test_edit_%d.txt", (int)getpid());
    unlink(test_path);

    char args[512];

    /* 1. Setup initial file */
    puci_tool *tw = NULL;
    CHECK(puci_tools_lookup(ctx, "write_file", &tw) == 1);
    char *res = NULL;
    snprintf(args, sizeof(args), "{\"path\":\"%s\",\"content\":\"line1\\nAAA\\nBBB\\nCCC\\n\"}", test_path);
    int rc = tw->invoke(tw->userdata, NULL, args, &res);
    CHECK(rc == 0);
    free(res);
    chmod(test_path, 0755);

    puci_tool *te = NULL;
    CHECK(puci_tools_lookup(ctx, "edit_file", &te) == 1);

    /* 2. Successful single-hunk edit */
    snprintf(args, sizeof(args), "{\"path\":\"%s\",\"old_str\":\"BBB\",\"new_str\":\"XXX\"}", test_path);
    rc = te->invoke(te->userdata, NULL, args, &res);
    CHECK(rc == 0);
    CHECK(res != NULL);
    CHECK(strstr(res, "\"replacements\":1") != NULL);
    free(res);

    /* Check content after single edit */
    FILE *f = fopen(test_path, "r");
    CHECK(f != NULL);
    char buf[256];
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';
    CHECK(strcmp(buf, "line1\nAAA\nXXX\nCCC\n") == 0);

    /* Check mode preserved */
    struct stat st;
    CHECK(stat(test_path, &st) == 0);
    CHECK((st.st_mode & 0777) == 0755);

    /* 3. Successful multi-hunk edit */
    snprintf(args, sizeof(args), "{\"path\":\"%s\",\"edits\":[{\"old_str\":\"AAA\",\"new_str\":\"111\"},{\"old_str\":\"XXX\",\"new_str\":\"222\"}]}", test_path);
    rc = te->invoke(te->userdata, NULL, args, &res);
    CHECK(rc == 0);
    CHECK(strstr(res, "\"replacements\":2") != NULL);
    free(res);

    f = fopen(test_path, "r");
    CHECK(f != NULL);
    rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';
    CHECK(strcmp(buf, "line1\n111\n222\nCCC\n") == 0);

    /* 4. Missing match rejected */
    snprintf(args, sizeof(args), "{\"path\":\"%s\",\"old_str\":\"NON_EXISTENT\",\"new_str\":\"Z\"}", test_path);
    rc = te->invoke(te->userdata, NULL, args, &res);
    CHECK(rc == 1);
    CHECK(strstr(res, "not found") != NULL);
    free(res);

    /* Original unchanged after failure */
    f = fopen(test_path, "r");
    CHECK(f != NULL);
    rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';
    CHECK(strcmp(buf, "line1\n111\n222\nCCC\n") == 0);

    /* 5. Ambiguous match rejected */
    snprintf(args, sizeof(args), "{\"path\":\"%s\",\"content\":\"dup\\nmiddle\\ndup\\n\"}", test_path);
    rc = tw->invoke(tw->userdata, NULL, args, &res);
    CHECK(rc == 0);
    free(res);

    snprintf(args, sizeof(args), "{\"path\":\"%s\",\"old_str\":\"dup\",\"new_str\":\"unique\"}", test_path);
    rc = te->invoke(te->userdata, NULL, args, &res);
    CHECK(rc == 1);
    CHECK(strstr(res, "ambiguous") != NULL);
    free(res);

    /* Original preserved after ambiguous error */
    f = fopen(test_path, "r");
    CHECK(f != NULL);
    rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';
    CHECK(strcmp(buf, "dup\nmiddle\ndup\n") == 0);

    /* 6. Guard refusal on protected path */
    snprintf(args, sizeof(args), "{\"path\":\".git/config\",\"old_str\":\"a\",\"new_str\":\"b\"}");
    rc = te->invoke(te->userdata, NULL, args, &res);
    CHECK(rc == 1);
    CHECK(strstr(res, "refused by path guard") != NULL);
    free(res);

    unlink(test_path);
    puci_context_destroy(ctx);
}

static void test_grep_and_glob(void) {
    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    char base_dir[64];
    char sub_dir[128];
    char file1[256];
    char file2[256];
    char file_ign[256];
    char gi_path[256];
    snprintf(base_dir, sizeof(base_dir), "/tmp/puci_search_%d", (int)getpid());
    snprintf(sub_dir, sizeof(sub_dir), "%s/sub", base_dir);
    snprintf(file1, sizeof(file1), "%s/file1.c", base_dir);
    snprintf(file2, sizeof(file2), "%s/file2.h", sub_dir);
    snprintf(file_ign, sizeof(file_ign), "%s/ignored.txt", base_dir);
    snprintf(gi_path, sizeof(gi_path), "%s/.gitignore", base_dir);

    CHECK(mkdir(base_dir, 0777) == 0 || errno == EEXIST);
    CHECK(mkdir(sub_dir, 0777) == 0 || errno == EEXIST);

    FILE *f1 = fopen(file1, "w");
    CHECK(f1 != NULL);
    if (f1) {
        fputs("hello world\nTARGET_SYMBOL line\nanother line\n", f1);
        fclose(f1);
    }
    FILE *f2 = fopen(file2, "w");
    CHECK(f2 != NULL);
    if (f2) {
        fputs("header file\ntarget_symbol lower\nmore TARGET_SYMBOL\n", f2);
        fclose(f2);
    }
    FILE *fign = fopen(file_ign, "w");
    CHECK(fign != NULL);
    if (fign) {
        fputs("TARGET_SYMBOL inside ignored file\n", fign);
        fclose(fign);
    }
    FILE *fgi = fopen(gi_path, "w");
    CHECK(fgi != NULL);
    if (fgi) {
        fputs("# gitignore comment\nignored.txt\n", fgi);
        fclose(fgi);
    }

    puci_tool *tg = NULL;
    CHECK(puci_tools_lookup(ctx, "grep", &tg) == 1);
    char *res = NULL;
    char args[512];
    snprintf(args, sizeof(args), "{\"pattern\":\"TARGET_SYMBOL\",\"path\":\"%s\"}", base_dir);
    int rc = tg->invoke(tg->userdata, NULL, args, &res);
    CHECK(rc == 0);
    CHECK(res != NULL);
    CHECK(strstr(res, "file1.c:2:") != NULL);
    /* Ignored file must be excluded */
    CHECK(strstr(res, "ignored.txt") == NULL);
    free(res);

    /* Case insensitive search */
    snprintf(args, sizeof(args), "{\"pattern\":\"TARGET_SYMBOL\",\"path\":\"%s\",\"case_sensitive\":false}", base_dir);
    rc = tg->invoke(tg->userdata, NULL, args, &res);
    CHECK(rc == 0);
    CHECK(res != NULL);
    CHECK(strstr(res, "file1.c:2:") != NULL);
    CHECK(strstr(res, "file2.h:2:") != NULL);
    CHECK(strstr(res, "ignored.txt") == NULL);
    free(res);

    /* No match reported distinctly from error */
    snprintf(args, sizeof(args), "{\"pattern\":\"COMPLETELY_ABSENT_STRING\",\"path\":\"%s\"}", base_dir);
    rc = tg->invoke(tg->userdata, NULL, args, &res);
    CHECK(rc == 0);
    CHECK(res != NULL);
    CHECK(strstr(res, "\"ok\":true") != NULL);
    CHECK(strstr(res, "\"matches\":0") != NULL);
    free(res);

    /* Truncation test with max_matches = 1 */
    snprintf(args, sizeof(args), "{\"pattern\":\"TARGET_SYMBOL\",\"path\":\"%s\",\"case_sensitive\":false,\"max_matches\":1}", base_dir);
    rc = tg->invoke(tg->userdata, NULL, args, &res);
    CHECK(rc == 0);
    CHECK(res != NULL);
    CHECK(strstr(res, "\"truncated\":true") != NULL);
    free(res);

    /* Glob test */
    puci_tool *tgl = NULL;
    CHECK(puci_tools_lookup(ctx, "glob", &tgl) == 1);
    snprintf(args, sizeof(args), "{\"pattern\":\"*.h\",\"path\":\"%s\"}", base_dir);
    rc = tgl->invoke(tgl->userdata, NULL, args, &res);
    CHECK(rc == 0);
    CHECK(res != NULL);
    CHECK(strstr(res, "file2.h") != NULL);
    free(res);

    unlink(file1);
    unlink(file2);
    unlink(file_ign);
    unlink(gi_path);
    rmdir(sub_dir);
    rmdir(base_dir);
    puci_context_destroy(ctx);
}

static void test_config_resolution(void) {
    const char *bad_proj = "/tmp/puci_bad_proj.toml";
    FILE *f = fopen(bad_proj, "w");
    CHECK(f != NULL);
    fputs("model = \"gpt-4o\"\napi_key = \"sk-forbidden-in-project\"\n", f);
    fclose(f);

    char err_buf[256];
    err_buf[0] = '\0';
    int rc = puci_config_parse_file(bad_proj, 1, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, NULL, err_buf, sizeof(err_buf));
    CHECK(rc == -1);
    CHECK(strstr(err_buf, "forbidden in project-local config") != NULL);
    unlink(bad_proj);

    const char *good_cfg = "/tmp/puci_good_cfg.toml";
    f = fopen(good_cfg, "w");
    CHECK(f != NULL);
    fputs("# Test config\nmodel = \"custom-model\"\ntls_verify = false\nstream_idle_timeout_secs = 45\n", f);
    fclose(f);

    char model[128] = "";
    int tls = 1;
    int idle_ms = 0;
    rc = puci_config_parse_file(good_cfg, 0, NULL, 0, model, sizeof(model), NULL, 0, NULL, 0, NULL, 0, &tls, &idle_ms, NULL, 0);
    CHECK(rc == 1);
    CHECK(strcmp(model, "custom-model") == 0);
    CHECK(tls == 0);
    CHECK(idle_ms == 45000);
    unlink(good_cfg);
}

int main(void) {
    test_builtins_registered();
    test_custom_tool_via_registry_path();
    test_async_tool_registration_validation();
    test_path_guards();
    test_edit_file_tool();
    test_grep_and_glob();
    test_config_resolution();
    TEST_END();
}
