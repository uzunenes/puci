/*
 * core/session.c - session lifecycle and message management.
 */

#include "puci_internal.h"

#include <string.h>

#include "json/json.h"

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

puci_status puci_session_cancel(puci_session *sess) {
    if (!sess) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (!sess->running) {
        return PUCI_ERR_OUT_OF_STATE;
    }
    sess->cancelled = 1;
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
    return out->oom != PUCI_OK ? out->oom : PUCI_OK;
}

/* Serializes the session's enabled tool schemas into a JSON array written to
 * out, or writes nothing and leaves it empty if there are no tools. */
puci_status session_serialize_tools(puci_session *sess, puci_strbuf *out) {
    if (sess->ntools == 0) {
        return PUCI_OK;
    }
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
    if (!wrote_any) {
        return PUCI_OK;
    }
    return out->oom != PUCI_OK ? out->oom : PUCI_OK;
}