/*
 * tools/edit_file.c - surgical search-and-replace file editing tool.
 *
 * Replaces exact unique occurrences of old_str with new_str sequentially.
 * Guarantees atomic write (temporary file + fsync + rename) and preserves file mode.
 */

#include "puci_internal.h"
#include "tools/tools_internal.h"
#include "path_guard.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json/json.h"
#include "util/arena.h"
#include "util/strbuf.h"

#define MAX_EDIT_FILE_SIZE ((size_t)8 * 1024 * 1024)

static puci_status read_target_file(const char *path, char **out_buf, size_t *out_len) {
    *out_buf = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return (errno == ENOENT) ? PUCI_ERR_NOT_FOUND : PUCI_ERR_IO;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz > MAX_EDIT_FILE_SIZE) {
        fclose(f);
        return PUCI_ERR_LIMIT_EXCEEDED;
    }
    size_t len = (size_t)sz;
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        fclose(f);
        return PUCI_ERR_NOMEM;
    }
    size_t rd = fread(buf, 1, len, f);
    fclose(f);
    buf[rd] = '\0';
    *out_buf = buf;
    *out_len = rd;
    return PUCI_OK;
}

/* Replaces the single unique occurrence of old_str in *cur_str with new_str.
 * Returns 0 on success, 1 on not-found, 2 on multiple occurrences, -1 on nomem. */
static int replace_unique(char **cur_str, size_t *cur_len,
                          const char *old_str, size_t old_len,
                          const char *new_str, size_t new_len) {
    if (old_len == 0) {
        return 1;
    }
    char *first = strstr(*cur_str, old_str);
    if (!first) {
        return 1; /* not found */
    }
    char *second = strstr(first + 1, old_str);
    if (second) {
        return 2; /* ambiguous / multiple matches */
    }

    size_t prefix_len = (size_t)(first - *cur_str);
    size_t suffix_len = *cur_len - prefix_len - old_len;
    size_t next_len = prefix_len + new_len + suffix_len;
    char *next_buf = (char *)malloc(next_len + 1);
    if (!next_buf) {
        return -1;
    }

    memcpy(next_buf, *cur_str, prefix_len);
    if (new_len > 0) {
        memcpy(next_buf + prefix_len, new_str, new_len);
    }
    memcpy(next_buf + prefix_len + new_len, first + old_len, suffix_len);
    next_buf[next_len] = '\0';

    free(*cur_str);
    *cur_str = next_buf;
    *cur_len = next_len;
    return 0;
}

int puci_apply_file_edits(const char *original, size_t orig_len,
                          const char *args_json, char **out_new, size_t *out_new_len,
                          char *err_msg, size_t err_cap) {
    *out_new = NULL;
    *out_new_len = 0;

    puci_arena arena;
    puci_arena_init(&arena, NULL, 0);
    pj_val *root = NULL;
    if (pj_parse(&arena, args_json, strlen(args_json), &root) != 0 || !root) {
        puci_arena_destroy(&arena);
        if (err_msg && err_cap > 0) {
            snprintf(err_msg, err_cap, "invalid arguments JSON");
        }
        return -1;
    }

    char *cur = (char *)malloc(orig_len + 1);
    if (!cur) {
        puci_arena_destroy(&arena);
        return -1;
    }
    memcpy(cur, original, orig_len);
    cur[orig_len] = '\0';
    size_t cur_len = orig_len;

    int replacements = 0;

    pj_val *edits_val = pj_get(root, "edits");
    if (edits_val && edits_val->type == PJ_ARR) {
        for (size_t i = 0; i < edits_val->u.arr.len; i++) {
            pj_val *item = edits_val->u.arr.items[i];
            pj_val *ov = item ? pj_get(item, "old_str") : NULL;
            pj_val *nv = item ? pj_get(item, "new_str") : NULL;
            const char *old_s = ov ? pj_str(ov) : NULL;
            const char *new_s = nv ? pj_str(nv) : "";
            if (!old_s) {
                free(cur);
                puci_arena_destroy(&arena);
                if (err_msg && err_cap > 0) {
                    snprintf(err_msg, err_cap, "hunk %zu missing 'old_str'", i);
                }
                return -1;
            }
            int rc = replace_unique(&cur, &cur_len, old_s, ov->u.str.len,
                                    new_s, nv ? nv->u.str.len : 0);
            if (rc == 1) {
                free(cur);
                puci_arena_destroy(&arena);
                if (err_msg && err_cap > 0) {
                    snprintf(err_msg, err_cap, "hunk %zu: old_str not found in file", i);
                }
                return 1;
            } else if (rc == 2) {
                free(cur);
                puci_arena_destroy(&arena);
                if (err_msg && err_cap > 0) {
                    snprintf(err_msg, err_cap, "hunk %zu: old_str matches multiple times (ambiguous)", i);
                }
                return 2;
            } else if (rc != 0) {
                free(cur);
                puci_arena_destroy(&arena);
                return -1;
            }
            replacements++;
        }
    } else {
        pj_val *ov = pj_get(root, "old_str");
        pj_val *nv = pj_get(root, "new_str");
        const char *old_s = ov ? pj_str(ov) : NULL;
        const char *new_s = nv ? pj_str(nv) : "";
        if (!old_s) {
            free(cur);
            puci_arena_destroy(&arena);
            if (err_msg && err_cap > 0) {
                snprintf(err_msg, err_cap, "missing 'old_str' or 'edits'");
            }
            return -1;
        }
        int rc = replace_unique(&cur, &cur_len, old_s, ov->u.str.len,
                                new_s, nv ? nv->u.str.len : 0);
        if (rc == 1) {
            free(cur);
            puci_arena_destroy(&arena);
            if (err_msg && err_cap > 0) {
                snprintf(err_msg, err_cap, "old_str not found in file");
            }
            return 1;
        } else if (rc == 2) {
            free(cur);
            puci_arena_destroy(&arena);
            if (err_msg && err_cap > 0) {
                snprintf(err_msg, err_cap, "old_str matches multiple times (ambiguous)");
            }
            return 2;
        } else if (rc != 0) {
            free(cur);
            puci_arena_destroy(&arena);
            return -1;
        }
        replacements++;
    }

    puci_arena_destroy(&arena);
    *out_new = cur;
    *out_new_len = cur_len;
    return replacements;
}

