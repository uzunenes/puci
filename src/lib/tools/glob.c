/*
 * tools/glob.c - file pattern discovery tool.
 *
 * Discovers file paths matching glob patterns, skipping .git and build dirs.
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

#define MAX_GLOB_RESULTS 100

typedef struct glob_ctx {
    const char *pattern;
    int max_results;
    int total_results;
    int truncated;
    puci_strbuf out;
} glob_ctx;

static int is_ignored_dir(const char *name) {
    if (strcmp(name, ".git") == 0 || strncmp(name, "build", 5) == 0) {
        return 1;
    }
    return 0;
}

static void walk_glob(const char *dirpath, glob_ctx *gc) {
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
        if (is_ignored_dir(ent->d_name)) {
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
            walk_glob(clean_path, gc);
        } else {
            if (fnmatch(gc->pattern, clean_path, FNM_PATHNAME) == 0 ||
                fnmatch(gc->pattern, ent->d_name, 0) == 0) {
                if (gc->total_results > 0) {
                    puci_strbuf_appendz(&gc->out, "\n");
                }
                puci_strbuf_appendz(&gc->out, clean_path);
                gc->total_results++;
                if (gc->total_results >= gc->max_results) {
                    gc->truncated = 1;
                    break;
                }
            }
        }
    }
    closedir(d);
}

static int invoke_glob(void *userdata, const puci_cancel_token *cancel,
                       const char *args_json, char **out_result) {
    (void)cancel;
    (void)userdata;
    *out_result = NULL;

    puci_arena arena;
    puci_arena_init(&arena, NULL, 0);
    pj_val *root = NULL;
    if (args_json && strlen(args_json) > 0) {
        (void)pj_parse(&arena, args_json, strlen(args_json), &root);
    }

    const char *pattern = "*";
    const char *search_path = ".";
    int max_results = MAX_GLOB_RESULTS;

    if (root) {
        pj_val *pv = pj_get(root, "pattern");
        if (pv && pj_str(pv)) {
            pattern = pj_str(pv);
        }
        pj_val *pathv = pj_get(root, "path");
        if (pathv && pj_str(pathv)) {
            search_path = pj_str(pathv);
        }
        pj_val *mv = pj_get(root, "max_results");
        if (mv && mv->type == PJ_NUM && mv->u.num > 0) {
            max_results = (int)mv->u.num;
            if (max_results > 500) max_results = 500;
        }
    }

    glob_ctx gc;
    memset(&gc, 0, sizeof(gc));
    gc.pattern = pattern;
    gc.max_results = max_results;
    puci_strbuf_init(&gc.out, NULL);

    walk_glob(search_path, &gc);

    if (gc.truncated) {
        puci_strbuf_appendz(&gc.out, "\n[glob truncated: max results limit reached]");
    }

    puci_strbuf res;
    puci_strbuf_init(&res, NULL);
    puci_strbuf_append_fmt(&res, "{\"ok\":true,\"count\":%d,\"truncated\":%s,\"files\":",
                           gc.total_results, gc.truncated ? "true" : "false");
    puci_strbuf_append_json_string(&res, puci_strbuf_cstr(&gc.out));
    puci_strbuf_appendz(&res, "}");

    *out_result = strdup(puci_strbuf_cstr(&res));

    puci_strbuf_destroy(&res);
    puci_strbuf_destroy(&gc.out);
    puci_arena_destroy(&arena);
    return 0;
}

puci_status puci_register_tool_glob(puci_context *ctx) {
    puci_tool_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = "glob";
    spec.description =
        "Discover files matching a name pattern. Skips .git and build directories.";
    spec.input_schema =
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\","
        "\"description\":\"Glob pattern to match files against (default: *)\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"Root directory (default: .)\"},"
        "\"max_results\":{\"type\":\"integer\",\"description\":\"Max file limit (default: 100)\"}}}";
    return puci_tool_register(ctx, &spec, invoke_glob, NULL);
}
