/*
 * repl/lineedit.c - raw-mode line editor (see lineedit.h).
 *
 * Minimal readline-style editing without GNU readline. Works on POSIX
 * terminals only; when stdin is not a TTY it falls back to plain line reading
 * so the REPL stays usable when piped/non-interactive.
 */

#include "lineedit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define LINE_CAP ((size_t)16384)

struct ledit {
    char **history;
    size_t nhist;
    size_t cap_hist;
    size_t max_history;

    /* current editing buffer */
    char *buf;
    size_t len;
    size_t cursor;

    /* drafts for history navigation */
    char *draft;
    int hist_id; /* -1 = editing a fresh line, else index into history */

    struct termios orig;
    int raw; /* 1 if raw mode is active */

    /* The raw termios currently applied (used to re-apply after a suspend). */
    struct termios rawset;
};

static ledit *ledit_alloc(void) {
    ledit *ed = (ledit *)calloc(1, sizeof(ledit));
    if (!ed) {
        return NULL;
    }
    ed->hist_id = -1;
    ed->buf = (char *)calloc(LINE_CAP, 1);
    if (!ed->buf) {
        free(ed);
        return NULL;
    }
    return ed;
}

ledit *ledit_new(size_t max_history) {
    ledit *ed = ledit_alloc();
    if (!ed) {
        return NULL;
    }
    ed->max_history = max_history ? max_history : 100;
    return ed;
}

void ledit_free(ledit *ed) {
    if (!ed) {
        return;
    }
    if (ed->raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &ed->orig);
    }
    for (size_t i = 0; i < ed->nhist; i++) {
        free(ed->history[i]);
    }
    free(ed->history);
    free(ed->buf);
    free(ed->draft);
    free(ed);
}

void ledit_history_add(ledit *ed, const char *line) {
    if (!ed || !line || !*line) {
        return;
    }
    if (ed->nhist > 0 &&
        strcmp(ed->history[ed->nhist - 1], line) == 0) {
        return; /* no consecutive duplicates */
    }
    if (ed->nhist == ed->cap_hist) {
        size_t ncap = ed->cap_hist ? ed->cap_hist * 2 : 16;
        char **nh = (char **)realloc(ed->history, ncap * sizeof(char *));
        if (!nh) {
            return;
        }
        ed->history = nh;
        ed->cap_hist = ncap;
    }
    char *copy = strdup(line);
    if (!copy) {
        return;
    }
    ed->history[ed->nhist++] = copy;
    if (ed->nhist > ed->max_history) {
        free(ed->history[0]);
        memmove(&ed->history[0], &ed->history[1],
                (ed->nhist - 1) * sizeof(char *));
        ed->nhist--;
    }
}

static int enable_raw(ledit *ed) {
    if (ed->raw) {
        return 0;
    }
    if (!isatty(STDIN_FILENO)) {
        return 0;
    }
    if (tcgetattr(STDIN_FILENO, &ed->orig) != 0) {
        return -1;
    }
    struct termios raw = ed->orig;
    raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t) ~(OPOST);
    raw.c_cflag |= (tcflag_t)CS8;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return -1;
    }
    ed->rawset = raw;
    ed->raw = 1;
    ledit_arm_restore(ed);
    return 0;
}

static void restore(ledit *ed) {
    if (ed->raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &ed->orig);
        ed->raw = 0;
        ledit_arm_restore(ed);
    }
}

/* ---- process-global terminal restoration snapshot ---- */

static struct termios g_orig;
static struct termios g_rawset;
static int g_raw_active;

void ledit_arm_restore(ledit *ed) {
    if (ed && ed->raw) {
        g_orig = ed->orig;
        g_rawset = ed->rawset;
        g_raw_active = 1;
    } else if (ed) {
        g_raw_active = 0;
    }
}

void ledit_restore_global(void) {
    if (g_raw_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig);
        g_raw_active = 0;
    }
}

void ledit_resume_global(void) {
    if (g_raw_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_rawset);
    }
}

static void render(ledit *ed, const char *prompt) {
    /* Move to column 0, clear to end, draw prompt+line, park cursor. */
    fputs("\r\x1b[K", stdout);
    fputs(prompt, stdout);
    if (ed->len > 0) {
        fwrite(ed->buf, 1, ed->len, stdout);
    }
    size_t back = ed->len - ed->cursor;
    if (back > 0) {
        fprintf(stdout, "\x1b[%zuD", back);
    }
    fflush(stdout);
}

static void clear_line(ledit *ed) {
    ed->len = 0;
    ed->cursor = 0;
    ed->buf[0] = '\0';
}

