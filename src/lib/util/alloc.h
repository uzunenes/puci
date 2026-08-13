#ifndef PUCI_INTERNAL_ALLOC_H
#define PUCI_INTERNAL_ALLOC_H

/*
 * util/alloc.h - internal allocator helpers.
 *
 * Resolves a caller-supplied allocator (filling in libc defaults when fields
 * are missing) and provides strdup/memdup backed by the context allocator so
 * session/context-owned strings can be freed individually.
 */

#include <stddef.h>
#include "puci/puci.h"

/* Fills *out with the effective allocator for *in. If in is NULL or has a NULL
 * malloc_fn, out uses the libc allocator. */
void puci_alloc_resolve(puci_allocator *out, const puci_allocator *in);

/* Like malloc but through the resolved allocator. Returns NULL on failure. */
void *puci_alloc_malloc(const puci_allocator *a, size_t size);

/* Like calloc. */
void *puci_alloc_calloc(const puci_allocator *a, size_t n, size_t size);

/* Frees memory allocated via puci_alloc_*. NULL is a no-op. */
void puci_alloc_free(const puci_allocator *a, void *ptr);

/* Duplicates a NUL-terminated string. Returns NULL on failure. */
char *puci_alloc_strdup(const puci_allocator *a, const char *s);

/* Duplicates n bytes as a NUL-terminated string. */
char *puci_alloc_strndup(const puci_allocator *a, const char *s, size_t n);

#endif /* PUCI_INTERNAL_ALLOC_H */
