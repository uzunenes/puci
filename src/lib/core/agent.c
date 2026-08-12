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
    size_t prev_reasoning_len;
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
    /* Emit a delta for the newly appended reasoning content (shown separately,
     * never concatenated into the answer). */
    if (b->turn.reasoning.len > b->prev_reasoning_len && b->sess->event_cb) {
        const char *newr = b->turn.reasoning.data + b->prev_reasoning_len;
        size_t n = b->turn.reasoning.len - b->prev_reasoning_len;
        b->prev_reasoning_len = b->turn.reasoning.len;
        b->sess->event_cb(b->sess->event_userdata, b->sess,
                          PUCI_EVENT_REASONING_DELTA, (const void *)newr);
        (void)n;
    }
    /* Emit a delta for the newly appended answer text. */
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

/* Merges caller-supplied extra_body (a JSON object string) into the generated
 * request body stored in *req. extra_body keys override generated keys. Invalid
 * extra_body is logged and ignored (never fatal). */
static puci_status merge_extra_body(puci_session *sess, puci_strbuf *req) {
    if (!sess->cfg.extra_body || !sess->cfg.extra_body[0]) {
        return PUCI_OK;
    }
    puci_arena a;
    puci_arena_init(&a, &sess->alloc, 0);
    pj_val *reqdom = NULL;
    pj_val *extra = NULL;
    const char *body = puci_strbuf_cstr(req);
    if (pj_parse(&a, body, req->len, &reqdom) != 0 ||
        pj_parse(&a, sess->cfg.extra_body, strlen(sess->cfg.extra_body),
                 &extra) != 0 ||
        !reqdom || reqdom->type != PJ_OBJ || !extra || extra->type != PJ_OBJ) {
        puci_logf(sess->ctx, PUCI_LOG_WARN,
                  "extra_body ignored: could not parse as a JSON object");
        puci_arena_destroy(&a);
        return PUCI_OK;
    }

    /* Build a merged object in the arena, then serialize. */
    pj_val *merged = (pj_val *)puci_arena_calloc(&a, sizeof(pj_val));
    if (!merged) {
        puci_arena_destroy(&a);
        return PUCI_ERR_NOMEM;
    }
    merged->type = PJ_OBJ;
    size_t cap = reqdom->u.obj.len + extra->u.obj.len + 1;
    merged->u.obj.keys = (char **)puci_arena_alloc(&a, cap * sizeof(char *));
    merged->u.obj.vals = (pj_val **)puci_arena_alloc(&a, cap * sizeof(pj_val *));
    if (!merged->u.obj.keys || !merged->u.obj.vals) {
        puci_arena_destroy(&a);
        return PUCI_ERR_NOMEM;
    }
    for (size_t i = 0; i < reqdom->u.obj.len; i++) {
        merged->u.obj.keys[merged->u.obj.len] = reqdom->u.obj.keys[i];
        merged->u.obj.vals[merged->u.obj.len] = reqdom->u.obj.vals[i];
        merged->u.obj.len++;
    }
    for (size_t i = 0; i < extra->u.obj.len; i++) {
        int found = -1;
        for (size_t j = 0; j < merged->u.obj.len; j++) {
            if (strcmp(merged->u.obj.keys[j], extra->u.obj.keys[i]) == 0) {
                found = (int)j;
                break;
            }
        }
        if (found >= 0) {
            merged->u.obj.vals[(size_t)found] = extra->u.obj.vals[i];
        } else {
            merged->u.obj.keys[merged->u.obj.len] = extra->u.obj.keys[i];
            merged->u.obj.vals[merged->u.obj.len] = extra->u.obj.vals[i];
            merged->u.obj.len++;
        }
    }

    puci_strbuf newreq;
    puci_strbuf_init(&newreq, &sess->alloc);
    pj_serialize(merged, &newreq);
    puci_strbuf_destroy(req);
    *req = newreq;
    puci_arena_destroy(&a);
    return PUCI_OK;
}

/* Reads an entire file into a malloc'd NUL-terminated buffer. Returns PUCI_OK
 * and sets *data and *len. If the file does not exist or cannot be read,
 * returns PUCI_ERR_NOT_FOUND (with *data NULL). Caller frees *data. */
static puci_status read_file_whole(const char *path, char **data, size_t *len) {
    *data = NULL;
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return PUCI_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return PUCI_ERR_NOT_FOUND;
    }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > (size_t)-1 - 1) {
        fclose(f);
        return PUCI_ERR_NOT_FOUND;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return PUCI_ERR_NOMEM;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *data = buf;
    *len = got;
    return PUCI_OK;
}

