/*
 * mcp/mcp_client.c - Model Context Protocol stdio client implementation.
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "puci/mcp.h"
#include "puci_internal.h"
#include "json/json.h"
#include "tools/path_guard.h"
#include "util/arena.h"
#include "util/strbuf.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct puci_mcp_client {
    char *name;
    char *command;
    pid_t pid;
    int in_fd;
    int out_fd;
    int req_id;
    puci_context *ctx;
    struct puci_mcp_client *next;
    struct puci_mcp_tool_info *tool_infos; /* owned list for cleanup */
} puci_mcp_client;

typedef struct puci_mcp_tool_info {
    char *tool_name;
    puci_mcp_client *client;
    struct puci_mcp_tool_info *next;
} puci_mcp_tool_info;

static void mcp_client_destroy(puci_mcp_client *c) {
    if (!c) return;
    /* Free the per-tool info records registered on the context. */
    puci_mcp_tool_info *ti = c->tool_infos;
    while (ti) {
        puci_mcp_tool_info *next = ti->next;
        free(ti->tool_name);
        free(ti);
        ti = next;
    }
    c->tool_infos = NULL;

    if (c->in_fd >= 0) {
        close(c->in_fd);
        c->in_fd = -1;
    }
    if (c->out_fd >= 0) {
        close(c->out_fd);
        c->out_fd = -1;
    }
    if (c->pid > 0) {
        (void)killpg(c->pid, SIGTERM);
        int ws = 0;
        int reaped = 0;
        for (int i = 0; i < 10; i++) {
            pid_t r = waitpid(c->pid, &ws, WNOHANG);
            if (r == c->pid || (r < 0 && errno == ECHILD)) {
                reaped = 1;
                break;
            }
            struct timespec ts = {0, 50000000L}; /* 50ms */
            nanosleep(&ts, NULL);
        }
        if (!reaped) {
            (void)killpg(c->pid, SIGKILL);
            waitpid(c->pid, &ws, 0);
        }
        c->pid = -1;
    }
    free(c->name);
    free(c->command);
    free(c);
}

void puci_mcp_clients_destroy(puci_context *ctx) {
    if (!ctx) return;
    puci_mcp_client *cur = (puci_mcp_client *)ctx->mcp_clients;
    while (cur) {
        puci_mcp_client *next = cur->next;
        mcp_client_destroy(cur);
        cur = next;
    }
    ctx->mcp_clients = NULL;
}

static int mcp_write_line(puci_mcp_client *c, const char *data) {
    if (!c || c->in_fd < 0 || !data) return -1;
    size_t len = strlen(data);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(c->in_fd, data + off, len - off);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    if (write(c->in_fd, "\n", 1) <= 0) return -1;
    return 0;
}

#define MCP_BUF_SIZE 4096

typedef struct puci_mcp_buf_reader {
    char buf[MCP_BUF_SIZE];
    size_t pos;
    size_t len;
    int eof;
} puci_mcp_buf_reader;

typedef enum mcp_read_result {
    MCP_READ_OK = 0,
    MCP_READ_CANCELLED = 1,
    MCP_READ_TIMEOUT = 2,
    MCP_READ_PROTOCOL = 3
} mcp_read_result;

