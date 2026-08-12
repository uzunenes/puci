#ifndef PUCI_APPROVAL_H
#define PUCI_APPROVAL_H

/*
 * puci/approval.h - tool-call approval policy hook.
 *
 * The agent loop consults an optional approval callback before running any
 * tool. This is the embeddable, host-controlled safety surface: an embedded
 * consumer implements its own policy (interactive prompt, allowlist, remote
 * approval service, ...) entirely in the host process. libpuci never blocks
 * on a human; it only asks the callback and honours the answer.
 *
 * The request carries everything a policy needs to render a faithful prompt:
 * the raw JSON arguments, a ready-to-show display string, and the parsed argv
 * (for run_shell) or a unified diff (for write_file over an existing file) so
 * the host can render it its own way.
 *
 * Thread-safety: the callback is invoked on the thread driving the session.
 * It must not call back into the session being directly driven.
 */

#include "puci/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The approval decision returned by the host policy. */
typedef enum puci_approval {
    PUCI_APPROVAL_DENY = 0, /**< refuse to run the tool */
    PUCI_APPROVAL_ALLOW,    /**< allow the tool to run */
} puci_approval;

/** Context supplied to the approval callback for one pending tool call. All
 *  strings are owned by the session and valid only for the duration of the
 *  callback. Do not retain the pointers. */
typedef struct puci_approval_request {
    /** Tool name, e.g. "run_shell". */
    const char *tool;
    /** Raw tool arguments as JSON text. */
    const char *args_json;
    /** A ready-to-print human-readable description of the exact action:
     *  for run_shell, the parsed argv (one argument per line, faithfully
     *  quoted); for write_file over an existing file, a unified diff; for
     *  other tools, the raw args_json. NULL if nothing is renderable. */
    const char *display;
    /** Parsed argv for run_shell (argv[0] is the command), or NULL when the
     *  tool has no argv concept. Null-terminated; argc is the element count. */
    int argc;
    const char **argv;
    /** Unified diff (old -> new) for write_file over an existing file, or
     *  NULL. Diff computation is provided by libpuci (see puci/diff.h). */
    const char *diff;
    /** Nonzero if the target file already exists (the write would replace it). */
    int file_existed;
} puci_approval_request;

/** Approval policy callback.
 *  @param req the pending tool call to approve or deny.
 *  @param userdata opaque pointer supplied in puci_session_options.
 *  @return PUCI_APPROVAL_ALLOW to run the tool, PUCI_APPROVAL_DENY to refuse.
 *  A denied tool yields a tool result of {"ok":false,"error":"blocked by
 *  approval policy"} fed back to the model. */
typedef puci_approval (*puci_approval_cb)(const puci_approval_request *req,
                                          void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* PUCI_APPROVAL_H */