/* Builds the display argv listing (one element per line) for run_shell into a
 * heap strbuf, then copies it into the arena. */
static void build_argv_display(puci_arena *arena, puci_strbuf *sb,
                               const char *const *argv, int argc) {
    puci_strbuf_appendz(sb, "run_shell argv:");
    for (int i = 0; i < argc; i++) {
        puci_strbuf_append_fmt(sb, "\n  %d: ", i);
        puci_strbuf_appendz(sb, argv[i] ? argv[i] : "");
    }
}

/* Fills *req for a pending tool call using arena-scoped memory. If a diff is
 * computed (write_file over an existing file), req->diff is heap-allocated and
 * must be released by the caller via destroy_approval_request. */
static void build_approval_request(puci_session *sess, const puci_tool_call *pub,
                                   puci_arena *arena,
                                   puci_approval_request *req) {
    memset(req, 0, sizeof(*req));
    req->tool = pub->name;
    req->args_json = pub->args_json;

    if (strcmp(pub->name, "run_shell") == 0) {
        pj_val *root = NULL;
        if (pub->args_json &&
            pj_parse(arena, pub->args_json, strlen(pub->args_json), &root) == 0 &&
            root) {
            pj_val *cmdv = pj_get(root, "command");
            const char *cmd = cmdv ? pj_str(cmdv) : NULL;
            pj_val *argsv = pj_get(root, "args");
            size_t nargs = argsv && argsv->type == PJ_ARR
                               ? argsv->u.arr.len
                               : 0;
            if (cmd) {
                size_t total = nargs + 1;
                const char **argv = (const char **)puci_arena_alloc(
                    arena, (total + 1) * sizeof(char *));
                if (argv) {
                    argv[0] = cmd;
                    for (size_t i = 0; i < nargs; i++) {
                        pj_val *a = argsv->u.arr.items[i];
                        argv[i + 1] = a && a->type == PJ_STR ? a->u.str.s : "";
                    }
                    argv[total] = NULL;
                    req->argv = argv;
                    req->argc = (int)total;

                    puci_strbuf sb;
                    puci_strbuf_init(&sb, &sess->alloc);
                    build_argv_display(arena, &sb, argv, (int)total);
                    req->display = puci_arena_strdup(arena, puci_strbuf_cstr(&sb));
                    puci_strbuf_destroy(&sb);
                }
            }
        }
    } else if (strcmp(pub->name, "write_file") == 0) {
        pj_val *root = NULL;
        if (pub->args_json &&
            pj_parse(arena, pub->args_json, strlen(pub->args_json), &root) == 0 &&
            root) {
            pj_val *pathv = pj_get(root, "path");
            pj_val *contentv = pj_get(root, "content");
            const char *path = pathv ? pj_str(pathv) : NULL;
            const char *content = contentv && contentv->type == PJ_STR
                                      ? contentv->u.str.s
                                      : NULL;
            if (path && content) {
                char *old = NULL;
                size_t old_len = 0;
                puci_status rst =
                    read_file_whole(path, &old, &old_len);
                if (rst == PUCI_OK) {
                    req->file_existed = 1;
                    req->diff = puci_compute_diff(old, old_len, content,
                                                  contentv->u.str.len);
                    free(old);
                    if (req->diff) {
                        req->display = req->diff;
                    }
                } else if (rst == PUCI_ERR_NOT_FOUND) {
                    req->file_existed = 0;
                    req->display = "create new file";
                } else {
                    req->display = "unable to read target file";
                }
            }
        }
    }

    if (!req->display) {
        req->display = pub->args_json ? pub->args_json : "";
    }
}

