#ifndef PUCI_TYPES_H
#define PUCI_TYPES_H

/*
 * puci/types.h - public opaque types and enums.
 *
 * All types here are opaque to consumers. Their layouts are private and may
 * change at any time without an ABI break; the ABI guarantee is provided by
 * the function signatures in the other public headers, which operate on these
 * handles exclusively.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque context: owns allocator, logging callback, and global state. */
typedef struct puci_context puci_context;

/** Opaque session: an independent conversation + agent loop. */
typedef struct puci_session puci_session;

/** Opaque tool handle used to register custom tools. */
typedef struct puci_tool puci_tool;

/** Opaque provider handle. */
typedef struct puci_provider puci_provider;

/** Status codes returned by every fallible public function. */
typedef enum puci_status {
    PUCI_OK = 0,
    PUCI_ERR_UNKNOWN,       /**< generic / unexpected error */
    PUCI_ERR_NOMEM,         /**< allocation failed */
    PUCI_ERR_INVALID_ARG,   /**< a required argument is NULL or invalid */
    PUCI_ERR_NOT_FOUND,     /**< a requested entity does not exist */
    PUCI_ERR_IO,            /**< underlying I/O failure */
    PUCI_ERR_NETWORK,       /**< network / HTTP failure */
    PUCI_ERR_PROTOCOL,      /**< unexpected or malformed upstream response */
    PUCI_ERR_UNAUTHORIZED,  /**< authentication required or failed */
    PUCI_ERR_TIMEOUT,       /**< an operation timed out */
    PUCI_ERR_TOOL_FAILED,   /**< a tool invocation returned an error */
    PUCI_ERR_OUT_OF_STATE,  /**< an operation was performed in the wrong state */
    PUCI_ERR_CANCELLED,     /**< the operation was cancelled by the caller */
} puci_status;

/** Allocator vtable. All three callbacks may be NULL to use the standard
 *  library (malloc/realloc/free). If provided, they must behave like their
 *  libc equivalents in terms of alignment and the realloc shrink-grow
 *  contract. */
typedef struct puci_allocator {
    void *(*malloc_fn)(void *ud, size_t size);
    void *(*realloc_fn)(void *ud, void *ptr, size_t size);
    void (*free_fn)(void *ud, void *ptr);
    void *userdata; /**< opaque pointer passed to every callback */
} puci_allocator;

/** Log levels passed to the logging callback. */
typedef enum puci_log_level {
    PUCI_LOG_TRACE = 0,
    PUCI_LOG_DEBUG,
    PUCI_LOG_INFO,
    PUCI_LOG_WARN,
    PUCI_LOG_ERROR,
} puci_log_level;

/** Logging callback. Thread-safe: may be invoked from any thread. */
typedef void (*puci_log_cb)(void *userdata, puci_log_level level, const char *msg);

/** Roles in a conversation message. */
typedef enum puci_role {
    PUCI_ROLE_SYSTEM = 0,
    PUCI_ROLE_USER,
    PUCI_ROLE_ASSISTANT,
    PUCI_ROLE_TOOL,
} puci_role;

/** Events emitted by the agent loop through the session callback. */
typedef enum puci_event {
    /** A text delta has been produced (streaming). ctx = const char*. */
    PUCI_EVENT_TOKEN_DELTA = 0,
    /** A tool call has started. ctx = const puci_tool_call*. */
    PUCI_EVENT_TOOL_CALL_START,
    /** A tool call has finished. ctx = const puci_tool_result*. */
    PUCI_EVENT_TOOL_CALL_END,
    /** The current turn is complete. ctx = const char* (final text, may be NULL). */
    PUCI_EVENT_TURN_COMPLETE,
    /** An error occurred. ctx = const char* (message). */
    PUCI_EVENT_ERROR,
    /** A reasoning/thinking delta from a reasoning model, surfaced separately
     *  from the final answer so consumers may display or discard it. Never
     *  concatenated into the answer text. ctx = const char* (recent delta text);
     *  consumers accumulate across deltas. */
    PUCI_EVENT_REASONING_DELTA,
} puci_event;

/** A tool call being executed by the agent loop. */
typedef struct puci_tool_call {
    const char *name;    /**< tool name, owned by session, valid for the event */
    const char *id;      /**< opaque tool call id, owned by session */
    const char *args_json; /**< arguments as JSON text, owned by session */
} puci_tool_call;

/** The result of a tool call. */
typedef struct puci_tool_result {
    const char *call_id;  /**< id of the originating tool call */
    const char *content;  /**< result text */
    int is_error;         /**< nonzero if the tool failed */
} puci_tool_result;

/** Config for a single provider connection. */
typedef struct puci_provider_config {
    const char *name;       /**< provider name, e.g. "openai" */
    const char *base_url;   /**< API base URL including scheme and host */
    const char *api_key;    /**< optional API key */
    const char *model;      /**< model identifier */
    int timeout_ms;         /**< per-request timeout in ms, 0 = default */
    const char *extra_headers; /**< optional JSON object of extra HTTP headers */

    /* New fields (keep appended; extending a public struct is ABI-visible). */

    /** Set to 0 to explicitly allow TLS verification relaxation for this
     *  provider's host. Default is 1 (verify). Relaxing TLS is a MITM risk and
     *  must be gated behind a host allowlist (see PUCI_INSECURE_TLS_HOSTS) and
     *  never enabled silently. */
    int tls_verify;
    /** Path to a custom CA bundle (PEM) used instead of the default store.
     *  The preferred alternative to disabling verification. May be NULL. */
    const char *ca_bundle;
    /** JSON object string of extra TOP-LEVEL request fields merged verbatim
     *  into the request body. Keys here override generated fields. May be NULL. */
    const char *extra_body;
    /** Idle/stream timeout in ms: if no bytes arrive within this window the
     *  request is aborted with a distinct idle-timeout error. 0 = default
     *  (180000 ms). */
    int stream_idle_timeout_ms;
} puci_provider_config;

/** Options passed to puci_context_create. Zero-init and set only what you need. */
typedef struct puci_context_options {
    puci_allocator allocator;   /**< optional custom allocator */
    puci_log_cb log_cb;         /**< optional logging callback */
    void *log_userdata;         /**< passed to log_cb */
    puci_log_level log_level;   /**< minimum level to emit */
} puci_context_options;

#ifdef __cplusplus
}
#endif

#endif /* PUCI_TYPES_H */