static int mcp_read_line_ex(puci_mcp_client *c, const puci_cancel_token *cancel,
                            char **out_line, int timeout_ms,
                            mcp_read_result *out_reason) {
    if (out_reason) *out_reason = MCP_READ_PROTOCOL;
    *out_line = NULL;
    if (!c || c->out_fd < 0) return -1;

    puci_strbuf sb;
    puci_strbuf_init(&sb, NULL);
    puci_mcp_buf_reader br;
    memset(&br, 0, sizeof(br));

    long long start = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    start = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    for (;;) {
        if (cancel && cancel->flag && *cancel->flag) {
            puci_strbuf_destroy(&sb);
            if (out_reason) *out_reason = MCP_READ_CANCELLED;
            return -1;
        }

        long long now = 0;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        int rem = timeout_ms - (int)(now - start);
        if (rem <= 0 && timeout_ms > 0) {
            puci_strbuf_destroy(&sb);
            if (out_reason) *out_reason = MCP_READ_TIMEOUT;
            return -1;
        }

        if (br.pos < br.len) {
            char ch = br.buf[br.pos++];
            if (ch == '\n') {
                if (sb.len > 0 && sb.data[sb.len - 1] == '\r') {
                    sb.data[sb.len - 1] = '\0';
                    sb.len--;
                }
                break;
            }
            puci_strbuf_append_char(&sb, ch);
            if (sb.len > 4 * 1024 * 1024) {
                puci_strbuf_destroy(&sb);
                if (out_reason) *out_reason = MCP_READ_PROTOCOL;
                return -1;
            }
            continue;
        }

        if (br.eof && sb.len > 0) break;
        if (br.eof) {
            puci_strbuf_destroy(&sb);
            if (out_reason) *out_reason = MCP_READ_PROTOCOL;
            return -1;
        }

        struct pollfd pfd[2];
        pfd[0].fd = c->out_fd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        int nfds = 1;
        if (cancel && cancel->fd >= 0) {
            pfd[1].fd = cancel->fd;
            pfd[1].events = POLLIN;
            pfd[1].revents = 0;
            nfds = 2;
        }

        int prc = poll(pfd, (nfds_t)nfds, (rem > 0 && timeout_ms > 0) ? rem : 200);
        if (prc < 0) {
            if (errno == EINTR) continue;
            puci_strbuf_destroy(&sb);
            if (out_reason) *out_reason = MCP_READ_PROTOCOL;
            return -1;
        }
        if (prc == 0) {
            if (timeout_ms > 0) {
                puci_strbuf_destroy(&sb);
                if (out_reason) *out_reason = MCP_READ_TIMEOUT;
                return -1;
            }
            continue;
        }
        if (nfds > 1 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            puci_strbuf_destroy(&sb);
            if (out_reason) *out_reason = MCP_READ_CANCELLED;
            return -1;
        }
        if (pfd[0].revents & (POLLHUP | POLLERR)) {
            if (sb.len > 0) break;
            puci_strbuf_destroy(&sb);
            if (out_reason) *out_reason = MCP_READ_PROTOCOL;
            return -1;
        }
        if (pfd[0].revents & POLLIN) {
            ssize_t n = read(c->out_fd, br.buf, sizeof(br.buf));
            if (n <= 0) {
                if (n == 0) {
                    br.eof = 1;
                    if (sb.len > 0) break;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    continue;
                }
                if (sb.len > 0) break;
                puci_strbuf_destroy(&sb);
                if (out_reason) *out_reason = MCP_READ_PROTOCOL;
                return -1;
            }
            br.pos = 0;
            br.len = (size_t)n;
        }
    }

    *out_line = strdup(puci_strbuf_cstr(&sb));
    puci_strbuf_destroy(&sb);
    if (!*out_line) {
        if (out_reason) *out_reason = MCP_READ_PROTOCOL;
        return -1;
    }
    if (out_reason) *out_reason = MCP_READ_OK;
    return 0;
}

static int mcp_read_line(puci_mcp_client *c, const puci_cancel_token *cancel, char **out_line, int timeout_ms) {
    return mcp_read_line_ex(c, cancel, out_line, timeout_ms, NULL);
}

