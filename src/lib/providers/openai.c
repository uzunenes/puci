/*
 * providers/openai.c - OpenAI-compatible provider backend.
 *
 * This one provider covers OpenAI, OpenRouter, vLLM, Ollama, and LM Studio
 * because they all speak the same /v1/chat/completions wire format (modulo a
 * base URL and API key).
 */

#include "puci_internal.h"

#include <string.h>

#include "json/json.h"

/* Ensure turn->tool_calls has room for at least index+1 entries. Strings are
 * borrowed via the allocator stored in turn->text.alloc. */
static puci_status ensure_tool_slots(puci_turn *turn, size_t index) {
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
        /* Move (not copy-deep) the existing slots. */
        memcpy(nt, turn->tool_calls, turn->ntool_calls * sizeof(puci_internal_tool_call));
        puci_alloc_free(&turn->text.alloc, turn->tool_calls);
    }
    turn->tool_calls = nt;
    turn->tool_calls_cap = ncap;
    return PUCI_OK;
}

static puci_status build_request(const puci_session *sess,
                                 const char *msgs_json, const char *tools_json,
                                 puci_strbuf *out) {
    puci_status st = puci_strbuf_append_fmt(out, "{\"model\":");
    if (st != PUCI_OK) {
        return st;
    }
    st = puci_strbuf_append_json_string(out, sess->cfg.model);
    if (st != PUCI_OK) {
        return st;
    }
    st = puci_strbuf_appendz(out, ",\"messages\":");
    if (st != PUCI_OK) {
        return st;
    }
    st = puci_strbuf_appendz(out, msgs_json);
    if (st != PUCI_OK) {
        return st;
    }
    if (tools_json) {
        st = puci_strbuf_appendz(out, ",\"tools\":");
        if (st != PUCI_OK) {
            return st;
        }
        st = puci_strbuf_appendz(out, tools_json);
        if (st != PUCI_OK) {
            return st;
        }
    }
    return puci_strbuf_appendz(out, ",\"stream\":true}");
}

static puci_status parse_stream(const puci_session *sess, puci_arena *scratch,
                                const char *data, size_t len, puci_turn *turn) {
    (void)sess;
    pj_val *root = NULL;
    if (pj_parse(scratch, data, len, &root) != 0) {
        return PUCI_ERR_PROTOCOL;
    }
    pj_val *choices = pj_get(root, "choices");
    pj_val *choice0 = choices ? pj_arr_get(choices, 0) : NULL;
    pj_val *delta = choice0 ? pj_get(choice0, "delta") : NULL;

    if (delta) {
        pj_val *content = pj_get(delta, "content");
        if (content && content->type == PJ_STR) {
            if (puci_strbuf_append(&turn->text, content->u.str.s,
                                   content->u.str.len) != PUCI_OK) {
                return PUCI_ERR_NOMEM;
            }
        }
        /* Reasoning content, if the model emits it separately, is accumulated
         * into its own buffer so it can be surfaced as distinct events and is
         * never concatenated into the final answer text. */
        pj_val *reasoning = pj_get(delta, "reasoning_content");
        if (reasoning && reasoning->type == PJ_STR) {
            turn->had_reasoning = 1;
            if (puci_strbuf_append(&turn->reasoning, reasoning->u.str.s,
                                   reasoning->u.str.len) != PUCI_OK) {
                return PUCI_ERR_NOMEM;
            }
        }
        pj_val *tool_calls = pj_get(delta, "tool_calls");
        if (tool_calls && tool_calls->type == PJ_ARR) {
            turn->had_tool_calls = 1;
            for (size_t i = 0; i < tool_calls->u.arr.len; i++) {
                pj_val *tc = tool_calls->u.arr.items[i];
                if (!tc || tc->type != PJ_OBJ) {
                    continue;
                }
                pj_val *idx = pj_get(tc, "index");
                size_t tindex =
                    (idx && idx->type == PJ_NUM) ? (size_t)idx->u.num : i;
                puci_status st = ensure_tool_slots(turn, tindex);
                if (st != PUCI_OK) {
                    return st;
                }
                puci_internal_tool_call *slot = &turn->tool_calls[tindex];
                if (tindex >= turn->ntool_calls) {
                    turn->ntool_calls = tindex + 1;
                }
                if (!slot->name.alloc.malloc_fn) {
                    puci_strbuf_init(&slot->name, &turn->text.alloc);
                    puci_strbuf_init(&slot->args, &turn->text.alloc);
                }
                pj_val *tid = pj_get(tc, "id");
                if (tid && tid->type == PJ_STR && !slot->id) {
                    slot->id = puci_alloc_strndup(&turn->text.alloc, tid->u.str.s,
                                                  tid->u.str.len);
                    if (!slot->id) {
                        return PUCI_ERR_NOMEM;
                    }
                }
                pj_val *fn = pj_get(tc, "function");
                if (fn && fn->type == PJ_OBJ) {
                    pj_val *fname = pj_get(fn, "name");
                    if (fname && fname->type == PJ_STR) {
                        if (puci_strbuf_append(&slot->name, fname->u.str.s,
                                               fname->u.str.len) != PUCI_OK) {
                            return PUCI_ERR_NOMEM;
                        }
                    }
                    pj_val *fargs = pj_get(fn, "arguments");
                    if (fargs && fargs->type == PJ_STR) {
                        if (puci_strbuf_append(&slot->args, fargs->u.str.s,
                                               fargs->u.str.len) != PUCI_OK) {
                            return PUCI_ERR_NOMEM;
                        }
                    }
                }
            }
        }
    }

    pj_val *fr = choice0 ? pj_get(choice0, "finish_reason") : NULL;
    if (fr && fr->type == PJ_STR) {
        if (strcmp(fr->u.str.s, "stop") == 0) {
            turn->finish_reason_stop = 1;
        }
        turn->stream_done = 1;
    }

    /* Usage is reported on the final chunk as {"usage": {"prompt_tokens":N,
     * "completion_tokens":M, "prompt_tokens_details":{"cached_tokens":K}}}. */
    pj_val *usage = pj_get(root, "usage");
    if (usage && usage->type == PJ_OBJ) {
        pj_val *pt = pj_get(usage, "prompt_tokens");
        if (pt && pt->type == PJ_NUM) {
            turn->prompt_tokens = (long long)pt->u.num;
        }
        pj_val *ct = pj_get(usage, "completion_tokens");
        if (ct && ct->type == PJ_NUM) {
            turn->completion_tokens = (long long)ct->u.num;
        }
        pj_val *details = pj_get(usage, "prompt_tokens_details");
        if (details && details->type == PJ_OBJ) {
            pj_val *cached = pj_get(details, "cached_tokens");
            if (cached && cached->type == PJ_NUM) {
                turn->cached_tokens = (long long)cached->u.num;
            }
        }
    }
    return PUCI_OK;
}

const puci_provider_vtable puci_openai_provider = {
    .name = "openai",
    .endpoint = "/chat/completions",
    .supports_tools = 1,
    .build_request = build_request,
    .parse_stream = parse_stream,
};