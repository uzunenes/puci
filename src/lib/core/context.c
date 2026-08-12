/*
 * core/context.c - context lifecycle, provider config, tool registry.
 *
 * The context owns the allocator, the logging callback, the provider registry,
 * and the tool registry. Sessions hang off a context.
 */

#include "puci_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools/tools_internal.h"

void puci_logf(puci_context *ctx, puci_log_level level, const char *fmt, ...) {
    if (!ctx || !ctx->log_cb || level < ctx->log_level) {
        return;
    }
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ctx->log_cb(ctx->log_userdata, level, buf);
}

static void set_err(puci_context *ctx, const char *msg) {
    if (ctx->errmsg) {
        puci_alloc_free(&ctx->alloc, (void *)ctx->errmsg);
        ctx->errmsg = NULL;
    }
    if (msg) {
        ctx->errmsg = puci_alloc_strdup(&ctx->alloc, msg);
    }
}

puci_status puci_context_create(const puci_context_options *opts,
                                puci_context **out) {
    if (!out) {
        return PUCI_ERR_INVALID_ARG;
    }
    puci_context *ctx =
        (puci_context *)calloc(1, sizeof(puci_context));
    if (!ctx) {
        return PUCI_ERR_NOMEM;
    }
    puci_alloc_resolve(&ctx->alloc, opts ? &opts->allocator : NULL);
    if (opts) {
        ctx->log_cb = opts->log_cb;
        ctx->log_userdata = opts->log_userdata;
        ctx->log_level = opts->log_level;
    } else {
        ctx->log_level = PUCI_LOG_INFO;
    }

    /* Register the default communities of providers and tools. */
    puci_status st = puci_provider_register_internal(ctx, &puci_openai_provider);
    if (st == PUCI_OK) {
        st = puci_register_tool_read_file(ctx);
    }
    if (st == PUCI_OK) {
        st = puci_register_tool_write_file(ctx);
    }
    if (st == PUCI_OK) {
        st = puci_register_tool_run_shell(ctx);
    }
    if (st != PUCI_OK) {
        /* Free partially constructed context. */
        puci_context_destroy(ctx);
        return st;
    }

    *out = ctx;
    return PUCI_OK;
}

static void free_tools(puci_context *ctx) {
    for (size_t i = 0; i < ctx->ntools; i++) {
        puci_tool *t = ctx->tools[i];
        if (!t) {
            continue;
        }
        if (t->spec.name) {
            puci_alloc_free(&ctx->alloc, (void *)t->spec.name);
        }
        if (t->spec.description) {
            puci_alloc_free(&ctx->alloc, (void *)t->spec.description);
        }
        if (t->spec.input_schema) {
            puci_alloc_free(&ctx->alloc, (void *)t->spec.input_schema);
        }
        puci_alloc_free(&ctx->alloc, (void *)t);
    }
    if (ctx->tools) {
        puci_alloc_free(&ctx->alloc, (void *)ctx->tools);
    }
    ctx->tools = NULL;
    ctx->ntools = 0;
    ctx->tools_cap = 0;
}

static void free_providers(puci_context *ctx) {
    for (size_t i = 0; i < ctx->nproviders; i++) {
        puci_provider *p = ctx->providers[i];
        if (!p) {
            continue;
        }
        if (p->name) {
            puci_alloc_free(&ctx->alloc, (void *)p->name);
        }
        puci_alloc_free(&ctx->alloc, (void *)p);
    }
    if (ctx->providers) {
        puci_alloc_free(&ctx->alloc, (void *)ctx->providers);
    }
    ctx->providers = NULL;
    ctx->nproviders = 0;
    ctx->providers_cap = 0;
}