static void destroy_approval_request(puci_approval_request *req) {
    if (req->diff) {
        puci_diff_free(req->diff);
        req->diff = NULL;
    }
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
        puci_strbuf_init(&b.turn.reasoning, &sess->alloc);
        b.parser = puci_sse_parser_create(&sess->alloc);
        if (!b.parser) {
            puci_strbuf_destroy(&auth);
            puci_alloc_free(&sess->alloc, url);
            puci_strbuf_destroy(&req);
            puci_strbuf_destroy(&b.turn.text);
            puci_strbuf_destroy(&b.turn.reasoning);
            return PUCI_ERR_NOMEM;
        }

        st = merge_extra_body(sess, &req);
        if (st != PUCI_OK) {
            puci_strbuf_destroy(&auth);
            puci_alloc_free(&sess->alloc, url);
            puci_strbuf_destroy(&req);
            puci_strbuf_destroy(&b.turn.text);
            puci_strbuf_destroy(&b.turn.reasoning);
            return st;
        }

        const char *auth_header = auth.len > 0 ? puci_strbuf_cstr(&auth) : NULL;
        int http_status = 0;
        char errbuf[256];
        errbuf[0] = '\0';
        puci_http_request hreq;
        memset(&hreq, 0, sizeof(hreq));
        hreq.url = url;
        hreq.body = puci_strbuf_cstr(&req);
        hreq.body_len = req.len;
        hreq.auth_header = auth_header;
        hreq.content_type = "application/json";
        hreq.accept = "text/event-stream";
        hreq.timeout_ms = sess->cfg.timeout_ms;
        hreq.idle_timeout_ms = sess->cfg.stream_idle_timeout_ms;
        hreq.ca_bundle = sess->cfg.ca_bundle;
        hreq.tls_verify = sess->cfg.tls_verify;
        if (sess->ctx->ninsecure_hosts > 0) {
            hreq.insecure_hosts = (const char *const *)sess->ctx->insecure_hosts;
            hreq.ninsecure_hosts = sess->ctx->ninsecure_hosts;
        }
        hreq.log_cb = sess->ctx->log_cb;
        hreq.log_userdata = sess->ctx->log_userdata;
        hreq.log_level = sess->ctx->log_level;
        st = puci_http_post_stream(&sess->alloc, &hreq, on_http_data, &b,
                                   &http_status, errbuf, sizeof(errbuf));

        /* Flush any trailing SSE event. */
        puci_sse_parser_flush(b.parser, on_sse_event, &b);
        puci_sse_parser_destroy(b.parser);

        puci_strbuf_destroy(&auth);
        puci_alloc_free(&sess->alloc, url);
        puci_strbuf_destroy(&req);

        if (st != PUCI_OK) {
            session_set_err(sess, errbuf[0] ? errbuf : "network error");
            puci_strbuf_destroy(&b.turn.text);
            puci_strbuf_destroy(&b.turn.reasoning);
            return st;
        }
        if (http_status >= 400) {
            char msg[256];
            snprintf(msg, sizeof(msg), "upstream returned HTTP %d%s%s",
                     http_status, errbuf[0] ? ": " : "",
                     errbuf[0] ? errbuf : "");
            session_set_err(sess, msg);
            puci_strbuf_destroy(&b.turn.text);
            puci_strbuf_destroy(&b.turn.reasoning);
            return (http_status == 401 || http_status == 403)
                       ? PUCI_ERR_UNAUTHORIZED
                       : PUCI_ERR_PROTOCOL;
        }
        if (b.first_error != PUCI_OK) {
            session_set_err(sess, "malformed stream from provider");
            puci_strbuf_destroy(&b.turn.text);
            puci_strbuf_destroy(&b.turn.reasoning);
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
                    int rcerr = 0;
                    int run = 1;
                    if (sess->approve_cb) {
                        puci_arena ar;
                        puci_approval_request apreq;
                        puci_arena_init(&ar, &sess->alloc, 0);
                        build_approval_request(sess, &pub, &ar, &apreq);
                        if (sess->approve_cb(&apreq, sess->approve_userdata) ==
                            PUCI_APPROVAL_DENY) {
                            run = 0;
                            rcerr = 1;
                            inv_res = strdup(
                                "{\"ok\":false,\"error\":\"blocked by "
                                "approval policy\"}");
                        }
                        destroy_approval_request(&apreq);
                        puci_arena_destroy(&ar);
                    }
                    if (run) {
                        rcerr = tool->invoke(tool->userdata, pub.args_json,
                                             &inv_res);
                    }
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
                        puci_strbuf_destroy(&b.turn.reasoning);
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
                puci_strbuf_destroy(&b.turn.reasoning);
                return st;
            }
            st = session_msg_finish_assistant(sess, text, b.turn.tool_calls,
                                              b.turn.ntool_calls);
            b.turn.tool_calls = NULL;
            b.turn.ntool_calls = 0;
            b.turn.tool_calls_cap = 0;
            puci_strbuf_destroy(&b.turn.text);
            puci_strbuf_destroy(&b.turn.reasoning);
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
            puci_strbuf_destroy(&b.turn.reasoning);
            return st;
        }
        session_msg_finish_assistant(sess, text, NULL, 0);
        done_text = sess->messages[sess->nmessages - 1].content;
        puci_strbuf_destroy(&b.turn.text);
        puci_strbuf_destroy(&b.turn.reasoning);
        break;
    }

    if (sess->event_cb) {
        sess->event_cb(sess->event_userdata, sess, PUCI_EVENT_TURN_COMPLETE,
                       (const void *)done_text);
    }
    return PUCI_OK;
}