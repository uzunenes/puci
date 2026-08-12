/*
 * util/alloc.c - allocator helpers.
 */

#include "util/alloc.h"

#include <stdlib.h>
#include <string.h>

static void *libc_malloc(void *ud, size_t n) {
    (void)ud;
    return malloc(n);
}
static void *libc_realloc(void *ud, void *p, size_t n) {
    (void)ud;
    return realloc(p, n);
}
static void libc_free(void *ud, void *p) {
    (void)ud;
    free(p);
}

void puci_alloc_resolve(puci_allocator *out, const puci_allocator *in) {
    if (in && in->malloc_fn) {
        *out = *in;
    } else {
        out->malloc_fn = libc_malloc;
        out->realloc_fn = libc_realloc;
        out->free_fn = libc_free;
        out->userdata = NULL;
    }
}

void *puci_alloc_malloc(const puci_allocator *a, size_t size) {
    return a->malloc_fn(a->userdata, size);
}

void *puci_alloc_calloc(const puci_allocator *a, size_t n, size_t size) {
    if (n != 0 && size > (size_t)-1 / n) {
        return NULL;
    }
    size_t total = n * size;
    void *p = a->malloc_fn(a->userdata, total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void puci_alloc_free(const puci_allocator *a, void *ptr) {
    if (ptr) {
        a->free_fn(a->userdata, ptr);
    }
}

char *puci_alloc_strdup(const puci_allocator *a, const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *p = (char *)puci_alloc_malloc(a, len + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, len + 1);
    return p;
}

char *puci_alloc_strndup(const puci_allocator *a, const char *s, size_t n) {
    if (!s) {
        return NULL;
    }
    char *p = (char *)puci_alloc_malloc(a, n + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}