static int invoke_edit(void *userdata, const puci_cancel_token *cancel,
                       const char *args_json, char **out_result) {
    (void)cancel;
    (void)userdata;
    *out_result = NULL;

    puci_arena arena;
    puci_arena_init(&arena, NULL, 0);
    pj_val *root = NULL;
    if (pj_parse(&arena, args_json, strlen(args_json), &root) != 0 || !root) {
        puci_arena_destroy(&arena);
        *out_result = strdup("{\"ok\":false,\"error\":\"invalid arguments JSON\"}");
        return 1;
    }
    pj_val *pv = pj_get(root, "path");
    const char *path = pv ? pj_str(pv) : NULL;
    if (!path) {
        puci_arena_destroy(&arena);
        *out_result = strdup("{\"ok\":false,\"error\":\"missing required 'path'\"}");
        return 1;
    }

    if (puci_path_is_protected(path)) {
        puci_arena_destroy(&arena);
        *out_result = strdup("{\"ok\":false,\"error\":\"write refused by path guard: protected path\"}");
        return 1;
    }

    char *orig = NULL;
    size_t orig_len = 0;
    puci_status rst = read_target_file(path, &orig, &orig_len);
    if (rst != PUCI_OK) {
        puci_arena_destroy(&arena);
        puci_strbuf eb;
        puci_strbuf_init(&eb, NULL);
        puci_strbuf_append_fmt(&eb, "{\"ok\":false,\"error\":\"cannot read file: %s\"}",
                               puci_status_string(rst));
        *out_result = strdup(puci_strbuf_cstr(&eb));
        puci_strbuf_destroy(&eb);
        return 1;
    }

    char err_msg[256];
    err_msg[0] = '\0';
    char *new_content = NULL;
    size_t new_len = 0;
    int nrepl = puci_apply_file_edits(orig, orig_len, args_json, &new_content,
                                      &new_len, err_msg, sizeof(err_msg));
    free(orig);

    if (nrepl <= 0 || !new_content) {
        puci_arena_destroy(&arena);
        puci_strbuf eb;
        puci_strbuf_init(&eb, NULL);
        puci_strbuf_appendz(&eb, "{\"ok\":false,\"error\":");
        puci_strbuf_append_json_string(&eb, err_msg[0] ? err_msg : "edit failed");
        puci_strbuf_appendz(&eb, "}");
        *out_result = strdup(puci_strbuf_cstr(&eb));
        puci_strbuf_destroy(&eb);
        return 1;
    }

    size_t written = 0;
    char write_err[256];
    write_err[0] = '\0';
    if (puci_atomic_write_file(path, new_content, new_len, &written,
                               write_err, sizeof(write_err)) != 0) {
        free(new_content);
        puci_arena_destroy(&arena);
        puci_strbuf eb;
        puci_strbuf_init(&eb, NULL);
        puci_strbuf_appendz(&eb, "{\"ok\":false,\"error\":");
        puci_strbuf_append_json_string(&eb, write_err[0] ? write_err : "atomic write failed");
        puci_strbuf_appendz(&eb, "}");
        *out_result = strdup(puci_strbuf_cstr(&eb));
        puci_strbuf_destroy(&eb);
        return 1;
    }
    free(new_content);

    puci_strbuf res;
    puci_strbuf_init(&res, NULL);
    puci_strbuf_append_fmt(&res, "{\"ok\":true,\"path\":");
    puci_strbuf_append_json_string(&res, path);
    puci_strbuf_append_fmt(&res, ",\"replacements\":%d,\"bytes\":%zu}", nrepl, written);
    *out_result = strdup(puci_strbuf_cstr(&res));
    puci_strbuf_destroy(&res);
    puci_arena_destroy(&arena);
    return 0;
}

puci_status puci_register_tool_edit_file(puci_context *ctx) {
    puci_tool_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = "edit_file";
    spec.description =
        "Apply surgical search-and-replace edits to a file. Each old_str must match "
        "exactly once in the target file. Writes atomically and preserves permissions.";
    spec.input_schema =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"Path of the file to edit\"},\"edits\":{\"type\":\"array\","
        "\"description\":\"List of {old_str, new_str} pairs applied in sequence\","
        "\"items\":{\"type\":\"object\",\"properties\":{\"old_str\":{\"type\":\"string\"},"
        "\"new_str\":{\"type\":\"string\"}},\"required\":[\"old_str\",\"new_str\"]}},"
        "\"old_str\":{\"type\":\"string\",\"description\":\"Single edit old_str\"},"
        "\"new_str\":{\"type\":\"string\",\"description\":\"Single edit new_str\"}},"
        "\"required\":[\"path\"]}";
    return puci_tool_register(ctx, &spec, invoke_edit, NULL);
}
