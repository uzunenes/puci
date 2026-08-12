/* context.c - Phase 0 stub. */
#include "puci/puci.h"
puci_status puci_context_create(const puci_context_options *opts,
                                puci_context **out) {
    (void)opts;
    if (!out) return PUCI_ERR_INVALID_ARG;
    return PUCI_ERR_UNKNOWN;
}
