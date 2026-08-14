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

#include <poll.h> /* struct pollfd used by the async tool lifecycle */

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

/* ------------------------------------------------------------------ *
 * Non-blocking (async) tool lifecycle.                                *
 *                                                                     *
 * puci_tool_invoke_cb above runs to completion on the calling thread   *
 * before the engine can make further progress; it is appropriate for  *
 * fast, bounded work (e.g. an in-memory lookup) but a tool that does   *
 * its own I/O (spawns a subprocess, opens a socket) blocks the whole   *
 * agent loop, including any other session sharing the same host poll  *
 * loop, for as long as it runs.                                       *
 *                                                                     *
 * A tool may instead register the four callbacks below. The engine    *
 * drives them exactly like puci_session_get_fds / next_timeout_ms /   *
 * process: begin() starts the work and returns immediately; the       *
 * engine polls the fds get_fds() reports (folded into the session's   *
 * own fd set) using the timeout next_timeout_ms() reports; step() is  *
 * called after activity (or the timeout) to make bounded, non-        *
 * blocking progress. None of the four may block on I/O, sleep, or     *
 * wait for a subprocess. This is how the built-in run_shell tool       *
 * avoids stalling the host loop for the duration of the command.      *
 * ------------------------------------------------------------------ */

/** Opaque handle for one in-flight async tool invocation. Owned by the tool
 *  implementation between a successful begin() and the step() call that
 *  reports completion; the engine never dereferences it. */
typedef void *puci_tool_async_handle;

/** Starts a non-blocking tool invocation. Must return without blocking:
 *  fork/spawn/connect, stash whatever state is needed, and return.
 *  @param out_handle set to an opaque handle representing the in-flight work
 *         when returning 0. Untouched when returning non-zero.
 *  @param out_result same contract as puci_tool_invoke_cb's out_result: only
 *         used when begin() itself fails synchronously (bad arguments, fork
 *         failure); left NULL when out_handle is set.
 *  @return 0 if the invocation was started (out_handle set), non-zero if it
 *          failed immediately (out_result set, no handle produced). */
typedef int (*puci_tool_async_begin_cb)(void *userdata,
                                        const puci_cancel_token *cancel,
                                        const char *args_json,
                                        puci_tool_async_handle *out_handle,
                                        char **out_result);

/** Reports the fds the host should poll while this invocation is in flight.
 *  Same sizing contract as puci_session_get_fds: with cap == 0, writes the
 *  required count into *out_count without touching fds (fds may be NULL).
 *  The vtable implementation MUST NOT write more than cap entries into fds.
 *  Must not block. */
typedef void (*puci_tool_async_get_fds_cb)(void *userdata,
                                           puci_tool_async_handle handle,
                                           struct pollfd *fds, size_t cap,
                                           size_t *out_count);

/** Reports how long the host may block before calling step() again, same
 *  contract as puci_session_next_timeout_ms (-1 = wait indefinitely for fd
 *  activity, 0 = call step() now, >0 = milliseconds). Must not block. */
typedef int (*puci_tool_async_timeout_cb)(void *userdata,
                                          puci_tool_async_handle handle);

/** Performs one bounded, non-blocking unit of work (drain what is readable,
 *  a single non-blocking reap/poll check, and any timeout/cancellation
 *  bookkeeping). Must never block.
 *  @return 1 when the invocation has finished (out_result/out_is_error set;
 *          handle is consumed and must not be referenced again), or 0 if the
 *          engine should keep polling. */
typedef int (*puci_tool_async_step_cb)(void *userdata,
                                       puci_tool_async_handle handle,
                                       char **out_result, int *out_is_error);

/** Forcibly terminates and frees an in-flight invocation outside the normal
 *  step()-returns-1 lifecycle: the session (or context) is being destroyed
 *  while this tool is still running. This is an exceptional teardown path,
 *  not the hot loop; it may do a bounded blocking wait to reap a child
 *  process, but must not wait indefinitely. The handle is invalid after this
 *  call returns. */
typedef void (*puci_tool_async_abort_cb)(void *userdata,
                                         puci_tool_async_handle handle);

/** The five async callbacks for a non-blocking tool. All five are required. */
typedef struct puci_tool_async_vtable {
    puci_tool_async_begin_cb begin;
    puci_tool_async_get_fds_cb get_fds;
    puci_tool_async_timeout_cb next_timeout_ms;
    puci_tool_async_step_cb step;
    puci_tool_async_abort_cb abort;
} puci_tool_async_vtable;

/** Registers a custom tool with the non-blocking lifecycle instead of
 *  puci_tool_invoke_cb. Same name/description/schema/replace semantics as
 *  puci_tool_register. Returns PUCI_ERR_INVALID_ARG if any vtable member is
 *  NULL. */
puci_status puci_tool_register_async(puci_context *ctx,
                                     const puci_tool_spec *spec,
                                     const puci_tool_async_vtable *vt,
                                     void *userdata);

/** Session-scoped equivalent of puci_tool_register_async, mirroring
 *  puci_session_tool_register. */
puci_status puci_session_tool_register_async(puci_session *sess,
                                             const puci_tool_spec *spec,
                                             const puci_tool_async_vtable *vt,
                                             void *userdata);

/** Returns a descriptor for a registered tool by name, or NULL if not found.
 *  The returned spec is owned by the library and valid until the tool is
 *  replaced or the context is destroyed. */
const puci_tool_spec *puci_tool_get(const puci_context *ctx, const char *name);

/* Registration is confined to the owning context/session thread. A returned
 * tool descriptor is borrowed until replacement or context destruction. Tool
 * callback arguments are borrowed for the callback duration. */

#ifdef __cplusplus
}
#endif

#endif /* PUCI_TOOL_H */
