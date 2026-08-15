/*
 * main.c - puci-cli entry point and command dispatch.
 *
 * The CLI is a thin consumer of libpuci: it only ever calls the public API in
 * include/puci/. It never touches libpuci internals.
 */

#include <puci/puci.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd/cmd.h"
#include "cmd/session_mgr.h"
#include "repl/repl.h"
#include "tui/tui.h"

static void print_help(FILE *f) {
    fprintf(f,
            "puci - a minimal C17 AI coding agent harness\n"
            "\n"
            "Usage:\n"
            "  puci [options]                  Start the interactive REPL\n"
            "  puci [options] run \"<prompt>\"   Run a one-shot, non-interactive agent turn\n"
            "  puci doctor                     Diagnose config, credentials, and reachability\n"
            "  puci --list-sessions            List saved session transcripts\n"
            "  puci --version                  Print version\n"
            "  puci --help                     Show this help\n"
            "\n"
            "Options:\n"
            "  --session <id>                  Use or create a specific named session\n"
            "  --resume [latest|<id>]          Continue a previous session\n"
            "  --plan, --dry-run               Plan mode: simulate file mutations without writing to disk\n"
            "\n"
            "Configuration:\n"
            "  PUCI_BASE_URL                   Provider base URL (default https://api.openai.com/v1)\n"
            "  PUCI_API_KEY                    API key for hosted providers\n"
            "  PUCI_MODEL                      Model identifier (default gpt-4o-mini)\n"
            "  ~/.config/puci/config.toml      User global configuration file\n"
            "  .puci.toml                      Project configuration file (credentials forbidden)\n");
}

static void print_version(void) {
    printf("puci %s (ABI v%d)\n", puci_version_string(), puci_abi_version());
}

int main(int argc, char **argv) {
    puci_cli_params params;
    memset(&params, 0, sizeof(params));

    if (puci_tui_should_launch(argc, argv)) {
        return puci_tui_run(&params);
    }

    int argi = 1;
    char *prompt_buf = NULL;

    while (argi < argc) {
        const char *arg = argv[argi];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_help(stdout);
            return 0;
        } else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) {
            print_version();
            return 0;
        } else if (strcmp(arg, "--list-sessions") == 0) {
            return puci_cli_list_sessions();
        } else if (strcmp(arg, "doctor") == 0) {
            return puci_cli_doctor();
        } else if (strcmp(arg, "--no-tui") == 0 || strcmp(arg, "--tui") == 0) {
            if (strcmp(arg, "--tui")==0) return puci_tui_run(&params);
            argi++;
        } else if (strcmp(arg, "--plan") == 0 || strcmp(arg, "--dry-run") == 0) {
            params.plan_mode = 1;
            argi++;
        } else if (strcmp(arg, "--session") == 0) {
            if (argi + 1 < argc) {
                params.session_id = argv[++argi];
                argi++;
            } else {
                fprintf(stderr, "puci: --session requires an argument\n");
                return 2;
            }
        } else if (strcmp(arg, "--resume") == 0) {
            if (argi + 1 < argc && argv[argi + 1][0] != '-') {
                params.resume_id = argv[++argi];
            } else {
                params.resume_id = "latest";
            }
            argi++;
        } else if (strcmp(arg, "run") == 0) {
            argi++;
            if (argi >= argc) {
                fprintf(stderr, "usage: puci [options] run \"<prompt>\"\n");
                return 2;
            }
            size_t total = 1;
            for (int i = argi; i < argc; i++) {
                total += strlen(argv[i]) + 1;
            }
            prompt_buf = (char *)malloc(total);
            if (!prompt_buf) {
                fprintf(stderr, "out of memory\n");
                return 1;
            }
            size_t off = 0;
            for (int i = argi; i < argc; i++) {
                size_t len = strlen(argv[i]);
                if (i > argi) {
                    prompt_buf[off++] = ' ';
                }
                memcpy(prompt_buf + off, argv[i], len);
                off += len;
            }
            prompt_buf[off] = '\0';
            params.prompt = prompt_buf;
            int rc = puci_cli_run_with_params(&params);
            free(prompt_buf);
            return rc;
        } else {
            fprintf(stderr, "puci: unknown command or flag '%s'\n", arg);
            print_help(stderr);
            return 2;
        }
    }

    return puci_cli_repl_with_params(&params);
}
