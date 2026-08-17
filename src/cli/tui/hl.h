/* hl.h — small, deterministic terminal syntax highlighter for the PUCI TUI.
 *
 * Emits *semantic* style tokens (not terminal color numbers) per line for a
 * normalized language (see HlLang in md.h). The TUI maps those tokens to its
 * theme colors. The highlighter never mutates the source; it only annotates
 * byte ranges. If a token cannot be classified with confidence it is emitted
 * as HL_TK_TEXT (the safe fallback).
 *
 * Bounded: a single line produces at most HL_MAX_SPANS spans. There is no
 * regex / backtracking; it is a small stateful scan.
 */
#ifndef PUCI_TUI_HL_H
#define PUCI_TUI_HL_H

#include "md.h"   /* HlLang */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HL_MAX_SPANS 64

typedef enum {
    HL_TK_TEXT,
    HL_TK_KEYWORD,
    HL_TK_STRING,
    HL_TK_NUMBER,
    HL_TK_COMMENT,
    HL_TK_BOOL,
    HL_TK_PUNCT,
    HL_TK_HEADING,   /* markdown heading marker (#)  */
    HL_TK_DIFF_ADD,  /* diff: added line             */
    HL_TK_DIFF_DEL,  /* diff: removed line           */
    HL_TK_DIFF_HUNK, /* diff hunk header             */
    HL_TK_MD_LINK    /* markdown link / url          */
} HlToken;

typedef struct { HlToken kind; size_t off, len; } HlSpan;

/* Tokenize one line of `lang` source. Writes at most `cap` spans to `out`;
 * returns the number written (<= cap). `line` is not NUL-terminated; spans
 * reference byte offsets within [0,len). */
int hl_line(HlLang lang, const char *line, size_t len, HlSpan *out, int cap);

#ifdef __cplusplus
}
#endif

#endif
