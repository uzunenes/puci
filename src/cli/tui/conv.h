/* conv.h — owned semantic conversation model for the PUCI TUI.
 *
 * This is the authoritative source of conversation content. The visual Ring
 * (in tui.c) is only a wrapped-line render cache; it is never the source of
 * markdown, paste bodies, code bodies, or any data that can be copied or
 * persisted. This file and conv.c are dependency-free (no termbox), so they
 * are unit-testable in isolation.
 *
 * Architecture (Phase 1):
 *   semantic message  ->  parsed content parts  ->  visual wrapped-line cache  ->  terminal cells
 *
 * Ownership:
 *   - Every user and assistant message owns its complete raw source.
 *   - Streaming deltas append to the assistant raw source (caller must strip
 *     TTG envelopes *before* appending).
 *   - Markdown forms parts from raw source (not from visual lines).
 *   - Wrapping consumes parsed parts.
 *   - Copy consumes raw source / parts.
 *   - Clearing frees each allocation exactly once.
 */
#ifndef PUCI_TUI_CONV_H
#define PUCI_TUI_CONV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Roles and part kinds ------------------------------------------------ */
typedef enum {
    TUI_ROLE_USER,
    TUI_ROLE_ASSISTANT,
    TUI_ROLE_SYSTEM,
    TUI_ROLE_TOOL,
    TUI_ROLE_ERROR
} TuiMessageRole;

typedef enum {
    TUI_PART_TEXT,       /* ordinary paragraph/inline text                  */
    TUI_PART_PASTE,      /* owned user paste block                          */
    TUI_PART_CODE,       /* fenced code block from assistant markdown       */
    TUI_PART_TOOL_OUTPUT,/* tool stdout (no filename)                       */
    TUI_PART_FILE        /* trusted filename + source                       */
} TuiPartKind;

/* How a part relates to its complete content (Phase 13 semantics). */
typedef enum {
    TUI_COMPLETE,             /* body is the full content                      */
    TUI_PARTIAL_STREAM,       /* assistant stream was cancelled mid-fence      */
    TUI_PARTIAL_UPSTREAM,     /* truncated by tool/provider, unknown tail      */
    TUI_PARTIAL_HARDLIMIT,    /* rejected: exceeded configured hard limit      */
    TUI_COLLAPSED             /* complete but visually collapsed > threshold   */
} TuiPartStatus;

/* --- Content parts -------------------------------------------------------- */
typedef struct {
    uint64_t id;
    TuiPartKind kind;
    char     *raw_source;   /* owned exact bytes (may contain NULs) */
    size_t    raw_len;
    char     *language;     /* owned normalized language or NULL    */
    char     *filename;     /* owned trusted filename or NULL       */
    size_t    line_count;
    TuiPartStatus status;
    bool      has_newline_ends; /* raw_source ends with exactly one \n */
} TuiContentPart;

/* --- Messages ------------------------------------------------------------- */
typedef struct {
    uint64_t id;
    TuiMessageRole role;

    char   *raw_source;    /* owned, NUL-terminated */
    size_t  raw_len;
    size_t  raw_cap;

    TuiContentPart *parts;
    size_t  part_count;
    size_t  part_cap;

    unsigned content_version; /* bumped each append */

    bool streaming;
    bool cancelled;
    bool failed;

    /* transient: rolling plain-text cache (owned), updated on serialize */
    char   *plain;
    size_t  plain_len;
} TuiMessage;

/* --- Conversation --------------------------------------------------------- */
typedef struct {
    TuiMessage *messages;
    size_t count;
    size_t cap;
    unsigned version;
} TuiConversation;

/* --- Conversation lifecycle ---------------------------------------------- */
void conv_init(TuiConversation *c);
void conv_free(TuiConversation *c);            /* frees everything exactly once */
TuiMessage *conv_new_message(TuiConversation *c, TuiMessageRole role);

/* --- Assistant streaming -------------------------------------------------- */
/* Append a (TTG-filtered) delta to the streaming assistant message. Returns
 * the message, or NULL if none is currently streaming. */
TuiMessage *conv_begin_assistant(TuiConversation *c);
int  conv_append(TuiConversation *c, TuiMessage *m, const char *data, size_t n);
int  conv_append_raw(TuiConversation *c, TuiMessage *m, const char *data, size_t n);
int  conv_appendf(TuiConversation *c, TuiMessage *m, const char *fmt, ...);
/* Finalize the currently streaming assistant message; parses markdown parts. */
int  conv_end_assistant(TuiConversation *c, TuiMessage *m);
/* Cancel: keep the partial raw source, mark cancelled, parse what we have. */
int  conv_cancel_assistant(TuiConversation *c, TuiMessage *m);

/* --- User message with paste --------------------------------------------- */
/* Worker: a user message + one part. Returns message. */
TuiMessage *conv_user_message(TuiConversation *c, const char *text, size_t text_len);
/* Append a paste part to a user message. */
TuiContentPart *conv_user_add_paste(TuiConversation *c, TuiMessage *m,
                                    const char *data, size_t n, const char *lang);
/* Tool/file output part helper. */
TuiContentPart *conv_message_add_file(TuiConversation *c, TuiMessage *m,
                                      const char *filename, const char *lang,
                                      const char *data, size_t n, bool truncated);
TuiContentPart *conv_message_add_tool(TuiConversation *c, TuiMessage *m,
                                      const char *data, size_t n);

/* --- Queries ------------------------------------------------------------- */
uint64_t conv_max_id(TuiConversation *c);
/* Recompute plain text (rendered-markdown-lite) for a message. */
void conv_message_plain(TuiConversation *c, TuiMessage *m);

/* --- Copy / serialize ----------------------------------------------------- */
/* Exact copy of a part body (no decorations). Returns owned buffer. */
char *conv_part_copy(const TuiContentPart *p, size_t *out_len);
/* Exact raw markdown of a message (raw_source). */
char *conv_message_raw_copy(const TuiMessage *m, size_t *out_len);
/* Plain text copy (conv plain cache). */
char *conv_message_plain_copy(const TuiMessage *m, size_t *out_len);

/* --- Serialization (Phase 9: versioned sidecar) --------------------------- */
/* Serialize a conversation to a JSON sidecar string. Caller frees. */
char *conv_serialize(const TuiConversation *c, size_t *out_len);
/* Parse a JSON sidecar, appending messages. Returns 0 on success. */
int   conv_deserialize(TuiConversation *c, const char *json, size_t len);

#ifdef __cplusplus
}
#endif

#endif
