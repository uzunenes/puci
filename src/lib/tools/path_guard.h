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

/** Canonical absolute form of an existing directory (malloc'd), or NULL. */
char *puci_workspace_canon(const char *dir);

/** 1 if `path` (absolute, or relative to the process cwd) resolves inside the
 *  canonical directory `root`, following symlinks; a path that does not exist
 *  yet is judged by its deepest existing ancestor. 0 if outside or if the
 *  path cannot be resolved. NULL/empty path means the cwd. */
int puci_workspace_contains(const char *root, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* PUCI_PATH_GUARD_H */
