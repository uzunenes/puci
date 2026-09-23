/* clip.h — safe clipboard access for the PUCI TUI.
 *
 * Guarantees (Phase 5):
 *   - Backend commands are constant strings; copied bytes are fed via stdin,
 *     NEVER interpolated into a shell command.
 *   - The payload is never logged / traced.
 *   - A backend that fails reports failure; a partial copy is never reported
 *     as success.
 *   - OSC 52 has an explicit size policy: payloads that would exceed
 *     CTX_OSC52_MAX_PAYLOAD are refused (reported 'too large'), never silently
 *     truncated.
 *
 * A test sink can be installed so exact copied bytes can be asserted without
 * touching a host clipboard.
 */
#ifndef PUCI_TUI_CLIP_H
#define PUCI_TUI_CLIP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy `data` (len bytes, may contain NULs) to the system clipboard.
 * Returns 0 on success, nonzero on failure (no backend, or OSC 52 refused
 * because the payload is too large). */
int clip_copy(const char *data, size_t len);

/* Test hook: if `path` is non-NULL, copies are written to that file instead of
 * a real clipboard backend, and an empty path disables the sink. */
void clip_set_test_sink(const char *path);

/* Test hook: nonzero skips the OS clipboard backends so the OSC 52 path (and
 * its size policy) is exercised without touching a host clipboard. */
void clip_set_test_no_backend(int on);

/* Current maximum OSC 52 payload (bytes) allowed to go out. */
size_t clip_osc52_max(void);

#ifdef __cplusplus
}
#endif

#endif