static void reset_draft(ledit *ed) {
    if (ed->draft) {
        free(ed->draft);
        ed->draft = NULL;
    }
    ed->hist_id = -1;
}

/* Inserts ch at the cursor. */
static void insert_char(ledit *ed, char ch) {
    if (ed->len + 1 >= LINE_CAP) {
        return;
    }
    memmove(&ed->buf[ed->cursor + 1], &ed->buf[ed->cursor],
            ed->len - ed->cursor);
    ed->buf[ed->cursor] = ch;
    ed->len++;
    ed->cursor++;
    ed->buf[ed->len] = '\0';
}

/* Deletes the character before the cursor. */
static void backspace(ledit *ed) {
    if (ed->cursor == 0) {
        return;
    }
    memmove(&ed->buf[ed->cursor - 1], &ed->buf[ed->cursor],
            ed->len - ed->cursor);
    ed->cursor--;
    ed->len--;
    ed->buf[ed->len] = '\0';
}

/* Loads history[index] into the buffer (index is 0-based into history). */
static void history_load(ledit *ed, size_t index, const char *prompt) {
    if (index >= ed->nhist) {
        return;
    }
    const char *line = ed->history[index];
    size_t n = strlen(line);
    if (n >= LINE_CAP) {
        n = LINE_CAP - 1;
    }
    memcpy(ed->buf, line, n);
    ed->buf[n] = '\0';
    ed->len = n;
    ed->cursor = n;
    ed->hist_id = (int)index;
    render(ed, prompt);
}

/* Reverse incremental search state. */
static int search_history(ledit *ed, const char *prompt) {
    char query[256];
    size_t qlen = 0;
    query[0] = '\0';
    int cur = (int)ed->nhist;
    int found = 0;
    fputs("\r\x1b[K", stdout);
    fputs("(reverse-i-search)`',", stdout);
    fflush(stdout);

    while (1) {
        fputs("\r\x1b[K", stdout);
        printf("(reverse-i-search)`%s': ", query);
        fflush(stdout);
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            continue;
        }
        if (c == 0x03) { /* Ctrl-C */
            fputs("\r\x1b[K", stdout);
            fflush(stdout);
            return LEDIT_INTERRUPT;
        }
        if (c == 0x04) { /* Ctrl-D abort */
            continue;
        }
        if (c == '\r' || c == '\n') {
            break;
        }
        if (c == 0x7f || c == '\b') {
            if (qlen > 0) {
                qlen--;
                query[qlen] = '\0';
            }
            continue;
        }
        if (c == 0x12) { /* Ctrl-R again: next older match */
            if (qlen > 0 && cur > 0) {
                cur--;
            }
        } else if (c >= 0x20 && c < 0x7f) {
            if (qlen < sizeof(query) - 1) {
                query[qlen++] = c;
                query[qlen] = '\0';
                cur = (int)ed->nhist;
            }
        } else {
            continue;
        }

        if (qlen == 0) {
            continue;
        }
        /* find newest history entry at index < cur containing query */
        int i = (int)ed->nhist - 1;
        if (cur < (int)ed->nhist) {
            i = cur;
        }
        found = 0;
        for (; i >= 0; i--) {
            if (ed->history[i] &&
                strstr(ed->history[i], query) != NULL) {
                size_t n = strlen(ed->history[i]);
                if (n >= LINE_CAP) {
                    n = LINE_CAP - 1;
                }
                memcpy(ed->buf, ed->history[i], n);
                ed->buf[n] = '\0';
                ed->len = n;
                ed->cursor = n;
                ed->hist_id = i;
                cur = i;
                found = 1;
                break;
            }
        }
        if (found) {
            fputs("\r\x1b[K", stdout);
            printf("(reverse-i-search)`%s': %s", query, ed->history[cur]);
            fflush(stdout);
        }
    }
    free(ed->draft);
    ed->draft = NULL;
    render(ed, prompt);
    return LEDIT_OK;
}

