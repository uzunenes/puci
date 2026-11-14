/* tui/theme.c - the ONLY file allowed to name raw terminal colours.
 * See theme.h for the rules; scripts/check_tui_tokens.sh enforces them. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "theme.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

theme_t g_theme;

int theme_no_color_env(void) {
    const char *v = getenv("NO_COLOR");
    return v && v[0];
}

void theme_init(theme_bg bg, int no_color) {
    theme_t t;
    memset(&t, 0, sizeof(t));
    /* Roles shared by every palette: the terminal's own foreground. */
    t.bg = TB_DEFAULT;
    t.surface = TB_DEFAULT;
    t.border = TB_DEFAULT;
    t.text = TB_DEFAULT;
    t.muted = TB_DEFAULT | TB_DIM;
    t.assistant = TB_DEFAULT | TB_BOLD;
    t.hl_comment = TB_DEFAULT | TB_DIM;

    if (no_color) {
        /* Meaning through attributes only. */
        t.focus = TB_DEFAULT | TB_BOLD;
        t.accent = TB_DEFAULT | TB_BOLD;
        t.success = TB_DEFAULT;
        t.warning = TB_DEFAULT | TB_BOLD;
        t.error = TB_DEFAULT | TB_BOLD;
        t.user = TB_DEFAULT | TB_BOLD;
        t.tool = TB_DEFAULT | TB_BOLD;
        t.heading = TB_DEFAULT | TB_BOLD;
        t.quote = TB_DEFAULT | TB_DIM;
        t.code_fg = TB_DEFAULT | TB_REVERSE;
        t.code_bg = TB_DEFAULT;
        t.sel_fg = TB_DEFAULT | TB_REVERSE;
        t.sel_bg = TB_DEFAULT;
        t.banner_fg = TB_DEFAULT | TB_REVERSE | TB_BOLD;
        t.banner_bg = TB_DEFAULT;
        t.diff_add = TB_DEFAULT | TB_BOLD;
        t.diff_del = TB_DEFAULT | TB_DIM;
        t.diff_hunk = TB_DEFAULT | TB_UNDERLINE;
        t.hl_keyword = TB_DEFAULT | TB_BOLD;
        t.hl_string = TB_DEFAULT;
        t.hl_number = TB_DEFAULT;
        t.hl_bool = TB_DEFAULT | TB_BOLD;
        t.hl_link = TB_DEFAULT | TB_UNDERLINE;
    } else if (bg == THEME_BG_LIGHT) {
        /* Light ground: yellow and cyan wash out, so blue/magenta carry
         * accents; every colour is bold or on its own background. */
        t.focus = TB_BLUE | TB_BOLD;
        t.accent = TB_BLUE;
        t.success = TB_GREEN | TB_BOLD;
        t.warning = TB_MAGENTA | TB_BOLD;
        t.error = TB_RED | TB_BOLD;
        t.user = TB_BLUE | TB_BOLD;
        t.tool = TB_MAGENTA;
        t.heading = TB_BLUE | TB_BOLD;
        t.quote = TB_BLUE;
        t.code_fg = TB_DEFAULT | TB_REVERSE;
        t.code_bg = TB_DEFAULT;
        t.sel_fg = TB_WHITE | TB_BOLD;
        t.sel_bg = TB_BLUE;
        t.banner_fg = TB_WHITE | TB_BOLD;
        t.banner_bg = TB_MAGENTA;
        t.diff_add = TB_GREEN | TB_BOLD;
        t.diff_del = TB_RED | TB_BOLD;
        t.diff_hunk = TB_BLUE;
        t.hl_keyword = TB_BLUE | TB_BOLD;
        t.hl_string = TB_GREEN;
        t.hl_number = TB_MAGENTA;
        t.hl_bool = TB_MAGENTA | TB_BOLD;
        t.hl_link = TB_BLUE | TB_UNDERLINE;
    } else {
        /* Dark (and unknown) ground: the historical palette. */
        t.focus = TB_CYAN | TB_BOLD;
        t.accent = TB_CYAN;
        t.success = TB_GREEN;
        t.warning = TB_YELLOW;
        t.error = TB_RED | TB_BOLD;
        t.user = TB_CYAN | TB_BOLD;
        t.tool = TB_YELLOW;
        t.heading = TB_YELLOW | TB_BOLD;
        t.quote = TB_CYAN;
        t.code_fg = TB_BLACK;
        t.code_bg = TB_WHITE;
        t.sel_fg = TB_BLACK;
        t.sel_bg = TB_CYAN;
        t.banner_fg = TB_BLACK;
        t.banner_bg = TB_YELLOW;
        t.diff_add = TB_GREEN | TB_BOLD;
        t.diff_del = TB_RED | TB_BOLD;
        t.diff_hunk = TB_CYAN;
        t.hl_keyword = TB_CYAN | TB_BOLD;
        t.hl_string = TB_GREEN;
        t.hl_number = TB_MAGENTA;
        t.hl_bool = TB_YELLOW;
        t.hl_link = TB_CYAN | TB_UNDERLINE;
    }
    g_theme = t;
}

