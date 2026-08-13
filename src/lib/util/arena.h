#ifndef PUCI_INTERNAL_ARENA_H
#define PUCI_INTERNAL_ARENA_H

/*
 * util/arena.h - internal arena allocator.
 *
 * Allocates from fixed-size blocks obtained from a puci_allocator. A single
 * arena_reset frees everything allocated since the last reset, which makes it
 * ideal for per-request / per-chunk temporary data whose lifetime is scoped to
 * a single HTTP exchange. Blocks are never returned to the allocator until
 * reset or destroy, so peak memory bounded by max live allocation.
 *
 * Not thread-safe: an arena belongs to the thread that uses it.
 */

#include <stddef.h>
#include "puci/puci.h"

typedef struct puci_block puci_block;

typedef struct puci_arena {
    puci_allocator alloc;  /* copy of the supplied allocator */
    puci_block *head;      /* linked list of live blocks */
    size_t block_size;     /* bytes per allocated block */
} puci_arena;

/* Initializes an arena. alloc may be NULL to use the libc allocator.
 * block_size is the allocation granularity (0 => 64 KiB default). */
void puci_arena_init(puci_arena *a, const puci_allocator *alloc,
                     size_t block_size);

/* Allocates size bytes with the alignment of max_align_t. Returns NULL on
 * allocation failure. The returned memory is zero on the first use of a
 * region? No: uninitialized. Callers must initialize. */
void *puci_arena_alloc(puci_arena *a, size_t size);

/* Allocates a zero-filled buffer of size bytes. */
void *puci_arena_calloc(puci_arena *a, size_t size);

/* Duplicates a NUL-terminated string into the arena. Returns NULL on failure. */
char *puci_arena_strdup(puci_arena *a, const char *s);

/* Frees everything allocated since init or the last reset. Safe to call
 * repeatedly. Does not free the arena struct itself. */
void puci_arena_reset(puci_arena *a);

/* Frees all blocks; equivalent to reset. The arena struct may be reused by
 * re-calling init. */
void puci_arena_destroy(puci_arena *a);

#endif /* PUCI_INTERNAL_ARENA_H */