static void free_configs(puci_context *ctx) {
    for (size_t i = 0; i < ctx->nconfigs; i++) {
        puci_provider_config *c = &ctx->configs[i];
        if (c->name) {
            puci_alloc_free(&ctx->alloc, (void *)c->name);
        }
        if (c->base_url) {
            puci_alloc_free(&ctx->alloc, (void *)c->base_url);
        }
        if (c->api_key) {
            puci_alloc_free(&ctx->alloc, (void *)c->api_key);
        }
        if (c->model) {
            puci_alloc_free(&ctx->alloc, (void *)c->model);
        }
        if (c->extra_headers) {
            puci_alloc_free(&ctx->alloc, (void *)c->extra_headers);
        }
    }
    if (ctx->configs) {
        puci_alloc_free(&ctx->alloc, (void *)ctx->configs);
    }
    ctx->configs = NULL;
    ctx->nconfigs = 0;
    ctx->configs_cap = 0;
}

void puci_context_destroy(puci_context *ctx) {
    if (!ctx) {
        return;
    }
    free_tools(ctx);
    free_providers(ctx);
    free_configs(ctx);
    if (ctx->errmsg) {
        puci_alloc_free(&ctx->alloc, (void *)ctx->errmsg);
    }
    free(ctx);
}

const char *puci_context_error(const puci_context *ctx) {
    return ctx ? ctx->errmsg : NULL;
}

/* Locates a provider config entry by name, returning NULL if absent. */
static puci_provider_config *find_config(puci_context *ctx, const char *name) {
    for (size_t i = 0; i < ctx->nconfigs; i++) {
        if (strcmp(ctx->configs[i].name, name) == 0) {
            return &ctx->configs[i];
        }
    }
    return NULL;
}

puci_status puci_context_set_provider(puci_context *ctx,
                                      const puci_provider_config *config) {
    if (!ctx || !config || !config->name) {
        return PUCI_ERR_INVALID_ARG;
    }
    if (!puci_provider_available(ctx, config->name)) {
        set_err(ctx, "provider not registered");
        return PUCI_ERR_NOT_FOUND;
    }
    puci_provider_config *slot = find_config(ctx, config->name);
    if (!slot) {
        if (ctx->nconfigs == ctx->configs_cap) {
            size_t ncap = ctx->configs_cap ? ctx->configs_cap * 2 : 4;
            puci_provider_config *nc = (puci_provider_config *)puci_alloc_malloc(
                &ctx->alloc, ncap * sizeof(puci_provider_config));
            if (!nc) {
                return PUCI_ERR_NOMEM;
            }
            memset(nc, 0, ncap * sizeof(puci_provider_config));
            if (ctx->configs) {
                memcpy(nc, ctx->configs,
                       ctx->nconfigs * sizeof(puci_provider_config));
                puci_alloc_free(&ctx->alloc, (void *)ctx->configs);
            }
            ctx->configs = nc;
            ctx->configs_cap = ncap;
        }
        slot = &ctx->configs[ctx->nconfigs++];
        memset(slot, 0, sizeof(*slot));
    }
    /* Copy strings. */
    char *name = puci_alloc_strdup(&ctx->alloc, config->name);
    char *base = config->base_url ? puci_alloc_strdup(&ctx->alloc, config->base_url) : NULL;
    char *key = config->api_key ? puci_alloc_strdup(&ctx->alloc, config->api_key) : NULL;
    char *model = config->model ? puci_alloc_strdup(&ctx->alloc, config->model) : NULL;
    char *extra = config->extra_headers ? puci_alloc_strdup(&ctx->alloc, config->extra_headers) : NULL;
    if (!name || (config->base_url && !base) || (config->api_key && !key) ||
        (config->model && !model) || (config->extra_headers && !extra)) {
        if (name) {
            puci_alloc_free(&ctx->alloc, (void *)name);
        }
        if (base) {
            puci_alloc_free(&ctx->alloc, (void *)base);
        }
        if (key) {
            puci_alloc_free(&ctx->alloc, (void *)key);
        }
        if (model) {
            puci_alloc_free(&ctx->alloc, (void *)model);
        }
        if (extra) {
            puci_alloc_free(&ctx->alloc, (void *)extra);
        }
        return PUCI_ERR_NOMEM;
    }
    /* Free old values. */
    if (slot->name) {
        puci_alloc_free(&ctx->alloc, (void *)slot->name);
    }
    if (slot->base_url) {
        puci_alloc_free(&ctx->alloc, (void *)slot->base_url);
    }
    if (slot->api_key) {
        puci_alloc_free(&ctx->alloc, (void *)slot->api_key);
    }
    if (slot->model) {
        puci_alloc_free(&ctx->alloc, (void *)slot->model);
    }
    if (slot->extra_headers) {
        puci_alloc_free(&ctx->alloc, (void *)slot->extra_headers);
    }
    slot->name = name;
    slot->base_url = base;
    slot->api_key = key;
    slot->model = model;
    slot->extra_headers = extra;
    slot->timeout_ms = config->timeout_ms;
    return PUCI_OK;
}

