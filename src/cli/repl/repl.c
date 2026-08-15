/*
 * repl/repl.c - interactive REPL mode.
 *
 * `puci` with no subcommand enters an interactive loop: prompt, stream the
 * response, execute tools, prompt again. Offers /slash-commands and the
 * raw-mode line editor. Uses only the public libpuci API.
 */

#include "repl.h"
#include "lineedit.h"
#include "../cmd/config.h"
#include "../cmd/session_mgr.h"

#include <puci/puci.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SYSTEM_PROMPT                         \
    "You are puci, a fast C coding agent. Be concise. "\
    "Use the provided tools when they are the right way. "\
    "For run_shell, always pass an explicit argv array."

/* ---- Signal handling ------------------------------------------------------
 * libpuci never installs handlers; signal disposition is the CLI's job. The
 * SIGINT handler cancels the in-flight turn (async-signal-safe inside the
 * library), and a second Ctrl-C within 2s is a hard exit. SIGCONT re-applies
 * raw mode after suspend/resume. The terminal is restored on all exit paths. */

static volatile puci_session *g_cur_sess = NULL;
static volatile long long g_last_sigint_ms = -1;

static long long now_ms_ssafe(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_sigint(int sig) {
    (void)sig;
    long long now = now_ms_ssafe();
    if (g_last_sigint_ms >= 0 && (now - g_last_sigint_ms) < 2000) {
        /* Second Ctrl-C within 2s: hard exit. Restore the terminal best
         * effort then _exit — no graceful network teardown that could hang. */
        ledit_restore_global();
        _exit(130);
    }
    g_last_sigint_ms = now;
    if (g_cur_sess) {
        puci_session_cancel((puci_session *)g_cur_sess);
    }
}

static void on_sigcont(int sig) {
    (void)sig;
    /* On resume, the kernel restores the original termios; re-apply raw mode
     * if it was active when we were suspended. */
    ledit_resume_global();
}

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* restart interrupted blocking calls cleanly */
    sigaction(SIGINT, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigcont;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCONT, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, NULL);

    /* Restore the terminal on every normal exit path. The hard-exit path
     * (double Ctrl-C) restores it directly in the signal handler. */
    atexit(ledit_restore_global);
}

typedef struct repl {
    puci_context *ctx;
    puci_session *sess;
    ledit *ed;
    const char *system_prompt; /* owned only when allocated */
    int free_system_prompt;
    char session_id[256];
    char history_path[1024];
    int always_allow_turn;
    int plan_mode;
} repl;

static void print_colored_diff(FILE *out, const char *diff) {
    if (!diff) return;
    int is_tty = isatty(fileno(out));
    const char *p = diff;
    while (*p) {
        const char *next = strchr(p, '\n');
        size_t len = next ? (size_t)(next - p) : strlen(p);
        if (is_tty) {
            if (p[0] == '+' && (len < 3 || strncmp(p, "+++", 3) != 0)) {
                fprintf(out, "\033[32m%.*s\033[0m\n", (int)len, p);
            } else if (p[0] == '-' && (len < 3 || strncmp(p, "---", 3) != 0)) {
                fprintf(out, "\033[31m%.*s\033[0m\n", (int)len, p);
            } else if (p[0] == '@' && p[1] == '@') {
                fprintf(out, "\033[36m%.*s\033[0m\n", (int)len, p);
            } else {
                fprintf(out, "%.*s\n", (int)len, p);
            }
        } else {
            fprintf(out, "%.*s\n", (int)len, p);
        }
        if (!next) break;
        p = next + 1;
    }
}

static void on_event(void *ud, puci_session *sess, puci_event ev,
                     const void *ctx) {
    (void)ud;
    (void)sess;
    (void)ctx;
    switch (ev.kind) {
    case PUCI_EVENT_TOKEN_DELTA: {
        if (ev.text) {
            fputs(ev.text, stdout);
            fflush(stdout);
        }
        break;
    }
    case PUCI_EVENT_TOOL_CALL_START: {
        const puci_tool_call *tc = (const puci_tool_call *)ctx;
        if (tc) {
            fprintf(stderr, "\n[tool] %s\n", tc->name);
        }
        break;
    }
    case PUCI_EVENT_TURN_COMPLETE:
        fputs("\n", stdout);
        fflush(stdout);
        break;
    case PUCI_EVENT_TURN_CANCELLED:
        fputs("\n[cancelled]\n", stderr);
        fflush(stderr);
        break;
    default:
        break;
    }
}

