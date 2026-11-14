#ifndef PUCI_CLI_TUI_H
#define PUCI_CLI_TUI_H
#include "../cmd/cmd.h"
#if PUCI_FEATURE_TUI
int puci_tui_available(void);
int puci_tui_should_launch(int argc, char **argv);
int puci_tui_run(const puci_cli_params *params);
void tui_render_frame(void);
#else
/* Built without the TUI (meson -Dtui=disabled, make TUI=0): the CLI falls
 * back to the line-mode REPL and `--tui` explains why it is unavailable. */
#include <stdio.h>
static inline int puci_tui_available(void) { return 0; }
static inline int puci_tui_should_launch(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}
static inline int puci_tui_run(const puci_cli_params *params) {
    (void)params;
    fprintf(stderr, "puci: this build has no TUI (rebuild with TUI=1); use the REPL or `puci run`\n");
    return 2;
}
#endif
#endif
