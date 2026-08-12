#ifndef PUCI_CLI_CMD_H
#define PUCI_CLI_CMD_H

/*
 * cmd/cmd.h - CLI subcommand handlers.
 *
 * The CLI only uses the public libpuci headers (include/puci/); there is no
 * access to libpuci internals from here.
 */

/* Handles `puci run "<prompt>"`. argv[0] is the "run" token. Returns a process
 * exit code (0 success, 1 runtime error, 2 usage). */
int puci_cli_run(int argc, char **argv);

#endif /* PUCI_CLI_CMD_H */