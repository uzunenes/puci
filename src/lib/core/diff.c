/*
 * core/diff.c - unified diff computation (public puci/diff.h).
 *
 * Produces a compact unified diff between two texts, line by line, using a
 * dynamic-programming longest common subsequence. The DP is bounded to guard
 * against pathological inputs: if the table exceeds a threshold the diff falls
 * back to "whole file replaced", so huge approval previews never stall the
 * agent loop.
 *
 * The result is heap-allocated with malloc and freed with puci_diff_free. This
 * module uses the standard library allocator directly and never calls
 * printf/exit/abort.
 */

#include "puci/diff.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Cap on DP table cells before falling back to whole-file-replace. */
#define DIFF_MAX_CELLS ((size_t)16 * 1024 * 1024)

static size_t count_lines(const char *text, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            n++;
        }
    }
    if (len > 0 && text[len - 1] != '\n') {
        n++;
    }
    return n;
}

/* Copies the line [start, end) (excluding any newline) as a malloc'd string. */
static char *line_copy(const char *text, size_t start, size_t end) {
    size_t n = end - start;
    char *s = (char *)malloc(n + 1);
    if (!s) {
        return NULL;
    }
    memcpy(s, text + start, n);
    s[n] = '\0';
    return s;
}

/* Splits text into malloc'd line strings. Returns the array, setting *count.
 * On allocation failure frees what it allocated and returns NULL. */
static char **split_lines(const char *text, size_t len, size_t *count) {
    size_t n = count_lines(text, len);
    char **lines = (char **)malloc((n ? n : 1) * sizeof(char *));
    if (!lines) {
        *count = 0;
        return NULL;
    }
    size_t idx = 0;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            char *s = line_copy(text, start, i);
            if (!s) {
                for (size_t k = 0; k < idx; k++) free(lines[k]);
                free(lines);
                *count = 0;
                return NULL;
            }
            lines[idx++] = s;
            start = i + 1;
        }
    }
    if (len > 0 && text[len - 1] != '\n') {
        char *s = line_copy(text, start, len);
        if (!s) {
            for (size_t k = 0; k < idx; k++) free(lines[k]);
            free(lines);
            *count = 0;
            return NULL;
        }
        lines[idx++] = s;
    }
    *count = n;
    return lines;
}

static void free_lines(char **lines, size_t n) {
    if (!lines) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(lines[i]);
    }
    free(lines);
}

/* Simple growable output buffer. */
typedef struct buf {
    char *data;
    size_t len;
    size_t cap;
} buf;

static int buf_reserve(buf *b, size_t need) {
    if (b->len + need <= b->cap) {
        return 0;
    }
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + need) {
        if (cap > (size_t)-1 / 2) {
            return -1;
        }
        cap *= 2;
    }
    char *nd = (char *)realloc(b->data, cap);
    if (!nd) {
        return -1;
    }
    b->data = nd;
    b->cap = cap;
    return 0;
}