/* Interactive approval policy: show the exact action (parsed argv or unified
 * diff) and ask. Non-interactive stdin auto-allows so piped use still works. */
static puci_approval policy(const puci_approval_request *req, void *ud) {
    repl *r = (repl *)ud;
    if (r && r->always_allow_turn) {
        return PUCI_APPROVAL_ALLOW;
    }

    fprintf(stderr, "\n[approve] %s\n", req->tool);
    if (req->diff) {
        print_colored_diff(stderr, req->diff);
    } else if (req->display && req->display[0]) {
        fprintf(stderr, "%s\n", req->display);
    }

    if (!isatty(STDIN_FILENO)) {
        /* Non-interactive: do not block; allow. */
        return PUCI_APPROVAL_ALLOW;
    }

    for (;;) {
        fprintf(stderr, "Allow? [y]es, [n]o (with feedback), [a]lways (for turn), [q]uit: ");
        fflush(stderr);
        char b[256];
        if (!fgets(b, sizeof(b), stdin)) {
            return PUCI_APPROVAL_DENY;
        }
        char choice = b[0];
        if (choice == 'y' || choice == 'Y') {
            return PUCI_APPROVAL_ALLOW;
        }
        if (choice == 'a' || choice == 'A') {
            if (r) r->always_allow_turn = 1;
            return PUCI_APPROVAL_ALLOW;
        }
        if (choice == 'q' || choice == 'Q') {
            if (r && r->sess) {
                puci_session_cancel(r->sess);
            }
            return PUCI_APPROVAL_DENY;
        }
        if (choice == 'n' || choice == 'N') {
            fprintf(stderr, "Feedback / reason for agent: ");
            fflush(stderr);
            char fb[1024];
            if (fgets(fb, sizeof(fb), stdin)) {
                size_t flen = strlen(fb);
                while (flen > 0 && (fb[flen - 1] == '\r' || fb[flen - 1] == '\n')) {
                    fb[--flen] = '\0';
                }
                if (flen > 0 && r && r->sess) {
                    puci_session_set_approval_feedback(r->sess, fb);
                }
            }
            return PUCI_APPROVAL_DENY;
        }
    }
}

/* ---- slash-command dispatch ---- */

static void cmd_help(repl *r, const char *arg);
static void cmd_model(repl *r, const char *arg);
static void cmd_provider(repl *r, const char *arg);
static void cmd_clear(repl *r, const char *arg);
static void cmd_reset(repl *r, const char *arg);
static void cmd_tools(repl *r, const char *arg);
static void cmd_config(repl *r, const char *arg);
static void cmd_cost(repl *r, const char *arg);
static void cmd_plan(repl *r, const char *arg);
static void cmd_session(repl *r, const char *arg);
static void cmd_quit(repl *r, const char *arg);

typedef struct slash {
    const char *name;
    const char *desc;
    void (*fn)(repl *, const char *);
} slash;

static const slash g_slash[] = {
    {"help", "show this help", cmd_help},
    {"model", "show or set the model (/model [name])", cmd_model},
    {"provider", "show current provider", cmd_provider},
    {"clear", "start a fresh conversation", cmd_clear},
    {"reset", "reset the session", cmd_reset},
    {"tools", "list enabled tools", cmd_tools},
    {"config", "show provider configuration", cmd_config},
    {"cost", "show token usage and cost", cmd_cost},
    {"plan", "toggle plan / dry-run mode", cmd_plan},
    {"session", "show or set session id (/session [name])", cmd_session},
    {"quit", "exit the REPL", cmd_quit},
    {"exit", "exit the REPL", cmd_quit},
};
#define N_SLASH ((size_t)(sizeof(g_slash) / sizeof(g_slash[0])))

static void cmd_help(repl *r, const char *arg) {
    (void)r;
    (void)arg;
    printf("Slash commands:\n");
    for (size_t i = 0; i < N_SLASH; i++) {
        printf("  /%-9s %s\n", g_slash[i].name, g_slash[i].desc);
    }
    printf("\nType a message to talk to the model, or /quit to exit.\n");
}

