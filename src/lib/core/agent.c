/*
 * core/agent.c - the agent loop, as a non-blocking state machine.
 *
 * This is the single implementation of the turn logic. It is driven from a
 * host's own event loop: puci_agent_process() performs a bounded amount of
 * work and returns, emitting events into a queue the host drains with
 * puci_agent_next_event(). The blocking convenience entry point
 * (puci_agent_run_turn, used by puci_session_send) is a thin wrapper that
 * pumps the same state machine and forwards queued events to the session's
 * event callback. There is deliberately no separate synchronous variant that
 * could drift from the async one.
 *
 * States (puci_turn_state): BUILD_REQUEST -> STREAMING -> STREAM_COMPLETE ->
 * DISPATCHING_TOOLS -> (BUILD_REQUEST next iteration) or done.
 * RETRY_BACKOFF is expressed purely as a deadline surfaced through
 * puci_agent_next_timeout_ms(); the library never sleeps. Every state checks
 * cancellation and has a defined cleanup into TURN_DONE.
 *
 * No blocking syscall exists on the streaming path: the only waits are the fds
 * and timeout the host polls. Tool execution (Stage A) still runs
 * synchronously inside DISPATCHING_TOOLS (see DOGFOOD / issue #2 for the async
 * tool lifecycle that removes that last blocking step).
 */

#include "puci_internal.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "json/json.h"
#include "net/http.h"
#include "net/sse.h"
#include "util/arena.h"
#include "util/strbuf.h"

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char *dupn_new(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (!d) {
        return NULL;
    }
    if (n > 0) {
        memcpy(d, s, n);
    }
    d[n] = '\0';
    return d;
}

static char *dupstr(const char *s) {
    if (!s) {
        return NULL;
    }
    return strdup(s);
}

/* ------------------------------------------------------------------ */
/* Event queue (owned by the session, drained by next_event)           */
/* ------------------------------------------------------------------ */

static struct puci_evnode *evnode_alloc(puci_session *sess) {
    return (struct puci_evnode *)puci_alloc_malloc(
        &sess->alloc, sizeof(struct puci_evnode));
}

static void evnode_free(puci_session *sess, struct puci_evnode *n) {
    if (!n) {
        return;
    }
    free(n->text);
    free(n->tc_name);
    free(n->tc_id);
    free(n->tc_args);
    free(n->tr_call_id);
    free(n->tr_content);
    puci_alloc_free(&sess->alloc, n);
}

/* Frees every queued event node, clearing the queue. */
static void eventq_flush(puci_session *sess) {
    struct puci_evnode *q = sess->ev_head;
    while (q) {
        struct puci_evnode *next = q->next;
        evnode_free(sess, q);
        q = next;
    }
    sess->ev_head = NULL;
    sess->ev_tail = NULL;
}

void turn_run_eventq_push(puci_session *sess, struct puci_evnode *n) {
    if (!n) {
        return;
    }
    n->next = NULL;
    if (sess->ev_tail) {
        sess->ev_tail->next = n;
    } else {
        sess->ev_head = n;
    }
    sess->ev_tail = n;
}

static int queue_push_alloc(puci_session *sess, struct puci_evnode **out) {
    struct puci_evnode *n = evnode_alloc(sess);
    if (!n) {
        return 0;
    }
    memset(n, 0, sizeof(*n));
    *out = n;
    return 1;
}

/* Queue a text-bearing event. Takes ownership of `text` (may be NULL). */
static void qtext(puci_session *sess, puci_event ev, char *text) {
    struct puci_evnode *n;
    if (!queue_push_alloc(sess, &n)) {
        free(text);
        return;
    }
    n->ev = ev;
    n->text = text;
    turn_run_eventq_push(sess, n);
}

static void qtool_start(puci_session *sess, const char *name, const char *id,
                        const char *args) {
    struct puci_evnode *n;
    if (!queue_push_alloc(sess, &n)) {
        return;
    }
    n->ev = PUCI_EVENT_TOOL_CALL_START;
    n->tc_name = dupstr(name);
    n->tc_id = dupstr(id);
    n->tc_args = dupstr(args);
    turn_run_eventq_push(sess, n);
}

static void qtool_end(puci_session *sess, const char *call_id,
                      const char *content, int is_error) {
    struct puci_evnode *n;
    if (!queue_push_alloc(sess, &n)) {
        return;
    }
    n->ev = PUCI_EVENT_TOOL_CALL_END;
    n->tr_call_id = dupstr(call_id);
    n->tr_content = dupstr(content);
    n->tr_is_error = is_error;
    turn_run_eventq_push(sess, n);
}

/* ------------------------------------------------------------------ */
/* SSE streaming -> event queue                                        */
/* ------------------------------------------------------------------ */

