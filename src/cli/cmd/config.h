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

#endif /* PUCI_CLI_CONFIG_H */