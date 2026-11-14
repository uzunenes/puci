#!/bin/sh
# scripts/check_tui_tokens.sh - TUI design gate (runs in `meson test`).
#
# Rules (see src/cli/tui/theme.h):
#   - raw terminal colours (TB_RED, TB_YELLOW, ...) only in tui/theme.c;
#     everything else draws through the g_theme roles;
#   - no "bold black" (black on dark terminals that don't brighten bold).
set -u
cd "$(dirname "$0")/.." || exit 2
dir=src/cli/tui
bad=$(grep -n -E 'TB_(BLACK|RED|GREEN|YELLOW|BLUE|MAGENTA|CYAN|WHITE)\b' \
        "$dir"/*.c "$dir"/*.h 2>/dev/null | grep -v "^$dir/theme\.c:")
bold_black=$(grep -n -E 'TB_BLACK *\| *TB_BOLD|TB_BOLD *\| *TB_BLACK' "$dir"/*.c 2>/dev/null)
rc=0
if [ -n "$bad" ]; then
    echo "raw colour outside src/cli/tui/theme.c (use a g_theme role):"
    echo "$bad"
    rc=1
fi
if [ -n "$bold_black" ]; then
    echo "bold black is unreadable on dark terminals:"
    echo "$bold_black"
    rc=1
fi
[ $rc -eq 0 ] && echo "tui tokens: ok"
exit $rc