static void cmd_model(repl *r, const char *arg) {
    if (arg && *arg) {
        puci_status st = puci_session_set_model(r->sess, arg);
        printf("%s\n", st == PUCI_OK ? "model set" : "failed to set model");
    } else {
        printf("usage: /model <name>\n");
    }
}

static void cmd_provider(repl *r, const char *arg) {
    (void)arg;
    puci_provider_config *pc = puci_cli_provider("openai");
    if (pc) {
        printf("provider: %s\n  base_url: %s\n",
               pc->name, pc->base_url ? pc->base_url : "(default)");
    }
    (void)r;
}

static void cmd_clear(repl *r, const char *arg) {
    (void)arg;
    puci_session_clear(r->sess);
    printf("conversation cleared\n");
}

static void recreate_session(repl *r) {
    if (r->sess) {
        puci_session_destroy(r->sess);
        r->sess = NULL;
    }
    puci_provider_config *pc = puci_cli_provider("openai");
    puci_session_options op = {0};
    op.provider = *pc;
    op.event_cb = on_event;
    op.system_prompt = r->system_prompt;
    op.approve_cb = policy;
    op.approve_userdata = r;
    op.plan_mode = r->plan_mode;
    op.tool_timeout_ms = 60000;
    if (puci_session_create(r->ctx, &op, &r->sess) != PUCI_OK) {
        fprintf(stderr, "puci: failed to create session\n");
    }
}

static void cmd_reset(repl *r, const char *arg) {
    (void)arg;
    recreate_session(r);
    printf("session reset\n");
}

static void cmd_tools(repl *r, const char *arg) {
    (void)arg;
    size_t count = 0;
    const char *const *tools = puci_session_tools(r->sess, &count);
    printf("tools (%zu):\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  %s\n", tools[i]);
    }
}

static void cmd_config(repl *r, const char *arg) {
    (void)arg;
    puci_provider_config *pc = puci_cli_provider("openai");
    printf("config:\n");
    printf("  name:       %s\n", pc->name);
    printf("  base_url:   %s\n", pc->base_url ? pc->base_url : "(default)");
    printf("  api_key:    %s\n", pc->api_key ? "<set>" : "<missing>");
    printf("  tls_verify: %s\n", pc->tls_verify ? "on" : "off");
    printf("  idle_timeout_ms: %d\n", (int)pc->stream_idle_timeout_ms);
    (void)r;
}

static double price_per_token(const char *envname) {
    const char *v = getenv(envname);
    if (!v || !*v) {
        return -1.0;
    }
    char *end = NULL;
    double p = strtod(v, &end);
    if (end == v || p < 0.0) {
        return -1.0;
    }
    return p / 1000.0;
}

static void cmd_cost(repl *r, const char *arg) {
    (void)arg;
    long long p = 0, c = 0, cached = 0;
    puci_session_get_usage(r->sess, &p, &c, &cached);
    printf("tokens: %lld prompt, %lld cached, %lld completion\n",
           (long long)p, (long long)cached, (long long)c);
    double pp = price_per_token("PUCI_PRICE_PER_1K_PROMPT");
    double cp = price_per_token("PUCI_PRICE_PER_1K_COMPLETION");
    if (pp >= 0.0 && cp >= 0.0) {
        double kp = price_per_token("PUCI_PRICE_PER_1K_CACHED_PROMPT");
        if (kp < 0.0) {
            kp = pp;
        }
        double cost = (double)p * pp + (double)c * cp + (double)cached * kp;
        printf("cost: $%.6f\n", cost);
    } else {
        printf("cost: (no price table; set PUCI_PRICE_PER_1K_PROMPT/COMPLETION)\n");
    }
}

static void cmd_plan(repl *r, const char *arg) {
    (void)arg;
    int cur = puci_session_get_plan_mode(r->sess);
    puci_session_set_plan_mode(r->sess, !cur);
    r->plan_mode = !cur;
    printf("plan mode: %s\n", !cur ? "enabled (read-only dry-run)" : "disabled");
}

static void cmd_session(repl *r, const char *arg) {
    if (arg && *arg) {
        snprintf(r->session_id, sizeof(r->session_id), "%s", arg);
        printf("session id set to: %s\n", r->session_id);
    } else {
        printf("current session: %s\n", r->session_id[0] ? r->session_id : "(unnamed)");
    }
}

static void cmd_quit(repl *r, const char *arg) {
    (void)r;
    (void)arg;
    printf("bye\n");
}

