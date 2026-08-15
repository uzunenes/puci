/*
 * core/session.c - session lifecycle and message management.
 */

#include "puci_internal.h"
#include "tools/path_guard.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "json/json.h"
#include "net/http.h"

#ifdef __linux__
#include <sys/eventfd.h>
#endif

/* --- Message lifecycle helpers (used by the agent loop) --- */

static void free_tool_call_content(puci_session *sess, puci_internal_tool_call *tc) {
    if (!tc) {
        return;
    }
    if (tc->id) {
        puci_alloc_free(&sess->alloc, tc->id);
    }
    puci_strbuf_destroy(&tc->name);
    puci_strbuf_destroy(&tc->args);
}

static void free_msg(puci_session *sess, puci_msg *m) {
    if (!m) {
        return;
    }
    if (m->content) {
        puci_alloc_free(&sess->alloc, m->content);
    }
    if (m->tool_call_id) {
        puci_alloc_free(&sess->alloc, m->tool_call_id);
    }
    for (size_t i = 0; i < m->ntool_calls; i++) {
        free_tool_call_content(sess, &m->tool_calls[i]);
    }
    if (m->tool_calls) {
        puci_alloc_free(&sess->alloc, m->tool_calls);
    }
    memset(m, 0, sizeof(*m));
}

/* Appends a new message. role/content: content is copied (may be NULL for
 * assistant tool-call-only messages). tool_call_id optional. */
puci_status session_msg_begin(puci_session *sess, puci_role role,
                              const char *content) {
    if (sess->nmessages == sess->messages_cap) {
        size_t ncap = sess->messages_cap ? sess->messages_cap * 2 : 8;
        puci_msg *nm = (puci_msg *)puci_alloc_malloc(&sess->alloc,
                                                     ncap * sizeof(puci_msg));
        if (!nm) {
            return PUCI_ERR_NOMEM;
        }
        if (sess->messages) {
            memcpy(nm, sess->messages, sess->nmessages * sizeof(puci_msg));
            puci_alloc_free(&sess->alloc, sess->messages);
        }
        sess->messages = nm;
        sess->messages_cap = ncap;
    }
    puci_msg *m = &sess->messages[sess->nmessages];
    memset(m, 0, sizeof(*m));
    m->role = role;
    if (content) {
        m->content = puci_alloc_strdup(&sess->alloc, content);
        if (!m->content) {
            return PUCI_ERR_NOMEM;
        }
    }
    sess->nmessages++;
    return PUCI_OK;
}

static puci_msg *last_msg(puci_session *sess) {
    if (sess->nmessages == 0) {
        return NULL;
    }
    return &sess->messages[sess->nmessages - 1];
}

/* Sets tool_call_id on the last message (assumes it was begun as role TOOL). */
puci_status session_msg_set_tool_call_id(puci_session *sess,
                                         const char *call_id) {
    puci_msg *m = last_msg(sess);
    if (!m || !call_id) {
        return PUCI_ERR_INVALID_ARG;
    }
    m->tool_call_id = puci_alloc_strdup(&sess->alloc, call_id);
    return m->tool_call_id ? PUCI_OK : PUCI_ERR_NOMEM;
}

/* Finalizes an assistant message that carries tool calls. The turn's tool call
 * slots are moved (ownership transferred) into the new message. */
puci_status session_msg_finish_assistant(puci_session *sess, const char *text,
                                         puci_internal_tool_call *tcs, size_t ntcs) {
    puci_msg *m = last_msg(sess);
    if (!m) {
        return PUCI_ERR_OUT_OF_STATE;
    }
    if (text) {
        if (m->content) {
            puci_alloc_free(&sess->alloc, m->content);
            m->content = NULL;
        }
        m->content = puci_alloc_strdup(&sess->alloc, text);
        if (!m->content) {
            return PUCI_ERR_NOMEM;
        }
    }
    if (ntcs > 0) {
        /* Move the tool-call slots into the message. */
        m->tool_calls = tcs;
        m->ntool_calls = ntcs;
        m->tool_calls_cap = ntcs;
    }
    return PUCI_OK;
}

/* --- Public session API --- */

