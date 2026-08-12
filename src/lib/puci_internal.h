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
    int cancelled;

    char *errmsg; /* last error message, owned */
};

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

/* core/agent.c */
puci_status puci_agent_run_turn(struct puci_session *sess, const char *msg);

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