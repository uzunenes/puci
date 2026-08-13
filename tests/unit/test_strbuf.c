/* test_strbuf.c - unit tests for the internal string builder. */
#include "util/strbuf.h"

#include <string.h>

#include "../test_util.h"

static void test_append(void) {
    puci_strbuf b;
    puci_strbuf_init(&b, NULL);
    CHECK(strcmp(puci_strbuf_cstr(&b), "") == 0);
    CHECK(puci_strbuf_appendz(&b, "hello") == PUCI_OK);
    CHECK(puci_strbuf_appendz(&b, " world") == PUCI_OK);
    CHECK(strcmp(puci_strbuf_cstr(&b), "hello world") == 0);
    CHECK(b.len == 11);
    puci_strbuf_destroy(&b);
}

static void test_char_and_fmt(void) {
    puci_strbuf b;
    puci_strbuf_init(&b, NULL);
    puci_strbuf_append_char(&b, 'x');
    puci_strbuf_append_fmt(&b, "-%d-%zu", 42, (size_t)7);
    CHECK(strcmp(puci_strbuf_cstr(&b), "x-42-7") == 0);
    puci_strbuf_destroy(&b);
}

static void test_json_string(void) {
    puci_strbuf b;
    puci_strbuf_init(&b, NULL);
    puci_strbuf_append_json_string(&b, "a\"b\\c\nd\te\ff");
    CHECK(strcmp(puci_strbuf_cstr(&b),
                 "\"a\\\"b\\\\c\\nd\\te\\ff\"") == 0);
    puci_strbuf_destroy(&b);
}

static void test_growth(void) {
    puci_strbuf b;
    puci_strbuf_init(&b, NULL);
    char big[4096];
    memset(big, 'a', sizeof(big));
    big[4095] = '\0';
    CHECK(puci_strbuf_appendz(&b, big) == PUCI_OK);
    CHECK(puci_strbuf_appendz(&b, "!") == PUCI_OK);
    CHECK(b.len == 4096);
    CHECK(puci_strbuf_cstr(&b)[4095] == '!');
    CHECK(puci_strbuf_cstr(&b)[4096] == '\0');
    puci_strbuf_destroy(&b);
}

int main(void) {
    test_append();
    test_char_and_fmt();
    test_json_string();
    test_growth();
    TEST_END();
}
