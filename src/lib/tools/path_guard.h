#ifndef PUCI_PATH_GUARD_H
#define PUCI_PATH_GUARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Returns 1 if path is protected (.git, build directories, running binary,
 *  known-good binary), 0 if safe to write. */
int puci_path_is_protected(const char *path);

/** Atomically writes content to path using temporary file + fsync + rename.
 *  Preserves original file permissions if the file already exists.
 *  Refuses writes to protected paths.
 *  Returns 0 on success, non-zero on error with err_buf populated. */
int puci_atomic_write_file(const char *path, const char *content, size_t content_len,
                           size_t *out_written, char *err_buf, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* PUCI_PATH_GUARD_H */
