/*
 * cmd/config.c - environment-based provider configuration (see config.h).
 *
 * TLS always verifies by default; relaxation is possible only when the user
 * explicitly opts in via PUCI_TLS_VERIFY=0 AND the host is allowlisted through
 * PUCI_INSECURE_TLS_HOSTS (consumed at context creation).
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Static storage for strings that cannot point directly at the environment. */
static char g_ca_bundle[2048];
static char g_extra_body[8192];
static puci_provider_config g_cfg;

static int env_true(const char *v, const char **falsy) {
    if (!v) {
        return 1;
    }
    for (int i = 0; falsy[i]; i++) {
        if (strcmp(v, falsy[i]) == 0) {
            return 0;
        }
    }
    return 1;
}

puci_provider_config *puci_cli_provider(const char *name) {
    const char *base = getenv("PUCI_BASE_URL");
    if (!base || !*base) {
        base = "https://api.openai.com/v1";
    }
    const char *model = getenv("PUCI_MODEL");
    if (!model || !*model) {
        model = "gpt-4o-mini";
    }
    const char *key = getenv("PUCI_API_KEY");
    const char *cafile = getenv("PUCI_CA_BUNDLE");
    const char *extra = getenv("PUCI_EXTRA_BODY");
    const char *tls = getenv("PUCI_TLS_VERIFY");

    static const char *falsy[] = {"0", "false", "off", "no", NULL};
    int tls_verify = env_true(tls, falsy) ? 1 : 0;

    int idle_ms = 0;
    const char *idle = getenv("PUCI_STREAM_IDLE_TIMEOUT_SECS");
    if (idle && *idle) {
        long secs = strtol(idle, NULL, 10);
        if (secs > 0 && secs <= 2147483) {
            idle_ms = (int)(secs * 1000);
        }
    }

    g_ca_bundle[0] = '\0';
    if (cafile && *cafile) {
        snprintf(g_ca_bundle, sizeof(g_ca_bundle), "%s", cafile);
    }
    g_extra_body[0] = '\0';
    if (extra && *extra) {
        snprintf(g_extra_body, sizeof(g_extra_body), "%s", extra);
    }

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.name = name;
    g_cfg.base_url = base;
    g_cfg.api_key = key;
    g_cfg.model = model;
    g_cfg.timeout_ms = 120000;
    g_cfg.tls_verify = tls_verify;
    g_cfg.stream_idle_timeout_ms = idle_ms;
    g_cfg.ca_bundle = g_ca_bundle[0] ? g_ca_bundle : NULL;
    g_cfg.extra_body = g_extra_body[0] ? g_extra_body : NULL;
    return &g_cfg;
}
