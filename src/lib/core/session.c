/*
 * core/session.c - session lifecycle and message management.
 */

#include "puci_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
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

puci_status puci_session_send(puci_session *sess, const char *message) {
    if (!sess || !message) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (sess->running) {
        return PUCI_ERR_OUT_OF_STATE;
    }
    sess->running = 1;
    sess->cancelled = 0;
    puci_status st = puci_agent_run_turn(sess, message);
    sess->running = 0;
    return st;
}

puci_status puci_session_poll(puci_session *sess) {
    (void)sess;
    /* Non-blocking pollable mode is not yet available on this build path. */
    return PUCI_ERR_OUT_OF_STATE;
}

int puci_session_poll_fd(const puci_session *sess) {
    (void)sess;
    return -1;
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
    for (size_t i = 0; i < sess->ntools; i++) {
        if (sess->tool_names[i] && strcmp(sess->tool_names[i], spec->name) == 0) {
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
    char *n = puci_alloc_strdup(&sess->alloc, spec->name);
    if (!n) {
        return PUCI_ERR_NOMEM;
    }
    sess->tool_names[sess->ntools++] = n;
    return PUCI_OK;
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