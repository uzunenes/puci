/* md.h — small, bounded Markdown block parser for the PUCI TUI.
 *
 * Purpose: classify raw assistant Markdown into block records and, for code
 * fences, extract the exact (unstyled) code body and language tag so that:
 *   - copy-code returns only the code body;
 *   - the language tag is available for syntax highlighting;
 *   - the renderer degrades to readable plain text for everything else.
 *
 * It is intentionally NOT CommonMark. It is a deterministic, bounds-checked
 * block scanner. Malformed input degrades to paragraphs.
 */
#ifndef PUCI_TUI_MD_H
#define PUCI_TUI_MD_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MD_MAX_BLOCKS 256

typedef enum {
    MD_BLOCK_PARAGRAPH,
    MD_BLOCK_HEADING,
    MD_BLOCK_ULIST,
    MD_BLOCK_OLIST,
    MD_BLOCK_QUOTE,
    MD_BLOCK_HR,
    MD_BLOCK_CODE,      /* fenced code: see lang/body */
    MD_BLOCK_TOOL,      /* application-produced tool/file block */
    MD_BLOCK_TEXT       /* raw text line (inside a code/quote group) */
} MdBlockType;

/* Normalized language family for highlighting. */
typedef enum {
    HL_LANG_NONE,
    HL_LANG_C, HL_LANG_CPP, HL_LANG_PYTHON, HL_LANG_JSON, HL_LANG_MARKDOWN,
    HL_LANG_SHELL, HL_LANG_JS, HL_LANG_TS, HL_LANG_YAML, HL_LANG_TOML,
    HL_LANG_DIFF, HL_LANG_RUST, HL_LANG_GO
} HlLang;

typedef struct {
    MdBlockType type;
    /* byte offsets into the source (for copy and raw access) */
    bool     is_fence;
    HlLang   lang;             /* normalized language for FENCE blocks      */
    char     lang_str[32];     /* original info string (trimmed)            */
    /* For MD_BLOCK_CODE: exact body span (without fences) in the source. */
    size_t   body_off, body_len;
    bool     closed;           /* false if the fence never closed            */
} MdBlock;

typedef struct {
    MdBlock     blocks[MD_MAX_BLOCKS];
    size_t      count;
    /* total fence count seen (to cap runaway inputs) */
    bool        truncated;    /* set if MD_MAX_BLOCKS was reached */
} MdDoc;

/* Parse 'src' (not NUL-terminated) into a MdDoc. streamed=true tells the
 * parser an unclosed fence at end-of-input is expected (still streaming). */
MdDoc md_parse(const char *src, size_t len, bool streaming);

/* Normalize a language info tag -> HlLang and canonical alias. */
HlLang md_lang_normalize(const char *tag, char *alias, size_t alias_cap);
/* Infer a language from a filename extension. Returns HlLang::NONE_unknown. */
HlLang md_lang_from_filename(const char *path);

#ifdef __cplusplus
}
#endif

#endif
