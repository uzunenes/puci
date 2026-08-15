/*
 * config/config.c - configuration file parsing for PUCI.
 */

#include "puci_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim_str(char *s) {
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void unquote_str(char *val) {
    trim_str(val);
    size_t len = strlen(val);
    if (len >= 2 && ((val[0] == '"' && val[len - 1] == '"') || (val[0] == '\'' && val[len - 1] == '\''))) {
        memmove(val, val + 1, len - 2);
        val[len - 2] = '\0';
    }
}

int puci_config_parse_file(const char *filepath, int is_project_local,
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
        trim_str(p);
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
        trim_str(key);
        unquote_str(val);

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
