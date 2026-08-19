/* content.h — owned, structured content for the PUCI TUI.
 *
 * Two layers back the conversation:
 *   1. The ephemeral visual Ring (fixed display rows).
 *   2. This owned-content layer: dynamically allocated source that is the only
 *      authoritative copy of paste blocks, raw assistant Markdown, and tool
 *      source. Copy, send, and session save/load never reconstruct data from
 *      terminal cells.
 *
 * Ownership: every heap object has exactly one owner (the creator also frees).
 * All allocation arithmetic is overflow-checked; on failure the destination is
 * left untouched and a NULL result is returned.
 */
#ifndef PUCI_TUI_CONTENT_H
#define PUCI_TUI_CONTENT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resource limits (Phase 12). Generous for a coding agent yet bounded. */
#define CTX_PASTE_MAX_BYTES       (8u*1024u*1024u)
#define CTX_DRAFT_MAX_BYTES       (8u*1024u*1024u)
#define CTX_MSG_MAX_BYTES         (8u*1024u*1024u)
#define CTX_OSC52_MAX_PAYLOAD     (64u*1024u)
#define CTX_PREVIEW_LINES_MAX     1024
#define CTX_PASTE_COLLAPSE_LINES  4u          /* >= N lines -> a paste block  */
#define CTX_PASTE_COLLAPSE_BYTES  4096u       /* 1-line byte threshold        */
#define CTX_CODE_COLLAPSE_LINES   40u
#define CTX_LANG_MAX              32

/* --- Paste block ---------------------------------------------------------- */
typedef struct {
    uint64_t id;
    char    *data;                 /* owned exact bytes (no implicit NUL)     */
    size_t   byte_len;
    size_t   line_count;
    bool     ends_with_newline;
    char     language_hint[CTX_LANG_MAX];   /* normalized alias or ""        */
} TuiPasteBlock;

/* --- Composer segments (ordered) ------------------------------------------ */
typedef enum { COMPOSER_SEG_TEXT, COMPOSER_SEG_PASTE } ComposerSegmentKind;

typedef struct {
    ComposerSegmentKind kind;
    char      *text;      /* owned when kind==TEXT; else NULL  */
    size_t     text_len;
    TuiPasteBlock *paste; /* borrowed ref when kind==PASTE     */
} ComposerSegment;

/* Ordered composer: text and paste segments preserve exact user ordering.
 * total_bytes sums TEXT bytes plus the byte length of every paste (used for
 * the draft budget). */
typedef struct {
    ComposerSegment *segs;
    size_t seg_n, seg_cap;
    size_t total_bytes;
} Composer;

/* --- Owned content records for the conversation --------------------------- */
typedef enum {
    CTX_KIND_TEXT,      /* a text-only display record   */
    CTX_KIND_PASTE,     /* a paste block card            */
    CTX_KIND_ASSISTANT  /* assistant message (raw md)    */
} TuiContentKind;

typedef struct TuiMessage {
    TuiContentKind kind;
    char     *raw_source;   /* owned; set for ASSISTANT and paste-derived */
    size_t    raw_len;
    unsigned  content_version;   /* bumped per appended delta            */
} TuiMessage;

/* --- Paste block API ------------------------------------------------------- */
TuiPasteBlock *ctx_paste_new(const char *data, size_t n, const char *lang_hint);
void  ctx_paste_free(TuiPasteBlock *pb);
/* Collapsed label: "[pasted lines 1-327 · Python · 18.4 KiB]" (width-limited). */
void  ctx_paste_label(const TuiPasteBlock *pb, char *buf, size_t cap, int width);

/* --- Composer API ----------------------------------------------------------- */
bool  ctx_composer_init(Composer *c);
void  ctx_composer_free(Composer *c);
/* Insert a paste block at ordinal position pos (0..seg_n). Splits the text
 * segment at that position if needed. Pb is borrowed (reference only). */
bool  ctx_composer_insert_paste(Composer *c, size_t pos, TuiPasteBlock *pb);
/* Insert raw text into the text segment at ordinal pos; a paste segment at
 * pos causes a new text segment to be opened before it. */
bool  ctx_composer_insert_text(Composer *c, size_t pos, const char *s, size_t n);
/* Serialize to the model payload: text bodies and full paste bodies in order.
 * Result is owned; *out_len set. CRLF is normalized to LF (see docs/AUDIT). */
char *ctx_composer_serialize(Composer *c, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
