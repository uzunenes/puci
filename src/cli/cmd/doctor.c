/*
 * cmd/doctor.c - `puci doctor` diagnostics.
 *
 * Runs environment / credential / reachability / TLS / tool / terminal checks
 * and prints actionable output. Every failure line says what to do next, so a
 * user who runs it first on a broken setup gets a fix, not a status dump.
 */

#include "cmd.h"
#include "config.h"

#include <puci/puci.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Track whether any check failed so the command can return a nonzero exit code
 * (0 all pass, 1 at least one problem). */
static int g_fail = 0;

/* Uniform line prefix for each status class, so a user scanning output can
 * separate "did it pass", "what broke", and "what to do next". */
#define HEAD(label) printf("\n[%s]\n", label)
#define OK    "  OK   "
#define FAIL  "  FAIL "
#define NOTE  "       "

static void vline(const char *prefix, const char *fmt, va_list ap) {
    fputs(prefix, stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
}

static void ok(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vline(OK, fmt, ap);
    va_end(ap);
}

static void fail(const char *fmt, ...) {
    g_fail = 1;
    va_list ap;
    va_start(ap, fmt);
    vline(FAIL, fmt, ap);
    va_end(ap);
}

static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vline(NOTE, fmt, ap);
    va_end(ap);
}

static const char *isatty_str(int fd) {
    return isatty(fd) ? "tty" : "piped";
}

int puci_cli_doctor(void) {
    printf("puci doctor\n");
    printf("  version: %s (ABI v%d)\n", puci_version_string(),
           puci_abi_version());

    /* Build facts: what features this binary was compiled with and how it
     * links libpuci. These macros are injected by meson.build; they are the
     * first thing to ask for in any embedder bug report. */
    HEAD("build");
    printf("  ABI version      : %d\n", puci_abi_version());
#if PUCI_FEATURE_TUI
    printf("  features         : tui");
#else
    printf("  features         : (none)");
#endif
#if PUCI_FEATURE_MEMORY
    printf(" memory");
#endif
#if PUCI_FEATURE_SWARM
    printf(" swarm");
#endif
#if PUCI_FEATURE_BROWSER
    printf(" browser");
#endif
#if PUCI_FEATURE_MCP
    printf(" mcp");
#endif
#if PUCI_FEATURE_LTO
    printf(" lto");
#endif
    printf("\n");
#if PUCI_CLI_LINK_SHARED
    printf("  libpuci link     : shared (%s)\n", "libpuci.so");
#else
    printf("  libpuci link     : static (%s)\n", "libpuci.a");
#endif

    HEAD("config");
    puci_provider_config *pc = puci_cli_provider("openai");
    printf("  name:     %s\n", pc->name);
    printf("  base_url: %s\n", pc->base_url ? pc->base_url : "(default)");
    printf("  model:    %s\n", pc->model ? pc->model : "(unset)");
    if (pc->base_url) {
        ok("base_url set");
    } else {
        fail("no base_url configured");
        note("set PUCI_BASE_URL, e.g. export PUCI_BASE_URL=https://api.openai.com/v1");
    }

    HEAD("credentials");
    if (pc->api_key && *pc->api_key) {
        const char *src = getenv("PUCI_API_KEY") ? "PUCI_API_KEY" : (getenv("OPENROUTER_API_KEY") ? "OPENROUTER_API_KEY (fallback)" : "config");
        ok("API key present via %s", src);
    } else {
        note("no PUCI_API_KEY nor OPENROUTER_API_KEY set; this is fine for local endpoints that need none.");
        note("for OpenRouter BYOK: export OPENROUTER_API_KEY=sk-or-v1-... (PUCI_API_KEY fallback, base https://openrouter.ai/api/v1)");
    }

    HEAD("TLS");
    if (pc->tls_verify) {
        ok("TLS verification ON");
    } else {
        fail("TLS verification is disabled for this provider");
        note("do NOT disable verification casually (MITM risk). If you must trust an "
             "internal CA, set PUCI_CA_BUNDLE to a PEM file instead.");
    }

    HEAD("tools");
    {
        puci_context *ctx = NULL;
        if (puci_context_create(NULL, &ctx) != PUCI_OK) {
            fail("failed to create context (allocator?)");
        } else {
            puci_status sp = puci_context_set_provider(ctx, pc);
            if (sp != PUCI_OK) {
                fail("set_provider failed: %s", puci_status_string(sp));
            } else {
                puci_session_options op = {0};
                op.provider = *pc;
                puci_session *sess = NULL;
                if (puci_session_create(ctx, &op, &sess) != PUCI_OK) {
                    fail("session create failed");
                } else {
                    size_t n = 0;
                    const char *const *names = puci_session_tools(sess, &n);
                    printf("  %zu tool(s) loaded:\n", n);
                    for (size_t i = 0; i < n; i++) {
                        printf("    - %s\n", names[i]);
                    }
                    puci_session_destroy(sess);
                }
            }
            puci_context_destroy(ctx);
        }
    }

    HEAD("terminal");
    printf("  stdin : %s\n", isatty_str(STDIN_FILENO));
    printf("  stdout: %s\n", isatty_str(STDOUT_FILENO));
    {
        const char *term = getenv("TERM");
        if (term && *term) {
            printf("  TERM  : %s\n", term);
            ok("TERM set");
        } else {
            note("TERM is unset; colours/line-editing may be downgraded. "
                 "export TERM=xterm-256color in interactive shells.");
        }
    }

    HEAD("endpoint reachability");
    {
        /* Short-timeout probe session against the configured provider. */
        puci_provider_config probe = *pc;
        probe.timeout_ms = 8000; /* force a short bound so doctor does not hang */
        puci_context *ctx = NULL;
        if (puci_context_create(NULL, &ctx) != PUCI_OK) {
            fail("failed to create context");
        } else {
            puci_context_set_provider(ctx, &probe);
            puci_session_options op = {0};
            op.provider = probe;
            puci_session *sess = NULL;
            if (puci_session_create(ctx, &op, &sess) != PUCI_OK) {
                fail("session create failed");
            } else {
                puci_status st = puci_session_send(sess, "Reply with exactly: OK");
                if (st == PUCI_OK) {
                    ok("live round-trip succeeded");
                } else {
                    fail("live round-trip failed: %s", puci_status_string(st));
                    const char *err = puci_session_error(sess);
                    if (err) {
                        note("  %s", err);
                    }
                    note("check PUCI_BASE_URL is reachable and PUCI_API_KEY/OPENROUTER_API_KEY is "
                         "correct; for OpenRouter BYOK ensure base https://openrouter.ai/api/v1; for TLS set PUCI_CA_BUNDLE.");
                }
                puci_session_destroy(sess);
            }
            puci_context_destroy(ctx);
        }
    }

    printf("\n");
    if (g_fail) {
        printf("Diagnostics completed with %d problem(s) to fix.\n", g_fail);
        return 1;
    }
    printf("All checks passed.\n");
    return 0;
}
