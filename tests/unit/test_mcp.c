/*
 * test_mcp.c - test MCP client handshake, tool discovery, invocation, and shutdown.
 *
 * Uses a minimal sh mock server (no compilation, no system()) that responds to
 * the JSON-RPC 2.0 methods puci_mcp_server_register emits. Verifies:
 *   - initialize / notifications/initialized handshake
 *   - tools/list -> dynamic tool discovery and registration
 *   - tools/call -> tool invocation and result capture
 *   - error response propagation
 *   - path guard rejection
 *   - clean shutdown with no zombie/orphaned child processes
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "puci/puci.h"
#include "puci/mcp.h"
#include "puci_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../test_util.h"

/* The mock MCP server: a POSIX sh script. Reads one JSON-RPC line per request
 * and emits one JSON-RPC line per response, exactly like a real stdio MCP
 * server. Uses only shell builtins + printf. */
static const char *MOCK_SH =
    "#!/bin/sh\n"
    "while IFS= read -r line; do\n"
    "  case \"$line\" in\n"
    "    *'\"method\":\"initialize\"'*) printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"mock-mcp\",\"version\":\"1.0\"}}}';;\n"
    "    *'\"method\":\"tools/list\"'*) printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"mcp_echo\",\"description\":\"Echo input\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"msg\":{\"type\":\"string\"}},\"required\":[\"msg\"]}},{\"name\":\"mcp_fail\",\"description\":\"Always errors\",\"inputSchema\":{\"type\":\"object\"}}]}}';;\n"
    "    *'\"method\":\"tools/call\"'*'mcp_fail'*) printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32000,\"message\":\"mcp_fail always errors\"}}';;\n"
    "    *'\"method\":\"tools/call\"'*) printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"mcp-echo-output\"}]}}';;\n"
    "    *) : ;;\n"
    "  esac\n"
    "done\n";

static void write_mock_script(const char *path) {
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs(MOCK_SH, f);
    fclose(f);
    CHECK(chmod(path, 0755) == 0);
}

static void test_mcp_registration_and_invocation(void) {
    const char *mock_script = "/tmp/puci_mock_mcp.sh";

    write_mock_script(mock_script);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    puci_mcp_server_config mcfg = {
        .name = "mock",
        .command = mock_script,
        .args = NULL,
        .argc = 0,
        .cwd = NULL,
    };

    puci_status st = puci_mcp_server_register(ctx, &mcfg);
    CHECK(st == PUCI_OK);

    /* Verify both tools were discovered and registered on the context. */
    const puci_tool_spec *spec = puci_tool_get(ctx, "mcp_echo");
    CHECK(spec != NULL);
    if (spec) {
        CHECK(strcmp(spec->name, "mcp_echo") == 0);
    }
    CHECK(puci_tool_get(ctx, "mcp_fail") != NULL);

    /* ---- successful invocation ---- */
    puci_tool *t = NULL;
    CHECK(puci_tools_lookup(ctx, "mcp_echo", &t) == 1);
    CHECK(t != NULL && t->invoke != NULL);

    char *res = NULL;
    int rc = t->invoke(t->userdata, NULL, "{\"msg\":\"hello\"}", &res);
    CHECK(rc == 0);
    CHECK(res != NULL);
    if (res) {
        CHECK(strstr(res, "mcp-echo-output") != NULL);
        free(res);
    }

    /* ---- error response propagation ---- */
    puci_tool *tf = NULL;
    CHECK(puci_tools_lookup(ctx, "mcp_fail", &tf) == 1);
    char *res_fail = NULL;
    int rc_fail = tf->invoke(tf->userdata, NULL, "{}", &res_fail);
    CHECK(rc_fail != 0);
    CHECK(res_fail != NULL);
    if (res_fail) {
        CHECK(strstr(res_fail, "mcp_fail always errors") != NULL);
        free(res_fail);
    }

    /* ---- path guard rejects protected commands ---- */
    puci_mcp_server_config bad = mcfg;
    bad.command = "/tmp/.git/evil-mcp";
    CHECK(puci_mcp_server_register(ctx, &bad) == PUCI_ERR_INVALID_ARG);

    /* Clean shutdown: destroying the context must reap the MCP child. */
    puci_context_destroy(ctx);
    unlink(mock_script);

    /* No zombies/orphans: waitpid(-1, WNOHANG) should report ECHILD (no live
     * children remain) after a short settle. */
    int ws = 0;
    pid_t r = waitpid(-1, &ws, WNOHANG);
    CHECK(r < 0); /* ECHILD expected: nothing left to reap */
}

static void test_mcp_config_file_loader(void) {
    const char *mock_script = "/tmp/puci_mock_mcp.sh";
    write_mock_script(mock_script);

    const char *cfg_path = "/tmp/puci_mcp_test_config.toml";
    FILE *f = fopen(cfg_path, "w");
    CHECK(f != NULL);
    fprintf(f, "[mcp_servers.echo_server]\n"
               "command = \"/tmp/puci_mock_mcp.sh\"\n"
               "args = [\"--arg\", \"value\"]\n");
    fclose(f);

    puci_context *ctx = NULL;
    CHECK(puci_context_create(NULL, &ctx) == PUCI_OK);

    int count = puci_mcp_load_servers_from_file(ctx, cfg_path);
    CHECK(count == 1);

    /* The discovered tools must be present. */
    CHECK(puci_tool_get(ctx, "mcp_echo") != NULL);

    puci_context_destroy(ctx);
    unlink(cfg_path);
    unlink(mock_script);
}

int main(void) {
    test_mcp_registration_and_invocation();
    test_mcp_config_file_loader();
    TEST_END();
}
