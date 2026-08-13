#ifndef PUCI_INTERNAL_H
#define PUCI_INTERNAL_H

/*
 * puci_internal.h - libpuci internal data model and engine interfaces.
 *
 * This header is PRIVATE to libpuci. It is never installed and never included
 * from src/cli/. Everything here mirrors the public opaque handles declared in
 * include/puci/.
 */

#include "puci/puci.h"

#include "util/arena.h"
#include "util/strbuf.h"
#include "util/alloc.h"

/* ------------------------------------------------------------------ */
/* Forward declarations.                                               */
/* ------------------------------------------------------------------ */

typedef struct puci_provider_vtable puci_provider_vtable;

/* ------------------------------------------------------------------ */
/* Messages.                                                           */
/* ------------------------------------------------------------------ */

/* A single tool call fragment being assembled from a stream, and as persisted
 * in an assistant message. name/args are accumulated via strbuf. Named
 * puci_internal_tool_call to avoid clashing with the public puci_tool_call. */
typedef struct puci_internal_tool_call {
    char *id;    /* NUL-terminated id, may be NULL until first seen */
    puci_strbuf name;
    puci_strbuf args; /* accumulated argument JSON (valid JSON at completion) */
} puci_internal_tool_call;

typedef struct puci_msg {
    puci_role role;
    char *content;      /* NUL-terminated content or NULL */
    char *tool_call_id; /* only meaningful when role == PUCI_ROLE_TOOL */
    puci_internal_tool_call *tool_calls;
    size_t ntool_calls;
    size_t tool_calls_cap;
} puci_msg;

/* ------------------------------------------------------------------ */
/* Tools.                                                              */
/* ------------------------------------------------------------------ */

/* A registered tool. The public spec is embedded at offset 0 so it can be
 * returned directly (a pointer to the spec is a valid puci_tool_spec*). */
struct puci_tool {
    puci_tool_spec spec; /* owned name/description/input_schema strings */
    puci_tool_invoke_cb invoke; /* NULL for tools resolved by name at run time */
    void *userdata;
    int is_builtin;
};

/* ------------------------------------------------------------------ */
/* Provider vtable (wire-format backend).                              */
/* ------------------------------------------------------------------ */

typedef struct puci_session puci_session;
typedef struct puci_turn puci_turn;

struct puci_provider_vtable {
    const char *name;
    const char *endpoint; /* URL path appended to base_url, e.g. "/chat/completions" */
    int supports_tools;

    /* Serialize an OpenAI-style message array (msgs_json is a valid JSON array
     * of message objects) plus an optional tools array (tools_json may be NULL)
     * into the provider's chat request body, written to out. */
    puci_status (*build_request)(const puci_session *sess,
                                 const char *msgs_json, const char *tools_json,
                                 puci_strbuf *out);

    /* Parse one SSE data payload (a JSON object) and fold it into turn.
     * scratch is reset before each call by the engine. */
    puci_status (*parse_stream)(const puci_session *sess, puci_arena *scratch,
                                const char *data, size_t len, puci_turn *turn);
};

struct puci_provider {
    char *name; /* owned */
    const puci_provider_vtable *vtable;
    void *priv;
};

/* ------------------------------------------------------------------ */
/* Turn accumulation state (one assistant generation).                 */
/* ------------------------------------------------------------------ */

struct puci_turn {
    puci_strbuf text;          /* accumulated assistant text */
    puci_strbuf reasoning;     /* accumulated reasoning content (kept separate) */
    puci_internal_tool_call *tool_calls; /* tool calls being assembled */
    size_t ntool_calls;
    size_t tool_calls_cap;
    int had_tool_calls;
    int had_reasoning;
    int stream_done;           /* [DONE] or finish_reason seen */
    int finish_reason_stop;    /* normal stop */
    long long prompt_tokens;   /* reported usage (this turn), 0 if unspecified */
    long long completion_tokens;
    long long cached_tokens;
};

/* ------------------------------------------------------------------ */
/* Session.                                                            */
/* ------------------------------------------------------------------ */