static int mcp_tool_invoke(void *userdata, const puci_cancel_token *cancel,
                           const char *args_json, char **out_result) {
    puci_mcp_tool_info *info = (puci_mcp_tool_info *)userdata;
    *out_result = NULL;
    if (!info || !info->client) {
        *out_result = strdup("{\"ok\":false,\"error\":\"MCP client unavailable\"}");
        return 1;
    }
    puci_mcp_client *c = info->client;
    int req_id = ++c->req_id;

    puci_strbuf req;
    puci_strbuf_init(&req, NULL);
    puci_strbuf_append_fmt(&req, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":{\"name\":", req_id);
    puci_strbuf_append_json_string(&req, info->tool_name);
    puci_strbuf_appendz(&req, ",\"arguments\":");
    if (args_json && *args_json) {
        puci_strbuf_appendz(&req, args_json);
    } else {
        puci_strbuf_appendz(&req, "{}");
    }
    puci_strbuf_appendz(&req, "}}");

    if (mcp_write_line(c, puci_strbuf_cstr(&req)) != 0) {
        puci_strbuf_destroy(&req);
        *out_result = strdup("{\"ok\":false,\"error\":\"failed to write to MCP server\"}");
        return 1;
    }
    puci_strbuf_destroy(&req);

    char *resp_line = NULL;
    mcp_read_result rreason = MCP_READ_PROTOCOL;
    if (mcp_read_line_ex(c, cancel, &resp_line, 60000, &rreason) != 0 || !resp_line) {
        if (rreason == MCP_READ_CANCELLED) {
            *out_result = strdup("{\"ok\":false,\"error\":\"cancelled\",\"code\":\"PUCI_ERR_CANCELLED\"}");
        } else if (rreason == MCP_READ_TIMEOUT) {
            *out_result = strdup("{\"ok\":false,\"error\":\"MCP server timeout\",\"code\":\"PUCI_ERR_TOOL_TIMEOUT\"}");
        } else {
            *out_result = strdup("{\"ok\":false,\"error\":\"MCP server response timeout or error\"}");
        }
        return 1;
    }

    puci_arena arena;
    puci_arena_init(&arena, NULL, 0);
    pj_val *root = NULL;
    if (pj_parse(&arena, resp_line, strlen(resp_line), &root) != 0 || !root || root->type != PJ_OBJ) {
        free(resp_line);
        puci_arena_destroy(&arena);
        *out_result = strdup("{\"ok\":false,\"error\":\"invalid JSON-RPC response from MCP server\"}");
        return 1;
    }

    pj_val *err_val = pj_get(root, "error");
    if (err_val) {
        pj_val *msg_v = pj_get(err_val, "message");
        const char *m = msg_v ? pj_str(msg_v) : "MCP execution error";
        puci_strbuf eb;
        puci_strbuf_init(&eb, NULL);
        puci_strbuf_appendz(&eb, "{\"ok\":false,\"error\":");
        puci_strbuf_append_json_string(&eb, m);
        puci_strbuf_appendz(&eb, "}");
        *out_result = strdup(puci_strbuf_cstr(&eb));
        puci_strbuf_destroy(&eb);
        free(resp_line);
        puci_arena_destroy(&arena);
        return 1;
    }

    pj_val *res_val = pj_get(root, "result");
    if (res_val) {
        pj_val *is_err = pj_get(res_val, "isError");
        int err_flag = (is_err && is_err->type == PJ_BOOL && is_err->u.bval);
        pj_val *content_val = pj_get(res_val, "content");
        puci_strbuf out_sb;
        puci_strbuf_init(&out_sb, NULL);

        if (content_val && content_val->type == PJ_ARR) {
            for (size_t i = 0; i < content_val->u.arr.len; i++) {
                pj_val *item = content_val->u.arr.items[i];
                if (item && item->type == PJ_OBJ) {
                    pj_val *tv = pj_get(item, "text");
                    if (tv && pj_str(tv)) {
                        puci_strbuf_appendz(&out_sb, pj_str(tv));
                    }
                }
            }
        }
        puci_strbuf final_res;
        puci_strbuf_init(&final_res, NULL);
        puci_strbuf_append_fmt(&final_res, "{\"ok\":%s,\"content\":", err_flag ? "false" : "true");
        puci_strbuf_append_json_string(&final_res, puci_strbuf_cstr(&out_sb));
        puci_strbuf_appendz(&final_res, "}");
        *out_result = strdup(puci_strbuf_cstr(&final_res));
        puci_strbuf_destroy(&final_res);
        puci_strbuf_destroy(&out_sb);
        free(resp_line);
        puci_arena_destroy(&arena);
        return err_flag ? 1 : 0;
    }

    *out_result = strdup("{\"ok\":true,\"content\":\"\"}");
    free(resp_line);
    puci_arena_destroy(&arena);
    return 0;
}

