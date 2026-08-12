/* fuzz/fuzz_sse.c - libFuzzer harness for the internal SSE stream parser.
 *
 * Feeds arbitrary bytes to a puci_sse_parser in varying chunk sizes and then
 * flushes, so partial/malformed/truncated event boundaries are exercised. The
 * parser must never crash, leak, or read out of bounds on any input, and must
 * yield only the events it can reconstruct.
 */

#include "net/sse.h"

#include <stddef.h>
#include <stdint.h>

static void noop(void *ud, const char *data, size_t len) {
    (void)ud;
    (void)data;
    (void)len;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    puci_sse_parser *p = puci_sse_parser_create(NULL);
    if (!p) {
        return 0;
    }
    /* Deterministic variable chunking from the input itself. */
    size_t i = 0;
    uint32_t state = (uint32_t)size + 1;
    while (i < size) {
        /* Advance a small xorshift to vary the split point. */
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        size_t step = (size_t)(state % 31) + 1;
        if (step > size - i) {
            step = size - i;
        }
        puci_sse_parser_feed(p, (const char *)data + i, step, noop, NULL);
        i += step;
    }
    puci_sse_parser_flush(p, noop, NULL);
    puci_sse_parser_destroy(p);
    return 0;
}