/* ttg.h — streaming strip of TTG instrumentation envelopes.
 *
 * Providers may wrap each token with an instrumentation envelope
 *   \xE2\x81\xA3 <digits> \xE2\x81\xA3   (U+2063 "invisible separator")
 * that must never surface as visible conversation content. The envelope may
 * be split across arbitrary stream chunks, and several may occur in one
 * chunk. This module strips complete envelopes and emits only genuine
 * content, carrying a partial prefix across calls so a marker split across
 * reads can neither leak nor reconstruct a false envelope.
 *
 * The filter is deliberately dependency-free and side-effect-free apart from
 * its carry state, so it can be unit-tested standalone. The TUI embeds one
 * puci_ttg_state per pane.
 */
#ifndef PUCI_TTG_H
#define PUCI_TTG_H

#include <stddef.h>

#define PUCI_TTG_CARRY_MAX 48 /* holds a partial envelope (marker+digits) between chunks */
#define PUCI_TTG_ENV_CAP  40  /* 3 + up to ~33 digits + 3 */

typedef struct {
    char carry[PUCI_TTG_CARRY_MAX];
    int  len;
} puci_ttg_state;

/* Feed a chunk of raw stream bytes; write filtered bytes to out[outcap] and
 * return the number written. *hits is incremented once per complete envelope
 * consumed. If dig is non-null (with digcap>0), the last complete envelope's
 * decimal timestamp digits are copied into it (NUL-terminated, truncated) so
 * the caller can keep tracing TTG latency. Any incomplete prefix is retained
 * in st. */
size_t puci_ttg_filter(puci_ttg_state *st, const char *txt, size_t len,
                       char *out, size_t outcap,
                       char *dig, size_t digcap, int *hits);

/* Reset the carry (used when a line/turn is discarded). */
void puci_ttg_reset(puci_ttg_state *st);

/* Turn completed with an unfinished envelope: emit any genuinely-partial
 * content bytes (a lone marker, <= 3 bytes, is dropped). Returns the number
 * of bytes written to out. Call exactly once at end-of-turn. */
size_t puci_ttg_flush(puci_ttg_state *st, char *out, size_t outcap);

#endif /* PUCI_TTG_H */