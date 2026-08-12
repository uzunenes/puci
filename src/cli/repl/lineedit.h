#ifndef PUCI_CLI_LINEEDIT_H
#define PUCI_CLI_LINEEDIT_H

/*
 * repl/lineedit.h - minimal raw-mode terminal line editor.
 *
 * In-house line editor used by the puci REPL (deliberately NOT GNU readline:
 * GPL and a heavy dependency for a project that wants static musl builds).
 * Supports typing, backspace, cursor movement, history (up/down), Ctrl-R
 * incremental search, and the common kill motions. Ctrl-C during editing is
 * reported as an interrupt (LEDIT_INTERRUPT) rather than terminating the
 * process.
 */

#include <stddef.h>

typedef struct ledit ledit;

/* Read-line results. */
#define LEDIT_OK 0        /* a line was read into *out (NUL-terminated) */
#define LEDIT_EOF 1       /* EOF / Ctrl-D on an empty line */
#define LEDIT_INTERRUPT 2 /* Ctrl-C during editing */

/* Creates an editor with up to max_history remembered lines. Returns NULL on
 * allocation failure. */
ledit *ledit_new(size_t max_history);

/* Frees the editor (and its history). NULL is a no-op. */
void ledit_free(ledit *ed);

/* Appends line to the history. Skipped if identical to the most recent entry.
 * Never exceeds max_history (oldest dropped). */
void ledit_history_add(ledit *ed, const char *line);

/* Reads one line from the terminal (fd 0), echoing prompt. On LEDIT_OK the
 * caller owns *out (heap) and must free it; the caller is expected to call
 * ledit_history_add with it when it is a real command. */
int ledit_readline(ledit *ed, const char *prompt, char **out);

/* If the terminal is not a TTY, the editor returns lines verbatim (no raw
 * mode), so puci still works piped (non-interactive). */

#endif /* PUCI_CLI_LINEEDIT_H */