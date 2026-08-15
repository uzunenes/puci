/*
 * providers/registry.c - provider registry on the context.
 */

#include "puci_internal.h"

#include <string.h>

#include "json/json.h"

static const puci_provider_vtable *g_builtin_known[] = { &puci_openai_provider, &puci_anthropic_provider };

puci_status puci_provider_register_internal(struct puci_context *ctx,
                                            const puci_provider_vtable *vt) {
    if (!ctx || !vt || !vt->name) {
        return PUCI_ERR_INVALID_ARG;
    }
    /* Replace existing provider with the same name. */
    for (size_t i = 0; i < ctx->nproviders; i++) {
        if (strcmp(ctx->providers[i]->name, vt->name) == 0) {
            ctx->providers[i]->vtable = vt;
            return PUCI_OK;
        }
    }
    if (ctx->nproviders == ctx->providers_cap) {
        size_t ncap = ctx->providers_cap ? ctx->providers_cap * 2 : 4;
        puci_provider **np = (puci_provider **)puci_alloc_malloc(
            &ctx->alloc, ncap * sizeof(puci_provider *));
        if (!np) {
            return PUCI_ERR_NOMEM;
        }
        if (ctx->providers) {
            memcpy(np, ctx->providers,
                   ctx->nproviders * sizeof(puci_provider *));
            puci_alloc_free(&ctx->alloc, ctx->providers);
        }
        ctx->providers = np;
        ctx->providers_cap = ncap;
    }
    puci_provider *p = (puci_provider *)puci_alloc_calloc(&ctx->alloc, 1,
                                                          sizeof(puci_provider));
    if (!p) {
        return PUCI_ERR_NOMEM;
    }
    p->name = puci_alloc_strdup(&ctx->alloc, vt->name);
    if (!p->name) {
        puci_alloc_free(&ctx->alloc, p);
        return PUCI_ERR_NOMEM;
    }
    p->vtable = vt;
    ctx->providers[ctx->nproviders++] = p;
    return PUCI_OK;
}

puci_status puci_provider_register(puci_context *ctx, const char *name) {
    if (!ctx || !name) {
        return PUCI_ERR_INVALID_ARG;
    }
    for (size_t i = 0;
         i < sizeof(g_builtin_known) / sizeof(g_builtin_known[0]); i++) {
        if (strcmp(g_builtin_known[i]->name, name) == 0) {
            return puci_provider_register_internal(ctx, g_builtin_known[i]);
        }
    }
    return PUCI_ERR_NOT_FOUND;
}

puci_provider *puci_provider_get(const puci_context *ctx, const char *name) {
    if (!ctx || !name) {
        return NULL;
    }
    for (size_t i = 0; i < ctx->nproviders; i++) {
        if (strcmp(ctx->providers[i]->name, name) == 0) {
            return ctx->providers[i];
        }
    }
    return NULL;
}

int puci_provider_available(const puci_context *ctx, const char *name) {
    return puci_provider_get(ctx, name) != NULL;
}