puci_status puci_mcp_server_register(puci_context *ctx, const puci_mcp_server_config *cfg) {
    if (!ctx || !cfg || !cfg->command || !cfg->command[0]) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (puci_path_is_protected(cfg->command)) {
        return PUCI_ERR_INVALID_ARG;
    }

    int p_in[2] = {-1, -1};
    int p_out[2] = {-1, -1};
    if (pipe(p_in) != 0 || pipe(p_out) != 0) {
        if (p_in[0] >= 0) { close(p_in[0]); close(p_in[1]); }
        if (p_out[0] >= 0) { close(p_out[0]); close(p_out[1]); }
        return PUCI_ERR_IO;
    }

    size_t nargs = cfg->argc;
    if (nargs == 0 && cfg->args) {
        while (cfg->args[nargs]) nargs++;
    }
    char **argv = (char **)calloc(nargs + 2, sizeof(char *));
    if (!argv) {
        close(p_in[0]); close(p_in[1]);
        close(p_out[0]); close(p_out[1]);
        return PUCI_ERR_NOMEM;
    }
    argv[0] = (char *)cfg->command;
    for (size_t i = 0; i < nargs; i++) {
        argv[i + 1] = (char *)cfg->args[i];
    }
    argv[nargs + 1] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        free(argv);
        close(p_in[0]); close(p_in[1]);
        close(p_out[0]); close(p_out[1]);
        return PUCI_ERR_IO;
    }

    if (pid == 0) {
        (void)setpgid(0, 0);
        close(p_in[1]);
        close(p_out[0]);
        if (dup2(p_in[0], STDIN_FILENO) < 0 || dup2(p_out[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(p_in[0]);
        close(p_out[1]);
        if (cfg->cwd && chdir(cfg->cwd) != 0) {
            _exit(126);
        }
        execvp(cfg->command, argv);
        _exit(127);
    }

    free(argv);
    close(p_in[0]);
    close(p_out[1]);
    (void)setpgid(pid, pid);

    puci_mcp_client *client = (puci_mcp_client *)calloc(1, sizeof(puci_mcp_client));
    if (!client) {
        (void)killpg(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(p_in[1]);
        close(p_out[0]);
        return PUCI_ERR_NOMEM;
    }
    client->name = strdup(cfg->name ? cfg->name : "mcp");
    client->command = strdup(cfg->command);
    client->pid = pid;
    client->in_fd = p_in[1];
    client->out_fd = p_out[0];
    client->req_id = 0;
    client->ctx = ctx;

    /* 1. Send initialize */
    const char *init_req =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"puci\",\"version\":\"0.2.0\"}}}";
    if (mcp_write_line(client, init_req) != 0) {
        mcp_client_destroy(client);
        return PUCI_ERR_PROTOCOL;
    }

    char *init_resp = NULL;
    if (mcp_read_line(client, NULL, &init_resp, 10000) != 0 || !init_resp) {
        mcp_client_destroy(client);
        return PUCI_ERR_PROTOCOL;
    }
    free(init_resp);

    /* 2. Send initialized notification */
    const char *notif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}";
    (void)mcp_write_line(client, notif);

    /* 3. Send tools/list */
    const char *list_req = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}";
    if (mcp_write_line(client, list_req) != 0) {
        mcp_client_destroy(client);
        return PUCI_ERR_PROTOCOL;
    }

    char *list_resp = NULL;
    if (mcp_read_line(client, NULL, &list_resp, 10000) != 0 || !list_resp) {
        mcp_client_destroy(client);
        return PUCI_ERR_PROTOCOL;
    }

    puci_arena arena;
    puci_arena_init(&arena, NULL, 0);
    pj_val *root = NULL;
    if (pj_parse(&arena, list_resp, strlen(list_resp), &root) != 0 || !root) {
        free(list_resp);
        puci_arena_destroy(&arena);
        mcp_client_destroy(client);
        return PUCI_ERR_PROTOCOL;
    }

    pj_val *res = pj_get(root, "result");
    pj_val *tools = res ? pj_get(res, "tools") : NULL;
    if (tools && tools->type == PJ_ARR) {
        for (size_t i = 0; i < tools->u.arr.len; i++) {
            pj_val *tv = tools->u.arr.items[i];
            if (!tv || tv->type != PJ_OBJ) continue;
            pj_val *nv = pj_get(tv, "name");
            pj_val *dv = pj_get(tv, "description");
            pj_val *sv = pj_get(tv, "inputSchema");
            const char *tname = nv ? pj_str(nv) : NULL;
            const char *tdesc = dv ? pj_str(dv) : "";
            if (!tname) continue;

            puci_strbuf schema_sb;
            puci_strbuf_init(&schema_sb, NULL);
            if (sv) {
                pj_serialize(sv, &schema_sb);
            } else {
                puci_strbuf_appendz(&schema_sb, "{\"type\":\"object\"}");
            }

            puci_tool_spec spec;
            memset(&spec, 0, sizeof(spec));
            spec.name = tname;
            spec.description = tdesc;
            spec.input_schema = puci_strbuf_cstr(&schema_sb);

            puci_mcp_tool_info *tinfo = (puci_mcp_tool_info *)calloc(1, sizeof(puci_mcp_tool_info));
            if (tinfo) {
                tinfo->tool_name = strdup(tname);
                tinfo->client = client;
                if (puci_tool_register(ctx, &spec, mcp_tool_invoke, tinfo) == PUCI_OK) {
                    tinfo->next = client->tool_infos;
                    client->tool_infos = tinfo;
                } else {
                    free(tinfo->tool_name);
                    free(tinfo);
                }
            }
            puci_strbuf_destroy(&schema_sb);
        }
    }

    free(list_resp);
    puci_arena_destroy(&arena);

    client->next = (puci_mcp_client *)ctx->mcp_clients;
    ctx->mcp_clients = client;
    return PUCI_OK;
}

static void trim_mcp(char *s) {
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void unquote_mcp(char *val) {
    trim_mcp(val);
    size_t len = strlen(val);
    if (len >= 2 && ((val[0] == '"' && val[len - 1] == '"') || (val[0] == '\'' && val[len - 1] == '\''))) {
        memmove(val, val + 1, len - 2);
        val[len - 2] = '\0';
    }
}

/* Parses a TOML array-of-strings value like ["--repository", "."] into a
 * malloc'd argv array (NULL-terminated) suitable for puci_mcp_server_config.
 * Returns the number of args, or -1 on malformed input. */
static int parse_mcp_args(const char *val, char ***out_argv) {
    *out_argv = NULL;
    if (!val || !val[0]) {
        return 0;
    }

    size_t cap = 4;
    size_t n = 0;
    char **argv = (char **)calloc(cap, sizeof(char *));
    if (!argv) {
        return -1;
    }

    const char *p = val;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '[') {
        free(argv);
        return -1;
    }
    p++;

    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        if (*p == ']' || *p == '\0') {
            break;
        }
        if (*p != '"' && *p != '\'') {
            free(argv);
            return -1;
        }
        char quote = *p++;
        const char *start = p;
        while (*p && *p != quote) {
            p++;
        }
        if (*p != quote) {
            free(argv);
            return -1;
        }
        size_t len = (size_t)(p - start);
        p++;

        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char **na = (char **)realloc(argv, ncap * sizeof(char *));
            if (!na) {
                free(argv);
                return -1;
            }
            argv = na;
            cap = ncap;
        }
        argv[n] = (char *)malloc(len + 1);
        if (!argv[n]) {
            free(argv);
            return -1;
        }
        memcpy(argv[n], start, len);
        argv[n][len] = '\0';
        n++;
    }

    argv[n] = NULL;
    *out_argv = argv;
    return (int)n;
}