int ledit_readline(ledit *ed, const char *prompt, char **out) {
    if (!ed || !out) {
        return LEDIT_EOF;
    }
    *out = NULL;

    clear_line(ed);
    reset_draft(ed);

    if (!isatty(STDIN_FILENO)) {
        /* Non-interactive: plain getline. */
        char *line = NULL;
        size_t cap = 0;
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            if (line) {
                free(line);
            }
            return LEDIT_EOF;
        }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        *out = line;
        return LEDIT_OK;
    }

    if (enable_raw(ed) != 0) {
        /* Could not set raw mode; fall back to plain line. */
        char buf[LINE_CAP];
        if (fgets(buf, (int)sizeof(buf), stdin) == NULL) {
            return LEDIT_EOF;
        }
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        *out = strdup(buf);
        return *out ? LEDIT_OK : LEDIT_EOF;
    }

    while (1) {
        render(ed, prompt);
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            if (errno == EINTR) {
                /* Ctrl-C handled by the REPL's signal handler; surface it. */
                clear_line(ed);
                restore(ed);
                return LEDIT_INTERRUPT;
            }
            continue;
        }

        if (c == '\r' || c == '\n') {
            fputs("\r\n", stdout);
            fflush(stdout);
            /* copy the line out */
            char *line = (char *)malloc(ed->len + 1);
            if (!line) {
                restore(ed);
                return LEDIT_EOF;
            }
            memcpy(line, ed->buf, ed->len);
            line[ed->len] = '\0';
            *out = line;
            clear_line(ed);
            reset_draft(ed);
            restore(ed);
            return LEDIT_OK;
        }
        if (c == 0x03) { /* Ctrl-C */
            clear_line(ed);
            reset_draft(ed);
            fputs("^C\r\n", stdout);
            fflush(stdout);
            return LEDIT_INTERRUPT;
        }
        if (c == 0x04) { /* Ctrl-D */
            if (ed->len == 0) {
                restore(ed);
                return LEDIT_EOF;
            }
            continue;
        }
        if (c == 0x12) { /* Ctrl-R */
            int r = search_history(ed, prompt);
            if (r == LEDIT_INTERRUPT) {
                clear_line(ed);
                fputs("^R\r\n", stdout);
                fflush(stdout);
                return LEDIT_INTERRUPT;
            }
            continue;
        }
        if (c == 0x15) { /* Ctrl-U: kill to start */
            ed->len -= ed->cursor;
            memmove(ed->buf, &ed->buf[ed->cursor], ed->len);
            ed->cursor = 0;
            ed->buf[ed->len] = '\0';
            continue;
        }
        if (c == 0x0b) { /* Ctrl-K: kill to end */
            ed->len = ed->cursor;
            ed->buf[ed->len] = '\0';
            continue;
        }
        if (c == 0x01) { /* Ctrl-A: home */
            ed->cursor = 0;
            continue;
        }
        if (c == 0x05) { /* Ctrl-E: end */
            ed->cursor = ed->len;
            continue;
        }
        if (c == 0x17) { /* Ctrl-W: delete word backwards */
            size_t i = ed->cursor;
            while (i > 0 && ed->buf[i - 1] == ' ') {
                i--;
            }
            while (i > 0 && ed->buf[i - 1] != ' ') {
                i--;
            }
            size_t del = ed->cursor - i;
            memmove(&ed->buf[i], &ed->buf[ed->cursor], ed->len - ed->cursor);
            ed->cursor = i;
            ed->len -= del;
            ed->buf[ed->len] = '\0';
            continue;
        }
        if (c == 0x7f || c == '\b') { /* Backspace */
            backspace(ed);
            continue;
        }
        if (c == 0x1b) { /* ESC sequence */
            char seq[2];
            ssize_t n1 = read(STDIN_FILENO, &seq[0], 1);
            ssize_t n2 = read(STDIN_FILENO, &seq[1], 1);
            if (n1 == 1 && n2 == 1 && seq[0] == '[') {
                if (seq[1] == 'A') { /* up */
                    if (ed->nhist > 0) {
                        if (ed->hist_id < 0) {
                            free(ed->draft);
                            ed->draft = strdup(ed->buf);
                            ed->hist_id = (int)ed->nhist;
                        }
                        if (ed->hist_id > 0) {
                            history_load(ed, (size_t)ed->hist_id - 1, prompt);
                        }
                    }
                } else if (seq[1] == 'B') { /* down */
                    if (ed->hist_id >= 0) {
                        if (ed->hist_id < (int)ed->nhist - 1) {
                            history_load(ed, (size_t)ed->hist_id + 1, prompt);
                        } else if (ed->draft) {
                            size_t n = strlen(ed->draft);
                            if (n >= LINE_CAP) {
                                n = LINE_CAP - 1;
                            }
                            memcpy(ed->buf, ed->draft, n);
                            ed->buf[n] = '\0';
                            ed->len = n;
                            ed->cursor = n;
                            ed->hist_id = -1;
                            render(ed, prompt);
                        }
                    }
                } else if (seq[1] == 'C') { /* right */
                    if (ed->cursor < ed->len) {
                        ed->cursor++;
                    }
                } else if (seq[1] == 'D') { /* left */
                    if (ed->cursor > 0) {
                        ed->cursor--;
                    }
                }
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7f) {
            insert_char(ed, c);
        }
    }
}