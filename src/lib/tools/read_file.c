/*
 * tools/read_file.c - built-in read_file tool.
 *
 * Reads a file from disk and returns its contents. Bounded read for safety on
 * embedded devices (no munlimited memory blowups from a giant file).
 */

#include "puci_internal.h"
#include "tools/tools_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json/json.h"
#include "util/arena.h"
#include "util/strbuf.h"

#define MAX_READ_BYTES ((size_t)8 * 1024 * 1024)

/* Invoke callback: args_json = {"path": "..."}. Result alloc'd with malloc. */
static int invoke_read(void *userdata, const char *args_json, char **out_result) {
    (void)userdata;
    *out_result = NULL;

    puci_arena arena;
    puci_arena_init(&arena, NULL, 0);
    pj_val *root = NULL;
    if (pj_parse(&arena, args_json, strlen(args_json), &root) != 0) {
        puci_arena_destroy(&arena);
        const char *msg = "{\"ok\":false,\"error\":\"invalid arguments JSON\"}";
        *out_result = strdup(msg);
        return 1;
    }
    pj_val *pv = pj_get(root, "path");
    const char *path = pv ? pj_str(pv) : NULL;
    if (!path) {
        puci_arena_destroy(&arena);
        *out_result =
            strdup("{\"ok\":false,\"error\":\"missing required 'path'\"}");
        return 1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        char ebuf[160];
        snprintf(ebuf, sizeof(ebuf),
                 "{\"ok\":false,\"error\":\"cannot open file: %s\"}",
                 strerror(errno));
        puci_arena_destroy(&arena);
        *out_result = strdup(ebuf);
        return 1;
    }

    puci_strbuf content;
    puci_strbuf_init(&content, NULL);
    char chunk[8192];
    size_t total = 0;
    int truncated = 0;
    while (total < MAX_READ_BYTES) {
        size_t want = sizeof(chunk);
        if (want > MAX_READ_BYTES - total) {
            want = MAX_READ_BYTES - total;
        }
        size_t got = fread(chunk, 1, want, f);
        if (got == 0) {
            break;
        }
        puci_strbuf_append(&content, chunk, got);
        total += got;
    }
    if (total >= MAX_READ_BYTES) {
        /* Probe whether more remains. */
        char probe;
        if (fread(&probe, 1, 1, f) > 0) {
            truncated = 1;
        }
    }
    fclose(f);

    puci_strbuf res;
    puci_strbuf_init(&res, NULL);
    puci_strbuf_append_fmt(&res, "{\"ok\":true,\"path\":");
    puci_strbuf_append_json_string(&res, path);
    puci_strbuf_append_fmt(&res, ",\"bytes\":%zu,\"truncated\":%s,\"content\":",
                           total, truncated ? "true" : "false");
    puci_strbuf_append_json_string(&res, puci_strbuf_cstr(&content));
    puci_strbuf_appendz(&res, "}");
    puci_strbuf_destroy(&content);

    *out_result = strdup(puci_strbuf_cstr(&res));
    puci_strbuf_destroy(&res);
    puci_arena_destroy(&arena);
    return 0;
}

/* Registration callback and JSON schema for this tool. */
puci_status puci_register_tool_read_file(puci_context *ctx) {
    puci_tool_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = "read_file";
    spec.description =
        "Read a file from disk and return its contents. Use for files that are "
        "small enough to read in one shot; prefer grep for searching. Returns "
        "the file bytes and a 'truncated' flag if the file exceeded the read cap.";
    spec.input_schema =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"Absolute or relative path of the file to read\"}},"
        "\"required\":[\"path\"]}";
    return puci_tool_register(ctx, &spec, invoke_read, NULL);
}