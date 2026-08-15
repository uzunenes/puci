#ifndef PUCI_CLI_CONFIG_H
#define PUCI_CLI_CONFIG_H

/*
 * cmd/config.h - environment-based provider configuration for the CLI.
 *
 * Until config.toml support lands (Phase 3b), the CLI reads provider settings
 * from the environment. This helper builds a puci_provider_config once and
 * returns a stable pointer, shared by `puci run`, the REPL, and `puci doctor`.
 */

#include <puci/puci.h>

/* Loads the provider config from the environment and returns a pointer to a
 * static instance (valid for the lifetime of the process). name is the provider
 * name (e.g. "openai"). Calling it again re-reads the environment. */
puci_provider_config *puci_cli_provider(const char *name);

const char *puci_cli_config_error(void);

int puci_cli_parse_toml_file(const char *filepath, int is_project_local,
                             char *base_url, size_t base_cap,
                             char *model, size_t model_cap,
                             char *api_key, size_t key_cap,
                             char *ca_bundle, size_t ca_cap,
                             char *extra_body, size_t extra_cap,
                             int *tls_verify, int *stream_idle_ms,
                             char *err_buf, size_t err_cap);

/* Connects all MCP servers configured in the global user config
 * (~/.config/puci/config.toml) and project-local config (.puci.toml). Returns
 * the total number of MCP servers successfully connected, or -1 if the
 * project-local config contains a credential violation. Safe to call once after
 * puci_context_create; the discovered MCP tools become available to every
 * session on the context. */
int puci_cli_load_mcp_servers(puci_context *ctx);

#endif /* PUCI_CLI_CONFIG_H */