static void on_sse_event(void *ud, const char *data, size_t len) {
    puci_session *sess = (puci_session *)ud;
    struct puci_turn_run *run = sess->run;
    const puci_provider *provider =
        puci_provider_get(sess->ctx, sess->cfg.name);
    const puci_provider_vtable *vt = provider ? provider->vtable : NULL;
    if (!vt || !run) {
        return;
    }
    puci_arena_reset(&sess->scratch);
    puci_status st = vt->parse_stream(sess, &sess->scratch, data, len,
                                      &run->turn);
    if (st != PUCI_OK) {
        if (run->first_err == PUCI_OK) {
            run->first_err = st;
        }
        return;
    }
    run->at_least_one_event = 1;
    if (run->turn.reasoning.len > run->prev_reasoning_len) {
        const char *nr = run->turn.reasoning.data + run->prev_reasoning_len;
        size_t dl = run->turn.reasoning.len - run->prev_reasoning_len;
        qtext(sess, PUCI_EVENT_REASONING_DELTA, dupn_new(nr, dl));
        run->prev_reasoning_len = run->turn.reasoning.len;
    }
    if (run->turn.text.len > run->prev_text_len) {
        const char *nt = run->turn.text.data + run->prev_text_len;
        size_t dl = run->turn.text.len - run->prev_text_len;
        qtext(sess, PUCI_EVENT_TOKEN_DELTA, dupn_new(nt, dl));
        run->prev_text_len = run->turn.text.len;
    }
}

static void on_http_data(void *ud, const char *data, size_t len) {
    puci_session *sess = (puci_session *)ud;
    struct puci_turn_run *run = sess->run;
    if (!run || run->first_err != PUCI_OK || !run->parser) {
        return;
    }
    (void)puci_sse_parser_feed(run->parser, data, len, on_sse_event, sess);
}

static int tool_call_ready(const struct puci_turn *turn) {
    for (size_t i = 0; i < turn->ntool_calls; i++) {
        if (turn->tool_calls[i].name.len > 0) {
            return 1;
        }
    }
    return 0;
}

/* Strips a trailing slash from base_url and appends endpoint. The result is
 * allocator-owned (the caller frees it with puci_alloc_free). */
