#ifndef PUCI_DIFF_H
#define PUCI_DIFF_H

/*
 * puci/diff.h - unified diff computation in libpuci.
 *
 * Diff computation is kept in libpuci (rather than the CLI) so an embedded
 * consumer can render a before/after preview its own way without reimplementing
 * a differ. The function produces a compact unified-style diff string; callers
 * render it (e.g. terminal colour, a web UI, a log) as they see fit.
 *
 * Simplifies to "entirely added"/"entirely removed"/"unchanged" for files that
 * do not previously exist, so the output is reasonable even with no prior
 * content.
 */

#include "puci/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Computes a line-based diff between old_text and new_text and returns a
 *  NUL-terminated unified-style diff string (heap-allocated, freed with
 *  puci_diff_free). Returns NULL on allocation failure or if the inputs are
 *  identical. old_text may be NULL/len 0 to represent a non-existent file
 *  (the result is the full new text as additions). */
const char *puci_compute_diff(const char *old_text, size_t old_len,
                              const char *new_text, size_t new_len);

/** Frees a string previously returned by puci_compute_diff. NULL is a no-op.
 *  The caller owns the returned string and must not retain it after this call. */
void puci_diff_free(const char *diff);

#ifdef __cplusplus
}
#endif

#endif /* PUCI_DIFF_H */