static int dispatch_slash(repl *r, const char *line) {
    const char *name = line + 1;
    const char *arg = NULL;
    size_t namelen = 0;
    for (const char *p = name; *p && *p != ' ' && *p != '\t'; p++) {
        namelen++;
    }
    arg = name + namelen;
    while (*arg == ' ' || *arg == '\t') {
        arg++;
    }

    const slash *match = NULL;
    int nmatches = 0;
    for (size_t i = 0; i < N_SLASH; i++) {
        if (strncmp(g_slash[i].name, name, namelen) == 0) {
            nmatches++;
            match = &g_slash[i];
        }
    }
    if (nmatches == 1 && match) {
        match->fn(r, arg);
        return (strcmp(match->name, "quit") == 0 ||
                strcmp(match->name, "exit") == 0);
    }
    if (nmatches > 1) {
        printf("ambiguous '/%.*s':", (int)namelen, name);
        for (size_t i = 0; i < N_SLASH; i++) {
            if (strncmp(g_slash[i].name, name, namelen) == 0) {
                printf(" %s", g_slash[i].name);
            }
        }
        printf("\n");
    } else {
        printf("unknown '/%.*s'; try /help\n", (int)namelen, name);
    }
    return 0;
}

/* ---- project context detection (2.5) ---- */

static int has_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static const char *detect_project_type(void) {
    if (has_file("Cargo.toml")) return "rust";
    if (has_file("package.json")) return "node";
    if (has_file("go.mod")) return "go";
    if (has_file("pyproject.toml")) return "python";
    if (has_file("meson.build")) return "meson";
    if (has_file("CMakeLists.txt")) return "cmake";
    if (has_file("Makefile")) return "make";
    if (has_file("pom.xml")) return "java";
    return NULL;
}

static void read_cmd(FILE *f, char *out, size_t cap) {
    out[0] = '\0';
    size_t n = fread(out, 1, cap - 1, f);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
        out[--n] = '\0';
    }
    out[n] = '\0';
}

static char *build_project_context(char *branch, size_t branch_cap,
                                   char *listing, size_t listing_cap) {
    /* git branch (presentation only; no secret data). */
    FILE *f = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
    if (f) {
        read_cmd(f, branch, branch_cap);
        (void)pclose(f);
    }
    /* shallow listing */
    listing[0] = '\0';
    f = popen("find . -maxdepth 1 -not -path './.git' -not -path './.git/*' "
              "2>/dev/null | head -24",
              "r");
    if (f) {
        read_cmd(f, listing, listing_cap);
        (void)pclose(f);
    }

    char *out = (char *)malloc(1024);
    if (!out) {
        return NULL;
    }
    snprintf(out, 1024,
             "\n\n[project context]\n  branch: %s\n  type: %s\n%s",
             branch[0] ? branch : "(not a git repo)",
             detect_project_type() ? detect_project_type() : "(unknown)",
             listing);
    return out;
}

/* ---- main loop ---- */