static int buf_puts(buf *b, const char *s) {
    size_t n = strlen(s);
    if (buf_reserve(b, n + 1) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

/* Appends one diff line with a leading '+', '-', ' ' or '\\'. */
static int buf_line(buf *b, char mark, const char *s, size_t n) {
    if (buf_reserve(b, n + 2) != 0) {
        return -1;
    }
    b->data[b->len++] = mark;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len++] = '\n';
    b->data[b->len] = '\0';
    return 0;
}

/* Emits a full-file replacement diff (old or new side empty, or DP fallback). */
static const char *whole_diff(char **old, size_t n, char **neu, size_t m) {
    buf b = {0, 0, 0};
    buf_puts(&b, "@@\n");
    for (size_t i = 0; i < n; i++) {
        if (buf_line(&b, '-', old[i], strlen(old[i])) != 0) {
            free(b.data);
            free_lines(old, n);
            free_lines(neu, m);
            return NULL;
        }
    }
    for (size_t i = 0; i < m; i++) {
        if (buf_line(&b, '+', neu[i], strlen(neu[i])) != 0) {
            free(b.data);
            free_lines(old, n);
            free_lines(neu, m);
            return NULL;
        }
    }
    free_lines(old, n);
    free_lines(neu, m);
    if (b.len == 0) {
        free(b.data);
        return NULL;
    }
    return b.data;
}

/* Myers-free LCS-based diff limited to a single @ range, then group into hunks
 * with 3 lines of context and @@ headers. */
static const char *diff_with_lcs(char **old, size_t n, char **neu, size_t m) {
    size_t cells = (n + 1) * (m + 1);
    if (cells > DIFF_MAX_CELLS) {
        return whole_diff(old, n, neu, m);
    }
    /* LCS lengths. Store as uint16; enough for the bounded cell cap. */
    size_t width = m + 1;
    uint16_t *dp = (uint16_t *)malloc(cells * sizeof(uint16_t));
    if (!dp) {
        free_lines(old, n);
        free_lines(neu, m);
        return NULL;
    }
    for (size_t j = 0; j <= m; j++) {
        dp[j] = 0;
    }
    for (size_t i = 0; i <= n; i++) {
        dp[i * width] = 0;
    }
    for (size_t i = 1; i <= n; i++) {
        for (size_t j = 1; j <= m; j++) {
            unsigned short v;
            if (strcmp(old[i - 1], neu[j - 1]) == 0) {
                v = (unsigned short)(dp[(i - 1) * width + (j - 1)] + 1);
            } else {
                unsigned short a = dp[(i - 1) * width + j];
                unsigned short b = dp[i * width + (j - 1)];
                v = a > b ? a : b;
            }
            dp[i * width + j] = v;
        }
    }

    /* Backtrack to collect ops in reverse. op codes: 0 keep, 1 remove, 2 add. */
    size_t maxops = n + m;
    unsigned char *ops = (unsigned char *)malloc((maxops + 1));
    size_t nops = 0;
    if (!ops) {
        free(dp);
        free_lines(old, n);
        free_lines(neu, m);
        return NULL;
    }
    size_t i = n, j = m;
    while (i > 0 && j > 0) {
        if (strcmp(old[i - 1], neu[j - 1]) == 0) {
            ops[nops++] = 0;
            i--;
            j--;
        } else if (dp[(i - 1) * width + j] >= dp[i * width + (j - 1)]) {
            ops[nops++] = 1;
            i--;
        } else {
            ops[nops++] = 2;
            j--;
        }
    }
    while (i > 0) {
        ops[nops++] = 1;
        i--;
    }
    while (j > 0) {
        ops[nops++] = 2;
        j--;
    }
    /* ops was collected in reverse LCS order; flip to forward order. */
    for (size_t r = 0; r < nops / 2; r++) {
        unsigned char t = ops[r];
        ops[r] = ops[nops - 1 - r];
        ops[nops - 1 - r] = t;
    }
    free(dp);

    buf b = {0, 0, 0};
    /* ops holds operations in forward order: 0 keep, 1 remove (old), 2 add. */
    buf_puts(&b, "@@\n");
    size_t oi = 0, ni = 0;
    for (size_t t = 0; t < nops; t++) {
        if (ops[t] == 0) {
            buf_line(&b, ' ', old[oi], strlen(old[oi]));
            oi++;
            ni++;
        } else if (ops[t] == 1) {
            buf_line(&b, '-', old[oi], strlen(old[oi]));
            oi++;
        } else {
            buf_line(&b, '+', neu[ni], strlen(neu[ni]));
            ni++;
        }
    }
    free(ops);
    free_lines(old, n);
    free_lines(neu, m);
    if (b.len == 0) {
        free(b.data);
        return NULL;
    }
    return b.data;
}

const char *puci_compute_diff(const char *old_text, size_t old_len,
                              const char *new_text, size_t new_len) {
    char **old, **neu;
    size_t n, m;
    old = split_lines(old_text ? old_text : "", old_len, &n);
    neu = split_lines(new_text ? new_text : "", new_len, &m);
    if (!old || !neu) {
        free_lines(old, n);
        free_lines(neu, m);
        return NULL;
    }
    /* Identical texts -> no diff. */
    if (n == m) {
        int same = 1;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(old[i], neu[i]) != 0) {
                same = 0;
                break;
            }
        }
        if (same) {
            free_lines(old, n);
            free_lines(neu, m);
            return NULL;
        }
    }
    if (n == 0 || m == 0) {
        return whole_diff(old, n, neu, m);
    }
    return diff_with_lcs(old, n, neu, m);
}

void puci_diff_free(const char *diff) {
    free((void *)diff);
}