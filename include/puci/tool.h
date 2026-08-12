#ifndef PUCI_TOOL_H
#define PUCI_TOOL_H

/*
 * puci/tool.h - tool registration and custom tool callbacks.
 *
 * Tools are callable units the agent loop can invoke. libpuci ships built-in
 * tools; consumers can register additional custom tools that run in the host
 * process. A custom tool receives its JSON arguments plus an opaque context
 * and produces a result string.
 *
 * Thread-safety: registration must be done before the session runs and from
 * the thread that owns the context. The invoke callback may be called on the
 * session thread.
 */

#include "puci/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A descriptor describing a tool for downstream serialization. */
typedef struct puci_tool_spec {
    const char *name;        /**< unique tool name */
    const char *description; /**< short human/LLM description */
    const char *input_schema; /**< JSON Schema object as text (optional) */
} puci_tool_spec;

/** Custom tool invocation callback.
 *  @param userdata opaque pointer supplied at registration
 *  @param cancel cancellation token for the in-flight turn. Use it to abort a
 *         long-running or blocking tool (e.g. one that spawns a subprocess) when
 *         the caller cancels: check `cancel->flag` and/or poll `cancel->fd`.
 *         NEVER wait on or block inside the tool; the token is advisory. It is
 *         non-NULL during a normal invocation and valid only for the duration of
 *         the call. May be NULL if the engine was built without cancellation.
 *  @param args_json the tool arguments as JSON text (owned by session, valid
 *         for the duration of the call)
 *  @param out_result set to a NUL-terminated result string. On success the
 *         library copies it; on error set *out_result to NULL and return a
 *         non-zero status.
 *  @return 0 on success, non-zero on tool failure. If the tool aborts because
 *         cancellation was requested, treat it as a normal tool error and set
 *         *out_result to a JSON string describing the abort. */
typedef int (*puci_tool_invoke_cb)(void *userdata,
                                   const puci_cancel_token *cancel,
                                   const char *args_json, char **out_result);

/** Registers a custom tool with the context. The name, description, and
 *  schema strings are copied. If a tool with the same name already exists,
 *  the existing one is replaced. Returns PUCI_OK on success. */
puci_status puci_tool_register(puci_context *ctx, const puci_tool_spec *spec,
                               puci_tool_invoke_cb invoke_cb, void *userdata);

/** Registers a custom tool with a specific session so it is only available
 *  within that session's tool set. Same semantics as puci_tool_register. */
puci_status puci_session_tool_register(puci_session *sess,
                                       const puci_tool_spec *spec,
                                       puci_tool_invoke_cb invoke_cb,
                                       void *userdata);

/** Returns a descriptor for a registered tool by name, or NULL if not found.
 *  The returned spec is owned by the library and valid until the tool is
 *  replaced or the context is destroyed. */
const puci_tool_spec *puci_tool_get(const puci_context *ctx, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PUCI_TOOL_H */