struct puci_http_conn; /* net/http.h; opaque persistent HTTP connection */
struct puci_http_req;  /* net/http.h; opaque in-flight streaming request */
struct puci_sse_parser; /* net/sse.h; opaque SSE stream parser */

struct puci_session {
    struct puci_context *ctx;
    puci_allocator alloc; /* resolved context allocator (copy) */

    puci_provider_config cfg; /* owned copies of strings */

    puci_session_event_cb event_cb;
    void *event_userdata;

    puci_approval_cb approve_cb;
    void *approve_userdata;

    char *system_prompt;
    puci_msg *messages;
    size_t nmessages;
    size_t messages_cap;

    size_t max_tool_iterations;

    long long total_prompt_tokens;
    long long total_completion_tokens;
    long long total_cached_tokens;

    char **tool_names; /* names of tools enabled for this session */
    size_t ntools;
    size_t tools_cap;

    puci_arena scratch;
    int running;
    /* Set (non-zero) by puci_session_cancel. volatile sig_atomic_t so it can be
     * written from a signal handler (async-signal-safe). Read on the driver
     * thread at every cancellation point. */
    volatile sig_atomic_t cancelled;
    /* Readable end of the cancel transport (eventfd on Linux, self-pipe
     * elsewhere); -1 until first created. Written by puci_session_cancel. */
    int cancel_fd;
    /* Write end used to notify the transport. Equals cancel_fd for eventfd. */
    int cancel_fd_write;

    char *errmsg; /* last error message, owned */

    /* Connection-dirty flag for the HTTP layer: set when the previous transfer
     * was aborted/cancelled/incomplete so the next request forces a fresh
     * connection; cleared once consumed. Preserves keep-alive for normal turns. */
    int conn_dirty;
    /* Persistent HTTP connection (owns a libcurl easy+multi handle), reused
     * across turns so the TCP/TLS connection can be kept alive. NULL until
     * first needed; freed in puci_session_destroy. */
    struct puci_http_conn *http_conn;

    /* ---- Asynchronous turn state machine ---- */
    /* Non-NULL between puci_agent_begin_turn and turn teardown. Owns the
     * resumable state, the in-flight HTTP request, the SSE parser, the event
     * queue and the request buffers. Freed (and the turn finalised) by
     * puci_agent_process when the terminal state is reached. */
    struct puci_turn_run *run;
    /* True while a turn is active (an asynchronous turn has begun and has not
     * yet reached a terminal state). Blocking puci_session_send sets this too. */
    int async_active;
    /* Reentrancy guard: set for the duration of puci_agent_process, so calling
     * process() (or begin_turn) from inside an event callback is rejected with
     * PUCI_ERR_OUT_OF_STATE rather than corrupting state. */
    int in_process;

    /* Event queue (singly-linked FIFO) produced by the state machine and
     * drained by puci_agent_next_event. Owned by the session so events that
     * signal a terminal turn (TURN_COMPLETE / TURN_CANCELLED / ERROR) survive
     * the teardown of `run`. Every string in a node is owned by the node. */
    struct puci_evnode *ev_head, *ev_tail;

    /* Reusable buffer that backs the text pointers returned by
     * puci_session_next_event(). One buffer per session (never per token).
     * Reset (length zeroed, capacity retained) at the start of every
     * next_event() and process() call before any new data is written, so a
     * pointer handed out earlier is invalidated exactly as documented. The
     * node's own payload storage is freed only after its text has been copied
     * in here. Capacity retention makes steady-state delivery allocation-free. */
    char *event_text_buf;
    size_t event_text_len;
    size_t event_text_cap;

    /* Terminal status of the most recently completed turn, retained until the
     * next turn begins. Read by puci_session_last_status. */
    puci_status last_status;
};

/* ---- Asynchronous agent loop ------------------------------------------- *
 * The turn state machine and the event queue it feeds. One `run` per session,
 * created by puci_agent_begin_turn and torn down when the turn reaches a
 * terminal state. */