static puci_status build_url(puci_session *sess, const char *endpoint,
                             char **out_url) {
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

static void build_argv_display(puci_arena *arena, puci_strbuf *sb,
                               const char *const *argv, int argc) {
    (void)arena;
    puci_strbuf_appendz(sb, "run_shell argv:");
    for (int i = 0; i < argc; i++) {
        puci_strbuf_append_fmt(sb, "\n  %d: ", i);
        puci_strbuf_appendz(sb, argv[i] ? argv[i] : "");
    }
}

static void build_approval_request(puci_session *sess, const puci_tool_call *pub,
                                   puci_arena *arena,
                                   puci_approval_request *req,
                                   const puci_cancel_token *cancel) {
    memset(req, 0, sizeof(*req));
    req->tool = pub->name;
    req->args_json = pub->args_json;
    req->cancel = cancel;

    if (strcmp(pub->name, "run_shell") == 0) {
        pj_val *root = NULL;
        if (pub->args_json &&
            pj_parse(arena, pub->args_json, strlen(pub->args_json), &root) == 0 &&
            root) {
            pj_val *cmdv = pj_get(root, "command");
            const char *cmd = cmdv ? pj_str(cmdv) : NULL;
            pj_val *argsv = pj_get(root, "args");
            size_t nargs =
                argsv && argsv->type == PJ_ARR ? argsv->u.arr.len : 0;
            if (cmd) {
                size_t total = nargs + 1;
                const char **argv = (const char **)puci_arena_alloc(
                    arena, (total + 1) * sizeof(char *));
                if (argv) {
                    argv[0] = cmd;
                    for (size_t i = 0; i < nargs; i++) {
                        pj_val *a = argsv->u.arr.items[i];
                        argv[i + 1] =
                            a && a->type == PJ_STR ? a->u.str.s : "";
                    }
                    argv[total] = NULL;
                    req->argv = argv;
                    req->argc = (int)total;
                    puci_strbuf sb;
                    puci_strbuf_init(&sb, &sess->alloc);
                    build_argv_display(arena, &sb, argv, (int)total);
                    req->display =
                        puci_arena_strdup(arena, puci_strbuf_cstr(&sb));
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
            const char *content =
                contentv && contentv->type == PJ_STR ? contentv->u.str.s : NULL;
            if (path && content) {
                char *old = NULL;
                size_t old_len = 0;
                puci_status rst = read_file_whole(path, &old, &old_len);
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

/* ------------------------------------------------------------------ */
/* Teardown / terminal helpers                                         */
/* ------------------------------------------------------------------ */

/* Frees the run and all its owned state. Does NOT touch the session event
 * queue, so terminal events queued immediately before survive for the host to
 * drain with next_event(). */
static void run_free(puci_session *sess, struct puci_turn_run *run) {
    if (!run) {
        return;
    }
    if (run->req) {
        puci_http_end(run->req);
        run->req = NULL;
    }
    if (run->parser) {
        puci_sse_parser_destroy(run->parser);
        run->parser = NULL;
    }
    if (run->url) {
        puci_alloc_free(&sess->alloc, run->url);
        run->url = NULL;
    }
    if (run->req_body) {
        free(run->req_body);
        run->req_body = NULL;
    }
    if (run->auth) {
        free(run->auth);
        run->auth = NULL;
    }
    if (run->stream_text) {
        free(run->stream_text);
        run->stream_text = NULL;
    }
    puci_strbuf_destroy(&run->turn.text);
    puci_strbuf_destroy(&run->turn.reasoning);
    for (size_t i = 0; i < run->turn.ntool_calls; i++) {
        free(run->turn.tool_calls[i].id);
        puci_strbuf_destroy(&run->turn.tool_calls[i].name);
        puci_strbuf_destroy(&run->turn.tool_calls[i].args);
    }
    if (run->turn.tool_calls) {
        free(run->turn.tool_calls);
        run->turn.tool_calls = NULL;
    }
    run->turn.ntool_calls = 0;
    run->turn.tool_calls_cap = 0;
    sess->run = NULL;
    puci_alloc_free(&sess->alloc, run);
}

/* Terminal path: normal completion. `done_text` is owned by committed history
 * (points into sess->messages) or NULL; it is not freed here. */
static void fin_complete(puci_session *sess, struct puci_turn_run *run,
                         const char *done_text) {
    qtext(sess, PUCI_EVENT_TURN_COMPLETE, dupstr(done_text));
    sess->last_status = PUCI_OK;
    run_free(sess, run);
}

/* Terminal path: cancellation. Keeps history valid for the next turn by
 * synthesising any missing tool results, then frees the run. */
static void fin_cancelled(puci_session *sess, struct puci_turn_run *run) {
    (void)session_fixup_dangling_tool_calls(sess);
    qtext(sess, PUCI_EVENT_TURN_CANCELLED, NULL);
    sess->last_status = PUCI_ERR_CANCELLED;
    run_free(sess, run);
}

/* Terminal path: error. */
static void fin_error(puci_session *sess, struct puci_turn_run *run,
                      puci_status st, const char *msg) {
    session_set_err(sess, msg ? msg : "unknown error");
    qtext(sess, PUCI_EVENT_ERROR, dupstr(msg));
    sess->last_status = st;
    run_free(sess, run);
}

/* ------------------------------------------------------------------ */
/* State transitions                                                   */
/* ------------------------------------------------------------------ */

/* Serializes the history and starts the in-flight HTTP request. On success the
 * run is left in STREAMING with req owned by run. */
static puci_status run_build_request(puci_session *sess,
                                     struct puci_turn_run *run) {
    const puci_provider *provider =
        puci_provider_get(sess->ctx, sess->cfg.name);
    if (!provider || !provider->vtable) {
        return PUCI_ERR_NOT_FOUND;
    }
    const puci_provider_vtable *vt = provider->vtable;

    /* A single run may build the request more than once: after tool dispatch
     * (iter++) or a retry, run_build_request runs again on the same run. The
     * previous iteration's request resources are owned by the run and must be
     * released before they are overwritten below, or that set leaks. (These
     * are exactly the fields run_free releases, minus the turn text buffers,
     * which are deliberately re-initialised later in this function.) */
    if (run->req) {
        puci_http_end(run->req);
        run->req = NULL;
    }
    if (run->parser) {
        puci_sse_parser_destroy(run->parser);
        run->parser = NULL;
    }
    if (run->url) {
        puci_alloc_free(&sess->alloc, run->url);
        run->url = NULL;
    }
    if (run->req_body) {
        free(run->req_body);
        run->req_body = NULL;
    }
    if (run->auth) {
        free(run->auth);
        run->auth = NULL;
    }
    if (run->stream_text) {
        free(run->stream_text);
        run->stream_text = NULL;
    }

    puci_strbuf msgs;
    puci_strbuf_init(&msgs, &sess->alloc);
    puci_status st = session_serialize_messages(sess, &msgs);
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
    puci_strbuf req;
    puci_strbuf_init(&req, &sess->alloc);
    st = vt->build_request(sess, puci_strbuf_cstr(&msgs), tools_json, &req);
    puci_strbuf_destroy(&msgs);
    puci_strbuf_destroy(&tools);
    if (st != PUCI_OK) {
        puci_strbuf_destroy(&req);
        return st;
    }
    st = merge_extra_body(sess, &req);
    if (st != PUCI_OK) {
        puci_strbuf_destroy(&req);
        return st;
    }

    char *url = NULL;
    st = build_url(sess, vt->endpoint, &url);
    if (st != PUCI_OK) {
        puci_strbuf_destroy(&req);
        return st;
    }
    char *auth = NULL;
    if (sess->cfg.api_key && sess->cfg.api_key[0]) {
        size_t n = strlen("Bearer ") + strlen(sess->cfg.api_key);
        auth = (char *)malloc(n + 1);
        if (auth) {
            memcpy(auth, "Bearer ", strlen("Bearer "));
            memcpy(auth + strlen("Bearer "), sess->cfg.api_key,
                   strlen(sess->cfg.api_key) + 1);
        } else {
            puci_alloc_free(&sess->alloc, url);
            puci_strbuf_destroy(&req);
            return PUCI_ERR_NOMEM;
        }
    }

    /* Reset per-iteration accumulation state. */
    puci_strbuf_init(&run->turn.text, &sess->alloc);
    puci_strbuf_init(&run->turn.reasoning, &sess->alloc);
    run->prev_text_len = 0;
    run->prev_reasoning_len = 0;
    run->first_err = PUCI_OK;
    run->at_least_one_event = 0;
    run->turn.ntool_calls = 0;
    run->turn.tool_calls_cap = 0;
    run->turn.tool_calls = NULL;
    run->turn.had_tool_calls = 0;
    run->turn.had_reasoning = 0;
    run->turn.stream_done = 0;
    run->turn.finish_reason_stop = 0;
    run->turn.prompt_tokens = 0;
    run->turn.completion_tokens = 0;
    run->turn.cached_tokens = 0;

    run->parser = puci_sse_parser_create(&sess->alloc);
    if (!run->parser) {
        free(auth);
        puci_alloc_free(&sess->alloc, url);
        puci_strbuf_destroy(&req);
        return PUCI_ERR_NOMEM;
    }
    if (!sess->http_conn) {
        sess->http_conn = puci_http_conn_create();
        if (!sess->http_conn) {
            free(auth);
            puci_alloc_free(&sess->alloc, url);
            puci_strbuf_destroy(&req);
            return PUCI_ERR_NOMEM;
        }
    }

    /* Transfer ownership of the request body into the run. */
    size_t reqlen = req.len;
    run->req_body = dupn_new(puci_strbuf_cstr(&req), reqlen);
    if (!run->req_body) {
        free(auth);
        puci_alloc_free(&sess->alloc, url);
        puci_strbuf_destroy(&req);
        return PUCI_ERR_NOMEM;
    }
    puci_strbuf_destroy(&req);
    run->url = url;
    run->auth = auth;

    puci_http_request hreq;
    memset(&hreq, 0, sizeof(hreq));
    hreq.url = run->url;
    hreq.body = run->req_body;
    hreq.body_len = reqlen;
    hreq.auth_header = run->auth;
    hreq.content_type = "application/json";
    hreq.accept = "text/event-stream";
    hreq.timeout_ms = sess->cfg.timeout_ms;
    hreq.idle_timeout_ms = sess->cfg.stream_idle_timeout_ms;
    hreq.ca_bundle = sess->cfg.ca_bundle;
    hreq.tls_verify = sess->cfg.tls_verify;
    hreq.conn_dirty = &sess->conn_dirty;
    hreq.conn = sess->http_conn;
    if (sess->ctx->ninsecure_hosts > 0) {
        hreq.insecure_hosts = (const char *const *)sess->ctx->insecure_hosts;
        hreq.ninsecure_hosts = sess->ctx->ninsecure_hosts;
    }
    hreq.log_cb = sess->ctx->log_cb;
    hreq.log_userdata = sess->ctx->log_userdata;
    hreq.log_level = sess->ctx->log_level;

    /* The cancel token must outlive the request (the HTTP layer holds it by
     * borrowed pointer), so it lives in the run, not on this stack frame. */
    session_build_cancel_token(sess, &run->cancel_token);
    run->req = puci_http_begin(&sess->alloc, sess->http_conn, &hreq,
                               &run->cancel_token, on_http_data, sess);
    if (!run->req) {
        /* run_free will release url/auth/req_body/parser we set on success
         * path; but on early failure we already own url/auth/req_body. Mirror
         * the failure cleanup here since run_free is not reached. */
        puci_sse_parser_destroy(run->parser);
        run->parser = NULL;
        free(run->req_body);
        run->req_body = NULL;
        free(run->auth);
        run->auth = NULL;
        puci_alloc_free(&sess->alloc, run->url);
        run->url = NULL;
        return PUCI_ERR_NOMEM;
    }
    run->state = PUCI_TS_STREAMING;
    return PUCI_OK;
}

/* Advances the in-flight request by one step. Returns 0 if still streaming
 * (host must wait on fds), or 1 if the transfer reached a terminal condition
 * (the run state has moved on / run freed). */
static int run_stream_step(puci_session *sess, struct puci_turn_run *run) {
    int done = 0, hs = 0;
    char err[256];
    err[0] = '\0';
    puci_status st = puci_http_step(run->req, &done, &hs, err, sizeof(err));
    if (!done) {
        return 0;
    }
    run->http_status = hs;
    if (run->parser) {
        (void)puci_sse_parser_flush(run->parser, on_sse_event, sess);
    }
    if (st == PUCI_ERR_CANCELLED) {
        fin_cancelled(sess, run);
        return 1;
    }
    if (st != PUCI_OK) {
        fin_error(sess, run, st, err[0] ? err : "network error");
        return 1;
    }
    if (hs >= 400) {
        char msg[256];
        if (err[0]) {
            snprintf(msg, sizeof(msg), "upstream returned HTTP %d: %s", hs,
                     err);
        } else {
            snprintf(msg, sizeof(msg), "upstream returned HTTP %d", hs);
        }
        puci_status st2 = (hs == 401 || hs == 403) ? PUCI_ERR_UNAUTHORIZED
                                                   : PUCI_ERR_PROTOCOL;
        fin_error(sess, run, st2, msg);
        return 1;
    }
    if (run->first_err != PUCI_OK) {
        fin_error(sess, run, PUCI_ERR_PROTOCOL, "malformed stream from provider");
        return 1;
    }
    sess->total_prompt_tokens += run->turn.prompt_tokens;
    sess->total_completion_tokens += run->turn.completion_tokens;
    sess->total_cached_tokens += run->turn.cached_tokens;

    const char *text = puci_strbuf_cstr(&run->turn.text);
    run->stream_text = dupstr(text);
    run->want_tools = tool_call_ready(&run->turn) && run->turn.had_tool_calls &&
                      run->iter + 1 < run->max_iter;
    run->state = PUCI_TS_STREAM_COMPLETE;
    return 1;
}

/* Finalises the completed stream: commits the assistant message (with tool
 * calls, if any) into history, then either dispatches tools or terminates.
 * Returns 1 if the turn finished (run freed), 0 to continue. */
static int run_finalize_stream(puci_session *sess, struct puci_turn_run *run) {
    const char *text = run->stream_text ? run->stream_text : "";
    puci_status st = session_msg_begin(sess, PUCI_ROLE_ASSISTANT, NULL);
    if (st != PUCI_OK) {
        fin_error(sess, run, st, "failed to begin assistant message");
        return 1;
    }
    if (run->want_tools) {
        st = session_msg_finish_assistant(sess, text, run->turn.tool_calls,
                                          run->turn.ntool_calls);
        /* Ownership of the tool_calls array transfers to the message on
         * success; clear the run's view so run_free does not double-free. On
         * failure the array stays owned by run (finish_assistant only
         * transfers on success). */
        run->turn.tool_calls = NULL;
        run->turn.ntool_calls = 0;
        run->turn.tool_calls_cap = 0;
    } else {
        st = session_msg_finish_assistant(sess, text, NULL, 0);
    }
    if (st != PUCI_OK) {
        fin_error(sess, run, st, "failed to commit assistant message");
        return 1;
    }
    if (run->want_tools) {
        run->iter++;
        run->state = PUCI_TS_DISPATCHING_TOOLS;
        return 0;
    }
    /* Final answer: no (more) tool calls. TURN_COMPLETE carries the final
     * text from the committed message. */
    const char *final_text =
        sess->nmessages > 0 ? sess->messages[sess->nmessages - 1].content
                            : NULL;
    fin_complete(sess, run, final_text);
    return 1;
}

/* Runs the tool calls carried by the last committed assistant message and
 * appends their results. Stage A: tools run synchronously here; see DOGFOOD /
 * issue #2 for the async tool lifecycle that removes this blocking step.
 * Returns 1 if the turn finished, 0 to continue to the next iteration. */
static int run_execute_tools(puci_session *sess, struct puci_turn_run *run) {
    puci_msg *am =
        sess->nmessages > 0 ? &sess->messages[sess->nmessages - 1] : NULL;
    if (!am) {
        fin_error(sess, run, PUCI_ERR_OUT_OF_STATE,
                  "no assistant message to dispatch tools");
        return 1;
    }
    size_t ntcs = am->ntool_calls;
    char **contents = (ntcs > 0) ? (char **)calloc(ntcs, sizeof(char *)) : NULL;
    if (ntcs > 0 && !contents) {
        fin_error(sess, run, PUCI_ERR_NOMEM, "out of memory");
        return 1;
    }
    int cancelled = 0;
    puci_cancel_token ctok;
    session_build_cancel_token(sess, &ctok);

    for (size_t k = 0; k < ntcs && !cancelled; k++) {
        puci_internal_tool_call *slot = &am->tool_calls[k];
        if (slot->name.len == 0) {
            continue;
        }
        puci_tool_call pub;
        pub.name = puci_strbuf_cstr(&slot->name);
        pub.id = slot->id ? slot->id : "";
        pub.args_json = puci_strbuf_cstr(&slot->args);
        qtool_start(sess, pub.name, pub.id, pub.args_json);

        char *content = NULL;
        int is_err = 0;
        puci_tool *tool = NULL;
        if (puci_tools_lookup(sess->ctx, pub.name, &tool) && tool &&
            tool->invoke) {
            char *inv_res = NULL;
            int rcerr = 0;
            int runit = 1;
            if (sess->approve_cb) {
                puci_arena ar;
                puci_approval_request apreq;
                puci_arena_init(&ar, &sess->alloc, 0);
                build_approval_request(sess, &pub, &ar, &apreq, &ctok);
                int ap_cancelled = sess->cancelled ? 1 : 0;
                puci_approval ap = ap_cancelled
                                       ? PUCI_APPROVAL_DENY
                                       : sess->approve_cb(&apreq,
                                                          sess->approve_userdata);
                if (sess->cancelled) {
                    ap_cancelled = 1;
                }
                destroy_approval_request(&apreq);
                puci_arena_destroy(&ar);
                if (ap_cancelled) {
                    cancelled = 1;
                    break;
                }
                if (ap == PUCI_APPROVAL_DENY) {
                    inv_res = strdup(
                        "{\"ok\":false,\"error\":\"blocked by approval policy\"}");
                    rcerr = 1;
                    runit = 0;
                }
            }
            if (runit) {
                rcerr = tool->invoke(tool->userdata, &ctok, pub.args_json,
                                     &inv_res);
            }
            content = inv_res;
            if (!content) {
                content = strdup("{\"ok\":false,\"error\":\"tool error\"}");
            }
            is_err = rcerr || !inv_res;
        } else {
            content = strdup("{\"ok\":false,\"error\":\"unknown tool\"}");
            is_err = 1;
        }
        qtool_end(sess, pub.id, content, is_err);
        contents[k] = content;
    }

    /* Append the tool-result messages after the assistant (which carries the
     * calls). The assistant was committed by run_finalize_stream. */
    for (size_t i = 0; i < ntcs && !cancelled; i++) {
        if (am->tool_calls[i].name.len == 0) {
            continue;
        }
        if (contents[i]) {
            session_msg_begin(sess, PUCI_ROLE_TOOL, contents[i]);
            session_msg_set_tool_call_id(sess, am->tool_calls[i].id);
        }
    }
    for (size_t i = 0; i < ntcs; i++) {
        free(contents[i]);
    }
    free(contents);

    if (cancelled) {
        fin_cancelled(sess, run);
        return 1;
    }
    if (run->iter >= run->max_iter) {
        const char *final_text =
            sess->nmessages > 0 ? sess->messages[sess->nmessages - 1].content
                                : NULL;
        fin_complete(sess, run, final_text);
        return 1;
    }
    run->state = PUCI_TS_BUILD_REQUEST;
    return 0;
}

/* Whether cancellation has been requested (beyond the async-signal-safe flag;
 * also drains the transport so a latched cancel is not re-triggered). */
static int cancel_requested(puci_session *sess) {
    return sess->cancelled ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Public async API                                                    */
/* ------------------------------------------------------------------ */

puci_status puci_agent_begin_turn(puci_session *sess, const char *message) {
    if (!sess || !message) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (sess->run || sess->in_process) {
        return PUCI_ERR_OUT_OF_STATE;
    }
    session_ensure_cancel_fd(sess);

    struct puci_turn_run *run =
        (struct puci_turn_run *)puci_alloc_calloc(&sess->alloc, 1,
                                                  sizeof(struct puci_turn_run));
    if (!run) {
        return PUCI_ERR_NOMEM;
    }
    run->state = PUCI_TS_BUILD_REQUEST;
    run->iter = 0;
    run->max_iter =
        (int)(sess->max_tool_iterations ? sess->max_tool_iterations : 32);
    run->backoff_deadline_ms = -1;
    run->backoff_pending = 0;
    run->cancelled_latched = 0;
    run->terminal_status = PUCI_OK;
    run->done_text = NULL;
    run->in_process = 0;
    sess->last_status = PUCI_OK;
    sess->run = run;

    /* Clear any latched cancellation AND drain the cancel fd so the fresh turn
     * starts clean (a prior cancel must not bleed in, and a leftover readable
     * byte would make a host pump spin on the cancel fd). */
    session_cancel_clear(sess);
    puci_status st = session_msg_begin(sess, PUCI_ROLE_USER, message);
    if (st != PUCI_OK) {
        sess->last_status = st;
        run_free(sess, run);
        return st;
    }
    return PUCI_OK;
}

puci_status puci_agent_get_fds(puci_session *sess, struct pollfd *fds,
                               size_t cap, size_t *out_count) {
    if (!sess || !out_count) {
        return PUCI_ERR_INVALID_ARG;
    }
    size_t n = 0;
    struct puci_turn_run *run = sess->run;
    if (run && run->req) {
        n = puci_http_get_fds(run->req, fds, cap);
    }
    if (cap == 0) {
        /* Sizing query: report the required count (http fds + cancel fd).
         * puci_http_get_fds wrote nothing here (cap==0), so query the count. */
        size_t required = 0;
        if (run && run->req) {
            required += puci_http_fd_count(run->req);
        }
        if (sess->cancel_fd >= 0) {
            required++;
        }
        *out_count = required;
        return PUCI_OK;
    }
    int cfd = sess->cancel_fd;
    if (n < cap && cfd >= 0) {
        fds[n].fd = cfd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    *out_count = n;
    return PUCI_OK;
}

puci_status puci_agent_next_timeout_ms(puci_session *sess, int *out_ms) {
    if (!sess || !out_ms) {
        return PUCI_ERR_INVALID_ARG;
    }
    struct puci_turn_run *run = sess->run;
    if (!run) {
        *out_ms = -1;
        return PUCI_OK;
    }
    if (run->state == PUCI_TS_RETRY_BACKOFF && run->backoff_pending) {
        long long rem = run->backoff_deadline_ms - now_ms();
        *out_ms = rem <= 0 ? 0
                           : (rem > 0x7FFFFFFFLL ? 0x7FFFFFFF : (int)rem);
        return PUCI_OK;
    }
    if (run->req) {
        size_t nfds = puci_http_fd_count(run->req);
        int t = puci_http_next_timeout_ms(run->req);
        if (nfds > 0) {
            /* curl's timeout is an UPPER BOUND. With fds to wait on, floor to a
             * small positive value (curl's own driver uses ~1000 ms) so a host
             * never busy-spins; poll() returns as soon as an fd is ready. */
            *out_ms = t <= 0 ? 1000 : (t > 1000 ? 1000 : t);
        } else if (t == 0) {
            /* No pollable fds yet (e.g. mid-connect) but curl reports work
             * pending: return 0 so the host calls process() immediately to
             * advance, rather than blocking (which would starve the connect
             * that the socket creation depends on). */
            *out_ms = 0;
        } else if (t > 0) {
            *out_ms = t; /* no fds, finite wait */
        } else {
            /* t < 0 and no fds: nothing pending. Fall through so the state
             * machine (e.g. STREAM_COMPLETE awaiting finalisation) is served. */
        }
        if (!(nfds == 0 && t < 0)) {
            return PUCI_OK;
        }
    }
    switch (run->state) {
    case PUCI_TS_STREAM_COMPLETE:
    case PUCI_TS_DISPATCHING_TOOLS:
    case PUCI_TS_BUILD_REQUEST:
        *out_ms = 0; /* work pending; host should call process() now */
        break;
    default:
        *out_ms = -1;
        break;
    }
    return PUCI_OK;
}

/* Drives the turn by one bounded step. Performs a small, fixed amount of work
 * and returns; never loops-until-done and never blocks on network or sleep. */
puci_status puci_agent_process(puci_session *sess, const struct pollfd *fds,
                               size_t count) {
    if (!sess) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (!sess->run) {
        return PUCI_ERR_OUT_OF_STATE;
    }
    if (sess->in_process || sess->run->in_process) {
        return PUCI_ERR_OUT_OF_STATE;
    }
    sess->in_process = 1;
    sess->run->in_process = 1;

    for (int guard = 0; guard < 8; guard++) {
        if (!sess->run) {
            break; /* turn already ended (e.g. teardown after a terminal) */
        }
        struct puci_turn_run *run = sess->run;
        if (cancel_requested(sess)) {
            run->cancelled_latched = 1;
        }
        switch (run->state) {
        case PUCI_TS_BUILD_REQUEST: {
            puci_status st = run_build_request(sess, run);
            if (st != PUCI_OK) {
                const char *e = puci_session_error(sess);
                fin_error(sess, run, st, e ? e : "request build failed");
            }
            /* Once the request is streaming we need the host's fds before
             * doing more; yield to return to the caller's poll(). */
            if (sess->run && sess->run->state == PUCI_TS_STREAMING) {
                goto yield;
            }
            break;
        }
        case PUCI_TS_STREAMING: {
            /* Feed poll results to the HTTP layer so it knows which sockets are
             * ready for I/O, allowing curl to efficiently make progress. */
            (void)puci_http_process_fds(run->req, fds, count);
            if (run_stream_step(sess, run)) {
                break; /* transfer ended; loop handles new state */
            }
            goto yield; /* still streaming; wait on fds */
        }
        case PUCI_TS_STREAM_COMPLETE: {
            (void)run_finalize_stream(sess, run);
            break;
        }
        case PUCI_TS_DISPATCHING_TOOLS: {
            (void)run_execute_tools(sess, run);
            break;
        }
        case PUCI_TS_RETRY_BACKOFF: {
            if (run->backoff_pending && now_ms() >= run->backoff_deadline_ms) {
                run->backoff_pending = 0;
                run->state = PUCI_TS_BUILD_REQUEST;
            } else {
                goto yield;
            }
            break;
        }
        case PUCI_TS_TURN_DONE:
        case PUCI_TS_FATAL:
        case PUCI_TS_IDLE:
        case PUCI_TS_AWAITING_CONNECT:
        default:
            goto yield;
        }
    }

yield:
    sess->in_process = 0;
    if (sess->run) {
        sess->run->in_process = 0;
    }
    return sess->last_status;
}

int puci_agent_next_event(puci_session *sess, puci_event *out) {
    if (!sess || !out) {
        return 0;
    }
    struct puci_evnode *q = sess->ev_head;
    if (!q) {
        return 0;
    }
    sess->ev_head = q->next;
    if (!sess->ev_head) {
        sess->ev_tail = NULL;
    }
    *out = q->ev;
    evnode_free(sess, q);
    return 1;
}

int puci_agent_cancel_fd(puci_session *sess) {
    return puci_session_cancel_fd(sess);
}

puci_status puci_agent_last_status(const struct puci_session *sess) {
    return sess ? sess->last_status : PUCI_ERR_INVALID_ARG;
}

/* Frees the run mid-flight (destroy/teardown) without synthesising history,
 * and flushes the event queue. Used by puci_session_destroy to support
 * destroying an agent while a turn is streaming or a tool is running. */
void turn_run_force_teardown(puci_session *sess) {
    if (!sess) {
        return;
    }
    if (sess->run) {
        run_free(sess, sess->run);
    }
    eventq_flush(sess);
}

/* ------------------------------------------------------------------ */
/* Blocking convenience wrapper                                        */
/* ------------------------------------------------------------------ */

/* Forwards one queued event to the session event callback. */
static void replay_to_cb(puci_session *sess, const struct puci_evnode *n) {
    if (!n) {
        return;
    }
    switch (n->ev) {
    case PUCI_EVENT_TOKEN_DELTA:
    case PUCI_EVENT_REASONING_DELTA:
    case PUCI_EVENT_TURN_COMPLETE:
    case PUCI_EVENT_ERROR:
        if (sess->event_cb) {
            sess->event_cb(sess->event_userdata, sess, n->ev, n->text);
        }
        break;
    case PUCI_EVENT_TURN_CANCELLED:
        if (sess->event_cb) {
            sess->event_cb(sess->event_userdata, sess,
                           PUCI_EVENT_TURN_CANCELLED, NULL);
        }
        break;
    case PUCI_EVENT_TOOL_CALL_START: {
        puci_tool_call tc;
        tc.name = n->tc_name;
        tc.id = n->tc_id;
        tc.args_json = n->tc_args;
        if (sess->event_cb) {
            sess->event_cb(sess->event_userdata, sess, n->ev, &tc);
        }
        break;
    }
    case PUCI_EVENT_TOOL_CALL_END: {
        puci_tool_result tr;
        tr.call_id = n->tr_call_id;
        tr.content = n->tr_content;
        tr.is_error = n->tr_is_error;
        if (sess->event_cb) {
            sess->event_cb(sess->event_userdata, sess, n->ev, &tr);
        }
        break;
    }
    default:
        break;
    }
}

/* Drains every queued event to the session callback, freeing each node. */
static void drain_to_cb(puci_session *sess) {
    while (sess->ev_head) {
        struct puci_evnode *q = sess->ev_head;
        replay_to_cb(sess, q);
        sess->ev_head = q->next;
        if (!sess->ev_head) {
            sess->ev_tail = NULL;
        }
        evnode_free(sess, q);
    }
}

/* Blocking convenience wrapper: pumps the state machine until the turn ends,
 * delivering events synchronously via the session's event callback. This is a
 * thin wrapper; all turn logic lives in the states above. */
puci_status puci_agent_run_turn(puci_session *sess, const char *message) {
    if (!sess || !message) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (sess->run) {
        return PUCI_ERR_OUT_OF_STATE;
    }
    session_cancel_clear(sess);
    puci_status st = puci_agent_begin_turn(sess, message);
    if (st != PUCI_OK) {
        return st;
    }
    for (;;) {
        if (!sess->run) {
            break;
        }
        int ms = 1000;
        puci_agent_next_timeout_ms(sess, &ms);
        struct pollfd fds[33];
        size_t n = 0;
        puci_agent_get_fds(sess, fds, 33, &n);
        int cfd = sess->cancel_fd;
        if (cfd >= 0 && n < 33) {
            fds[n].fd = cfd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        ms = ms < 0 ? 1000 : ms;
        if (ms > 1000) {
            ms = 1000;
        }
        (void)poll(fds, (nfds_t)n, ms);
        puci_agent_process(sess, fds, n);
        drain_to_cb(sess);
    }
    drain_to_cb(sess);
    return sess->last_status;
}
