#ifndef PUCI_INTERNAL_TOOLS_H
#define PUCI_INTERNAL_TOOLS_H

/*
 * tools/tools_internal.h - built-in tool registration.
 *
 * Each built-in tool file exposes a registration function that registers its
 * schema and invoke callback on a context. The invoke callback contract matches
 * puci_tool_invoke_cb; the returned *out_result must be allocated with libc
 * malloc and is freed by the engine after it is consumed.
 */

#include "puci/puci.h"

puci_status puci_register_tool_read_file(puci_context *ctx);
puci_status puci_register_tool_write_file(puci_context *ctx);
puci_status puci_register_tool_run_shell(puci_context *ctx);

#endif /* PUCI_INTERNAL_TOOLS_H */