typedef enum puci_turn_state {
    PUCI_TS_IDLE,            /* no turn in flight */
    PUCI_TS_BUILD_REQUEST,   /* serializing messages + first request setup */
    PUCI_TS_AWAITING_CONNECT,/* HTTP connecting / TLS handshake (curl in flight) */
    PUCI_TS_STREAMING,       /* SSE bytes flowing; curl still in flight */
    PUCI_TS_STREAM_COMPLETE, /* transfer done; finalise the assistant message */
    PUCI_TS_DISPATCHING_TOOLS,  /* run tool calls the model requested */
    PUCI_TS_RETRY_BACKOFF,   /* waiting out a retry deadline before retrying */
    PUCI_TS_TURN_DONE,       /* normal completion, waiting for teardown */
    PUCI_TS_FATAL            /* unrecoverable error, waiting for teardown */
} puci_turn_state;

/* One queued event for puci_agent_next_event. Owns every string it carries so
 * the consumer may keep it as long as it likes and free via
 * puci_agent_next_event (which frees the slot in-place). */
typedef struct puci_evnode {
    puci_event_kind ev;
    struct puci_evnode *next;
    /* TEXT payloads: single owned string (TOKEN_DELTA, REASONING_DELTA,
     * TURN_COMPLETE, ERROR). */
    char *text;
    /* TOOL_CALL_START: owned name/id/args_json. */
    char *tc_name, *tc_id, *tc_args;
    /* TOOL_CALL_END: owned call_id/content, plus is_error. */
    char *tr_call_id, *tr_content;
    int tr_is_error;
      long long prompt_tokens, completion_tokens, total_tokens;
      const char *finish_reason;
      puci_status error_status;
} puci_evnode;

/* The resumable per-turn engine state. Non-NULL (`sess->run`) while a turn is
 * active; freed by the teardown path when the turn reaches a terminal state. */
struct puci_turn_run {
    puci_turn_state state;

    int iter;
    int max_iter;

    /* In-flight streaming request (NULL except between AWAITING_CONNECT and a
     * terminal result). */
    struct puci_http_req *req;
    struct puci_sse_parser *parser;
    struct puci_turn turn;   /* text/reasoning/tool_calls accumulation */
    size_t prev_text_len;
    size_t prev_reasoning_len;
    puci_status first_err;
    int at_least_one_event;
    int http_status;

    /* Owned committed-stream text (the assistant payload text) carried from
     * STREAM_COMPLETE into finalise; owned, freed at teardown. */
    char *stream_text;
    int want_tools;

    /* Owned request buffers; freed at teardown. */
    char *req_body;
    char *url;
    char *auth;

    /* Retry backoff deadline (monotonic ms), or -1 if none pending. */
    long long backoff_deadline_ms;
    int backoff_pending;

    /* Cancellation token passed (by borrowed pointer) to the in-flight HTTP
     * request. It must outlive the request, so it lives here rather than on a
     * stack frame in run_build_request (the request stores it by pointer).
     * Set from the session at request build time; the flag/fd are stable for
     * the session. */
    puci_cancel_token cancel_token;

    /* Whether cancellation was requested mid-turn (latched). */
    int cancelled_latched;

    /* Terminal status carried out to the caller of the driving loop. */
    puci_status terminal_status;
    const char *done_text;   /* points into committed history; not owned */

    int in_process;          /* reentrancy guard for the run */
};

void turn_run_eventq_push(struct puci_session *sess, struct puci_evnode *n);
/* Forces teardown of an in-flight run (if any) and flushes the event queue.
 * Used on session destroy so an agent can be torn down mid-turn (streaming or
 * tool execution) without leaks. NOT called during normal turn completion. */
void turn_run_force_teardown(struct puci_session *sess);

/* ------------------------------------------------------------------ */
/* Context.                                                            */
/* ------------------------------------------------------------------ */

struct puci_context {
    puci_allocator alloc; /* resolved allocator (copy) */

    puci_log_cb log_cb;
    void *log_userdata;
    puci_log_level log_level;

    char *errmsg; /* last error message, owned */

