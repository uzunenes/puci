#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include "path_guard.h"

#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int segment_is_protected(const char *seg, size_t len) {
    if (len == 4 && memcmp(seg, ".git", 4) == 0) {
        return 1;
    }
    /* Build output dirs: "build" or "build-<variant>" (not "builder.c"). */
    if (len == 5 && memcmp(seg, "build", 5) == 0) {
        return 1;
    }
    if (len > 6 && memcmp(seg, "build-", 6) == 0) {
        return 1;
    }
    if (len >= 9 && memcmp(seg, "puci-good", 9) == 0) {
        return 1;
    }
    return 0;
}

static int path_string_has_protected_segments(const char *p) {
    if (!p || !*p) {
        return 0;
    }
    const char *cur = p;
    while (*cur) {
        while (*cur == '/') {
            cur++;
        }
        if (!*cur) {
            break;
        }
        const char *end = strchr(cur, '/');
        size_t len = end ? (size_t)(end - cur) : strlen(cur);
        if (segment_is_protected(cur, len)) {
            return 1;
        }
        cur += len;
    }
    return 0;
}

int puci_path_is_protected(const char *path) {
    if (!path || !path[0]) {
        return 0;
    }

    /* 1. Check raw path string for protected segments (.git, build*, puci-good) */
    if (path_string_has_protected_segments(path)) {
        return 1;
    }

    /* 2. For absolute paths or specific files, check canonical realpath against binary */
    char resolved[PATH_MAX];
    if (realpath(path, resolved) != NULL) {
        if (path[0] == '/' && path_string_has_protected_segments(resolved)) {
            return 1;
        }
        /* Check against currently running binary */
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            if (strcmp(resolved, exe) == 0) {
                return 1;
            }
        }
    }

    return 0;
}

int puci_atomic_write_file(const char *path, const char *content, size_t content_len,
                           size_t *out_written, char *err_buf, size_t err_cap) {
    if (out_written) {
        *out_written = 0;
    }
    if (!path || !content) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "invalid arguments");
        }
        return -1;
    }

    if (puci_path_is_protected(path)) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "write refused by path guard: protected path");
        }
        return -1;
    }

    struct stat st;
    int existed = (stat(path, &st) == 0);
    mode_t mode = existed ? (st.st_mode & 0777) : 0664;

    static _Atomic unsigned int counter = 0;
    unsigned int cn = ++counter;
    char tmp_path[PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d.%u", path, (int)getpid(), cn);
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "path too long");
        }
        return -1;
    }

    /* O_EXCL|O_NOFOLLOW: never write through a pre-planted symlink/temp. */
    int tfd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    FILE *f = tfd >= 0 ? fdopen(tfd, "wb") : NULL;
    if (!f) {
        if (tfd >= 0) {
            close(tfd);
            unlink(tmp_path);
        }
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "cannot open temp file for write: %s", strerror(errno));
        }
        return -1;
    }

    int fd = fileno(f);
    if (fd >= 0) {
        (void)fchmod(fd, mode);
    }

    size_t written = fwrite(content, 1, content_len, f);
    if (written != content_len) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "short write to temp file: %s", strerror(errno));
        }
        fclose(f);
        unlink(tmp_path);
        return -1;
    }

    if (fd >= 0) {
        if (fsync(fd) != 0) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap, "fsync failed: %s", strerror(errno));
            }
            fclose(f);
            unlink(tmp_path);
            return -1;
        }
    }
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "rename failed: %s", strerror(errno));
        }
        unlink(tmp_path);
        return -1;
    }

    if (out_written) {
        *out_written = written;
    }
    return 0;
}

/* ---- workspace confinement ------------------------------------------ */

char *puci_workspace_canon(const char *dir) {
    if (!dir || !dir[0]) {
        return NULL;
    }
    char buf[PATH_MAX];
    struct stat st;
    if (!realpath(dir, buf) || stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return NULL;
    }
    return strdup(buf);
}

static int under(const char *root, const char *p) {
    size_t n = strlen(root);
    if (n == 1 && root[0] == '/') {
        return p[0] == '/';
    }
    return strncmp(root, p, n) == 0 && (p[n] == '\0' || p[n] == '/');
}

int puci_workspace_contains(const char *root, const char *path) {
    if (!root || !root[0]) {
        return 1;
    }
    char want[PATH_MAX];
    if (!path || !path[0]) {
        path = ".";
    }
    if (path[0] == '/') {
        if (snprintf(want, sizeof(want), "%s", path) >= (int)sizeof(want)) return 0;
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return 0;
        if (snprintf(want, sizeof(want), "%s/%s", cwd, path) >= (int)sizeof(want)) return 0;
    }
    /* Resolve the deepest existing ancestor; the rest must not climb. */
    char head[PATH_MAX];
    snprintf(head, sizeof(head), "%s", want);
    char tailbuf[PATH_MAX];
    tailbuf[0] = '\0';
    for (;;) {
        char resolved[PATH_MAX];
        if (realpath(head, resolved)) {
            if (tailbuf[0]) {
                /* Reject ".." in the not-yet-existing remainder. */
                const char *c = tailbuf;
                while (*c) {
                    while (*c == '/') c++;
                    const char *e = strchr(c, '/');
                    size_t l = e ? (size_t)(e - c) : strlen(c);
                    if (l == 2 && c[0] == '.' && c[1] == '.') return 0;
                    c += l;
                }
            }
            return under(root, resolved);
        }
        char *slash = strrchr(head, '/');
        if (!slash) return 0;
        char rest[PATH_MAX];
        if (snprintf(rest, sizeof(rest), "%s%s%s", slash + 1, tailbuf[0] ? "/" : "", tailbuf) >= (int)sizeof(rest)) return 0;
        snprintf(tailbuf, sizeof(tailbuf), "%s", rest);
        if (slash == head) {
            head[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
}
