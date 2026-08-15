#ifndef PUCI_CLI_TUI_H
#define PUCI_CLI_TUI_H
#include "../cmd/cmd.h"
int puci_tui_available(void);
int puci_tui_should_launch(int argc, char **argv);
int puci_tui_run(const puci_cli_params *params);
void tui_render_frame(void);
#endif
