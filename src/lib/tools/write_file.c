/*
 * tools/write_file.c - built-in write_file tool.
 *
 * Writes a string to a file on disk (truncating). Used by the agent to create
 * or modify files.
 */

#include "puci_internal.h"
#include "tools/tools_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json/json.h"
#include "util/arena.h"
#include "util/strbuf.h"

/* Invoke callback: args_json = {"path": "...", "content": "..."}. Result is
 * allocated with malloc. */
static int invoke_write(void *userdata, const puci_cancel_token *cancel,
                        const char *args_json, char **out_result) {
    (void)cancel;
    (void)userdata;
    *out_result = NULL;

    puci_arena arena;
    puci_arena_init(&arena, NULL, 0);
    pj_val *root = NULL;
    if (pj_parse(&arena, args_json, strlen(args_json), &root) != 0) {
        puci_arena_destroy(&arena);
        *out_result = strdup("{\"ok\":false,\"error\":\"invalid arguments\"}");
        return 1;
    }
    pj_val *pv = pj_get(root, "path");
    pj_val *cv = pj_get(root, "content");
    const char *path = pv ? pj_str(pv) : NULL;
    const char *content = cv ? pj_str(cv) : NULL;
    if (!path || !content) {
        puci_arena_destroy(&arena);
        *out_result =
            strdup("{\"ok\":false,\"error\":\"missing 'path' or 'content'\"}");
        return 1;
    }
    size_t content_len = cv->u.str.len;
    if (strlen(content) != content_len) {
        /* Embedded NUL in content: discouraged; reject. */
        puci_arena_destroy(&arena);
        *out_result =
            strdup("{\"ok\":false,\"error\":\"content must not contain NUL\"}");
        return 1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        char ebuf[160];
        snprintf(ebuf, sizeof(ebuf),
                 "{\"ok\":false,\"error\":\"cannot open for write: %s\"}",
                 strerror(errno));
        puci_arena_destroy(&arena);
        *out_result = strdup(ebuf);
        return 1;
    }
    size_t written = fwrite(content, 1, content_len, f);
    fclose(f);

    puci_strbuf res;
    puci_strbuf_init(&res, NULL);
    if (written != content_len) {
        puci_strbuf_appendz(&res, "{\"ok\":false,\"error\":\"short write\"}");
    } else {
        puci_strbuf_append_fmt(&res, "{\"ok\":true,\"path\":");
        puci_strbuf_append_json_string(&res, path);
        puci_strbuf_append_fmt(&res, ",\"bytes\":%zu}", written);
    }
    *out_result = strdup(puci_strbuf_cstr(&res));
    puci_strbuf_destroy(&res);
    puci_arena_destroy(&arena);
    return 0;
}

puci_status puci_register_tool_write_file(puci_context *ctx) {
    puci_tool_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = "write_file";
    spec.description =
        "Create or overwrite a file with the given contents. The parent "
        "directory must already exist. Returns the number of bytes written.";
    spec.input_schema =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"Path of the file to write\"},\"content\":{\"type\":"
        "\"string\",\"description\":\"Full contents to write\"}},"
        "\"required\":[\"path\",\"content\"]}";
    return puci_tool_register(ctx, &spec, invoke_write, NULL);
}