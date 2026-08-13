#ifndef PUCI_PROVIDER_H
#define PUCI_PROVIDER_H

/*
 * puci/provider.h - provider registration and configuration.
 *
 * A provider encapsulates a wire protocol backend (e.g. OpenAI-compatible,
 * Anthropic, Gemini). Providers are identified by name and can carry optional
 * per-provider configuration and capabilities.
 *
 * Thread-safety: registration functions mutate the context and must be called
 * from the thread that created the context. Query functions are safe to call
 * from any thread once registration is complete.
 */

#include "puci/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Registers a provider with the context by name. The name string is copied.
 *  If a provider of the same name already exists, the existing one is left
 *  untouched and PUCI_OK is returned (idempotent). Used internally for built-in
 *  providers and available to consumers extending the library. */
puci_status puci_provider_register(puci_context *ctx, const char *name);

/** Returns a provider handle for a registered provider name, or NULL if the
 *  provider is not registered. The returned handle is owned by the context
 *  and remains valid until the context is destroyed. */
puci_provider *puci_provider_get(const puci_context *ctx, const char *name);

/** Returns 1 if the named provider is registered, 0 otherwise. */
int puci_provider_available(const puci_context *ctx, const char *name);

/* Registration is confined to the context's creating thread. Provider handles
 * and names returned by query functions are context-owned borrowed values and
 * remain valid until context destruction or replacement. */

#ifdef __cplusplus
}
#endif

#endif /* PUCI_PROVIDER_H */