puci_status puci_session_create(const puci_context *ctx,
                                const puci_session_options *opts,
                                puci_session **out) {
    if (!ctx || !out) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (!opts || !opts->provider.name) {
        return PUCI_ERR_INVALID_ARG;
    }
    puci_session *sess =
        (puci_session *)puci_alloc_calloc(&ctx->alloc, 1, sizeof(puci_session));
    if (!sess) {
        return PUCI_ERR_NOMEM;
    }
    sess->ctx = (puci_context *)ctx;
    puci_alloc_resolve(&sess->alloc, &ctx->alloc);
    sess->cancel_fd = -1;
    sess->cancel_fd_write = -1;

    /* Copy provider config. Defaults resolve from the context's stored config
     * for the named provider. */
    const char *pname = opts->provider.name;
    sess->cfg.name = puci_alloc_strdup(&sess->alloc, pname);
    if (!sess->cfg.name) {
        puci_alloc_free(&sess->alloc, sess);
        return PUCI_ERR_NOMEM;
    }
    sess->cfg.model =
        opts->provider.model
            ? puci_alloc_strdup(&sess->alloc, opts->provider.model)
            : NULL;
    sess->cfg.base_url =
        opts->provider.base_url
            ? puci_alloc_strdup(&sess->alloc, opts->provider.base_url)
            : NULL;
    sess->cfg.api_key =
        opts->provider.api_key
            ? puci_alloc_strdup(&sess->alloc, opts->provider.api_key)
            : NULL;
    sess->cfg.timeout_ms = opts->provider.timeout_ms;
    sess->cfg.tls_verify = opts->provider.tls_verify;
    /* Default the per-stream idle timeout to 180s when not configured. */
    sess->cfg.stream_idle_timeout_ms =
        opts->provider.stream_idle_timeout_ms != 0
            ? opts->provider.stream_idle_timeout_ms
            : 180000;
    sess->cfg.ca_bundle =
        opts->provider.ca_bundle
            ? puci_alloc_strdup(&sess->alloc, opts->provider.ca_bundle)
            : NULL;
    if (opts->provider.ca_bundle && !sess->cfg.ca_bundle) {
        puci_session_destroy(sess);
        return PUCI_ERR_NOMEM;
    }
    sess->cfg.extra_body =
        opts->provider.extra_body
            ? puci_alloc_strdup(&sess->alloc, opts->provider.extra_body)
            : NULL;
    if (opts->provider.extra_body && !sess->cfg.extra_body) {
        puci_session_destroy(sess);
        return PUCI_ERR_NOMEM;
    }

    /* Copy system prompt. */
    if (opts->system_prompt) {
        sess->system_prompt = puci_alloc_strdup(&sess->alloc, opts->system_prompt);
        if (!sess->system_prompt) {
            puci_session_destroy(sess);
            return PUCI_ERR_NOMEM;
        }
    }
    sess->event_cb = opts->event_cb;
    sess->event_userdata = opts->event_userdata;
    sess->approve_cb = opts->approve_cb;
    sess->approve_userdata = opts->approve_userdata;
    sess->max_tool_iterations =
        opts->max_tool_iterations ? opts->max_tool_iterations : 32;
    sess->plan_mode = opts->plan_mode ? 1 : 0;
    sess->tool_timeout_ms = opts->tool_timeout_ms > 0 ? opts->tool_timeout_ms : 60000;
    sess->approval_feedback = NULL;
    {
        const char *env = getenv("PUCI_CONTEXT_BUDGET");
        if (env && env[0]) {
            long v = strtol(env, NULL, 10);
            if (v > 0 && v < 100 * 1024 * 1024) sess->context_budget_bytes = (size_t)v;
        }
    }

    /* Default the session's enabled tools to all context tools. */
    for (size_t i = 0; i < ctx->ntools; i++) {
        if (!ctx->tools[i] || !ctx->tools[i]->spec.name) {
            continue;
        }
        if (sess->ntools == sess->tools_cap) {
            size_t ncap = sess->tools_cap ? sess->tools_cap * 2 : 8;
            char **nt = (char **)puci_alloc_malloc(&sess->alloc,
                                                   ncap * sizeof(char *));
            if (!nt) {
                puci_session_destroy(sess);
                return PUCI_ERR_NOMEM;
            }
            if (sess->tool_names) {
                memcpy(nt, sess->tool_names, sess->ntools * sizeof(char *));
                puci_alloc_free(&sess->alloc, sess->tool_names);
            }
            sess->tool_names = nt;
            sess->tools_cap = ncap;
        }
        char *n = puci_alloc_strdup(&sess->alloc, ctx->tools[i]->spec.name);
        if (!n) {
            puci_session_destroy(sess);
            return PUCI_ERR_NOMEM;
        }
        sess->tool_names[sess->ntools++] = n;
    }

    puci_arena_init(&sess->scratch, &sess->alloc, 0);
    *out = sess;
    return PUCI_OK;
}

void puci_session_destroy(puci_session *sess) {
    if (!sess) {
        return;
    }
    /* Tear down any in-flight turn (streaming or tool execution) and flush the
     * event queue, so destroying an agent mid-turn leaks nothing (requirement 8).
     * Does so before closing the persistent HTTP connection it may be using. */
    turn_run_force_teardown(sess);
    puci_arena_destroy(&sess->scratch);
    for (size_t i = 0; i < sess->nmessages; i++) {
        free_msg(sess, &sess->messages[i]);
    }
    if (sess->messages) {
        puci_alloc_free(&sess->alloc, sess->messages);
    }
    for (size_t i = 0; i < sess->ntools; i++) {
        if (sess->tool_names[i]) {
            puci_alloc_free(&sess->alloc, sess->tool_names[i]);
        }
    }
    if (sess->tool_names) {
        puci_alloc_free(&sess->alloc, sess->tool_names);
    }
    if (sess->errmsg) {
        puci_alloc_free(&sess->alloc, sess->errmsg);
    }
    if (sess->system_prompt) {
        puci_alloc_free(&sess->alloc, sess->system_prompt);
    }
    if (sess->cfg.name) {
        puci_alloc_free(&sess->alloc, (void *)sess->cfg.name);
    }
    if (sess->cfg.model) {
        puci_alloc_free(&sess->alloc, (void *)sess->cfg.model);
    }
    if (sess->cfg.base_url) {
        puci_alloc_free(&sess->alloc, (void *)sess->cfg.base_url);
    }
    if (sess->cfg.api_key) {
        puci_alloc_free(&sess->alloc, (void *)sess->cfg.api_key);
    }
    if (sess->cfg.ca_bundle) {
        puci_alloc_free(&sess->alloc, (void *)sess->cfg.ca_bundle);
    }
    if (sess->cfg.extra_body) {
        puci_alloc_free(&sess->alloc, (void *)sess->cfg.extra_body);
    }
    if (sess->event_text_buf) {
        puci_alloc_free(&sess->alloc, sess->event_text_buf);
    }
    if (sess->approval_feedback) {
        puci_alloc_free(&sess->alloc, sess->approval_feedback);
    }
    /* Close the cancel transport (eventfd or self-pipe). */
    if (sess->cancel_fd >= 0) {
        close(sess->cancel_fd);
    }
    if (sess->cancel_fd_write >= 0 && sess->cancel_fd_write != sess->cancel_fd) {
        close(sess->cancel_fd_write);
    }
    if (sess->http_conn) {
        puci_http_conn_destroy(sess->http_conn);
        sess->http_conn = NULL;
    }
    puci_alloc_free(&sess->alloc, sess);
}

const char *puci_session_error(const puci_session *sess) {
    return sess ? sess->errmsg : NULL;
}

