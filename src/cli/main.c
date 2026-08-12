/*
 * main.c - puci-cli entry point and command dispatch.
 *
 * The CLI is a thin consumer of libpuci: it only ever calls the public API in
 * include/puci/. It never touches libpuci internals.
 */

#include <puci/puci.h>

#include <stdio.h>
#include <string.h>

#include "cmd/cmd.h"
#include "repl/repl.h"

static void print_help(FILE *f) {
    fprintf(f,
            "puci - a minimal C17 AI coding agent harness\n"
            "\n"
            "Usage:\n"
            "  puci run \"<prompt>\"   Run a one-shot, non-interactive agent turn\n"
            "  puci                  Start the interactive REPL\n"
            "  puci --version        Print version\n"
            "  puci --help           Show this help\n"
            "\n"
            "Configuration (environment, until config.toml support lands):\n"
            "  PUCI_BASE_URL   provider base URL (default https://api.openai.com/v1)\n"
            "  PUCI_API_KEY    API key for hosted providers\n"
            "  PUCI_MODEL      model identifier (default gpt-4o-mini)\n");
}

static void print_version(void) {
    printf("puci %s (ABI v%d)\n", puci_version_string(), puci_abi_version());
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return puci_cli_repl();
    }
    const char *a1 = argv[1];
    if (strcmp(a1, "--help") == 0 || strcmp(a1, "-h") == 0) {
        print_help(stdout);
        return 0;
    }
    if (strcmp(a1, "--version") == 0 || strcmp(a1, "-V") == 0) {
        print_version();
        return 0;
    }
    if (strcmp(a1, "run") == 0) {
        return puci_cli_run(argc - 1, argv + 1);
    }
    fprintf(stderr, "puci: unknown command or flag '%s'\n", a1);
    print_help(stderr);
    return 2;
}