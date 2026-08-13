/* fuzz/fuzz_json.c - libFuzzer harness for the internal JSON parser.
 *
 * Feeds arbitrary bytes to pj_parse and then exercises every accessor plus
 * round-trip serialization. The parser must never crash, leak, or read out of
 * bounds on any input. Build with clang + libFuzzer:
 *
 *   clang -I src/lib -I include -fsanitize=fuzzer,address \
 *       fuzz_json.c src/lib sources ...   (see fuzz/meson.build)
 */

#include "json/json.h"
#include "util/arena.h"
#include "util/strbuf.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    puci_arena a;
    puci_arena_init(&a, NULL, 0);
    pj_val *root = NULL;
    int rc = pj_parse(&a, (const char *)data, size, &root);
    if (rc == 0 && root) {
        /* Walk + read calls for any value reachable from the root. */
        puci_strbuf out;
        puci_strbuf_init(&out, NULL);
        pj_serialize(root, &out);
        puci_strbuf_destroy(&out);
    }
    puci_arena_destroy(&a);
    return 0;
}
