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
 * taking a session must be called from that thread. The puci_session_call
 * (pollable) and puci_session_send (blocking) variants are mutually exclusive
 * ways to drive one session. The provided event callback may also be invoked
 * on the caller's thread.
 */

#include "puci/types.h"
#include "puci/approval.h"

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
    size_t max_tool_iterations;     /**< cap on tool calls per turn, 0 = default(32) */
    puci_approval_cb approve_cb;    /**< tool approval policy hook, invoked before each tool runs (optional) */
    void *approve_userdata;         /**< passed to approve_cb */
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
 *  another thread. */
puci_status puci_session_send(puci_session *sess, const char *message);

/** Non-blocking entry point designed to be polled by an external event loop.
 *  Call puci_session_poll_fd() to obtain a file descriptor (may be -1 if the
 *  session is idle). Returns PUCI_ERR_OUT_OF_STATE if called in the wrong
 *  stage. */
puci_status puci_session_poll(puci_session *sess);

/** Returns a file descriptor usable with select/poll/epoll, or -1 if the
 *  session currently has no pending I/O. The fd is owned by the session and is
 *  only valid until the next call to a session function. */
int puci_session_poll_fd(const puci_session *sess);

/** Requests cancellation of any in-flight work on the session. Safe to call
 *  from any thread. Returns PUCI_ERR_OUT_OF_STATE if the session is idle. */
puci_status puci_session_cancel(puci_session *sess);

#ifdef __cplusplus
}
#endif

#endif /* PUCI_SESSION_H */