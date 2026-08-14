#ifndef PUCI_CLI_REPL_H
#define PUCI_CLI_REPL_H

/* repl/repl.h - interactive REPL mode. */
#include "../cmd/cmd.h"

/* Runs the interactive REPL until the user quits. Returns 0 on clean exit. */
int puci_cli_repl(void);

int puci_cli_repl_with_params(const puci_cli_params *params);

#endif /* PUCI_CLI_REPL_H */
