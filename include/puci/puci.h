#ifndef PUCI_PUCI_H
#define PUCI_PUCI_H

/*
 * puci/puci.h - umbrella header for libpuci. This is the ONLY header external
 * consumers need to include. It pulls in the full public API.
 *
 * Include as:  #include <puci/puci.h>
 *
 * Consumers (C, Go/cgo, Rust, Python, Zig) should treat everything under the
 * puci_/PUCI_ namespace as the stable public contract. Anything else is
 * internal and must not be referenced.
 */

#include "puci/version.h"
#include "puci/types.h"
#include "puci/error.h"
#include "puci/session.h"
#include "puci/provider.h"
#include "puci/tool.h"
#include "puci/event.h"
#include "puci/approval.h"
#include "puci/diff.h"
#include "puci/mcp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Context lifecycle.
 *
 * A context owns the allocator, the logging callback, and the registry of
 * providers and tools. Sessions hang off a context. Multiple independent
 * contexts (and thus independent allocators/configs) may coexist in one
 * process.
 *
 * Thread-safety:
 *   - puci_context_create / puci_context_destroy must be called from the same
 *     thread and not concurrently with any other context operation.
 *   - puci_context_set_provider_config / puci_context_ready are NOT safe to
 *     call concurrently with session creation. Call them before creating
 *     sessions.
 *   - all other context query functions are safe to call from any thread
 *     once the context is fully configured.
 */

/** Creates a context. opts may be NULL to use defaults (libc allocator, no
 *  logging). Returns PUCI_OK and sets *out on success. The context is owned by
 *  the caller and must be released with puci_context_destroy. */
puci_status puci_context_create(const puci_context_options *opts,
                                puci_context **out);

/** Destroys a context and frees all associated resources, including any
 *  sessions still alive (they are destroyed too). NULL is a no-op. Must not be
 *  called concurrently with other context operations. */
void puci_context_destroy(puci_context *ctx);

/** Sets a named provider configuration on the context, e.g. {"openai",
 *  base_url, api_key, model}. Copies all strings. Returns PUCI_ERR_NOT_FOUND
 *  if the provider name is not registered. Call before creating sessions. */
puci_status puci_context_set_provider(puci_context *ctx,
                                      const puci_provider_config *config);

#ifdef __cplusplus
}
#endif

#endif /* PUCI_PUCI_H */
