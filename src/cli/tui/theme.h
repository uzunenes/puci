/* tui/theme.h - semantic colour roles for the TUI.
 *
 * Design rules (enforced by scripts/check_tui_tokens.sh in `meson test`):
 *   - Drawing code uses ONLY these roles (g_theme.*), never a raw TB_<colour>.
 *     Raw colours live in theme.c alone.
 *   - Body text, borders and labels use the terminal's own foreground
 *     (TB_DEFAULT): readable on every palette. Never hard-code white/black
 *     text on the default background.
 *   - Accents are chosen per background: yellow/cyan read on dark terminals
 *     but not on light ones, so a light background gets blue/magenta.
 *   - NO_COLOR (https://no-color.org) drops every colour; meaning is carried
 *     by bold / dim / reverse alone.
 */
#ifndef PUCI_TUI_THEME_H
#define PUCI_TUI_THEME_H

/* termbox2's implementation half is not include-guarded: tui.c includes it
 * with TB_IMPL first, so only pull in the declarations when absent. */
#ifndef TERMBOX_H_INCL
#include "vendor/termbox2.h"
#endif

typedef struct {
    uintattr_t bg;         /* default background (TB_DEFAULT) */
    uintattr_t surface;    /* overlay/panel background */
    uintattr_t border;     /* faint border */
    uintattr_t focus;      /* focused border */
    uintattr_t text;       /* body text */
    uintattr_t muted;      /* dimmed/secondary text */
    uintattr_t accent;     /* primary accent */
    uintattr_t success;    /* ok / ready */
    uintattr_t warning;    /* pending / attention */
    uintattr_t error;      /* failure */
    uintattr_t user;       /* user message label */
    uintattr_t assistant;  /* assistant label */
    uintattr_t tool;       /* tool card */
    /* markdown */
    uintattr_t heading;
    uintattr_t quote;
    uintattr_t code_fg, code_bg;     /* inline code */
    /* selection + banners */
    uintattr_t sel_fg, sel_bg;       /* selected picker row */
    uintattr_t banner_fg, banner_bg; /* approval prompt bar */
    /* diffs */
    uintattr_t diff_add, diff_del, diff_hunk;
    /* syntax highlighting */
    uintattr_t hl_keyword, hl_string, hl_number, hl_comment, hl_bool, hl_link;
} theme_t;

typedef enum { THEME_BG_UNKNOWN = -1, THEME_BG_DARK = 0, THEME_BG_LIGHT = 1 } theme_bg;

extern theme_t g_theme;

/* Fills g_theme. `bg` picks the accent palette; `no_color` strips colour. */
void theme_init(theme_bg bg, int no_color);

/* Decides the background: PUCI_THEME=light|dark, else COLORFGBG, else asks
 * the terminal (OSC 11) on `tty_path` (NULL = /dev/tty) with a short bound.
 * Must run BEFORE tb_init (it briefly owns the tty). */
theme_bg theme_detect_bg(const char *tty_path);

/* 1 when NO_COLOR is set to a non-empty value. */
int theme_no_color_env(void);

#endif
