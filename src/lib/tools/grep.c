/*
 * tools/grep.c - fast file content search tool.
 *
 * Searches file contents across directory trees with line numbers.
 * Skips .git, build directories, and patterns listed in .gitignore.
 * Enforces hard match and byte caps with explicit truncation messages.
 */

#define _GNU_SOURCE
#include "puci_internal.h"
#include "tools/tools_internal.h"
#include "path_guard.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json/json.h"
#include "util/arena.h"
#include "util/strbuf.h"

#define MAX_GREP_MATCHES 100
#define MAX_GREP_BYTES (64 * 1024)

typedef struct gitignore_list {
    char **patterns;
    size_t count;
    size_t cap;
} gitignore_list;

static void load_gitignore(const char *root, gitignore_list *gi) {
    memset(gi, 0, sizeof(*gi));
    char gipath[1024];
    snprintf(gipath, sizeof(gipath), "%s/.gitignore", root);
    FILE *f = fopen(gipath, "r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        if (len > 1 && line[len - 1] == '/') {
            line[--len] = '\0';
        }
        if (gi->count == gi->cap) {
            size_t ncap = gi->cap ? gi->cap * 2 : 16;
            char **np = (char **)realloc(gi->patterns, ncap * sizeof(char *));
            if (!np) {
                break;
            }
            gi->patterns = np;
            gi->cap = ncap;
        }
        char *dup = strdup(line);
        if (!dup) break;
        gi->patterns[gi->count++] = dup;
    }
    fclose(f);
}

static void free_gitignore(gitignore_list *gi) {
    for (size_t i = 0; i < gi->count; i++) {
        free(gi->patterns[i]);
    }
    free(gi->patterns);
    memset(gi, 0, sizeof(*gi));
}

static int is_ignored(const char *name, const gitignore_list *gi) {
    if (strcmp(name, ".git") == 0 || strncmp(name, "build", 5) == 0) {
        return 1;
    }
    if (!gi) {
        return 0;
    }
    for (size_t i = 0; i < gi->count; i++) {
        const char *pat = gi->patterns[i];
        if (fnmatch(pat, name, FNM_PATHNAME) == 0 || fnmatch(pat, name, 0) == 0) {
            return 1;
        }
    }
    return 0;
}

typedef struct grep_ctx {
    const char *pattern;
    int case_sensitive;
    int max_matches;
    int total_matches;
    int truncated;
    puci_strbuf out;
    const gitignore_list *gi;
} grep_ctx;

static void grep_file(const char *filepath, grep_ctx *gc) {
    if (gc->total_matches >= gc->max_matches || gc->out.len >= MAX_GREP_BYTES) {
        gc->truncated = 1;
        return;
    }
    FILE *f = fopen(filepath, "r");
    if (!f) {
        return;
    }
    char line[4096];
    int line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        char *match = gc->case_sensitive ? strstr(line, gc->pattern)
                                         : strcasestr(line, gc->pattern);
        if (match) {
            gc->total_matches++;
            /* Strip trailing newline */
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
                line[--len] = '\0';
            }
            puci_strbuf_append_fmt(&gc->out, "%s:%d: %s\n", filepath, line_no, line);
            if (gc->total_matches >= gc->max_matches || gc->out.len >= MAX_GREP_BYTES) {
                gc->truncated = 1;
                break;
            }
        }
    }
    fclose(f);
}

static void grep_dir(const char *dirpath, grep_ctx *gc) {
    if (gc->truncated) {
        return;
    }
    DIR *d = opendir(dirpath);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (gc->truncated) {
            break;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (is_ignored(ent->d_name, gc->gi)) {
            continue;
        }
        char subpath[PATH_MAX];
        snprintf(subpath, sizeof(subpath), "%s/%s", strcmp(dirpath, ".") == 0 ? "." : dirpath, ent->d_name);
        const char *clean_path = (strncmp(subpath, "./", 2) == 0) ? subpath + 2 : subpath;

        struct stat st;
        if (lstat(clean_path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            grep_dir(clean_path, gc);
        } else if (S_ISREG(st.st_mode)) {
            grep_file(clean_path, gc);
        }
    }
    closedir(d);
}

static int invoke_grep(void *userdata, const puci_cancel_token *cancel,
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

    pj_val *pv = pj_get(root, "pattern");
    const char *pattern = pv ? pj_str(pv) : NULL;
    if (!pattern || !pattern[0]) {
        puci_arena_destroy(&arena);
        *out_result = strdup("{\"ok\":false,\"error\":\"missing required 'pattern'\"}");
        return 1;
    }

    pj_val *pathv = pj_get(root, "path");
    const char *search_path = pathv ? pj_str(pathv) : ".";
    if (!search_path || !search_path[0]) {
        search_path = ".";
    }

    pj_val *csv = pj_get(root, "case_sensitive");
    int case_sensitive = 1;
    if (csv && csv->type == PJ_BOOL) {
        case_sensitive = csv->u.bval;
    }

    pj_val *mv = pj_get(root, "max_matches");
    int max_matches = MAX_GREP_MATCHES;
    if (mv && mv->type == PJ_NUM && mv->u.num > 0) {
        max_matches = (int)mv->u.num;
        if (max_matches > 500) max_matches = 500;
    }

    gitignore_list gi;
    load_gitignore(search_path, &gi);

    grep_ctx gc;
    memset(&gc, 0, sizeof(gc));
    gc.pattern = pattern;
    gc.case_sensitive = case_sensitive;
    gc.max_matches = max_matches;
    gc.gi = &gi;
    puci_strbuf_init(&gc.out, NULL);

    struct stat st;
    if (stat(search_path, &st) == 0 && S_ISREG(st.st_mode)) {
        grep_file(search_path, &gc);
    } else {
        grep_dir(search_path, &gc);
    }

    if (gc.truncated) {
        puci_strbuf_appendz(&gc.out, "\n[search truncated: match or byte limit reached]\n");
    }

    puci_strbuf res;
    puci_strbuf_init(&res, NULL);
    puci_strbuf_append_fmt(&res, "{\"ok\":true,\"matches\":%d,\"truncated\":%s,\"content\":",
                           gc.total_matches, gc.truncated ? "true" : "false");
    puci_strbuf_append_json_string(&res, puci_strbuf_cstr(&gc.out));
    puci_strbuf_appendz(&res, "}");

    *out_result = strdup(puci_strbuf_cstr(&res));

    puci_strbuf_destroy(&res);
    puci_strbuf_destroy(&gc.out);
    free_gitignore(&gi);
    puci_arena_destroy(&arena);
    return 0;
}

puci_status puci_register_tool_grep(puci_context *ctx) {
    puci_tool_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = "grep";
    spec.description =
        "Search file contents across the codebase. Returns matching line numbers and text. "
        "Skips .git and build directories. Truncates gracefully with clear reporting on limits.";
    spec.input_schema =
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\","
        "\"description\":\"Text pattern to search for\"},\"path\":{\"type\":\"string\","
        "\"description\":\"Root directory or file to search (default: .)\"},"
        "\"case_sensitive\":{\"type\":\"boolean\",\"description\":\"Match case (default: true)\"},"
        "\"max_matches\":{\"type\":\"integer\",\"description\":\"Max match limit (default: 100)\"}},"
        "\"required\":[\"pattern\"]}";
    return puci_tool_register(ctx, &spec, invoke_grep, NULL);
}