    puci_provider **providers;
    size_t nproviders;
    size_t providers_cap;

    puci_provider_config *configs; /* provider configs keyed by name */
    size_t nconfigs;
    size_t configs_cap;

    puci_tool **tools;
    size_t ntools;
    size_t tools_cap;

    /* Hosts for which TLS verification may be relaxed (opt-in allowlist).
     * Populated from PUCI_INSECURE_TLS_HOSTS and/or config. */
    char **insecure_hosts;
    size_t ninsecure_hosts;
    size_t insecure_hosts_cap;

    /* Credential values to redact (replace with "***") before any string
     * reaches a log callback, including HTTP header dumps. */
    char **secrets;
    size_t nsecrets;
    size_t secrets_cap;
};

/* ------------------------------------------------------------------ */
/* Internal function declarations.                                     */
/* ------------------------------------------------------------------ */

/* providers/registry.c */
puci_status puci_provider_register_internal(struct puci_context *ctx,
                                            const puci_provider_vtable *vt);

/* providers/openai.c */
extern const puci_provider_vtable puci_openai_provider;

/* core/agent.c - the non-blocking agent state machine + blocking wrapper */
/* Blocking convenience wrapper: pumps the state machine. */
puci_status puci_agent_run_turn(struct puci_session *sess, const char *msg);
/* Asynchronous, poll-loop-friendly entry points. */
puci_status puci_agent_begin_turn(struct puci_session *sess, const char *msg);
puci_status puci_agent_get_fds(struct puci_session *sess, struct pollfd *fds,
                               size_t cap, size_t *out_count);
puci_status puci_agent_next_timeout_ms(struct puci_session *sess, int *out_ms);
puci_status puci_agent_process(struct puci_session *sess,
                               const struct pollfd *fds, size_t count);
int puci_agent_next_event(struct puci_session *sess, puci_event *out);
int puci_agent_cancel_fd(struct puci_session *sess);
/* Current capacity (bytes) of the session's reusable event text buffer. Test
 * hook / internal accessor; not part of the public API. */
size_t puci_session_event_text_cap(const struct puci_session *sess);
/* Terminal status of the most recently completed turn. */
puci_status puci_agent_last_status(const struct puci_session *sess);

/* core/session.c - internal message helpers used by the agent loop */
puci_status session_msg_begin(puci_session *sess, puci_role role,
                              const char *content);
puci_status session_msg_set_tool_call_id(puci_session *sess,
                                         const char *call_id);
puci_status session_msg_finish_assistant(puci_session *sess, const char *text,
                                         puci_internal_tool_call *tcs,
                                         size_t ntcs);
void session_set_err(puci_session *sess, const char *msg);
puci_status session_serialize_messages(puci_session *sess, puci_strbuf *out);
puci_status session_serialize_tools(puci_session *sess, puci_strbuf *out);

/* Cancel transport helpers (core/session.c). */
int session_ensure_cancel_fd(puci_session *sess);
void session_build_cancel_token(puci_session *sess, puci_cancel_token *tok);
/* Resets the cancel flag and drains the cancel fd (see session.c). Must be
 * called before beginning a fresh turn so a prior cancellation does not bleed
 * into it. Not async-signal-safe. */
void session_cancel_clear(puci_session *sess);
/* Synthesises a tool result for every assistant tool call that is not followed
 * by a matching tool result message, so the history stays valid for the next
 * turn even after an interruption. Returns PUCI_OK or PUCI_ERR_NOMEM. */
puci_status session_fixup_dangling_tool_calls(puci_session *sess);

/* json/json.h helpers for message serialization */
typedef struct puci_json puci_json;

/* core/context.c - logging */
void puci_logf(struct puci_context *ctx, puci_log_level level, const char *fmt, ...);
/* Registers a credential value to be redacted from log output. */
void puci_secret_add(struct puci_context *ctx, const char *value);

/* tools/ internal registry helpers shared with built-in tools */
int puci_tools_lookup(struct puci_context *ctx, const char *name,
                      struct puci_tool **out);

#endif /* PUCI_INTERNAL_H */