/* --- Tool registry helpers --- */

static puci_tool *find_tool(puci_context *ctx, const char *name) {
    if (!ctx || !name) {
        return NULL;
    }
    for (size_t i = 0; i < ctx->ntools; i++) {
        if (ctx->tools[i] && strcmp(ctx->tools[i]->spec.name, name) == 0) {
            return ctx->tools[i];
        }
    }
    return NULL;
}

int puci_tools_lookup(puci_context *ctx, const char *name, puci_tool **out) {
    puci_tool *t = find_tool(ctx, name);
    if (out) {
        *out = t;
    }
    return t != NULL;
}

puci_status puci_tool_register(puci_context *ctx, const puci_tool_spec *spec,
                               puci_tool_invoke_cb invoke_cb, void *userdata) {
    if (!ctx || !spec || !spec->name) {
        return PUCI_ERR_INVALID_ARG;
    }
    puci_tool *t = find_tool(ctx, spec->name);
    if (!t) {
        if (ctx->ntools == ctx->tools_cap) {
            size_t ncap = ctx->tools_cap ? ctx->tools_cap * 2 : 4;
            puci_tool **nt = (puci_tool **)puci_alloc_malloc(
                &ctx->alloc, ncap * sizeof(puci_tool *));
            if (!nt) {
                return PUCI_ERR_NOMEM;
            }
            if (ctx->tools) {
                memcpy(nt, ctx->tools, ctx->ntools * sizeof(puci_tool *));
                puci_alloc_free(&ctx->alloc, (void *)ctx->tools);
            }
            ctx->tools = nt;
            ctx->tools_cap = ncap;
        }
        t = (puci_tool *)puci_alloc_calloc(&ctx->alloc, 1, sizeof(puci_tool));
        if (!t) {
            return PUCI_ERR_NOMEM;
        }
        ctx->tools[ctx->ntools++] = t;
    } else {
        /* Free old owned strings before replacing. */
        if (t->spec.name) {
            puci_alloc_free(&ctx->alloc, (void *)t->spec.name);
        }
        if (t->spec.description) {
            puci_alloc_free(&ctx->alloc, (void *)t->spec.description);
        }
        if (t->spec.input_schema) {
            puci_alloc_free(&ctx->alloc, (void *)t->spec.input_schema);
        }
        t->spec.name = NULL;
        t->spec.description = NULL;
        t->spec.input_schema = NULL;
    }
    t->spec.name = puci_alloc_strdup(&ctx->alloc, spec->name);
    if (spec->description) {
        t->spec.description = puci_alloc_strdup(&ctx->alloc, spec->description);
    }
    if (spec->input_schema) {
        t->spec.input_schema = puci_alloc_strdup(&ctx->alloc, spec->input_schema);
    }
    t->invoke = invoke_cb;
    t->userdata = userdata;
    if (!t->spec.name || (spec->description && !t->spec.description) ||
        (spec->input_schema && !t->spec.input_schema)) {
        return PUCI_ERR_NOMEM;
    }
    return PUCI_OK;
}

const puci_tool_spec *puci_tool_get(const puci_context *ctx, const char *name) {
    if (!ctx || !name) {
        return NULL;
    }
    for (size_t i = 0; i < ctx->ntools; i++) {
        if (ctx->tools[i] && strcmp(ctx->tools[i]->spec.name, name) == 0) {
            return &ctx->tools[i]->spec;
        }
    }
    return NULL;
}