/* ---- background detection ------------------------------------------- */

static long long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Parses one "rgb:R/G/B" component (1-4 hex digits) to 0..1. */
static int hex_comp(const char **p, double *out) {
    unsigned v = 0;
    int n = 0;
    while (n < 4) {
        char c = **p;
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        v = v * 16 + d;
        (*p)++;
        n++;
    }
    if (n == 0) return -1;
    *out = (double)v / (double)((1u << (4 * n)) - 1);
    return 0;
}

/* "...]11;rgb:RRRR/GGGG/BBBB..." -> light/dark by relative luminance. */
static theme_bg parse_osc11(const char *buf) {
    const char *p = strstr(buf, "]11;rgb:");
    if (!p) return THEME_BG_UNKNOWN;
    p += 8;
    double r, g, b;
    if (hex_comp(&p, &r) || *p++ != '/' || hex_comp(&p, &g) || *p++ != '/' ||
        hex_comp(&p, &b)) {
        return THEME_BG_UNKNOWN;
    }
    double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return lum > 0.5 ? THEME_BG_LIGHT : THEME_BG_DARK;
}

static theme_bg query_terminal(const char *tty_path) {
    int fd = open(tty_path ? tty_path : "/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return THEME_BG_UNKNOWN;
    struct termios old, raw;
    if (tcgetattr(fd, &old) != 0) {
        close(fd);
        return THEME_BG_UNKNOWN;
    }
    raw = old;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    (void)tcsetattr(fd, TCSANOW, &raw);

    /* OSC 11 (background colour) then DA1, which every terminal answers:
     * seeing the DA1 reply means "no OSC 11 support", so we never sit out the
     * whole timeout on terminals that ignore OSC 11. */
    static const char q[] = "\033]11;?\033\\\033[c";
    theme_bg res = THEME_BG_UNKNOWN;
    if (write(fd, q, sizeof(q) - 1) == (ssize_t)(sizeof(q) - 1)) {
        char buf[256];
        size_t len = 0;
        long long deadline = mono_ms() + 200;
        for (;;) {
            long long left = deadline - mono_ms();
            if (left <= 0 || len + 1 >= sizeof(buf)) break;
            struct pollfd pfd = {fd, POLLIN, 0};
            if (poll(&pfd, 1, (int)left) <= 0) break;
            ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
            if (n <= 0) break;
            len += (size_t)n;
            buf[len] = '\0';
            /* DA1 reply: ESC [ ? ... c  -> everything before it has arrived. */
            const char *da = strstr(buf, "\033[?");
            if (da && strchr(da, 'c')) break;
        }
        buf[len] = '\0';
        res = parse_osc11(buf);
    }
    (void)tcsetattr(fd, TCSANOW, &old);
    close(fd);
    return res;
}


/* ---- desktop (OS) appearance --------------------------------------- */

/* Runs argv (no shell) and captures up to cap-1 bytes of stdout, bounded by
 * timeout_ms. Returns the byte count, or -1 if it could not run in time. */
static int run_capture(char *const argv[], char *out, size_t cap, int timeout_ms) {
    int pfd[2];
    if (cap == 0 || pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 2);
        }
        dup2(pfd[1], 1);
        close(pfd[0]);
        close(pfd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t len = 0;
    long long deadline = mono_ms() + timeout_ms;
    for (;;) {
        long long left = deadline - mono_ms();
        if (left <= 0 || len + 1 >= cap) break;
        struct pollfd p = {pfd[0], POLLIN, 0};
        if (poll(&p, 1, (int)left) <= 0) break;
        ssize_t n = read(pfd[0], out + len, cap - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    out[len] = '\0';
    close(pfd[0]);
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return (int)len;
}

/* The desktop's light/dark preference, for terminals that won't say what
 * their background is. freedesktop portal (GNOME, KDE, ...), then GNOME's
 * gsettings, then macOS. Each probe is bounded; failures fall through. */
static theme_bg desktop_preference(void) {
    char out[256];
    char *portal[] = {"gdbus", "call", "--session",
                      "--dest", "org.freedesktop.portal.Desktop",
                      "--object-path", "/org/freedesktop/portal/desktop",
                      "--method", "org.freedesktop.portal.Settings.ReadOne",
                      "org.freedesktop.appearance", "color-scheme", NULL};
    if (run_capture(portal, out, sizeof(out), 300) > 0) {
        /* "(<uint32 1>,)": 1 = prefer dark, 2 = prefer light, 0 = none */
        if (strstr(out, "uint32 1")) return THEME_BG_DARK;
        if (strstr(out, "uint32 2")) return THEME_BG_LIGHT;
    }
    char *gs[] = {"gsettings", "get", "org.gnome.desktop.interface", "color-scheme", NULL};
    if (run_capture(gs, out, sizeof(out), 300) > 0) {
        if (strstr(out, "prefer-dark")) return THEME_BG_DARK;
        if (strstr(out, "prefer-light")) return THEME_BG_LIGHT;
        /* 'default': fall back to the GTK theme's name (Yaru-dark, Adwaita-dark). */
        char *gt[] = {"gsettings", "get", "org.gnome.desktop.interface", "gtk-theme", NULL};
        if (run_capture(gt, out, sizeof(out), 300) > 0) {
            for (char *c = out; *c; c++) {
                if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
            }
            return strstr(out, "dark") ? THEME_BG_DARK : THEME_BG_LIGHT;
        }
    }
#ifdef __APPLE__
    char *mac[] = {"defaults", "read", "-g", "AppleInterfaceStyle", NULL};
    int n = run_capture(mac, out, sizeof(out), 300);
    if (n > 0 && strstr(out, "Dark")) return THEME_BG_DARK;
    if (n < 0) return THEME_BG_LIGHT; /* key absent = light mode */
#endif
    return THEME_BG_UNKNOWN;
}

theme_bg theme_detect_bg(const char *tty_path) {
    const char *forced = getenv("PUCI_THEME");
    if (forced && strcasecmp(forced, "light") == 0) return THEME_BG_LIGHT;
    if (forced && strcasecmp(forced, "dark") == 0) return THEME_BG_DARK;

    /* rxvt/Konsole convention: "fg;bg" with ANSI colour indices. */
    const char *cfb = getenv("COLORFGBG");
    if (cfb) {
        const char *semi = strrchr(cfb, ';');
        if (semi) {
            int bgi = atoi(semi + 1);
            return (bgi == 7 || bgi == 15) ? THEME_BG_LIGHT : THEME_BG_DARK;
        }
    }
    if (forced && strcasecmp(forced, "auto") != 0 && forced[0]) {
        return THEME_BG_UNKNOWN;
    }
    /* The terminal knows its real background (its profile may differ from
     * the desktop); ask it first, then follow the computer's light/dark. */
    theme_bg t = query_terminal(tty_path);
    if (t != THEME_BG_UNKNOWN) return t;
    return desktop_preference();
}