/* Sets the session's error string (owned). */
void session_set_err(puci_session *sess, const char *msg) {
    if (sess->errmsg) {
        puci_alloc_free(&sess->alloc, sess->errmsg);
        sess->errmsg = NULL;
    }
    if (msg) {
        sess->errmsg = puci_alloc_strdup(&sess->alloc, msg);
    }
}

static void dispatch_public_event(puci_session *sess, const puci_event *event) {
    if (!sess || !event || !sess->event_cb) {
        return;
    }
    switch (event->kind) {
    case PUCI_EVENT_TOOL_CALL_START: {
        puci_tool_call call = {
            .name = event->payload.tool_call.name,
            .id = event->payload.tool_call.id,
            .args_json = event->payload.tool_call.args_json,
        };
        sess->event_cb(sess->event_userdata, sess, *event, &call);
        break;
    }
    case PUCI_EVENT_TOOL_CALL_END: {
        puci_tool_result result = {
            .call_id = event->payload.tool_result.call_id,
            .content = event->payload.tool_result.content,
            .is_error = event->payload.tool_result.is_error,
        };
        sess->event_cb(sess->event_userdata, sess, *event, &result);
        break;
    }
    default:
        sess->event_cb(sess->event_userdata, sess, *event, event->text);
        break;
    }
}

/* The blocking convenience API is a public-API driver over the same
 * non-blocking state machine used by embedders. */
puci_status puci_session_send(puci_session *sess, const char *message) {
    puci_status st = puci_session_begin_turn(sess, message);
    if (st != PUCI_OK) {
        return st;
    }
    for (;;) {
        int timeout_ms = -1;
        struct pollfd fds[64];
        size_t fd_count = 0;
        st = puci_session_next_timeout_ms(sess, &timeout_ms);
        if (st != PUCI_OK) {
            turn_run_force_teardown(sess);
            return st;
        }
        st = puci_session_get_fds(sess, fds, 64, &fd_count);
        if (st != PUCI_OK) {
            turn_run_force_teardown(sess);
            return st;
        }
        int wait_ms = timeout_ms < 0 ? 1000 : timeout_ms;
        if (wait_ms > 1000) {
            wait_ms = 1000;
        }
        int prc = poll(fd_count ? fds : NULL, (nfds_t)fd_count, wait_ms);
        if (prc < 0) {
            if (errno == EINTR) {
                continue;
            }
            turn_run_force_teardown(sess);
            return PUCI_ERR_IO;
        }
        st = puci_session_process(sess, fds, fd_count);

        puci_event event;
        while (puci_session_next_event(sess, &event)) {
            dispatch_public_event(sess, &event);
            if (event.kind == PUCI_EVENT_TURN_COMPLETE ||
                event.kind == PUCI_EVENT_TURN_CANCELLED ||
                event.kind == PUCI_EVENT_ERROR) {
                return puci_session_last_status(sess);
            }
        }
        if (st != PUCI_OK && st != PUCI_ERR_OUT_OF_STATE) {
            turn_run_force_teardown(sess);
            return st;
        }
    }
}

/* --- Non-blocking, poll-loop friendly agent driving ---
 * These forward directly to the state machine in core/agent.c; see the fd and
 * threading contract documented in include/puci/session.h. */

puci_status puci_session_begin_turn(puci_session *sess, const char *message) {
    session_cancel_clear(sess);
    return puci_agent_begin_turn(sess, message);
}

puci_status puci_session_get_fds(puci_session *sess, struct pollfd *fds,
                                 size_t cap, size_t *out_count) {
    return puci_agent_get_fds(sess, fds, cap, out_count);
}

puci_status puci_session_next_timeout_ms(puci_session *sess, int *out_ms) {
    return puci_agent_next_timeout_ms(sess, out_ms);
}

puci_status puci_session_process(puci_session *sess, const struct pollfd *fds,
                                 size_t count) {
    return puci_agent_process(sess, fds, count);
}

int puci_session_next_event(puci_session *sess, puci_event *out) {
    return puci_agent_next_event(sess, out);
}

puci_status puci_session_last_status(const puci_session *sess) {
    return puci_agent_last_status(sess);
}

/* --- Cancellation transport --- */

int session_ensure_cancel_fd(puci_session *sess) {
    if (!sess) {
        return -1;
    }
    if (sess->cancel_fd >= 0) {
        return sess->cancel_fd;
    }
#ifdef __linux__
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd >= 0) {
        sess->cancel_fd = fd;
        sess->cancel_fd_write = fd; /* eventfd is single-fd; write == read fd */
        return fd;
    }
