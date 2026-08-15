/*
 * cmd/config.c - configuration file and environment resolution for CLI.
 *
 * Precedence (highest first):
 *   1. Environment variables (PUCI_*)
 *   2. Project-local config (.puci.toml or .puci/config.toml) - credentials FORBIDDEN
 *   3. User global config (~/.config/puci/config.toml)
 *   4. Built-in defaults
 */

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_base_url[2048];
static char g_model[256];
static char g_api_key[2048];
static char g_ca_bundle[2048];
static char g_extra_body[8192];
static puci_provider_config g_cfg;
static char g_last_error[512];

static void trim_cli(char *s) {
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void unquote_cli(char *val) {
    trim_cli(val);
    size_t len = strlen(val);
    if (len >= 2 && ((val[0] == '"' && val[len - 1] == '"') || (val[0] == '\'' && val[len - 1] == '\''))) {
        memmove(val, val + 1, len - 2);
        val[len - 2] = '\0';
    }
}

int puci_cli_parse_toml_file(const char *filepath, int is_project_local,
                             char *base_url, size_t base_cap,
                             char *model, size_t model_cap,
                             char *api_key, size_t key_cap,
                             char *ca_bundle, size_t ca_cap,
                             char *extra_body, size_t extra_cap,
                             int *tls_verify, int *stream_idle_ms,
                             char *err_buf, size_t err_cap) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        return 0; /* Not found is fine */
    }

    char line[1024];
    int line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        char *p = line;
        trim_cli(p);
        if (!*p || *p == '#' || *p == ';') {
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        trim_cli(key);
        unquote_cli(val);

        if (is_project_local) {
            if (strcmp(key, "api_key") == 0 || strcmp(key, "base_url") == 0 ||
                strcmp(key, "key") == 0 || strcmp(key, "auth") == 0 ||
                strcmp(key, "token") == 0 || strcmp(key, "secret") == 0) {
                fclose(f);
                if (err_buf && err_cap > 0) {
                    snprintf(err_buf, err_cap,
                             "%s:%d: credentials (%s) are forbidden in project-local config. Use ~/.config/puci/config.toml or environment variables",
                             filepath, line_no, key);
                }
                return -1;
            }
        }

        if (strcmp(key, "base_url") == 0 && base_url) {
            snprintf(base_url, base_cap, "%s", val);
        } else if (strcmp(key, "model") == 0 && model) {
            snprintf(model, model_cap, "%s", val);
        } else if (strcmp(key, "api_key") == 0 && api_key) {
            snprintf(api_key, key_cap, "%s", val);
        } else if (strcmp(key, "ca_bundle") == 0 && ca_bundle) {
            snprintf(ca_bundle, ca_cap, "%s", val);
        } else if (strcmp(key, "extra_body") == 0 && extra_body) {
            snprintf(extra_body, extra_cap, "%s", val);
        } else if (strcmp(key, "context_budget_bytes") == 0) {
            /* parsed but not directly used in provider config; kept for template completeness */
        } else if (strcmp(key, "tool_timeout_ms") == 0) {
        } else if (strcmp(key, "tls_verify") == 0 && tls_verify) {
            *tls_verify = (strcmp(val, "0") == 0 || strcmp(val, "false") == 0 || strcmp(val, "no") == 0 || strcmp(val, "off") == 0) ? 0 : 1;
        } else if (strcmp(key, "stream_idle_timeout_secs") == 0 && stream_idle_ms) {
            long s = strtol(val, NULL, 10);
            if (s > 0) *stream_idle_ms = (int)(s * 1000);
        }
    }

    fclose(f);
    return 1;
}

const char *puci_cli_config_error(void) {
    return g_last_error[0] ? g_last_error : NULL;
}

