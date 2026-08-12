/*
 * core/agent.c - the agent loop.
 *
 * Drives one user turn to completion: send the message, stream the provider
 * response, execute tool calls, feed results back, and continue until the turn
 * terminates with a final answer.
 */

#include "puci_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json/json.h"
#include "net/http.h"
#include "net/sse.h"
#include "util/arena.h"
#include "util/strbuf.h"

/* Bridge between the HTTP stream callback, the SSE parser, and the provider
 * parser. Lives on the stack for the duration of one synchronous request. */
typedef struct sse_bridge {
    puci_session *sess;
    puci_sse_parser *parser;
    puci_turn turn;
    const puci_provider_vtable *vt;
    size_t prev_text_len;
    puci_status first_error;
    int at_least_one_event;
} sse_bridge;

static void on_sse_event(void *ud, const char *data, size_t len) {
    sse_bridge *b = (sse_bridge *)ud;
    puci_arena_reset(&b->sess->scratch);
    puci_status st = b->vt->parse_stream(b->sess, &b->sess->scratch, data, len,
                                         &b->turn);
    if (st != PUCI_OK) {
        if (b->first_error == PUCI_OK) {
            b->first_error = st;
        }
        return;
    }
    b->at_least_one_event = 1;
    /* Emit a delta for the newly appended text. */
    if (b->turn.text.len > b->prev_text_len && b->sess->event_cb) {
        const char *newtxt = b->turn.text.data + b->prev_text_len;
        size_t n = b->turn.text.len - b->prev_text_len;
        b->prev_text_len = b->turn.text.len;
        b->sess->event_cb(b->sess->event_userdata, b->sess,
                          PUCI_EVENT_TOKEN_DELTA, (const void *)newtxt);
        (void)n;
    }
}

static void on_http_data(void *ud, const char *data, size_t len) {
    sse_bridge *b = (sse_bridge *)ud;
    if (b->first_error != PUCI_OK) {
        return;
    }
    puci_sse_parser_feed(b->parser, data, len, on_sse_event, b);
}

static int tool_call_ready(const puci_turn *turn) {
    for (size_t i = 0; i < turn->ntool_calls; i++) {
        if (turn->tool_calls[i].name.len > 0) {
            return 1;
        }
    }
    return 0;
}

/* Builds the full request URL: base_url + endpoint. */
static puci_status build_url(puci_session *sess, const char *endpoint,
                             char **out_url) {
    /* Strip a trailing slash from base_url so the join is clean. */
    const char *base = sess->cfg.base_url;
    size_t blen = base ? strlen(base) : 0;
    if (blen > 0 && base[blen - 1] == '/') {
        blen--;
    }
    size_t n = blen + strlen(endpoint) + 1;
    char *url = (char *)puci_alloc_malloc(&sess->alloc, n);
    if (!url) {
        return PUCI_ERR_NOMEM;
    }
    memcpy(url, base, blen);
    memcpy(url + blen, endpoint, strlen(endpoint) + 1);
    *out_url = url;
    return PUCI_OK;
}

