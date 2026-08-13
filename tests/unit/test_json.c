/* test_json.c - unit tests for the internal JSON DOM parser. */
#include "json/json.h"
#include "util/arena.h"

#include <string.h>

#include "../test_util.h"

static void test_basic_object(void) {
    puci_arena a;
    puci_arena_init(&a, NULL, 0);
    pj_val *root = NULL;
    const char *txt = "{\"model\":\"gpt-4\",\"n\":3,\"ok\":true,\"x\":null}";
    CHECK(pj_parse(&a, txt, strlen(txt), &root) == 0);
    CHECK(root && root->type == PJ_OBJ);
    CHECK(pj_get(root, "model") && strcmp(pj_str(pj_get(root, "model")), "gpt-4") == 0);
    CHECK(pj_num(pj_get(root, "n")) == 3.0);
    CHECK(pj_bool(pj_get(root, "ok")) == 1);
    pj_val *x = pj_get(root, "x");
    CHECK(x && x->type == PJ_NULL);
    CHECK(pj_get(root, "missing") == NULL);
    CHECK(pj_obj_len(root) == 4);
    puci_arena_destroy(&a);
}

static void test_array(void) {
    puci_arena a;
    puci_arena_init(&a, NULL, 0);
    pj_val *root = NULL;
    const char *txt = "[1,2.5,-3,\"a\",true,null]";
    CHECK(pj_parse(&a, txt, strlen(txt), &root) == 0);
    CHECK(root && root->type == PJ_ARR);
    CHECK(pj_arr_len(root) == 6);
    CHECK(pj_num(pj_arr_get(root, 1)) == 2.5);
    CHECK(pj_num(pj_arr_get(root, 2)) == -3.0);
    CHECK(pj_str(pj_arr_get(root, 3)) && strcmp(pj_str(pj_arr_get(root, 3)), "a") == 0);
    puci_arena_destroy(&a);
}

static void test_escapes(void) {
    puci_arena a;
    puci_arena_init(&a, NULL, 0);
    pj_val *root = NULL;
    const char *txt = "{\"s\":\"line\\n\\t\\\"q\\\"\\\\\"}";
    CHECK(pj_parse(&a, txt, strlen(txt), &root) == 0);
    const char *s = pj_str(pj_get(root, "s"));
    CHECK(s && strcmp(s, "line\n\t\"q\"\\") == 0);
    puci_arena_destroy(&a);
}

static void test_unicode_escapes(void) {
    puci_arena a;
    puci_arena_init(&a, NULL, 0);
    /* Smiley (U+1F600) encoded as a surrogate pair. */
    pj_val *root = NULL;
    const char *txt = "{\"emoji\":\"\\uD83D\\uDE00\",\"e\":\"\\u0041\"}";
    CHECK(pj_parse(&a, txt, strlen(txt), &root) == 0);
    const char *em = pj_str(pj_get(root, "emoji"));
    CHECK(em && strcmp(em, "\xF0\x9F\x98\x80") == 0);
    const char *e = pj_str(pj_get(root, "e"));
    CHECK(e && strcmp(e, "A") == 0);
    puci_arena_destroy(&a);
}

static void test_pget_path(void) {
    puci_arena a;
    puci_arena_init(&a, NULL, 0);
    pj_val *root = NULL;
    const char *txt = "{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}";
    CHECK(pj_parse(&a, txt, strlen(txt), &root) == 0);
    const char *path[] = {"choices", "0", "delta", "content"};
    pj_val *c = pj_get_path(root, path, 4);
    CHECK(c && strcmp(pj_str(c), "hi") == 0);
    const char *bad[] = {"choices", "5"};
    CHECK(pj_get_path(root, bad, 2) == NULL);
    puci_arena_destroy(&a);
}

static void test_malformed(void) {
    const char *bad_inputs[] = {
        "", "{", "[", "{\"a\":}", "{\"a\" 1}", "{a:1}", "null key", "[1,2,]",
        "\"unterminated", "{\"x\":\"truncated", "tru", "123abc", "{}}",
        "[][", "{\"a\":\"\\uZZZZ\"}", "{\"a\":\"\\uD83D\"}", // lone high surrogate
    };
    for (size_t i = 0; i < sizeof(bad_inputs) / sizeof(bad_inputs[0]); i++) {
        puci_arena a;
        puci_arena_init(&a, NULL, 0);
        pj_val *root = NULL;
        int rc = pj_parse(&a, bad_inputs[i], strlen(bad_inputs[i]), &root);
        CHECK(rc == -1);
        puci_arena_destroy(&a);
    }
}

static void test_whitespace(void) {
    puci_arena a;
    puci_arena_init(&a, NULL, 0);
    pj_val *root = NULL;
    const char *txt = "  \n\t [ 1 , 2 ]  \r\n ";
    CHECK(pj_parse(&a, txt, strlen(txt), &root) == 0);
    CHECK(root && root->type == PJ_ARR);
    CHECK(pj_arr_len(root) == 2);
    puci_arena_destroy(&a);
}

int main(void) {
    test_basic_object();
    test_array();
    test_escapes();
    test_unicode_escapes();
    test_pget_path();
    test_malformed();
    test_whitespace();
    TEST_END();
}