int puci_cli_repl_with_params(const puci_cli_params *params) {
    puci_context *ctx = NULL;
    if (puci_context_create(NULL, &ctx) != PUCI_OK) {
        fprintf(stderr, "puci: failed to create context\n");
        return 1;
    }
    puci_provider_config *pc = puci_cli_provider("openai");
    if (puci_context_set_provider(ctx, pc) != PUCI_OK) {
        fprintf(stderr, "puci: failed to set provider\n");
        puci_context_destroy(ctx);
        return 1;
    }

    /* Connect any MCP servers declared in config.toml / .puci.toml. */
    (void)puci_cli_load_mcp_servers(ctx);

    repl r;
    memset(&r, 0, sizeof(r));
    r.ctx = ctx;
    r.ed = ledit_new(200);
    r.system_prompt = DEFAULT_SYSTEM_PROMPT;
    r.free_system_prompt = 0;
    r.plan_mode = params ? params->plan_mode : 0;

    puci_cli_get_history_file(r.history_path, sizeof(r.history_path));
    if (r.history_path[0]) {
        ledit_history_load(r.ed, r.history_path);
    }

    if (params && params->session_id && params->session_id[0]) {
        snprintf(r.session_id, sizeof(r.session_id), "%s", params->session_id);
    } else {
        snprintf(r.session_id, sizeof(r.session_id), "session_%lld", (long long)time(NULL));
    }

    /* Optional project context appended to the system prompt. */
    {
        char branch[256];
        char listing[1024];
        char *pctx = build_project_context(branch, sizeof(branch), listing,
                                           sizeof(listing));
        if (pctx) {
            size_t base = strlen(DEFAULT_SYSTEM_PROMPT);
            size_t add = strlen(pctx);
            char *full = (char *)malloc(base + add + 1);
            if (full) {
                memcpy(full, DEFAULT_SYSTEM_PROMPT, base);
                memcpy(full + base, pctx, add + 1);
                r.system_prompt = full;
                r.free_system_prompt = 1;
            }
            free(pctx);
        }
    }

    install_signal_handlers();
    recreate_session(&r);

    if (params && params->resume_id) {
        char loaded[256] = "";
        if (puci_cli_load_session(r.sess, params->resume_id, loaded, sizeof(loaded)) == 0) {
            printf("[resumed session: %s]\n", loaded);
            snprintf(r.session_id, sizeof(r.session_id), "%s", loaded);
        } else {
            fprintf(stderr, "puci: could not resume session '%s'\n", params->resume_id);
        }
    } else if (params && params->session_id) {
        char loaded[256] = "";
        if (puci_cli_load_session(r.sess, params->session_id, loaded, sizeof(loaded)) == 0) {
            printf("[resumed session: %s]\n", loaded);
        }
    }

    printf("puci REPL. /help for commands, /quit to exit.\n");
    if (isatty(STDOUT_FILENO)) {
        printf("(project: %s, session: %s%s)\n",
               detect_project_type() ? detect_project_type() : "none",
               r.session_id,
               r.plan_mode ? ", plan-mode" : "");
    }

    int quit = 0;
    while (!quit) {
        char *line = NULL;
        int rc = ledit_readline(r.ed, "puci> ", &line);
        if (rc == LEDIT_EOF) {
            printf("\n");
            break;
        }
        if (rc == LEDIT_INTERRUPT) {
            continue;
        }
        if (!line) {
            break;
        }
        if (line[0] == '\0') {
            free(line);
            continue;
        }

        if (line[0] == '/') {
            quit = dispatch_slash(&r, line);
            ledit_history_add(r.ed, line);
            if (r.history_path[0]) {
                ledit_history_save(r.ed, r.history_path);
            }
            free(line);
            continue;
        }

        ledit_history_add(r.ed, line);
        if (r.history_path[0]) {
            ledit_history_save(r.ed, r.history_path);
        }

        puci_provider_config *cur_pc = puci_cli_provider("openai");
        if (!cur_pc || !cur_pc->api_key || !cur_pc->api_key[0]) {
            fprintf(stderr, "puci: missing API key. Set PUCI_API_KEY environment variable or configure 'api_key' in ~/.config/puci/config.toml\n");
            free(line);
            continue;
        }

        r.always_allow_turn = 0;

        size_t est_chars = 0, est_tokens = 0;
        puci_session_estimate_context(r.sess, &est_chars, &est_tokens);
        if (est_tokens > 50000) {
            fprintf(stderr, "[notice] transcript context size: ~%zu tokens, nearing budget\n", est_tokens);
        }

        g_cur_sess = r.sess;
        puci_status st = puci_session_send(r.sess, line);
        g_cur_sess = NULL;

        if (st != PUCI_OK) {
            if (st != PUCI_ERR_CANCELLED) {
                fprintf(stderr, "puci: %s\n", puci_status_string(st));
                const char *err = puci_session_error(r.sess);
                if (err) {
                    fprintf(stderr, "  %s\n", err);
                }
            }
        } else {
            puci_cli_save_session(r.sess, r.session_id);
        }
        free(line);
    }

    if (r.ed) {
        if (r.history_path[0]) {
            ledit_history_save(r.ed, r.history_path);
        }
        ledit_free(r.ed);
    }
    if (r.sess) {
        puci_cli_save_session(r.sess, r.session_id);
        puci_session_destroy(r.sess);
    }
    if (r.free_system_prompt) {
        free((void *)r.system_prompt);
    }
    puci_context_destroy(ctx);
    signal(SIGINT, SIG_DFL);
    return 0;
}

int puci_cli_repl(void) {
    return puci_cli_repl_with_params(NULL);
}