puci_provider_config *puci_cli_provider(const char *name) {
    g_last_error[0] = '\0';

    /* 1. Defaults */
    char base[2048] = "https://api.openai.com/v1";
    char model[256] = "gpt-4o-mini";
    char key[2048] = "";
    char cafile[2048] = "";
    char extra[8192] = "";
    int tls_verify = 1;
    int idle_ms = 0;

    /* 2. Global user config (~/.config/puci/config.toml) */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char global_path[1024];
    if (xdg && xdg[0]) {
        snprintf(global_path, sizeof(global_path), "%s/puci/config.toml", xdg);
    } else {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(global_path, sizeof(global_path), "%s/.config/puci/config.toml", home);
        } else {
            global_path[0] = '\0';
        }
    }
    if (global_path[0]) {
        puci_cli_parse_toml_file(global_path, 0,
                                 base, sizeof(base),
                                 model, sizeof(model),
                                 key, sizeof(key),
                                 cafile, sizeof(cafile),
                                 extra, sizeof(extra),
                                 &tls_verify, &idle_ms,
                                 g_last_error, sizeof(g_last_error));
    }

    /* 3. Project-local config (.puci.toml or .puci/config.toml) */
    const char *proj_files[] = {".puci.toml", ".puci/config.toml", NULL};
    for (int i = 0; proj_files[i]; i++) {
        if (access(proj_files[i], R_OK) == 0) {
            int rc = puci_cli_parse_toml_file(proj_files[i], 1,
                                              base, sizeof(base),
                                              model, sizeof(model),
                                              key, sizeof(key),
                                              cafile, sizeof(cafile),
                                              extra, sizeof(extra),
                                              &tls_verify, &idle_ms,
                                              g_last_error, sizeof(g_last_error));
            if (rc < 0) {
                /* Fatal project credential violation */
                fprintf(stderr, "puci config error: %s\n", g_last_error);
            }
            break;
        }
    }

    /* 4. Environment overrides (highest precedence) */
    const char *env_base = getenv("PUCI_BASE_URL");
    if (env_base && *env_base) snprintf(base, sizeof(base), "%s", env_base);

    const char *env_model = getenv("PUCI_MODEL");
    if (env_model && *env_model) snprintf(model, sizeof(model), "%s", env_model);

    const char *env_key = getenv("PUCI_API_KEY");
    if (env_key && *env_key) snprintf(key, sizeof(key), "%s", env_key);

    /* OPENROUTER_API_KEY fallback (first-class BYOK): if PUCI_API_KEY absent, use OPENROUTER_API_KEY.
     * When falling back, also default base_url/model to OpenRouter if not explicitly set. */
    int used_openrouter_fallback = 0;
    if (!key[0]) {
        const char *or_key = getenv("OPENROUTER_API_KEY");
        if (or_key && *or_key) {
            snprintf(key, sizeof(key), "%s", or_key);
            used_openrouter_fallback = 1;
        }
    }
    if (used_openrouter_fallback) {
        const char *env_base_explicit = getenv("PUCI_BASE_URL");
        if ((!env_base_explicit || !*env_base_explicit) && strcmp(base, "https://api.openai.com/v1") == 0) {
            snprintf(base, sizeof(base), "%s", "https://openrouter.ai/api/v1");
        }
        const char *env_model_explicit = getenv("PUCI_MODEL");
        if ((!env_model_explicit || !*env_model_explicit) && strcmp(model, "gpt-4o-mini") == 0) {
            snprintf(model, sizeof(model), "%s", "anthropic/claude-3.5-sonnet");
        }
    }

    const char *env_ca = getenv("PUCI_CA_BUNDLE");
    if (env_ca && *env_ca) snprintf(cafile, sizeof(cafile), "%s", env_ca);

    const char *env_extra = getenv("PUCI_EXTRA_BODY");
    if (env_extra && *env_extra) snprintf(extra, sizeof(extra), "%s", env_extra);

    const char *env_tls = getenv("PUCI_TLS_VERIFY");
    if (env_tls && *env_tls) {
        tls_verify = (strcmp(env_tls, "0") == 0 || strcmp(env_tls, "false") == 0 ||
                      strcmp(env_tls, "no") == 0 || strcmp(env_tls, "off") == 0) ? 0 : 1;
    }

    const char *env_idle = getenv("PUCI_STREAM_IDLE_TIMEOUT_SECS");
    if (env_idle && *env_idle) {
        long secs = strtol(env_idle, NULL, 10);
        if (secs > 0 && secs <= 2147483) idle_ms = (int)(secs * 1000);
    }

    snprintf(g_base_url, sizeof(g_base_url), "%s", base);
    snprintf(g_model, sizeof(g_model), "%s", model);
    snprintf(g_api_key, sizeof(g_api_key), "%s", key);
    snprintf(g_ca_bundle, sizeof(g_ca_bundle), "%s", cafile);
    snprintf(g_extra_body, sizeof(g_extra_body), "%s", extra);

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.name = name ? name : "openai";
    g_cfg.base_url = g_base_url;
    g_cfg.model = g_model;
    g_cfg.api_key = g_api_key[0] ? g_api_key : NULL;
    g_cfg.ca_bundle = g_ca_bundle[0] ? g_ca_bundle : NULL;
    g_cfg.extra_body = g_extra_body[0] ? g_extra_body : NULL;
    g_cfg.timeout_ms = 120000;
    g_cfg.tls_verify = tls_verify;
    g_cfg.stream_idle_timeout_ms = idle_ms;

    return &g_cfg;
}

int puci_cli_load_mcp_servers(puci_context *ctx) {
    if (!ctx) {
        return -1;
    }

    int total = 0;

    /* 1. Global user config (~/.config/puci/config.toml) */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char global_path[1024];
    if (xdg && xdg[0]) {
        snprintf(global_path, sizeof(global_path), "%s/puci/config.toml", xdg);
    } else {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(global_path, sizeof(global_path),
                     "%s/.config/puci/config.toml", home);
        } else {
            global_path[0] = '\0';
        }
    }
    if (global_path[0] && access(global_path, R_OK) == 0) {
        int n = puci_mcp_load_servers_from_file(ctx, global_path);
        if (n >= 0) {
            total += n;
        }
    }

    /* 2. Project-local config (.puci.toml). MCP servers are allowed here (they
     *    are not credentials), but a project credential violation is fatal. */
    const char *proj_files[] = {".puci.toml", ".puci/config.toml", NULL};
    for (int i = 0; proj_files[i]; i++) {
        if (access(proj_files[i], R_OK) == 0) {
            int n = puci_mcp_load_servers_from_file(ctx, proj_files[i]);
            if (n >= 0) {
                total += n;
            }
            break;
        }
    }

    return total;
}
