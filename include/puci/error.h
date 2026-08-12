#ifndef PUCI_ERROR_H
#define PUCI_ERROR_H

/*
 * puci/error.h - error codes and error string lookup.
 *
 * Every fallible function in libpuci returns a puci_status (see types.h).
 * These helpers turn a status into a stable, human-readable string.
 */

#include "puci/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Returns a static, NUL-terminated human-readable description of a status.
 *  The returned pointer is owned by the library (static storage) and must not
 *  be freed. Never returns NULL. Thread-safe. */
const char *puci_status_string(puci_status status);

/** Returns a short stable machine-readable token ("ok", "nomem", ...) for a
 *  status. The returned pointer is owned by the library. Never returns NULL.
 *  Thread-safe. */
const char *puci_status_token(puci_status status);

/** Returns the last error message recorded on a session, or NULL if none.
 *  The returned pointer is owned by the session and remains valid until the
 *  session is destroyed or the next error is recorded. Not thread-safe; call
 *  from the thread that owns the session. */
const char *puci_session_error(const puci_session *sess);

/** Returns the last error string for a context, or NULL if none. Owned by the
 *  context. Thread-safe with respect to other context calls. */
const char *puci_context_error(const puci_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PUCI_ERROR_H */