#endif
    /* Fallback: self-pipe (POSIX, any platform). */
    int p[2];
    if (pipe(p) != 0) {
        return -1;
    }
    int fl = fcntl(p[0], F_GETFL, 0);
    fcntl(p[0], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(p[1], F_GETFL, 0);
    fcntl(p[1], F_SETFL, fl | O_NONBLOCK);
    (void)fcntl(p[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(p[1], F_SETFD, FD_CLOEXEC);
    sess->cancel_fd = p[0];
    sess->cancel_fd_write = p[1];
    return p[0];
}

void session_build_cancel_token(puci_session *sess, puci_cancel_token *tok) {
    tok->flag = &sess->cancelled;
    tok->fd = (sess->cancel_fd >= 0) ? sess->cancel_fd : -1;
}

/* Clears the latched cancel flag and drains any pending notification from the
 * cancel transport, so the next turn starts with a clean state. The flag must
 * be cleared before the next puci_agent_begin_turn; draining the fd is what
 * stops a host pump that polled on the cancel fd from busy-spinning on the
 * leftover readable byte. Not async-signal-safe (called from the driver
 * thread between turns, never from a signal handler). */
void session_cancel_clear(puci_session *sess) {
    if (!sess) {
        return;
    }
    sess->cancelled = 0;
    if (sess->cancel_fd >= 0) {
#ifdef __linux__
    eventfd_t value = 0;
    (void)eventfd_read(sess->cancel_fd, &value);
#else
        char tmp[64];
        while (read(sess->cancel_fd, tmp, sizeof(tmp)) > 0) {
            /* drain until EAGAIN */
        }
#endif
    }
}

puci_status puci_session_cancel(puci_session *sess) {
    if (!sess) {
        return PUCI_ERR_INVALID_ARG;
    }
    /* Async-signal-safe body: set a volatile flag and, if the cancel transport
     * exists, write one byte. No allocation, no locking, no libcurl. On an idle
     * session this is a harmless no-op that returns success. */
    sess->cancelled = 1;
    if (sess->cancel_fd_write >= 0) {
#ifdef __linux__
        (void)eventfd_write(sess->cancel_fd_write, 1);
#else
        static const char byte = 1;
        (void)write(sess->cancel_fd_write, &byte, 1);
#endif
    }
    return PUCI_OK;
}

int puci_session_cancel_fd(puci_session *sess) {
    return session_ensure_cancel_fd(sess);
}

#define PUCI_CANCEL_RESULT "{\"ok\":false,\"error\":\"cancelled by user\"}"

/* Synthesises a tool result for every assistant tool call not already followed
 * by a matching tool-result message, keeping the history valid for the next
 * turn even after an interruption (e.g. cancel before tools ran). */
puci_status session_fixup_dangling_tool_calls(puci_session *sess) {
    size_t n0 = sess->nmessages;
    for (size_t i = 0; i < n0; i++) {
        puci_msg *m = &sess->messages[i];
        if (m->role != PUCI_ROLE_ASSISTANT || m->ntool_calls == 0) {
            continue;
        }
        for (size_t k = 0; k < m->ntool_calls; k++) {
            const char *id = m->tool_calls[k].id ? m->tool_calls[k].id : "";
            int found = 0;
            for (size_t p = i + 1; p < n0; p++) {
                puci_msg *tm = &sess->messages[p];
                if (tm->role != PUCI_ROLE_TOOL) {
                    break;
                }
                if (tm->tool_call_id && strcmp(tm->tool_call_id, id) == 0) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                continue;
            }
            /* Append a synthetic result. Appends land beyond n0 so they are not
             * revisited; id and the tool_calls array it points into live on
             * separate heap allocations and stay valid across the append. */
            puci_status st = session_msg_begin(sess, PUCI_ROLE_TOOL,
                                               PUCI_CANCEL_RESULT);
            if (st != PUCI_OK) {
                return st;
            }
            st = session_msg_set_tool_call_id(sess, id);
            if (st != PUCI_OK) {
                return st;
            }
        }
    }
    return PUCI_OK;
}

puci_status puci_session_validate(const puci_session *sess) {
    if (!sess) {
        return PUCI_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sess->nmessages; i++) {
        const puci_msg *m = &sess->messages[i];
        if (m->role != PUCI_ROLE_ASSISTANT || m->ntool_calls == 0) {
            continue;
        }
        for (size_t k = 0; k < m->ntool_calls; k++) {
            const char *id = m->tool_calls[k].id ? m->tool_calls[k].id : "";
            int found = 0;
            for (size_t p = i + 1; p < sess->nmessages; p++) {
                const puci_msg *tm = &sess->messages[p];
                if (tm->role != PUCI_ROLE_TOOL) {
                    break;
                }
                if (tm->tool_call_id && strcmp(tm->tool_call_id, id) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                return PUCI_ERR_PROTOCOL;
            }
        }
    }
    return PUCI_OK;
}

void puci_session_get_usage(const puci_session *sess, long long *prompt_tokens,
                            long long *completion_tokens,
                            long long *cached_tokens) {
    if (!sess) {
        return;
    }
    if (prompt_tokens) {
        *prompt_tokens = sess->total_prompt_tokens;
    }
    if (completion_tokens) {
        *completion_tokens = sess->total_completion_tokens;
    }
    if (cached_tokens) {
        *cached_tokens = sess->total_cached_tokens;
    }
}

puci_status puci_session_set_model(puci_session *sess, const char *model) {
    if (!sess || !model) {
        return PUCI_ERR_INVALID_ARG;
    }
    char *copy = puci_alloc_strdup(&sess->alloc, model);
    if (!copy) {
        return PUCI_ERR_NOMEM;
    }
    if (sess->cfg.model) {
        puci_alloc_free(&sess->alloc, (void *)sess->cfg.model);
    }
    sess->cfg.model = copy;
    return PUCI_OK;
}

puci_status puci_session_clear(puci_session *sess) {
    if (!sess) {
        return PUCI_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sess->nmessages; i++) {
        free_msg(sess, &sess->messages[i]);
    }
    sess->nmessages = 0;
    return PUCI_OK;
}

puci_status puci_session_set_approval_feedback(puci_session *sess, const char *feedback) {
    if (!sess) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (sess->approval_feedback) {
        puci_alloc_free(&sess->alloc, sess->approval_feedback);
        sess->approval_feedback = NULL;
    }
    if (feedback && feedback[0]) {
        sess->approval_feedback = puci_alloc_strdup(&sess->alloc, feedback);
        if (!sess->approval_feedback) {
            return PUCI_ERR_NOMEM;
        }
    }
    return PUCI_OK;
}

puci_status puci_session_set_plan_mode(puci_session *sess, int plan_mode) {
    if (!sess) {
        return PUCI_ERR_INVALID_ARG;
    }
    sess->plan_mode = plan_mode ? 1 : 0;
    return PUCI_OK;
}

int puci_session_get_plan_mode(const puci_session *sess) {
    return sess ? sess->plan_mode : 0;
}

const char *const *puci_session_tools(const puci_session *sess, size_t *count) {
    if (count) {
        *count = 0;
    }
    if (!sess || sess->ntools == 0) {
        return NULL;
    }
    if (count) {
        *count = sess->ntools;
    }
    return (const char *const *)sess->tool_names;
}

/* Ensures `name` is present in the session's enabled tool-name list (the
 * underlying tool is already registered on the shared context by the caller).
 * Idempotent. */
static puci_status session_enable_tool_name(puci_session *sess,
                                            const char *name) {
    for (size_t i = 0; i < sess->ntools; i++) {
        if (sess->tool_names[i] && strcmp(sess->tool_names[i], name) == 0) {
            return PUCI_OK;
        }
    }
    if (sess->ntools == sess->tools_cap) {
        size_t ncap = sess->tools_cap ? sess->tools_cap * 2 : 8;
        char **nt =
            (char **)puci_alloc_malloc(&sess->alloc, ncap * sizeof(char *));
        if (!nt) {
            return PUCI_ERR_NOMEM;
        }
        if (sess->tool_names) {
            memcpy(nt, sess->tool_names, sess->ntools * sizeof(char *));
            puci_alloc_free(&sess->alloc, sess->tool_names);
        }
        sess->tool_names = nt;
        sess->tools_cap = ncap;
    }
    char *n = puci_alloc_strdup(&sess->alloc, name);
    if (!n) {
        return PUCI_ERR_NOMEM;
    }
    sess->tool_names[sess->ntools++] = n;
    return PUCI_OK;
}

puci_status puci_session_tool_register(puci_session *sess,
                                       const puci_tool_spec *spec,
                                       puci_tool_invoke_cb invoke_cb,
                                       void *userdata) {
    if (!sess || !spec || !spec->name) {
        return PUCI_ERR_INVALID_ARG;
    }
    /* Register on the shared context so lookup succeeds, then ensure the name
     * is enabled for this session. */
    puci_status st = puci_tool_register(sess->ctx, spec, invoke_cb, userdata);
    if (st != PUCI_OK) {
        return st;
    }
    return session_enable_tool_name(sess, spec->name);
}

puci_status puci_session_tool_register_async(puci_session *sess,
                                             const puci_tool_spec *spec,
                                             const puci_tool_async_vtable *vt,
                                             void *userdata) {
    if (!sess || !spec || !spec->name) {
        return PUCI_ERR_INVALID_ARG;
    }
    puci_status st = puci_tool_register_async(sess->ctx, spec, vt, userdata);
    if (st != PUCI_OK) {
        return st;
    }
    return session_enable_tool_name(sess, spec->name);
}

/* --- Internal helpers used by the agent loop --- */

/* Serializes the message history (plus an accumulating system prompt) into a
 * JSON array written to out. Owned strbuf passed in. */
puci_status session_serialize_messages(puci_session *sess, puci_strbuf *out) {
    puci_strbuf_append_char(out, '[');
    /* System prompt first. */
    if (sess->system_prompt) {
        puci_strbuf_appendz(out, "{\"role\":\"system\",\"content\":");
        puci_strbuf_append_json_string(out, sess->system_prompt);
        puci_strbuf_appendz(out, "}");
    }
    int first = (sess->system_prompt == NULL);
    for (size_t i = 0; i < sess->nmessages; i++) {
        puci_msg *m = &sess->messages[i];
        if (!first) {
            puci_strbuf_append_char(out, ',');
        }
        first = 0;
        puci_strbuf_append_char(out, '{');

        if (m->role == PUCI_ROLE_SYSTEM) {
            puci_strbuf_appendz(out, "\"role\":\"system\",\"content\":");
            puci_strbuf_append_json_string(out, m->content ? m->content : "");
        } else if (m->role == PUCI_ROLE_USER) {
            puci_strbuf_appendz(out, "\"role\":\"user\",\"content\":");
            puci_strbuf_append_json_string(out, m->content ? m->content : "");
        } else if (m->role == PUCI_ROLE_ASSISTANT) {
            puci_strbuf_appendz(out, "\"role\":\"assistant\"");
            if (m->ntool_calls > 0) {
                puci_strbuf_appendz(out, ",\"tool_calls\":[");
                for (size_t k = 0; k < m->ntool_calls; k++) {
                    if (k > 0) {
                        puci_strbuf_append_char(out, ',');
                    }
                    puci_strbuf_appendz(out,
                                        "{\"id\":");
                    puci_strbuf_append_json_string(out,
                                                   m->tool_calls[k].id ? m->tool_calls[k].id : "");
                    puci_strbuf_appendz(out, ",\"type\":\"function\",\"function\":{\"name\":");
                    puci_strbuf_append_json_string(out,
                                                   puci_strbuf_cstr(&m->tool_calls[k].name));
                    puci_strbuf_appendz(out, ",\"arguments\":");
                    puci_strbuf_append_json_string(out, puci_strbuf_cstr(&m->tool_calls[k].args));
                    puci_strbuf_appendz(out, "}}");
                }
                puci_strbuf_append_char(out, ']');
            } else {
                puci_strbuf_appendz(out, ",\"content\":");
                puci_strbuf_append_json_string(out, m->content ? m->content : "");
            }
        } else if (m->role == PUCI_ROLE_TOOL) {
            puci_strbuf_appendz(out, "\"role\":\"tool\",\"tool_call_id\":");
            puci_strbuf_append_json_string(out, m->tool_call_id ? m->tool_call_id : "");
            puci_strbuf_appendz(out, ",\"content\":");
            puci_strbuf_append_json_string(out, m->content ? m->content : "");
        } else {
            /* Unknown role: skip-invalid entry. */
            puci_strbuf_appendz(out, "\"role\":\"user\",\"content\":\"\"");
        }
        puci_strbuf_append_char(out, '}');
    }
    puci_strbuf_append_char(out, ']');
    return out->oom != PUCI_OK ? out->oom : PUCI_OK;
}

/* Serializes the session's enabled tool schemas into a JSON array written to
 * out, or writes nothing and leaves it empty if there are no tools. */
puci_status session_serialize_tools(puci_session *sess, puci_strbuf *out) {
    if (sess->ntools == 0) {
        return PUCI_OK;
    }
    puci_strbuf_append_char(out, '[');
    int wrote_any = 0;
    for (size_t i = 0; i < sess->ntools; i++) {
        const char *name = sess->tool_names[i];
        if (!name) {
            continue;
        }
        puci_tool *t = NULL;
        if (!puci_tools_lookup(sess->ctx, name, &t) || !t) {
            continue;
        }
        if (wrote_any) {
            puci_strbuf_append_char(out, ',');
        }
        wrote_any = 1;
        puci_strbuf_appendz(
            out, "{\"type\":\"function\",\"function\":{\"name\":");
        puci_strbuf_append_json_string(out, t->spec.name);
        puci_strbuf_appendz(out, ",\"description\":");
        puci_strbuf_append_json_string(out, t->spec.description ? t->spec.description : "");
        puci_strbuf_appendz(out, ",\"parameters\":");
        puci_strbuf_appendz(out, t->spec.input_schema ? t->spec.input_schema : "{}");
        puci_strbuf_appendz(out, "}}");
    }
    puci_strbuf_append_char(out, ']');
    return out->oom != PUCI_OK ? out->oom : PUCI_OK;
}

puci_status puci_session_export_json(const puci_session *sess, char **out_json) {
    if (!sess || !out_json) {
        return PUCI_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    puci_strbuf sb;
    puci_strbuf_init(&sb, NULL);

    puci_strbuf_appendz(&sb, "{\"version\":1");
    if (sess->cfg.model) {
        puci_strbuf_appendz(&sb, ",\"model\":");
        puci_strbuf_append_json_string(&sb, sess->cfg.model);
    }
    if (sess->system_prompt) {
        puci_strbuf_appendz(&sb, ",\"system_prompt\":");
        puci_strbuf_append_json_string(&sb, sess->system_prompt);
    }
    puci_strbuf_append_fmt(&sb, ",\"prompt_tokens\":%lld,\"completion_tokens\":%lld,\"cached_tokens\":%lld",
                           sess->total_prompt_tokens, sess->total_completion_tokens, sess->total_cached_tokens);
    puci_strbuf_append_fmt(&sb, ",\"updated_at\":%lld", (long long)time(NULL));

    puci_strbuf_appendz(&sb, ",\"messages\":[");
    for (size_t i = 0; i < sess->nmessages; i++) {
        const puci_msg *m = &sess->messages[i];
        if (i > 0) {
            puci_strbuf_append_char(&sb, ',');
        }
        puci_strbuf_append_char(&sb, '{');
        const char *role_str = "user";
        if (m->role == PUCI_ROLE_ASSISTANT) role_str = "assistant";
        else if (m->role == PUCI_ROLE_SYSTEM) role_str = "system";
        else if (m->role == PUCI_ROLE_TOOL) role_str = "tool";

        puci_strbuf_appendz(&sb, "\"role\":");
        puci_strbuf_append_json_string(&sb, role_str);

        if (m->content) {
            puci_strbuf_appendz(&sb, ",\"content\":");
            puci_strbuf_append_json_string(&sb, m->content);
        }
        if (m->tool_call_id) {
            puci_strbuf_appendz(&sb, ",\"tool_call_id\":");
            puci_strbuf_append_json_string(&sb, m->tool_call_id);
        }
        if (m->ntool_calls > 0) {
            puci_strbuf_appendz(&sb, ",\"tool_calls\":[");
            for (size_t k = 0; k < m->ntool_calls; k++) {
                if (k > 0) {
                    puci_strbuf_append_char(&sb, ',');
                }
                puci_strbuf_appendz(&sb, "{\"id\":");
                puci_strbuf_append_json_string(&sb, m->tool_calls[k].id ? m->tool_calls[k].id : "");
                puci_strbuf_appendz(&sb, ",\"name\":");
                puci_strbuf_append_json_string(&sb, puci_strbuf_cstr(&m->tool_calls[k].name));
                puci_strbuf_appendz(&sb, ",\"args\":");
                puci_strbuf_append_json_string(&sb, puci_strbuf_cstr(&m->tool_calls[k].args));
                puci_strbuf_append_char(&sb, '}');
            }
            puci_strbuf_append_char(&sb, ']');
        }
        puci_strbuf_append_char(&sb, '}');
    }
    puci_strbuf_appendz(&sb, "]}");

    *out_json = strdup(puci_strbuf_cstr(&sb));
    puci_strbuf_destroy(&sb);
    if (!*out_json) {
        return PUCI_ERR_NOMEM;
    }
    return PUCI_OK;
}

puci_status puci_session_import_json(puci_session *sess, const char *json_str) {
    if (!sess || !json_str || !*json_str) {
        return PUCI_ERR_INVALID_ARG;
    }

    puci_arena arena;
    puci_arena_init(&arena, NULL, 0);
    pj_val *root = NULL;
    if (pj_parse(&arena, json_str, strlen(json_str), &root) != 0 || !root || root->type != PJ_OBJ) {
        puci_arena_destroy(&arena);
        return PUCI_ERR_INVALID_ARG;
    }

    /* Model override if present in session JSON */
    pj_val *mv = pj_get(root, "model");
    if (mv && mv->type == PJ_STR && mv->u.str.s && mv->u.str.s[0]) {
        (void)puci_session_set_model(sess, mv->u.str.s);
    }

    /* System prompt if present */
    pj_val *spv = pj_get(root, "system_prompt");
    if (spv && spv->type == PJ_STR && spv->u.str.s && !sess->system_prompt) {
        sess->system_prompt = puci_alloc_strdup(&sess->alloc, spv->u.str.s);
    }

    /* Token counters */
    pj_val *ptv = pj_get(root, "prompt_tokens");
    if (ptv && ptv->type == PJ_NUM) sess->total_prompt_tokens = (long long)ptv->u.num;
    pj_val *ctv = pj_get(root, "completion_tokens");
    if (ctv && ctv->type == PJ_NUM) sess->total_completion_tokens = (long long)ctv->u.num;
    pj_val *kptv = pj_get(root, "cached_tokens");
    if (kptv && kptv->type == PJ_NUM) sess->total_cached_tokens = (long long)kptv->u.num;

    /* Messages array */
    pj_val *msgs = pj_get(root, "messages");
    if (msgs && msgs->type == PJ_ARR) {
        for (size_t i = 0; i < msgs->u.arr.len; i++) {
            pj_val *item = msgs->u.arr.items[i];
            if (!item || item->type != PJ_OBJ) {
                continue;
            }
            pj_val *rv = pj_get(item, "role");
            const char *role_str = rv ? pj_str(rv) : "user";
            puci_role role = PUCI_ROLE_USER;
            if (strcmp(role_str, "assistant") == 0) role = PUCI_ROLE_ASSISTANT;
            else if (strcmp(role_str, "system") == 0) role = PUCI_ROLE_SYSTEM;
            else if (strcmp(role_str, "tool") == 0) role = PUCI_ROLE_TOOL;

            pj_val *cv = pj_get(item, "content");
            const char *content = cv ? pj_str(cv) : NULL;

            puci_status st = session_msg_begin(sess, role, content);
            if (st != PUCI_OK) {
                puci_arena_destroy(&arena);
                return st;
            }

            if (role == PUCI_ROLE_TOOL) {
                pj_val *tcidv = pj_get(item, "tool_call_id");
                if (tcidv && pj_str(tcidv)) {
                    session_msg_set_tool_call_id(sess, pj_str(tcidv));
                }
            } else if (role == PUCI_ROLE_ASSISTANT) {
                pj_val *tcs = pj_get(item, "tool_calls");
                if (tcs && tcs->type == PJ_ARR && tcs->u.arr.len > 0) {
                    size_t ntc = tcs->u.arr.len;
                    puci_internal_tool_call *tcalls =
                        (puci_internal_tool_call *)puci_alloc_calloc(
                            &sess->alloc, ntc, sizeof(puci_internal_tool_call));
                    if (tcalls) {
                        for (size_t k = 0; k < ntc; k++) {
                            pj_val *tc_val = tcs->u.arr.items[k];
                            pj_val *idv = tc_val ? pj_get(tc_val, "id") : NULL;
                            pj_val *namev = tc_val ? pj_get(tc_val, "name") : NULL;
                            pj_val *argsv = tc_val ? pj_get(tc_val, "args") : NULL;
                            if (idv && pj_str(idv)) {
                                tcalls[k].id = puci_alloc_strdup(&sess->alloc, pj_str(idv));
                            }
                            puci_strbuf_init(&tcalls[k].name, &sess->alloc);
                            if (namev && pj_str(namev)) {
                                puci_strbuf_appendz(&tcalls[k].name, pj_str(namev));
                            }
                            puci_strbuf_init(&tcalls[k].args, &sess->alloc);
                            if (argsv && pj_str(argsv)) {
                                puci_strbuf_appendz(&tcalls[k].args, pj_str(argsv));
                            }
                        }
                        session_msg_finish_assistant(sess, content, tcalls, ntc);
                    }
                }
            }
        }
    }

    puci_arena_destroy(&arena);
    return puci_session_validate(sess);
}

void puci_session_estimate_context(const puci_session *sess, size_t *out_chars,
                                   size_t *out_est_tokens) {
    size_t total_chars = 0;
    if (!sess) {
        if (out_chars) *out_chars = 0;
        if (out_est_tokens) *out_est_tokens = 0;
        return;
    }
    if (sess->system_prompt) {
        total_chars += strlen(sess->system_prompt);
    }
    for (size_t i = 0; i < sess->nmessages; i++) {
        const puci_msg *m = &sess->messages[i];
        if (m->content) {
            total_chars += strlen(m->content);
        }
        if (m->tool_call_id) {
            total_chars += strlen(m->tool_call_id);
        }
        for (size_t k = 0; k < m->ntool_calls; k++) {
            if (m->tool_calls[k].id) {
                total_chars += strlen(m->tool_calls[k].id);
            }
            total_chars += m->tool_calls[k].name.len;
            total_chars += m->tool_calls[k].args.len;
        }
    }
    if (out_chars) *out_chars = total_chars;
    if (out_est_tokens) *out_est_tokens = (total_chars + 3) / 4;
}

#define PUCI_DEFAULT_CONTEXT_BUDGET ((size_t)131072)
#define PUCI_TAIL_PRESERVE_MSGS 8
#define PUCI_TOOL_PRUNE_THRESHOLD 512
#define PUCI_TOOL_KEEP_PREFIX 64

static size_t msg_bytes(const puci_msg *m) {
    size_t n = 0;
    if (m->content) n += strlen(m->content);
    if (m->tool_call_id) n += strlen(m->tool_call_id);
    for (size_t k = 0; k < m->ntool_calls; k++) {
        if (m->tool_calls[k].id) n += strlen(m->tool_calls[k].id);
        n += m->tool_calls[k].name.len;
        n += m->tool_calls[k].args.len;
    }
    return n;
}

size_t puci_session_context_bytes(const puci_session *sess) {
    if (!sess) return 0;
    size_t n = 0;
    if (sess->system_prompt) n += strlen(sess->system_prompt);
    for (size_t i = 0; i < sess->nmessages; i++) n += msg_bytes(&sess->messages[i]);
    return n;
}

size_t puci_session_get_context_budget(const puci_session *sess) {
    if (!sess) return PUCI_DEFAULT_CONTEXT_BUDGET;
    return sess->context_budget_bytes ? sess->context_budget_bytes : PUCI_DEFAULT_CONTEXT_BUDGET;
}

puci_status puci_session_set_context_budget(puci_session *sess, size_t budget_bytes) {
    if (!sess) return PUCI_ERR_INVALID_ARG;
    sess->context_budget_bytes = budget_bytes;
    return PUCI_OK;
}

void puci_session_estimate_context_budget(const puci_session *sess, size_t *out_bytes, size_t *out_budget) {
    if (out_bytes) *out_bytes = puci_session_context_bytes(sess);
    if (out_budget) *out_budget = puci_session_get_context_budget(sess);
}

static size_t hash_content(const char *s) {
    size_t h = 5381;
    if (!s) return 0;
    while (*s) { h = ((h << 5) + h) ^ (unsigned char)*s++; }
    return h;
}

puci_status puci_session_compact_context(puci_session *sess, size_t *out_pruned) {
    if (out_pruned) *out_pruned = 0;
    if (!sess) return PUCI_ERR_INVALID_ARG;
    size_t budget = puci_session_get_context_budget(sess);
    size_t cur = puci_session_context_bytes(sess);
    if (cur <= budget) return PUCI_OK;
    if (sess->nmessages <= 2) return PUCI_OK;

    size_t tail_start = sess->nmessages > PUCI_TAIL_PRESERVE_MSGS ? sess->nmessages - PUCI_TAIL_PRESERVE_MSGS : 1;
    if (tail_start < 1) tail_start = 1;
    size_t head_end = 1;

    size_t pruned = 0;
    /* Dedup map for middle TOOL messages: simple hash set */
    size_t *seen_hashes = NULL;
    size_t seen_cap = 0, seen_n = 0;

    for (size_t i = head_end; i < tail_start; i++) {
        if (cur <= budget) break;
        puci_msg *m = &sess->messages[i];
        if (m->role != PUCI_ROLE_TOOL || !m->content) continue;
        size_t clen = strlen(m->content);
        if (clen < PUCI_TOOL_PRUNE_THRESHOLD && clen < 512) continue;

        /* Tier 3: dedup - if identical content seen before in middle, replace with marker */
        size_t h = hash_content(m->content);
        int duplicate = 0;
        for (size_t j = 0; j < seen_n; j++) if (seen_hashes[j] == h) { duplicate = 1; break; }
        if (!duplicate) {
            if (seen_n == seen_cap) {
                size_t ncap = seen_cap ? seen_cap * 2 : 16;
                size_t *nh = (size_t *)puci_alloc_malloc(&sess->alloc, ncap * sizeof(size_t));
                if (!nh) { if (seen_hashes) puci_alloc_free(&sess->alloc, seen_hashes); return PUCI_ERR_NOMEM; }
                if (seen_hashes) { memcpy(nh, seen_hashes, seen_n * sizeof(size_t)); puci_alloc_free(&sess->alloc, seen_hashes); }
                seen_hashes = nh; seen_cap = ncap;
            }
            seen_hashes[seen_n++] = h;
        }
        if (duplicate) {
            const char *marker = "[Duplicate tool output removed for context budget]";
            size_t mlen = strlen(marker);
            char *nc = puci_alloc_strdup(&sess->alloc, marker);
            if (!nc) { if (seen_hashes) puci_alloc_free(&sess->alloc, seen_hashes); return PUCI_ERR_NOMEM; }
            size_t old = clen;
            puci_alloc_free(&sess->alloc, m->content);
            m->content = nc;
            size_t nb = mlen;
            if (old > nb) { pruned += old - nb; cur -= old - nb; }
            continue;
        }

        /* Tier 2: truncate verbose output */
        if (clen > PUCI_TOOL_PRUNE_THRESHOLD) {
            size_t keep = PUCI_TOOL_KEEP_PREFIX < clen ? PUCI_TOOL_KEEP_PREFIX : clen;
            char marker[128];
            int ml = snprintf(marker, sizeof(marker), "\n[Prior tool output truncated: %zu bytes pruned for context budget]", clen - keep);
            if (ml < 0) ml = 0;
            size_t nlen = keep + (size_t)ml;
            char *nc = (char *)puci_alloc_malloc(&sess->alloc, nlen + 1);
            if (!nc) { if (seen_hashes) puci_alloc_free(&sess->alloc, seen_hashes); return PUCI_ERR_NOMEM; }
            memcpy(nc, m->content, keep);
            memcpy(nc + keep, marker, (size_t)ml);
            nc[nlen] = '\0';
            puci_alloc_free(&sess->alloc, m->content);
            m->content = nc;
            pruned += clen - nlen;
            cur -= clen - nlen;
        }
    }
    if (seen_hashes) puci_alloc_free(&sess->alloc, seen_hashes);
    if (out_pruned) *out_pruned = pruned;
    return PUCI_OK;
}

puci_status puci_session_save_file(const puci_session *sess, const char *filepath) {
    if (!sess || !filepath || !*filepath) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (puci_path_is_protected(filepath)) {
        return PUCI_ERR_INVALID_ARG;
    }
    char *json_str = NULL;
    puci_status st = puci_session_export_json(sess, &json_str);
    if (st != PUCI_OK || !json_str) {
        return st;
    }
    size_t written = 0;
    char err_buf[256];
    int rc = puci_atomic_write_file(filepath, json_str, strlen(json_str), &written, err_buf, sizeof(err_buf));
    free(json_str);
    return (rc == 0) ? PUCI_OK : PUCI_ERR_IO;
}

puci_status puci_session_load_file(puci_session *sess, const char *filepath) {
    if (!sess || !filepath || !*filepath) {
        return PUCI_ERR_INVALID_ARG;
    }
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        return (errno == ENOENT) ? PUCI_ERR_NOT_FOUND : PUCI_ERR_IO;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz > 32 * 1024 * 1024) {
        fclose(f);
        return PUCI_ERR_LIMIT_EXCEEDED;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return PUCI_ERR_NOMEM;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    puci_status st = puci_session_import_json(sess, buf);
    free(buf);
    return st;
}
