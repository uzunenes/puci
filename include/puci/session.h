#ifndef PUCI_SESSION_H
#define PUCI_SESSION_H

/*
 * puci/session.h - session lifecycle and the agent loop.
 *
 * A session is an independent conversation plus a running agent loop. Sessions
 * hang off a context (see puci.h) and may coexist in any number within one
 * process.
 *
 * Thread-safety: a session is owned by the thread that created it. Functions
 * taking a session must be called from that thread and must not overlap,
 * except puci_session_cancel(), which is async-signal-safe and may be called
 * from another thread or a signal handler. The pollable and blocking variants
 * are mutually exclusive ways to drive one session.
 */

#include "puci/types.h"
#include "puci/approval.h"

#include <poll.h> /* struct pollfd used by the poll-loop driving API */

#ifdef __cplusplus
extern "C" {
#endif

/** Callback fired for streaming events during a session. The ctx pointer
 *  points to event-specific data (see puci_event in types.h) and is only
 *  valid for the duration of the callback. Must not block. Thread: the thread
 *  driving the session. */
typedef void (*puci_session_event_cb)(void *userdata, puci_session *sess,
                                      puci_event event, const void *ctx);

/** Options passed to puci_session_create. Zero-init and set only what you need. */
typedef struct puci_session_options {
    puci_provider_config provider; /**< provider + model + key + base_url */
    puci_session_event_cb event_cb; /**< streaming/event callback (optional) */
    void *event_userdata;           /**< passed to event_cb */
    const char *system_prompt;      /**< optional system prompt */
    size_t max_tool_iterations;     /**< cap on tool iterations per turn, 0 = default (32). A value of N permits up to N tool dispatch rounds; requesting tools on round N+1 aborts the turn with PUCI_ERR_LIMIT_EXCEEDED */
    puci_approval_cb approve_cb;    /**< tool approval policy hook, invoked before each tool runs (optional) */
    void *approve_userdata;         /**< passed to approve_cb */
    int plan_mode;                  /**< 1 = plan/dry-run mode: mutating file tools simulate success without writing to disk */
    long tool_timeout_ms;           /**< timeout for individual tool calls in ms (0 = default 60000) */
} puci_session_options;

/** Creates a new session bound to a context. On success returns PUCI_OK and
 *  sets *out to a session owned by the caller. All string fields in opts are
 *  copied. Returns PUCI_ERR_INVALID_ARG if ctx or out is NULL. */
puci_status puci_session_create(const puci_context *ctx,
                                const puci_session_options *opts,
                                puci_session **out);

/** Destroys a session and frees all associated resources. The session pointer
 *  must be a valid session returned by puci_session_create and is invalidated
 *  on return. Calling with NULL is a no-op. */
void puci_session_destroy(puci_session *sess);

/** Appends a user message to the conversation and starts the agent loop.
 *  Blocking convenience API: returns only when the turn completes (or an
 *  error or cancel occurs). Events are delivered synchronously via the
 *  session's event callback. Returns PUCI_OK on completion, a puci_status on
 *  failure, or PUCI_ERR_CANCELLED if puci_session_cancel was called from
 *  another thread. This is a thin wrapper over puci_session_begin_turn +
 *  puci_session_process + puci_session_next_event; see those for the
 *  non-blocking contract this returns from. */
puci_status puci_session_send(puci_session *sess, const char *message);

/* ------------------------------------------------------------------ *
 * Non-blocking, poll-loop friendly agent driving.                     *
 *                                                                     *
 * These mirror the curl multi interface. A host that owns its event   *
 * loop starts a turn, then repeatedly:                                *
 *     puci_session_get_fds()      fds libpuci wants polled            *
 *     puci_session_next_timeout_ms()  how long the host may block     *
 *     poll(fds, n, ms)                                               *
 *     puci_session_process(fds, n)   bounded non-blocking work        *
 *     while (puci_session_next_event(&ev))  drain produced events     *
 * and repeats until the turn reaches a terminal status.               *
 *                                                                     *
 * The single rule: puci_session_process() returns after bounded work; *
 * it never loops-until-done and never blocks on network or sleep.     *
 * All blocking waits are expressed as fds + a timeout the host drives.*
 *                                                                     *
 * fd/threading contract (embedders, read this):                       *
 *  - The fd set returned by get_fds() is only meaningful until the    *
 *    next call to process(); it may change between calls, including   *
 *    between two successive process() calls. An epoll host must       *
 *    re-register its interest after every process().                  *
 *  - All fds are level-triggered.                                     *
 *  - next_event() may be called without a preceding process() (it     *
 *    drains whatever has been queued so far), but events are          *
 *    generated by process(), so normally call it after process().     *
 *  - The cancel fd from puci_session_cancel_fd() is stable and polled *
 *    by the host directly.                                            *
 *  - Thread-safety: puci_session_cancel() may be called from any      *
 *    thread or a signal handler (it is async-signal-safe). Every      *
 *    other function on a session must be called from the thread that  *
 *    owns the session, and never concurrently. It is a programming    *
 *    error to call two session functions at once from two threads.    *
 * ------------------------------------------------------------------ */

/** Starts a turn for the given message (appends it to history as a user
 *  message) and transitions the session into the streaming state. Returns
 *  PUCI_OK, or PUCI_ERR_INVALID_ARG / PUCI_ERR_NOMEM / PUCI_ERR_OUT_OF_STATE
 *  if a turn is already in flight. Events are produced as the state machine
 *  advances via puci_session_process; drain them with puci_session_next_event. */
puci_status puci_session_begin_turn(puci_session *sess, const char *message);

/** Writes up to cap fds libpuci wants polled into fds, and the count into
 *  *out_count. With cap == 0 (and fds possibly NULL) returns the required
 *  count in *out_count so callers can size their array; this is not an error
 *  and returns PUCI_OK. When cap > 0 and the required count exceeds cap, the
 *  cancel fd is prioritized in fds[0], *out_count is set to cap, and
 *  PUCI_ERR_BUFFER_TOO_SMALL is returned. When idle (no turn in flight)
 *  *out_count is 0. The cancel fd is surfaced separately via
 *  puci_session_cancel_fd. */
puci_status puci_session_get_fds(puci_session *sess, struct pollfd *fds,
                                 size_t cap, size_t *out_count);

/** Writes into *out_ms how many milliseconds the host may block before it must
 *  call puci_session_process again. -1 means "block indefinitely (nothing is
 *  pending)"; 0 means "work is pending right now". When curl reports an
 *  immediate timeout with no read/write activity pending, the value is floored
 *  to ~1000 ms so a host does not busy-spin. Returns PUCI_OK. */
puci_status puci_session_next_timeout_ms(puci_session *sess, int *out_ms);

/** Drives the turn by one bounded step. fds is the array poll() reported on
 *  (may be NULL / count 0 if the host loop had nothing). Performs a bounded
 *  amount of work and returns without blocking on network or sleep. Returns
 *  PUCI_OK while the turn is still in progress, or a terminal status when the
 *  turn has ended: PUCI_OK (turn complete), PUCI_ERR_CANCELLED, or another
 *  puci_status on failure. After a terminal return the caller may inspect
 *  errors via puci_session_error. Returns PUCI_ERR_OUT_OF_STATE if no turn is
 *  in flight, or if called re-entrantly from inside an event callback. */
puci_status puci_session_process(puci_session *sess,
                                 const struct pollfd *fds, size_t count);

/** Dequeues one event produced by the last process() calls into *out.
 *
 *  The `text` pointer is owned by the library and points into an internal
 *  buffer belonging to the session. It remains valid until the next call to
 *  `puci_session_next_event()` or `puci_session_process()` on the same
 *  session, whichever happens first. Callers that need to retain the data
 *  must copy it. The buffer is NUL-terminated for convenience, but `text_len`
 *  is authoritative.
 *
 *  Returns 1 and fills *out, or 0 if the queue is empty. out must be non-NULL.
 *  The payload for each event type is interpreted the same way as in the
 *  event callback (see puci_event in types.h). */
int puci_session_next_event(puci_session *sess, puci_event *out);

/** Returns the terminal status of the most recently completed turn. Valid only
 *  after puci_session_process returns a terminal status. */
puci_status puci_session_last_status(const puci_session *sess);

/** Requests cancellation of any in-flight work on the session.
 *
 *  Async-signal-safe: this may be called from a signal handler or any thread.
 *  It is guaranteed not to allocate, lock, call into libcurl, or block; all it
 *  does is set a `volatile sig_atomic_t` flag and, if the session has a cancel
 *  fd, write a single byte to it. See puci_session_cancel_fd.
 *
 *  Returns PUCI_OK. Calling it on an idle session (no in-flight turn) is a
 *  harmless no-op that returns success, not an error. When a turn is in flight,
 *  the session's driver thread observes the request at the next cancellation
 *  point, stops, and finishes with PUCI_ERR_CANCELLED. */
puci_status puci_session_cancel(puci_session *sess);

/** Returns a file descriptor that becomes readable (and may be read to drain)
 *  when puci_session_cancel has been called while a turn is in flight. This is
 *  the read end of an eventfd (Linux) or self-pipe (elsewhere), marked
 *  O_NONBLOCK and FD_CLOEXEC. It lets an external event loop observe
 *  cancellation without spinning on the flag: select/poll/epoll on it, and on
 *  readability treat the in-flight request as cancelled.
 *
 *  The fd is owned by the session, remains valid until the session is
 *  destroyed, and is shared by cancel requests across turns. Reading from it
 *  drains the pending notification but does not cancel again. Returns -1 if the
 *  session has no cancel fd (e.g. the fd could not be created). */
int puci_session_cancel_fd(puci_session *sess);

/** Validates the session's message history invariants, primarily that every
 *  assistant tool call has a matching tool result immediately following it (no
 *  dangling tool_call_id, which OpenAI-compatible endpoints reject on the next
 *  turn). Returns PUCI_OK if the history is consistent, or PUCI_ERR_PROTOCOL
 *  (with puci_session_error set) if an invariant is violated. This is a
 *  diagnostic/sanity helper; the engine maintains these invariants itself
 *  (including synthesising results for dangling calls on cancellation), so a
 *  non-cancelled well-behaved session always validates. */
puci_status puci_session_validate(const puci_session *sess);

/** Returns cumulative token usage reported by the provider across the whole
 *  session. Any value may be left NULL to ignore it. Counts default to 0 when
 *  the provider did not report usage. */
void puci_session_get_usage(const puci_session *sess, long long *prompt_tokens,
                            long long *completion_tokens,
                            long long *cached_tokens);

/** Replaces the session's model identifier for subsequent turns. The string is
 *  copied. Returns PUCI_OK or PUCI_ERR_NOMEM. */
puci_status puci_session_set_model(puci_session *sess, const char *model);

/** Clears the conversation so the next send starts a fresh context, keeping the
 *  provider config and enabled tools. The system prompt, if any, is restored.
 *  Returns PUCI_OK. */
puci_status puci_session_clear(puci_session *sess);

/** Returns the session's enabled tool names as an array owned by the session,
 *  with the count written to *count. Valid until the session is mutated or
 *  destroyed. Returns NULL and sets *count to 0 if there are none. */
const char *const *puci_session_tools(const puci_session *sess, size_t *count);

/** Exports session transcript and metadata to a structured JSON string.
 *  Allocated in *out_json with malloc(); caller must free with free(). */
puci_status puci_session_export_json(const puci_session *sess, char **out_json);

/** Imports session transcript from a JSON string into the session, restoring
 *  conversation history, model, and token metrics. Validates history. */
puci_status puci_session_import_json(puci_session *sess, const char *json_str);

/** Calculates estimated character and token count of current session history. */
void puci_session_estimate_context(const puci_session *sess, size_t *out_chars,
                                   size_t *out_est_tokens);

/** Saves session transcript to a JSON file atomically (respecting path guards). */
puci_status puci_session_save_file(const puci_session *sess, const char *filepath);

/** Loads session transcript from a JSON file. */
puci_status puci_session_load_file(puci_session *sess, const char *filepath);

/** Sets an explanation/feedback string for a denied tool call.
 *  When the approval callback returns PUCI_APPROVAL_DENY, this feedback is
 *  forwarded to the model in the tool-result payload. Cleared after use. */
puci_status puci_session_set_approval_feedback(puci_session *sess, const char *feedback);

/** Sets whether the session is in plan / dry-run mode (1 = enabled, 0 = disabled). */
puci_status puci_session_set_plan_mode(puci_session *sess, int plan_mode);

/** Returns 1 if session is in plan / dry-run mode, 0 otherwise. */
int puci_session_get_plan_mode(const puci_session *sess);

/** Sets the context budget in bytes for automatic compaction (0 = default
 *  131072). When the serialized history exceeds this, the oldest verbose tool
 *  results are pruned before the next HTTP dispatch. Thread: owning thread. */
puci_status puci_session_set_context_budget(puci_session *sess, size_t budget_bytes);

/** Returns the effective context budget in bytes (never 0). */
size_t puci_session_get_context_budget(const puci_session *sess);

/** Returns the current serialized context size in bytes (chars). */
size_t puci_session_context_bytes(const puci_session *sess);

/** Compacts the session history to fit within the budget.
 *  Head (system prompt + first turn) and the last 4 turns are preserved.
 *  Older verbose TOOL results are truncated with a marker and duplicate
 *  tool outputs are deduplicated. Sets *out_pruned to bytes removed (0 if
 *  already within budget). Returns PUCI_OK or PUCI_ERR_NOMEM. */
puci_status puci_session_compact_context(puci_session *sess, size_t *out_pruned);

/* Ownership contract: the caller owns session handles and destroys them with
 * puci_session_destroy. Session-returned strings, arrays, descriptors, and
 * event payload pointers are borrowed until the documented next operation;
 * callers must copy them if they need to retain them. Output pointers are
 * caller-provided. All fallible session operations return puci_status. */

#ifdef __cplusplus
}
#endif

#endif /* PUCI_SESSION_H */
