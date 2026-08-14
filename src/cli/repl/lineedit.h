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

/* Loads history from file. Returns 0 on success, non-zero on failure. */
int ledit_history_load(ledit *ed, const char *filepath);

/* Saves history to file. Returns 0 on success, non-zero on failure. */
int ledit_history_save(ledit *ed, const char *filepath);

/* Reads one line from the terminal (fd 0), echoing prompt. On LEDIT_OK the
 * caller owns *out (heap) and must free it; the caller is expected to call
 * ledit_history_add with it when it is a real command. */
int ledit_readline(ledit *ed, const char *prompt, char **out);

/* ---- Terminal-restoration hooks for the host CLI ---------------------------
 * The host may need to restore or re-apply raw mode from outside the editor:
 * on exit (atexit), on a double-interrupt hard exit (signal handler), and on
 * resume after suspend (SIGCONT). These use a process-global snapshot of the
 * single active editor, which is the normal case for the puci REPL. */

/* Refresh the global raw-mode snapshot from the editor (arm if raw is active,
 * clear if not). Call after entering or leaving raw mode. */
void ledit_arm_restore(ledit *ed);

/* Restore the terminal to its original (cooked) settings if the snapshot
 * recorded raw mode as active. Suitable for atexit and best-effort signal
 * paths. Safe to call when no raw mode was active. */
void ledit_restore_global(void);

/* Re-apply raw mode if the snapshot recorded it as active. For SIGCONT: resume
 * from a suspend restores the terminal to its original settings, so re-apply
 * the raw termios to get back to the editing state. */
void ledit_resume_global(void);

/* If the terminal is not a TTY, the editor returns lines verbatim (no raw
 * mode), so puci still works piped (non-interactive). */

#endif /* PUCI_CLI_LINEEDIT_H */
