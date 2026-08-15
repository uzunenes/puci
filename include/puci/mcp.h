#ifndef PUCI_MCP_H
#define PUCI_MCP_H

/*
 * puci/mcp.h - Model Context Protocol (MCP) stdio client interface.
 *
 * Connects external MCP servers running as subprocesses via JSON-RPC 2.0
 * over stdin/stdout, performs handshake, and dynamically discovers and
 * registers tools onto a PUCI context.
 */

#include "puci/types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct puci_mcp_server_config {
    const char *name;           /**< Server identifier, e.g. "git" */
    const char *command;        /**< Command to execute, e.g. "mcp-server-git" */
    const char *const *args;    /**< Argument array (NULL-terminated or sized by argc) */
    size_t argc;                /**< Number of arguments in args */
    const char *cwd;            /**< Working directory (optional, NULL for inherited) */
} puci_mcp_server_config;

/** Connects to an MCP server via stdio, completes initialization handshake,
 *  discovers tools, and registers them onto the context.
 *  Returns PUCI_OK on success, or an error status on failure.
 *  Path guards are enforced on command. */
puci_status puci_mcp_server_register(puci_context *ctx, const puci_mcp_server_config *cfg);

/** Loads and connects all MCP servers defined in a configuration file (e.g. ~/.config/puci/config.toml or .puci.toml).
 *  Returns the number of servers successfully connected, or negative on error. */
int puci_mcp_load_servers_from_file(puci_context *ctx, const char *config_path);

#ifdef __cplusplus
}
#endif

#endif /* PUCI_MCP_H */
