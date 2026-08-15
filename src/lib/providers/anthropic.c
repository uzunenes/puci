/*
 * providers/anthropic.c - Anthropic Messages API provider backend.
 */

#include "puci_internal.h"
#include "json/json.h"
#include "util/arena.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static puci_status ensure_anthropic_tool_slots(puci_turn *turn, size_t index) {
    if (index < turn->tool_calls_cap) {
        return PUCI_OK;
    }
    size_t ncap = turn->tool_calls_cap ? turn->tool_calls_cap * 2 : 4;
    while (ncap <= index) {
        ncap *= 2;
    }
    size_t bytes = ncap * sizeof(puci_internal_tool_call);
    puci_internal_tool_call *nt =
        (puci_internal_tool_call *)puci_alloc_malloc(&turn->text.alloc, bytes);
    if (!nt) {
        return PUCI_ERR_NOMEM;
    }
    memset(nt, 0, bytes);
    if (turn->tool_calls) {
        memcpy(nt, turn->tool_calls, turn->ntool_calls * sizeof(puci_internal_tool_call));
        puci_alloc_free(&turn->text.alloc, turn->tool_calls);
    }
    turn->tool_calls = nt;
    turn->tool_calls_cap = ncap;
    return PUCI_OK;
}

static puci_status anthropic_build_request(const puci_session *sess,
                                           const char *msgs_json, const char *tools_json,
                                           puci_strbuf *out) {
    puci_arena arena;
    puci_arena_init(&arena, NULL, 0);

    const char *model = sess->cfg.model ? sess->cfg.model : "claude-3-5-sonnet-20241022";
    puci_strbuf_appendz(out, "{\"model\":");
    puci_strbuf_append_json_string(out, model);
    puci_strbuf_appendz(out, ",\"max_tokens\":4096,\"stream\":true");

    if (sess->system_prompt && sess->system_prompt[0]) {
        puci_strbuf_appendz(out, ",\"system\":");
        puci_strbuf_append_json_string(out, sess->system_prompt);
    }

    /* Convert messages */
    pj_val *root_msgs = NULL;
    if (msgs_json && pj_parse(&arena, msgs_json, strlen(msgs_json), &root_msgs) == 0 && root_msgs && root_msgs->type == PJ_ARR) {
        puci_strbuf_appendz(out, ",\"messages\":[");
        int wrote_msg = 0;
        for (size_t i = 0; i < root_msgs->u.arr.len; i++) {
            pj_val *m = root_msgs->u.arr.items[i];
            if (!m || m->type != PJ_OBJ) continue;
            pj_val *rv = pj_get(m, "role");
            const char *role = rv ? pj_str(rv) : "user";
            if (strcmp(role, "system") == 0) {
                continue; /* System handled via top-level system parameter */
            }

            if (wrote_msg) puci_strbuf_append_char(out, ',');
            wrote_msg = 1;

            if (strcmp(role, "user") == 0) {
                pj_val *cv = pj_get(m, "content");
                const char *cnt = cv ? pj_str(cv) : "";
                puci_strbuf_appendz(out, "{\"role\":\"user\",\"content\":");
                puci_strbuf_append_json_string(out, cnt);
                puci_strbuf_append_char(out, '}');
            } else if (strcmp(role, "assistant") == 0) {
                pj_val *cv = pj_get(m, "content");
                const char *cnt = cv ? pj_str(cv) : "";
                pj_val *tcs = pj_get(m, "tool_calls");
                if (tcs && tcs->type == PJ_ARR && tcs->u.arr.len > 0) {
                    puci_strbuf_appendz(out, "{\"role\":\"assistant\",\"content\":[");
                    int wrote_content = 0;
                    if (cnt && cnt[0]) {
                        puci_strbuf_appendz(out, "{\"type\":\"text\",\"text\":");
                        puci_strbuf_append_json_string(out, cnt);
                        puci_strbuf_append_char(out, '}');
                        wrote_content = 1;
                    }
                    for (size_t k = 0; k < tcs->u.arr.len; k++) {
                        pj_val *tc = tcs->u.arr.items[k];
                        if (!tc || tc->type != PJ_OBJ) continue;
                        pj_val *idv = pj_get(tc, "id");
                        pj_val *fn = pj_get(tc, "function");
                        pj_val *fn_name = fn ? pj_get(fn, "name") : NULL;
                        pj_val *fn_args = fn ? pj_get(fn, "arguments") : NULL;

                        if (wrote_content) puci_strbuf_append_char(out, ',');
                        wrote_content = 1;

                        puci_strbuf_appendz(out, "{\"type\":\"tool_use\",\"id\":");
                        puci_strbuf_append_json_string(out, idv ? pj_str(idv) : "");
                        puci_strbuf_appendz(out, ",\"name\":");
                        puci_strbuf_append_json_string(out, fn_name ? pj_str(fn_name) : "");
                        puci_strbuf_appendz(out, ",\"input\":");
                        const char *args_str = fn_args ? pj_str(fn_args) : "{}";
                        if (args_str && args_str[0] == '{') {
                            puci_strbuf_appendz(out, args_str);
                        } else {
                            puci_strbuf_appendz(out, "{}");
                        }
                        puci_strbuf_append_char(out, '}');
                    }
                    puci_strbuf_appendz(out, "]}");
                } else {
                    puci_strbuf_appendz(out, "{\"role\":\"assistant\",\"content\":");
                    puci_strbuf_append_json_string(out, cnt);
                    puci_strbuf_append_char(out, '}');
                }
            } else if (strcmp(role, "tool") == 0) {
                pj_val *tcid = pj_get(m, "tool_call_id");
                pj_val *cv = pj_get(m, "content");
                const char *cid = tcid ? pj_str(tcid) : "";
                const char *cnt = cv ? pj_str(cv) : "";
                puci_strbuf_appendz(out, "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":");
                puci_strbuf_append_json_string(out, cid);
                puci_strbuf_appendz(out, ",\"content\":");
                puci_strbuf_append_json_string(out, cnt);
                puci_strbuf_appendz(out, "}]}");
            }
        }
        puci_strbuf_append_char(out, ']');
    }

    /* Convert tools */
    if (tools_json && *tools_json) {
        pj_val *root_tools = NULL;
        if (pj_parse(&arena, tools_json, strlen(tools_json), &root_tools) == 0 && root_tools && root_tools->type == PJ_ARR) {
            puci_strbuf_appendz(out, ",\"tools\":[");
            int wrote_tool = 0;
            for (size_t i = 0; i < root_tools->u.arr.len; i++) {
                pj_val *item = root_tools->u.arr.items[i];
                if (!item || item->type != PJ_OBJ) continue;
                pj_val *fn = pj_get(item, "function");
                if (!fn || fn->type != PJ_OBJ) fn = item;

                pj_val *name_v = pj_get(fn, "name");
                pj_val *desc_v = pj_get(fn, "description");
                pj_val *params_v = pj_get(fn, "parameters");
                if (!name_v) continue;

                if (wrote_tool) puci_strbuf_append_char(out, ',');
                wrote_tool = 1;

                puci_strbuf_appendz(out, "{\"name\":");
                puci_strbuf_append_json_string(out, pj_str(name_v));
                puci_strbuf_appendz(out, ",\"description\":");
                puci_strbuf_append_json_string(out, desc_v ? pj_str(desc_v) : "");
                puci_strbuf_appendz(out, ",\"input_schema\":");
                if (params_v) {
                    pj_serialize(params_v, out);
                } else {
                    puci_strbuf_appendz(out, "{\"type\":\"object\"}");
                }
                puci_strbuf_append_char(out, '}');
            }
            puci_strbuf_append_char(out, ']');
        }
    }

    puci_strbuf_append_char(out, '}');
    puci_arena_destroy(&arena);
    return PUCI_OK;
}

static puci_status anthropic_parse_stream(const puci_session *sess, puci_arena *scratch,
                                          const char *data, size_t len, puci_turn *turn) {
    (void)sess;
    pj_val *root = NULL;
    if (pj_parse(scratch, data, len, &root) != 0 || !root || root->type != PJ_OBJ) {
        return PUCI_ERR_PROTOCOL;
    }

    pj_val *tv = pj_get(root, "type");
    const char *type = tv ? pj_str(tv) : "";

    if (strcmp(type, "message_start") == 0) {
        pj_val *msg = pj_get(root, "message");
        if (msg) {
            pj_val *usage = pj_get(msg, "usage");
            if (usage) {
                pj_val *it = pj_get(usage, "input_tokens");
                pj_val *ot = pj_get(usage, "output_tokens");
                if (it && it->type == PJ_NUM) turn->prompt_tokens = (long long)it->u.num;
                if (ot && ot->type == PJ_NUM) turn->completion_tokens = (long long)ot->u.num;
            }
        }
    } else if (strcmp(type, "content_block_start") == 0) {
        pj_val *idx_v = pj_get(root, "index");
        size_t idx = (idx_v && idx_v->type == PJ_NUM) ? (size_t)idx_v->u.num : 0;
        pj_val *cb = pj_get(root, "content_block");
        if (cb) {
            pj_val *cb_type = pj_get(cb, "type");
            const char *cbt = cb_type ? pj_str(cb_type) : "";
            if (strcmp(cbt, "tool_use") == 0) {
                ensure_anthropic_tool_slots(turn, idx);
                turn->had_tool_calls = 1;
                if (idx >= turn->ntool_calls) {
                    turn->ntool_calls = idx + 1;
                }
                puci_internal_tool_call *slot = &turn->tool_calls[idx];
                pj_val *idv = pj_get(cb, "id");
                if (idv && pj_str(idv)) {
                    slot->id = puci_alloc_strdup(&turn->text.alloc, pj_str(idv));
                }
                pj_val *nv = pj_get(cb, "name");
                puci_strbuf_init(&slot->name, &turn->text.alloc);
                if (nv && pj_str(nv)) {
                    puci_strbuf_appendz(&slot->name, pj_str(nv));
                }
                puci_strbuf_init(&slot->args, &turn->text.alloc);
            }
        }
    } else if (strcmp(type, "content_block_delta") == 0) {
        pj_val *idx_v = pj_get(root, "index");
        size_t idx = (idx_v && idx_v->type == PJ_NUM) ? (size_t)idx_v->u.num : 0;
        pj_val *delta = pj_get(root, "delta");
        if (delta) {
            pj_val *dt_v = pj_get(delta, "type");
            const char *dt = dt_v ? pj_str(dt_v) : "";
            if (strcmp(dt, "text_delta") == 0) {
                pj_val *txt_v = pj_get(delta, "text");
                if (txt_v && pj_str(txt_v)) {
                    puci_strbuf_appendz(&turn->text, pj_str(txt_v));
                }
            } else if (strcmp(dt, "input_json_delta") == 0) {
                pj_val *pj_v = pj_get(delta, "partial_json");
                if (pj_v && pj_str(pj_v) && idx < turn->ntool_calls) {
                    puci_strbuf_appendz(&turn->tool_calls[idx].args, pj_str(pj_v));
                }
            }
        }
    } else if (strcmp(type, "message_delta") == 0) {
        pj_val *usage = pj_get(root, "usage");
        if (usage) {
            pj_val *ot = pj_get(usage, "output_tokens");
            if (ot && ot->type == PJ_NUM) turn->completion_tokens += (long long)ot->u.num;
        }
        pj_val *delta = pj_get(root, "delta");
        if (delta) {
            pj_val *sr = pj_get(delta, "stop_reason");
            if (sr && pj_str(sr)) {
                turn->finish_reason_stop = 1;
            }
        }
    } else if (strcmp(type, "message_stop") == 0) {
        turn->stream_done = 1;
    }

    return PUCI_OK;
}

const puci_provider_vtable puci_anthropic_provider = {
    .name = "anthropic",
    .endpoint = "/v1/messages",
    .supports_tools = 1,
    .auth_prefix = "x-api-key: ",
    .extra_header = "anthropic-version: 2023-06-01",
    .build_request = anthropic_build_request,
    .parse_stream = anthropic_parse_stream,
};
