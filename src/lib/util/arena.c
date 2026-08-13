/*
 * util/arena.c - arena allocator implementation.
 */

#include "util/arena.h"

#include <stdlib.h>
#include <string.h>

#include "util/alloc.h"

#define DEFAULT_BLOCK_SIZE ((size_t)64 * 1024)

struct puci_block {
    puci_block *next;
    size_t used;
    size_t cap;
    /* block data follows the header, aligned to max_align_t */
};

static size_t align_up(size_t n) {
    const size_t a = _Alignof(max_align_t);
    return (n + a - 1) & ~(a - 1);
}

void puci_arena_init(puci_arena *a, const puci_allocator *alloc,
                     size_t block_size) {
    memset(a, 0, sizeof(*a));
    puci_alloc_resolve(&a->alloc, alloc);
    a->block_size = block_size ? block_size : DEFAULT_BLOCK_SIZE;
}

static puci_block *new_block(puci_arena *a, size_t need) {
    size_t cap = a->block_size;
    if (need > cap) {
        /* Grow for very large single allocations. */
        cap = align_up(need) + sizeof(puci_block) + align_up(1);
    }
    char *raw = (char *)a->alloc.malloc_fn(a->alloc.userdata,
                                           sizeof(puci_block) + cap);
    if (!raw) {
        return NULL;
    }
    puci_block *b = (puci_block *)raw;
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
    return b;
}

void *puci_arena_alloc(puci_arena *a, size_t size) {
    if (size == 0) {
        size = 1;
    }
    size_t aligned = align_up(size);
    puci_block *b = a->head;
    if (!b || b->cap - b->used < aligned) {
        b = new_block(a, aligned);
        if (!b) {
            return NULL;
        }
    }
    void *p = (void *)((char *)b + sizeof(puci_block) + b->used);
    b->used += aligned;
    return p;
}

void *puci_arena_calloc(puci_arena *a, size_t size) {
    void *p = puci_arena_alloc(a, size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

char *puci_arena_strdup(puci_arena *a, const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *p = (char *)puci_arena_alloc(a, len + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, len + 1);
    return p;
}

void puci_arena_reset(puci_arena *a) {
    puci_block *b = a->head;
    while (b) {
        puci_block *next = b->next;
        a->alloc.free_fn(a->alloc.userdata, (void *)b);
        b = next;
    }
    a->head = NULL;
}

void puci_arena_destroy(puci_arena *a) {
    puci_arena_reset(a);
}