puci_status puci_agent_run_turn(puci_session *sess, const char *msg) {
    puci_status st = session_msg_begin(sess, PUCI_ROLE_USER, msg);
    if (st != PUCI_OK) {
        return st;
    }

    puci_provider *provider = puci_provider_get(sess->ctx, sess->cfg.name);
    if (!provider || !provider->vtable) {
        session_set_err(sess, "provider not registered");
        return PUCI_ERR_NOT_FOUND;
    }
    const puci_provider_vtable *vt = provider->vtable;

    size_t iter = 0;
    const size_t max_iter =
        sess->max_tool_iterations ? sess->max_tool_iterations : 32;
    const char *done_text = NULL;

    while (iter < max_iter) {
        if (sess->cancelled) {
            return PUCI_ERR_CANCELLED;
        }

        /* Serialize messages once per round. */
        puci_strbuf msgs;
        puci_strbuf_init(&msgs, &sess->alloc);
        st = session_serialize_messages(sess, &msgs);
        if (st != PUCI_OK) {
            puci_strbuf_destroy(&msgs);
            return st;
        }

        const char *tools_json = NULL;
        puci_strbuf tools;
        puci_strbuf_init(&tools, &sess->alloc);
        if (vt->supports_tools && sess->ntools > 0) {
            st = session_serialize_tools(sess, &tools);
            if (st == PUCI_OK && tools.len > 0) {
                tools_json = puci_strbuf_cstr(&tools);
            }
            if (st != PUCI_OK) {
                puci_strbuf_destroy(&msgs);
                puci_strbuf_destroy(&tools);
                return st;
            }
        }

        /* Build the provider request body. */
        puci_strbuf req;
        puci_strbuf_init(&req, &sess->alloc);
        st = vt->build_request(sess, puci_strbuf_cstr(&msgs), tools_json, &req);
        puci_strbuf_destroy(&msgs);
        puci_strbuf_destroy(&tools);
        if (st != PUCI_OK) {
            puci_strbuf_destroy(&req);
            return st;
        }

        /* URL and auth. */
        char *url = NULL;
        st = build_url(sess, vt->endpoint, &url);
        if (st != PUCI_OK) {
            puci_strbuf_destroy(&req);
            return st;
        }
        puci_strbuf auth;
        puci_strbuf_init(&auth, &sess->alloc);
        if (sess->cfg.api_key && sess->cfg.api_key[0]) {
            puci_strbuf_appendz(&auth, "Bearer ");
            puci_strbuf_appendz(&auth, sess->cfg.api_key);
        }

        /* Set up the turn and bridge. */
        sse_bridge b;
        memset(&b, 0, sizeof(b));
        b.sess = sess;
        b.vt = vt;
        b.first_error = PUCI_OK;
        puci_strbuf_init(&b.turn.text, &sess->alloc);
        b.parser = puci_sse_parser_create(&sess->alloc);
        if (!b.parser) {
            puci_strbuf_destroy(&auth);
            puci_alloc_free(&sess->alloc, url);
            puci_strbuf_destroy(&req);
            puci_strbuf_destroy(&b.turn.text);
            return PUCI_ERR_NOMEM;
        }

        const char *auth_header = auth.len > 0 ? puci_strbuf_cstr(&auth) : NULL;
        int http_status = 0;
        char errbuf[256];
        errbuf[0] = '\0';
        st = puci_http_post_stream(
            &sess->alloc, url, puci_strbuf_cstr(&req), req.len, auth_header,
            "application/json", "text/event-stream", sess->cfg.timeout_ms,
            on_http_data, &b, &http_status, errbuf, sizeof(errbuf));

        /* Flush any trailing SSE event. */
        puci_sse_parser_flush(b.parser, on_sse_event, &b);
        puci_sse_parser_destroy(b.parser);

        puci_strbuf_destroy(&auth);
        puci_alloc_free(&sess->alloc, url);
        puci_strbuf_destroy(&req);

        if (st != PUCI_OK) {
            session_set_err(sess, errbuf[0] ? errbuf : "network error");
            puci_strbuf_destroy(&b.turn.text);
            return st;
        }
        if (http_status >= 400) {
            char msg[256];
            snprintf(msg, sizeof(msg), "upstream returned HTTP %d%s%s",
                     http_status, errbuf[0] ? ": " : "",
                     errbuf[0] ? errbuf : "");
            session_set_err(sess, msg);
            puci_strbuf_destroy(&b.turn.text);
            return (http_status == 401 || http_status == 403)
                       ? PUCI_ERR_UNAUTHORIZED
                       : PUCI_ERR_PROTOCOL;
        }
        if (b.first_error != PUCI_OK) {
            session_set_err(sess, "malformed stream from provider");
            puci_strbuf_destroy(&b.turn.text);
            return PUCI_ERR_PROTOCOL;
        }

        const char *text = puci_strbuf_cstr(&b.turn.text);
        int want_tools = tool_call_ready(&b.turn) &&
                         b.turn.had_tool_calls && iter + 1 < max_iter;

        if (want_tools) {
            /* Step 1: execute every tool, buffering results. The message array
             * is left untouched so the assistant message that carries the tool
             * calls is appended immediately before the tool-result messages. */
            typedef struct tresult {
                char *content;    /* owned (must be freed) */
                const char *id;   /* borrowed: points into a moved slot, stays valid */
            } tresult;
            tresult *results = NULL;
            size_t nresults = 0;
            size_t results_cap = 0;

            for (size_t k = 0; k < b.turn.ntool_calls; k++) {
                puci_internal_tool_call *slot = &b.turn.tool_calls[k];
                if (slot->name.len == 0) {
                    continue;
                }
                puci_tool_call pub;
                pub.name = puci_strbuf_cstr(&slot->name);
                pub.id = slot->id ? slot->id : "";
                pub.args_json = puci_strbuf_cstr(&slot->args);
                if (sess->event_cb) {
                    sess->event_cb(sess->event_userdata, sess,
                                   PUCI_EVENT_TOOL_CALL_START, &pub);
                }

                /* Look up and invoke the tool. */
                char *content = NULL;
                int is_err = 0;
                puci_tool *tool = NULL;
                if (puci_tools_lookup(sess->ctx, pub.name, &tool) &&
                    tool && tool->invoke) {
                    char *inv_res = NULL;
                    int rcerr = tool->invoke(tool->userdata, pub.args_json,
                                             &inv_res);
                    content = inv_res;
                    if (!content) {
                        content =
                            strdup("{\"ok\":false,\"error\":\"tool error\"}");
                    }
                    is_err = rcerr || !inv_res;
                } else {
                    content =
                        strdup("{\"ok\":false,\"error\":\"unknown tool\"}");
                    is_err = 1;
                }

                puci_tool_result res;
                res.call_id = pub.id;
                res.content = content;
                res.is_error = is_err;
                if (sess->event_cb) {
                    sess->event_cb(sess->event_userdata, sess,
                                   PUCI_EVENT_TOOL_CALL_END, &res);
                }

                /* Buffer the result for later message construction. */
                if (nresults == results_cap) {
                    size_t ncap = results_cap ? results_cap * 2 : 8;
                    tresult *nr = (tresult *)realloc(
                        results, ncap * sizeof(tresult));
                    if (!nr) {
                        free(content);
                        for (size_t i = 0; i < nresults; i++) {
                            free(results[i].content);
                        }
                        free(results);
                        puci_strbuf_destroy(&b.turn.text);
                        return PUCI_ERR_NOMEM;
                    }
                    results = nr;
                    results_cap = ncap;
                }
                results[nresults].content = content;
                results[nresults].id = pub.id;
                nresults++;
            }

            /* Step 2: append the assistant message carrying the tool calls. */
            st = session_msg_begin(sess, PUCI_ROLE_ASSISTANT, NULL);
            if (st != PUCI_OK) {
                for (size_t i = 0; i < nresults; i++) {
                    free(results[i].content);
                }
                free(results);
                puci_strbuf_destroy(&b.turn.text);
                return st;
            }
            st = session_msg_finish_assistant(sess, text, b.turn.tool_calls,
                                              b.turn.ntool_calls);
            b.turn.tool_calls = NULL;
            b.turn.ntool_calls = 0;
            b.turn.tool_calls_cap = 0;
            puci_strbuf_destroy(&b.turn.text);
            if (st != PUCI_OK) {
                for (size_t i = 0; i < nresults; i++) {
                    free(results[i].content);
                }
                free(results);
                return st;
            }

            /* Step 3: append the tool-result messages after the assistant. */
            for (size_t i = 0; i < nresults; i++) {
                if (results[i].id) {
                    session_msg_begin(sess, PUCI_ROLE_TOOL, results[i].content);
                    session_msg_set_tool_call_id(sess, results[i].id);
                }
                free(results[i].content);
            }
            free(results);

            iter++;
            continue;
        }

        /* Final answer: no (more) tool calls. */
        st = session_msg_begin(sess, PUCI_ROLE_ASSISTANT, NULL);
        if (st != PUCI_OK) {
            puci_strbuf_destroy(&b.turn.text);
            return st;
        }
        session_msg_finish_assistant(sess, text, NULL, 0);
        done_text = sess->messages[sess->nmessages - 1].content;
        puci_strbuf_destroy(&b.turn.text);
        break;
    }

    if (sess->event_cb) {
        sess->event_cb(sess->event_userdata, sess, PUCI_EVENT_TURN_COMPLETE,
                       (const void *)done_text);
    }
    return PUCI_OK;
}