static void free_mcp_args(char **argv, int n) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < n && argv[i]; i++) {
        free(argv[i]);
    }
    free(argv);
}

/* Registers the currently accumulated server section (if complete) and resets
 * the accumulation state. */
static int register_pending_server(puci_context *ctx, char *name, char *command,
                                   char *args_buf) {
    if (!name[0] || !command[0]) {
        return 0;
    }
    char **args = NULL;
    int nargs = parse_mcp_args(args_buf, &args);
    if (nargs < 0) {
        return 0;
    }

    puci_mcp_server_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = name;
    cfg.command = command;
    cfg.args = (const char *const *)args;
    cfg.argc = (size_t)nargs;

    int ok = (puci_mcp_server_register(ctx, &cfg) == PUCI_OK);
    free_mcp_args(args, nargs);
    return ok ? 1 : 0;
}

int puci_mcp_load_servers_from_file(puci_context *ctx, const char *config_path) {
    if (!ctx || !config_path) return -1;
    FILE *f = fopen(config_path, "r");
    if (!f) return 0;

    char line[1024];
    char current_server[1024] = "";
    char command[1024] = "";
    char args_buf[2048] = "";
    int in_server_section = 0;
    int loaded = 0;

    while (fgets(line, sizeof(line), f)) {
        trim_mcp(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[' && line[strlen(line) - 1] == ']') {
            /* If we were in a server section, register it */
            if (in_server_section) {
                loaded += register_pending_server(ctx, current_server, command,
                                                  args_buf);
            }
            in_server_section = 0;
            current_server[0] = '\0';
            command[0] = '\0';
            args_buf[0] = '\0';

            line[strlen(line) - 1] = '\0';
            const char *sec = line + 1;
            if (strncmp(sec, "mcp_servers.", 12) == 0) {
                in_server_section = 1;
                snprintf(current_server, sizeof(current_server), "%s", sec + 12);
            }
            continue;
        }

        if (in_server_section) {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *k = line;
                char *v = eq + 1;
                trim_mcp(k);
                unquote_mcp(v);
                if (strcmp(k, "command") == 0) {
                    snprintf(command, sizeof(command), "%s", v);
                } else if (strcmp(k, "args") == 0) {
                    snprintf(args_buf, sizeof(args_buf), "%s", v);
                }
            }
        }
    }

    if (in_server_section) {
        loaded += register_pending_server(ctx, current_server, command, args_buf);
    }

    fclose(f);
    return loaded;
}
