/* tui.c - PUCI Featherweight TUI v0.5.0 — termbox2 hybrid cell renderer
 * Master poll ring: STDIN + N*session fds. termbox2 = double-buffer only.
 * World-class chat flow: stream coalescing, word-wrap, scrollback, session & model pickers.
 */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wextra"
#pragma clang diagnostic ignored "-Wpedantic"
#pragma clang diagnostic ignored "-Wunused-result"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wunused-result"
#endif
#define TB_IMPL
#include "vendor/termbox2.h"
#include "ttg.h"
#include "clip.h"
#include "content.h"
#include "md.h"
#include "hl.h"
#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "tui.h"
#include "../cmd/config.h"
#include "../cmd/session_mgr.h"
#include <puci/puci.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stddef.h>
/* libcurl is a build-system feature: src/cli/meson.build defines -DHAVE_CURL=1
 * and links -lcurl together. Never auto-enable from __has_include alone —
 * a present header does not mean -lcurl is on the link line. Use the
 * pre-processor-safe `#if` form so -DHAVE_CURL=0 actually disables it. */
#if HAVE_CURL
#include <curl/curl.h>
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif

/* ── PUCI_TRACE instrumentation ── */
#include <stdint.h>
#include <stdatomic.h>
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#endif
#define TRACE_RING_CAP (1<<18) // 262k entries
typedef enum {
    TR_POLL_RET=0, TR_PROCESS, TR_EVENT, TR_RENDER_BEG, TR_RENDER_END, TR_PRESENT, TR_PROCFS, TR_SAVE, TR_ESCWAIT, TR_APPROVAL, TR_LAYOUT
} trace_phase_t;
static const char *trace_phase_name[] = {"POLL_RET","PROCESS","EVENT","RENDER_BEG","RENDER_END","PRESENT","PROCFS","SAVE","ESCWAIT","APPROVAL","LAYOUT"};
typedef struct { uint64_t mono_ns; uint64_t cpu_ns; uint32_t phase; uint32_t extra; } trec_t;
static trec_t g_trace_ring[TRACE_RING_CAP];
static _Atomic size_t g_trace_head=0;
static int g_trace_enabled=0;
static const char *g_trace_file=NULL;
static char g_trace_file_buf[1024];
static void trace_init(void){
    const char *v=getenv("PUCI_TRACE");
    if(v && *v && strcmp(v,"0")!=0) g_trace_enabled=1;
    const char *f=getenv("PUCI_TRACE_FILE");
    if(f && *f){
        snprintf(g_trace_file_buf, sizeof g_trace_file_buf, "%s", f);
    } else {
        /* Default to a per-user, per-process path so concurrent runs don't
           clobber each other and a predictable /tmp path isn't a symlink/TOCTOU
           target. Prefer the OS-backed state dir; fall back to /tmp with pid. */
        const char *base=getenv("XDG_STATE_HOME");
        char prefix[768];
        if(base && *base){
            snprintf(prefix,sizeof prefix,"%s/puci", base);
            mkdir(prefix, 0700);
        } else {
            const char *home=getenv("HOME");
            if(home && *home){
                snprintf(prefix,sizeof prefix,"%s/.local/state/puci", home);
                mkdir(prefix, 0700);
            } else {
                snprintf(prefix,sizeof prefix,"/tmp");
            }
        }
        snprintf(g_trace_file_buf,sizeof g_trace_file_buf,"%s/puci_trace.%ld.ndjson", prefix, (long)getpid());
    }
    g_trace_file = g_trace_file_buf;
}
static void trace_dump(void){
    if(!g_trace_enabled) return;
    size_t total = atomic_load(&g_trace_head);
    size_t start = total > TRACE_RING_CAP ? total - TRACE_RING_CAP : 0;
    size_t written = total - start;
    FILE *f=fopen(g_trace_file,"w");
    if(!f) f=stderr;
    for(size_t i=start;i<total;i++){
        trec_t *r=&g_trace_ring[i % TRACE_RING_CAP];
        const char *pn = r->phase <= TR_LAYOUT ? trace_phase_name[r->phase] : "UNKNOWN";
        fprintf(f,"{\"phase\":\"%s\",\"mono_ns\":%llu,\"cpu_ns\":%llu,\"extra\":%u,\"seq\":%zu}\n", pn, (unsigned long long)r->mono_ns, (unsigned long long)r->cpu_ns, r->extra, i);
    }
    if(f!=stderr) fclose(f);
    fprintf(stderr,"[trace] dumped %zu records to %s\n", written, g_trace_file);
}
static inline uint64_t trace_now_mono_ns(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline uint64_t trace_now_cpu_ns(void){
    struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline void trace_emit(uint32_t phase, uint32_t extra){
    if(!g_trace_enabled) return;
    size_t idx = atomic_fetch_add(&g_trace_head, 1);
    trec_t *r = &g_trace_ring[idx % TRACE_RING_CAP];
    r->mono_ns = trace_now_mono_ns();
    r->cpu_ns = trace_now_cpu_ns();
    r->phase = phase;
    r->extra = extra;
}
#define TRACE(p,e) trace_emit((p),(e))
#define MAX_PANES 4
#define RING_LINES 800
#define RING_COLS 4096
#define STREAM_BUF 16384

static volatile sig_atomic_t g_winch = 0;
static volatile sig_atomic_t g_terminate = 0; /* SIGTERM/SIGHUP -> clean teardown */
static struct sigaction g_old_winch;
static void on_winch(int s){ (void)s; g_winch=1; }
static void on_terminate(int s){ (void)s; g_terminate=1; }

/* ── Semantic theme tokens ──────────────────────────────────────────
   All colour decisions go through this palette so overlay/header blue
   no longer dominates the screen. The terminal default background is used
   everywhere except selected rows. */
typedef struct {
    uintattr_t bg;         /* default background (TB_DEFAULT) */
    uintattr_t surface;    /* overlay/panel background */
    uintattr_t border;     /* faint border */
    uintattr_t focus;      /* focused border / accent */
    uintattr_t text;       /* body text */
    uintattr_t muted;      /* dimmed/secondary text */
    uintattr_t accent;     /* primary accent */
    uintattr_t success;    /* ok / ready */
    uintattr_t warning;    /* pending / attention */
    uintattr_t error;      /* failure */
    uintattr_t user;       /* user message label */
    uintattr_t assistant;  /* assistant label */
    uintattr_t tool;       /* tool card */
} theme_t;
static const theme_t g_theme = {
    TB_DEFAULT, TB_DEFAULT, TB_WHITE|TB_BOLD, TB_CYAN|TB_BOLD,
    TB_WHITE, TB_BLACK|TB_BOLD, TB_CYAN, TB_GREEN,
    TB_YELLOW, TB_RED|TB_BOLD, TB_CYAN|TB_BOLD, TB_WHITE|TB_BOLD, TB_YELLOW
};

static int g_show_debug=0;   /* enable RSS/PSS/trace metrics in header */

static int g_headless=0;
static int g_tb_ok=0; /* 1 once tb_init() succeeded; guards termbox calls */
static void tui_teardown(void){
    if(g_tb_ok){ tb_shutdown(); g_tb_ok=0; }
    sigaction(SIGWINCH, &g_old_winch, NULL); /* restore the app's handler */
}

static void get_winsize(int *rows,int *cols){
    if(g_tb_ok){
        int w = tb_width(); int h = tb_height();
        if(w>0 && h>0){ *cols=w; *rows=h; return; }
    }
    struct winsize ws; if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0 && ws.ws_col>0 && ws.ws_row>0){ *rows=ws.ws_row; *cols=ws.ws_col; } else { *rows=24; *cols=80; }
}
static long g_rss_cache=-1; static long g_pss_cache=-1; static uint64_t g_rss_last_ns=0; static uint64_t g_pss_last_ns=0;
static long read_rss_kb(void){
    TRACE(TR_PROCFS,0);
    uint64_t now=trace_now_mono_ns(); if(g_rss_cache>=0 && now - g_rss_last_ns < 500000000ULL) return g_rss_cache;
    FILE *f=fopen("/proc/self/status","r"); if(!f) return g_rss_cache>=0?g_rss_cache:-1;
    char line[128]; long v=-1;
    while(fgets(line,sizeof line,f)){ if(strncmp(line,"VmRSS:",6)==0){ v=strtol(line+6,NULL,10); break; } }
    fclose(f); if(v>=0){ g_rss_cache=v; g_rss_last_ns=now; } return v>=0?v:(g_rss_cache>=0?g_rss_cache:-1);
}
static long read_pss_kb(void){
    TRACE(TR_PROCFS,1);
    uint64_t now=trace_now_mono_ns(); if(g_pss_cache>=0 && now - g_pss_last_ns < 500000000ULL) return g_pss_cache;
    FILE *f=fopen("/proc/self/smaps_rollup","r"); if(!f) return read_rss_kb();
    char line[128]; long v=-1;
    while(fgets(line,sizeof line,f)){ if(strncmp(line,"Pss:",4)==0){ v=strtol(line+4,NULL,10); break; } }
    fclose(f); if(v>=0){ g_pss_cache=v; g_pss_last_ns=trace_now_mono_ns(); }
    return v>=0?v:(g_pss_cache>=0?g_pss_cache:read_rss_kb());
}

/* ── Ring with logical lines + role ── */
typedef enum { RL_USER, RL_ASSISTANT, RL_TOOL, RL_SYSTEM, RL_ERROR } rtype_t;
typedef struct Ring {
    char lines[RING_LINES][RING_COLS];
    rtype_t types[RING_LINES];
    int head; int count;
} Ring;
static void ring_touch(Ring *r); /* defined after Pane */
static void ring_push_typed(Ring *r,const char *s, rtype_t t){
    int idx=(r->head + r->count)%RING_LINES;
    size_t n = s ? strlen(s) : 0; if(n>=RING_COLS) n=RING_COLS-1;
    if(s) memcpy(r->lines[idx],s,n);
    r->lines[idx][n]=0;
    r->types[idx]=t;
    if(r->count<RING_LINES) r->count++; else r->head=(r->head+1)%RING_LINES;
    ring_touch(r);
}
static void ring_push(Ring *r,const char *s){ ring_push_typed(r,s,RL_SYSTEM); }
static void ring_append_to_last(Ring *r,const char *s, size_t len){
    size_t off=0;
    while(off < len){
        if(r->count==0){
            ring_push(r, "");
        }
        int idx=(r->head + r->count -1)%RING_LINES;
        size_t cur=strlen(r->lines[idx]);
        size_t avail=RING_COLS-1 - cur;
        if(avail==0){
            /* Current line is full: spill the remainder into new ring lines
               (same visual role) instead of silently dropping it. */
            rtype_t t = r->types[idx];
            ring_push_typed(r, "", t);
            continue;
        }
        size_t add=len-off < avail ? len-off : avail;
        memcpy(r->lines[idx]+cur, s+off, add); r->lines[idx][cur+add]=0;
        off+=add;
    }
    ring_touch(r);
}

typedef struct Pane {
    puci_session *sess;
    Ring ring;
    char title[64];
    char session_id[128];
    int streaming;
    int layout_dirty;
    int cached_total_vis;
    int cached_inner_w;    // content width (excluding scrollbar gutter)
    int cached_scrollable; // whether the last layout had a scrollbar
    char stream_buf[STREAM_BUF];
    size_t stream_len;
    /* Streaming TTG-envelope filter state: carries a partial envelope across
       chunks so it can never reach the Ring as visible content. */
    puci_ttg_state ttg;
    int scroll_off; // 0=bottom, >0 scrolled up (visual lines)
    int pending_nl;
    int need_wrap_recalc;
    int follow;    // 1 = pinned to the bottom (new output auto-shown), 0 = scrolled up
    int unread;    // new visual lines that arrived after follow was disabled
    int  thinking;        // 1 while a reasoning phase is in progress
    int64_t think_start_ns; // mono nanoseconds when reasoning began (0 = none)
    /* Code-fence state carried across streaming chunks so a fence opened in
       one chunk and closed in a later one is recognized. */
    int    in_fence;       // currently inside a ``` code block
    char   fence_lang[CTX_LANG_MAX]; // normalized fence language tag (or "")
} Pane;

/* Every Ring lives embedded inside a Pane (g_panes[].ring); invalidate the
 * wrapper pane's visual layout cache whenever the ring content changes so the
 * cached_total_vis isn't stale after /clear, user msg, model change, etc. */
static Pane *pane_from_ring(Ring *r){
    char *base=(char*)r;
    return (Pane*)(base - offsetof(Pane, ring));
}
static void ring_touch(Ring *r){
    if(r) pane_from_ring(r)->layout_dirty = 1;
}

static Pane g_panes[MAX_PANES];
static int g_npane=1;
static int g_focus=0;
static int g_show_sidebar=0;
static int g_split=0;   /* 0=conversation-first single pane, 1=explicit split grid */
/* Async approval state: the TUI never blocks inside a session callback.  A
 * PUCI_EVENT_APPROVAL_REQUEST pauses the owning turn and the TUI shows a
 * modal overlay; the user's y/n/key resolves it via the non-blocking
 * puci_session_resolve_approval() so the main poll() loop keeps running
 * (other panes stream, resize/timers/toasts work). */
static int g_approval_pending=0;
static char g_approval_diff[262144];
static int g_hunk_off=0;
static int g_hunk_max=0;
static uint64_t g_approval_id=0;
static int g_approval_pane=-1;      /* g_panes[] index awaiting the decision */
static int g_approval_feedback_mode=0; /* 1 = reading a denial reason */
static char g_approval_feedback[512];  /* reason passed to set_approval_feedback */
static size_t g_approval_feedback_len=0;
/* Copy a string (truncated to cap-pos-1) into buf at pos; returns new pos. */
static size_t app_append(char *buf, size_t cap, size_t pos, const char *part){
    if(!part || pos>=cap) return pos;
    size_t avail = cap - pos - 1;
    size_t l = strlen(part);
    if(l > avail) l = avail;
    memcpy(buf+pos, part, l);
    pos += l;
    buf[pos] = 0;
    return pos;
}
static char g_input[2048];
static size_t g_input_len=0;
static size_t g_input_curs=0;
static int g_input_mode=0;
/* Bracketed-paste state: while set, typed/pasted bytes are data, never
 * command keys (so a paste cannot quit, open a picker, or approve). */
static int g_paste=0;
/* Pasted payload captured while g_paste is set. We keep it OUT of the flat
 * g_input authoring buffer so a large paste never truncates the typed text
 * (Phase 1: no silent data loss). It is presented in the composer as a
 * collapsed chip and sent to the provider byte-for-byte. */
static char    *g_paste_buf=NULL;
static size_t   g_paste_len=0;
static size_t   g_paste_cap=0;
static int g_rows=24, g_cols=80;
static char g_current_model[256]="";

static int g_sess_seq=0;
#define HIST_MAX 200
static char g_hist[HIST_MAX][2048];
static int g_hist_count=0;
static int g_hist_pos=0; // navigation index
static char g_hist_tmp[2048];

/* --- Paste capture helpers (Phase 1) -------------------------------------- */
/* Hard cap for a single pasted draft so the send buffer and transcript stay
 * bounded. Rejecting (rather than silently truncating) a paste keeps the
 * "no silent data loss" guarantee. */
#define PASTE_MAX  (4u*1024u*1024u) /* 4 MiB */
static void paste_buf_clear(void){
    free(g_paste_buf); g_paste_buf=NULL; g_paste_len=0; g_paste_cap=0;
}
/* Returns 0 on success, -1 if the paste would exceed PASTE_MAX (caller keeps
 * whatever accumulated so far and reports 'paste too large'). */
static int paste_buf_append(const char *data, size_t n){
    if(!n) return 0;
    if(g_paste_len+n > PASTE_MAX) return -1;
    size_t need=g_paste_len+n;
    if(need>g_paste_cap){
        size_t nc=g_paste_cap? g_paste_cap : 256u;
        while(nc<need) nc*=2;
        char *nb=realloc(g_paste_buf,nc); if(!nb) return -1;
        g_paste_buf=nb; g_paste_cap=nc;
    }
    memcpy(g_paste_buf+g_paste_len,data,n);
    g_paste_len+=n;
    return 0;
}
/* Collapsed composer chip describing a pending paste, e.g.
 * "[pasted 8734 lines · 2.3 MiB]" or "[pasted 12 lines · 4.1 KiB]". */
static void paste_buf_label(char *out, size_t cap){
    if(g_paste_len==0){ if(cap) out[0]=0; return; }
    size_t lines=1; for(size_t i=0;i<g_paste_len;i++) if(g_paste_buf[i]=='\n') lines++;
    double kib=(double)g_paste_len/1024.0;
    if(kib>=1024.0) snprintf(out,cap,"[pasted %zu lines · %.2f MiB]", lines, kib/1024.0);
    else snprintf(out,cap,"[pasted %zu lines · %.1f KiB]", lines, kib);
}
/* Build the full draft payload = typed text + (optionally) a newline + the
 * paste bytes, byte-for-byte, NUL-terminated. Returns an owned buffer or NULL
 * on allocation failure. Caller must free. */
static char *build_send_payload(void){
    if(g_paste_len==0){
        size_t tl=g_input_len;
        char *s=malloc(tl? tl+1 : 1); if(!s) return NULL;
        memcpy(s,g_input,tl); s[tl]=0; return s;
    }
    size_t tl=g_input_len;
    size_t sep=(tl>0)?1:0;             /* '\n' inserted only when both parts */
    size_t total=tl+sep+g_paste_len;
    char *s=malloc(total+1); if(!s) return NULL;
    size_t p=0;
    if(tl>0){ memcpy(s+p,g_input,tl); p+=tl; }
    if(sep){ s[p++]='\n'; }
    memcpy(s+p,g_paste_buf,g_paste_len); p+=g_paste_len;
    s[p]=0; return s;
}

static void history_load(void){
    char path[1024]; if(puci_cli_get_history_file(path,sizeof path)!=0) return;
    FILE *f=fopen(path,"r"); if(!f) return;
    char line[2048];
    while(fgets(line,sizeof line,f) && g_hist_count<HIST_MAX){
        size_t n=strlen(line); while(n>0 && (line[n-1]=='\n'||line[n-1]=='\r')) line[--n]=0;
        if(n==0) continue;
        snprintf(g_hist[g_hist_count],2048,"%s",line);
        g_hist_count++;
    }
    fclose(f); g_hist_pos=g_hist_count;
}
static void history_save_line(const char *line){
    if(!line||!*line) return;
    // dedup consecutive
    if(g_hist_count>0 && strcmp(g_hist[g_hist_count-1],line)==0) return;
    char path[1024]; if(puci_cli_get_history_file(path,sizeof path)!=0) return;
    FILE *f=fopen(path,"a"); if(!f) return;
    fprintf(f,"%s\n",line); fclose(f);
    if(g_hist_count<HIST_MAX){ snprintf(g_hist[g_hist_count],2048,"%s",line); g_hist_count++; }
    else { // rotate
        for(int i=1;i<HIST_MAX;i++) memcpy(g_hist[i-1],g_hist[i],2048);
        snprintf(g_hist[HIST_MAX-1],2048,"%s",line);
    }
    g_hist_pos=g_hist_count;
}
static void history_prev(void){
    if(g_hist_count==0) return;
    if(g_hist_pos==g_hist_count){ snprintf(g_hist_tmp,2048,"%s",g_input); }
    if(g_hist_pos>0) g_hist_pos--;
    snprintf(g_input,2048,"%s",g_hist[g_hist_pos]);
    g_input_len=strlen(g_input); g_input_curs=g_input_len;
}
static void history_next(void){
    if(g_hist_pos>=g_hist_count){ g_input_len=0; g_input[0]=0; g_input_curs=0; return; }
    g_hist_pos++;
    if(g_hist_pos==g_hist_count){ snprintf(g_input,2048,"%s",g_hist_tmp); }
    else snprintf(g_input,2048,"%s",g_hist[g_hist_pos]);
    g_input_len=strlen(g_input); g_input_curs=g_input_len;
}


/* ── Model & Session pickers ── */
#define MAX_MODELS 512
static char g_model_buf[MAX_MODELS][128];
static const char *g_static_models[] = {
    "deepseek/deepseek-chat",
    "deepseek/deepseek-v3.2-exp",
    "deepseek/deepseek-r1",
    "anthropic/claude-3.5-sonnet",
    "anthropic/claude-3.5-haiku",
    "openai/gpt-4o-mini",
    "openai/gpt-4o",
    "openai/gpt-4.1-mini",
    "google/gemini-2.0-flash-001",
    "google/gemini-2.0-flash-lite-001",
    "meta-llama/llama-3.3-70b-instruct",
    "qwen/qwen-2.5-72b-instruct",
    NULL
};
static const char *k_models[MAX_MODELS+1];
static int k_model_count = 0;

static void models_init_static(void){
    if(k_model_count>0) return;
    int i=0;
    for(; g_static_models[i] && i<MAX_MODELS; i++){
        snprintf(g_model_buf[i], sizeof g_model_buf[i], "%.127s", g_static_models[i]);
        k_models[i] = g_model_buf[i];
    }
    k_model_count = i;
    k_models[k_model_count]=NULL;
}

/* --- minimal, robust JSON string-field extractor -----------------------
 * Finds the first occurrence of `"key"` bound followed by ':' and reads the
 * JSON string value, handling whitespace and standard \x escapes. Plain
 * strstr/strchr parsing in this file mis-handled "id"/"model" (whitespace,
 * escaped quotes, unrelated "id" fields), so a single correct helper is used
 * everywhere instead. Returns 1 on success, 0 if not found/not a string. */
static int json_str_field(const char *json, const char *key, char *out, size_t outcap){
    if(!json || !key || !out || outcap==0) return 0;
    size_t klen=strlen(key);
    const char *p=json;
    while(p && (p=strstr(p,"\""))){           /* every opening quote */
        p++;                                  /* points after the quote */
        if(p && strncmp(p, key, klen)==0 && p[klen]=='"'){
            const char *q=p+klen+1;           /* after closing " of the key */
            while(*q && (*q==' '||*q=='\t'||*q=='\n'||*q=='\r')) q++;
            if(*q!=':'){ p=p+klen; continue; }
            q++;
            while(*q && (*q==' '||*q=='\t'||*q=='\n'||*q=='\r')) q++;
            if(*q!='"'){ p=p+klen; continue; }
            q++;
            size_t w=0;
            while(*q && *q!='"'){
                char add;
                if(*q=='\\'){
                    q++;
                    if(*q=='"'||*q=='\\'||*q=='/'||*q=='b') add=*q;
                    else if(*q=='n') add='\n';
                    else if(*q=='t') add='\t';
                    else if(*q=='r') add='\r';
                    else if(*q=='f') add='\f';
                    else { /* unknown escape: emit the backslash and letter */
                        q--; add=*q;
                    }
                    if(*q) q++;
                } else {
                    add=*q;
                    q++;
                }
                if(w+1 < outcap){ out[w++]=add; }
            }
            out[w]=0;
            return 1; /* found; empty value is still a valid match */
        }
    }
    return 0;
}

/* Read a JSON string at *ctx (which must point at a '"'). On success writes
 * the decoded string to out and advances *ctx past the closing quote.
 * Returns 1 on success, 0 if this is not a string. */
static int json_read_string(const char **ctx, char *out, size_t outcap){
    const char *p=*ctx;
    if(!p || *p!='"') return 0;
    p++;
    size_t w=0;
    while(*p && *p!='"'){
        char add;
        if(*p=='\\' && p[1]){
            p++;
            if(*p=='"'||*p=='\\'||*p=='/'||*p=='b') add=*p;
            else if(*p=='n') add='\n';
            else if(*p=='t') add='\t';
            else if(*p=='r') add='\r';
            else if(*p=='f') add='\f';
            else add='\\'; /* unknown escape: drop the backslash */
            p++;
        } else {
            add=*p; p++;
        }
        if(w+1 < outcap) out[w++]=add;
    }
    if(*p!='"') return 0; /* unterminated */
    p++;
    out[w]=0;
    *ctx=p;
    return 1;
}

#if HAVE_CURL
#define CURL_MAX_BUF (4u*1024u*1024u)
struct MemBuf { char *data; size_t len; size_t cap; };
static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata){
    struct MemBuf *m=(struct MemBuf*)userdata;
    /* size*nmemb can overflow; compute it safely. */
    if(size && nmemb > SIZE_MAX/size) return 0;            /* overflow -> fail transfer */
    size_t n = size*nmemb;
    /* Refuse data beyond the hard cap: return 0, which curl maps to
       CURLE_WRITE_ERROR, cleanly aborting the transfer. */
    if(m->len > CURL_MAX_BUF || n > CURL_MAX_BUF - m->len) return 0;
    size_t need = m->len + n + 1;                          /* +1 for NUL */
    if(need >= SIZE_MAX) return 0;
    if(need > m->cap){
        size_t ncap = m->cap ? m->cap : 8192;
        while(ncap < need){
            if(ncap > SIZE_MAX/2){ ncap = need; break; }   /* guard ncap*=2 */
            ncap *= 2;
        }
        if(ncap > CURL_MAX_BUF) ncap = CURL_MAX_BUF;
        if(ncap < need) return 0;                          /* cap too small to satisfy */
        char *nd=(char*)realloc(m->data, ncap);
        if(!nd) return 0;
        m->data=nd; m->cap=ncap;
    }
    memcpy(m->data + m->len, ptr, n);
    m->len+=n; m->data[m->len]=0;
    return n;
}
/* Install a fetched model list into the picker globals (single-threaded,
 * only called from the render/main thread). */
static void models_install(char dst[][128], int cnt){
    if(cnt<=0) return;
    if((size_t)cnt > MAX_MODELS) cnt=MAX_MODELS;
    for(int i=0;i<cnt;i++){
        snprintf(g_model_buf[i], sizeof g_model_buf[i], "%.127s", dst[i]);
        k_models[i]=g_model_buf[i];
    }
    k_model_count=cnt;
    k_models[k_model_count]=NULL;
}

/* Perform the HTTP model fetch, writing up to `cap` model ids into `out`.
 * Returns the number of ids found. Also updates `err` to 1 on any failure so
 * callers can decide whether to surface an error. */
static int fetch_models_into(char out[][128], int cap, int *err){
    *err=0;
    if(cap<=0) return 0;
#if !HAVE_CURL
    return 0;
#else
    puci_provider_config *pc = puci_cli_provider("openai");
    const char *base = pc ? pc->base_url : NULL;
    const char *key = pc ? pc->api_key : NULL;
    if(!base || !*base) base="https://openrouter.ai/api/v1";
    if(strstr(base, "anthropic.com")) return 0;
    char url[1024];
    size_t blen=strlen(base);
    while(blen>0 && base[blen-1]=='/') blen--;
    snprintf(url, sizeof url, "%.*s/models", (int)blen, base);
    CURL *curl=curl_easy_init();
    if(!curl) return 0;
    struct MemBuf mem={NULL,0,0};
    mem.data=(char*)malloc(8192); if(!mem.data){ curl_easy_cleanup(curl); return 0; }
    mem.cap=8192; mem.data[0]=0;
    struct curl_slist *hdrs=NULL;
    hdrs=curl_slist_append(hdrs, "Accept: application/json");
    hdrs=curl_slist_append(hdrs, "Content-Type: application/json");
    if(key && *key){
        char auth[2600];
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
        hdrs=curl_slist_append(hdrs, auth);
        if(strstr(base,"openrouter")){
            hdrs=curl_slist_append(hdrs, "HTTP-Referer: https://github.com/uzunenes/puci");
            hdrs=curl_slist_append(hdrs, "X-Title: PUCI");
        }
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "puci/0.5.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode rc=curl_easy_perform(curl);
    long code=0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if(rc!=CURLE_OK || code!=200 || !mem.data || mem.len==0){
        free(mem.data); return 0;
    }
    const char *data_start=strstr(mem.data, "\"data\"");
    const char *p = data_start ? data_start : mem.data;
    int tmp_cnt=0;
    /* Collect every "id" string value robustly (whitespace, escapes). */
    while(tmp_cnt<cap){
        p=strstr(p,"\"id\"");
        if(!p) break;
        const char *q=p+4;                       /* after the key's "id" */
        while(*q && (*q==' '||*q=='\t'||*q=='\n'||*q=='\r')) q++;
        if(*q!=':'){ p+=4; continue; }
        q++;
        while(*q && (*q==' '||*q=='\t'||*q=='\n'||*q=='\r')) q++;
        const char *vq=q;
        char id[128];
        if(json_read_string(&vq, id, sizeof id)){
            int dup=0;
            for(int i=0;i<tmp_cnt;i++) if(strcmp(out[i],id)==0) dup=1;
            if(!dup){
                snprintf(out[tmp_cnt], 128, "%.127s", id);
                tmp_cnt++;
            }
            p=vq;
        } else {
            p=q;
        }
    }
    free(mem.data);
    *err= (tmp_cnt==0 ? 0 : 1);
    return tmp_cnt;
#endif
}

/* Detached worker thread for the model fetch.
 *
 * Synchronization contract (single shared result buffer, no lock):
 *
 *   g_live          : non-atomic byte buffer; written ONLY by the producer
 *                     thread. The main thread reads it only after it observes
 *                     g_live_done == 1 (via the acquire load below), so there
 *                     is never a data race on the buffer.
 *   g_live_cnt      : _Atomic int; number of valid entries in g_live.
 *   g_live_done     : _Atomic int; 1 once a fetch has finished publishing.
 *   g_live_inflight : _Atomic int; 1 while a fetch may be in progress.
 *
 * Producer (happens-before):
 *   1. Fully populate g_live[0..n-1].
 *   2. PUBLISH g_live_cnt = n with a release store.
 *   3. PUBLISH g_live_done = 1 with a release store.
 *   The release stores order every preceding write (the buffer and the count)
 *   before the completion flag in the happens-before relation.
 *
 * Consumer (pump_models_fetch):
 *   1. ACCEPT g_live_done with an acquire load. If it reads 1, it
 *      synchronizes-with the producer's release store, so the buffer and the
 *      count writes are visible before this point.
 *   2. Only then read g_live_cnt (acquire) and g_live[0..cnt-1].
 *
 * Ownership / liveness guards:
 *   - A new fetch is launched only when g_live_inflight==0, and it re-arms
 *     g_live_done=0 only after the previous result was pumped, so an older
 *     generation can never overwrite a newer one and the result buffer is
 *     never mutated while the UI is still reading it.
 *   - The worker is detached and never touches g_live again after publishing
 *     done; the buffer is a static, never freed, so teardown cannot free
 *     storage while a worker thread may still access it.
 */
static char g_live[MAX_MODELS][128];
static _Atomic int g_live_cnt = 0;
static _Atomic int g_live_inflight = 0;
static _Atomic int g_live_done     = 0;
static void *models_fetch_thread(void *arg){
    (void)arg;
    int e=0;
    int n = fetch_models_into(g_live, MAX_MODELS, &e);
    /* Producer publish: buffer then count then done, all release-ordered. */
    atomic_store_explicit(&g_live_cnt, n, memory_order_release);
    atomic_store_explicit(&g_live_done, 1, memory_order_release);
    atomic_store_explicit(&g_live_inflight, 0, memory_order_relaxed);
    return NULL;
}

/* Launch an async model fetch. Returns immediately; result is merged via
 * pump_models_fetch() on a later frame. Never blocks the event loop. */
static void begin_models_fetch(void){
    if(atomic_load(&g_live_inflight)) return;   /* already fetching */
    atomic_store(&g_live_cnt, 0);
    atomic_store(&g_live_done, 0);
    atomic_store(&g_live_inflight, 1);
    pthread_t th;
    if(pthread_create(&th, NULL, models_fetch_thread, NULL)==0){
        pthread_detach(th);
    } else {
        /* No threading available: fall back to synchronous so the picker
           still gets populated (the freeze is a lesser evil than empty list). */
        atomic_store(&g_live_done, 0);
        atomic_store(&g_live_inflight, 0);
        int e=0; int n=fetch_models_into(g_live, MAX_MODELS, &e);
        if(n>0) models_install(g_live, n);
    }
}

/* Query entry point used by the picker. Launches a background fetch so keying
 * 'm' never blocks the TUI (previously curl_easy_perform froze it up to 6s). */
static void rebuild_pick_filter(void);   /* defined below; used by pump */
static int g_pick_sel;                    /* tentative definitions (real defs below) */
static int g_pick_filtered_count;
static int g_mode;

/* Called every frame from the renderer: if a background fetch finished,
 * install the fresh list into the picker globals. */
static int pump_models_fetch(void){
    /* Acquire: establishes happens-before with the producer's release store
       of g_live_done, making g_live and g_live_cnt reads below safe. */
    if(atomic_load_explicit(&g_live_done, memory_order_acquire)!=1) return 0;
    int cnt = atomic_load_explicit(&g_live_cnt, memory_order_acquire);
    if(cnt>0){
        models_install(g_live, cnt);
        /* The async list replaced k_models; refresh the picker's filtered
           view so fetched entries become visible and the selection stays in
           range (Phase 5: async results update safely, selection on-screen). */
        if(g_mode==3){
            rebuild_pick_filter();
            if(g_pick_sel >= g_pick_filtered_count)
                g_pick_sel = g_pick_filtered_count>0 ? g_pick_filtered_count-1 : 0;
        }
    }
    atomic_store_explicit(&g_live_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_live_done, 0, memory_order_relaxed);
    /* Indicate whether anything changed so the caller can schedule a repaint. */
    return 1;
}

/* Query entry point used by the picker. Launches a background fetch so keying
 * 'm' never blocks the TUI (previously curl_easy_perform froze it up to 6s). */
static int fetch_live_models(void){ begin_models_fetch(); return 0; }

#else
static int fetch_live_models(void){ return 0; }
#endif
#define MAX_SESS_LIST 64
typedef struct { char id[128]; char model[64]; char preview[96]; time_t mtime; } sess_entry_t;
static sess_entry_t g_sess_list[MAX_SESS_LIST];
static int g_sess_count=0;
static int g_mode=0; //0 normal,1 input,2 sess_pick,3 model_pick,4 approval handled via flag
static int g_pick_sel=0;
#define PICK_FILTER_MAX 64
static char g_pick_filter[PICK_FILTER_MAX]="";
static int g_pick_filtered_idx[MAX_MODELS];
static int g_pick_filtered_count=0;

static int strcasestr_contains(const char *hay, const char *needle){
    if(!needle||!*needle) return 1;
    if(!hay) return 0;
    size_t nl=strlen(needle), hl=strlen(hay);
    if(nl>hl) return 0;
    for(size_t i=0;i+nl<=hl;i++){
        size_t k=0;
        for(;k<nl;k++){
            char a=hay[i+k], b=needle[k];
            if(a>='A'&&a<='Z') a=(char)(a+32);
            if(b>='A'&&b<='Z') b=(char)(b+32);
            if(a!=b) break;
        }
        if(k==nl) return 1;
    }
    return 0;
}
static void rebuild_pick_filter(void){
    g_pick_filtered_count=0;
    if(g_mode==2){
        for(int i=0;i<g_sess_count;i++){
            if(g_pick_filtered_count >= (int)(sizeof g_pick_filtered_idx/sizeof g_pick_filtered_idx[0])) break;
            if(strcasestr_contains(g_sess_list[i].id, g_pick_filter) || strcasestr_contains(g_sess_list[i].model, g_pick_filter)){
                g_pick_filtered_idx[g_pick_filtered_count++]=i;
            }
        }
    } else if(g_mode==3){
        for(int i=0;k_models[i];i++){
            if(g_pick_filtered_count >= (int)(sizeof g_pick_filtered_idx/sizeof g_pick_filtered_idx[0])) break;
            if(strcasestr_contains(k_models[i], g_pick_filter)){
                g_pick_filtered_idx[g_pick_filtered_count++]=i;
            }
        }
    }
    if(g_pick_sel >= g_pick_filtered_count) g_pick_sel = g_pick_filtered_count>0 ? g_pick_filtered_count-1 : 0;
    if(g_pick_sel <0) g_pick_sel=0;
}
static void picker_open(int mode){
    g_mode=mode;
    g_pick_filter[0]=0; g_pick_sel=0;
    rebuild_pick_filter();
    if(mode==2) g_show_sidebar=1;
}
/* Apply a model selection to every session pane. Works even for panes that are
 * currently streaming: set_model only updates the pending request config, the
 * next request uses it. Returns PUCI_OK when all panes accepted; otherwise
 * the first failing status so the caller can surface the error. */
static puci_status apply_model_to_panes(const char *model){
    if(!model) return PUCI_ERR_INVALID_ARG;
    puci_status first_err=PUCI_OK;
    int have_pane=0;
    for(int i=0;i<g_npane && i<MAX_PANES;i++){
        if(!g_panes[i].sess) continue;
        have_pane=1;
        puci_status st = puci_session_set_model(g_panes[i].sess, model);
        if(st!=PUCI_OK && first_err==PUCI_OK) first_err=st;
    }
    if(!have_pane) return PUCI_OK;
    return first_err;
}
/* Safe clipboard: see clip.h. Mirrors are non-fatal; failures surface as a
 * transient toast in the TUI so a silently dropped copy is never reported. */
static int copy_to_clipboard(const char *text, size_t len){
    if(!text) return 1;
    if(!len) return 1;
    return clip_copy(text,len);
}


static void load_sessions(void){
    char dir[1024]; if(puci_cli_get_sessions_dir(dir,sizeof dir)!=0){ g_sess_count=0; return; }
    DIR *d=opendir(dir); if(!d){ g_sess_count=0; return; }  /* clear stale list */
    // Collect into temp large buffer then sort and truncate to MAX_SESS_LIST
    #define LOAD_TMP_CAP 512
    sess_entry_t tmp_list[LOAD_TMP_CAP]; int tmp_cnt=0;
    struct dirent *ent;
    while((ent=readdir(d))!=NULL && tmp_cnt<LOAD_TMP_CAP){
        size_t len=strlen(ent->d_name); if(len<6 || strcmp(ent->d_name+len-5,".json")!=0) continue;
        char full[2048]; snprintf(full,sizeof full,"%s/%s",dir,ent->d_name);
        struct stat st; if(stat(full,&st)!=0) continue;
        sess_entry_t *e=&tmp_list[tmp_cnt];
        snprintf(e->id,sizeof e->id,"%.*s",(int)(len-5),ent->d_name);
        e->mtime=st.st_mtime;
        // peek model/preview (defaults are set even when the file cannot be read)
        FILE *f=fopen(full,"r"); e->model[0]=0; e->preview[0]=0;
        if(f){
            char buf[4096]; size_t rd=fread(buf,1,sizeof buf-1,f); buf[rd]=0; fclose(f);
            if(json_str_field(buf,"model", e->model, sizeof e->model)) { /* ok */ }
            if(json_str_field(buf,"content", e->preview, sizeof e->preview)) { /* ok */ }
        }
        if(!e->model[0]) snprintf(e->model,sizeof e->model,"-");
        if(!e->preview[0]) snprintf(e->preview,sizeof e->preview,"(empty)");
        tmp_cnt++;
    }
    closedir(d);
    for(int i=0;i<tmp_cnt;i++) for(int j=i+1;j<tmp_cnt;j++) if(tmp_list[j].mtime > tmp_list[i].mtime){ sess_entry_t tmp=tmp_list[i]; tmp_list[i]=tmp_list[j]; tmp_list[j]=tmp; }
    g_sess_count = tmp_cnt < MAX_SESS_LIST ? tmp_cnt : MAX_SESS_LIST;
    for(int i=0;i<g_sess_count;i++) g_sess_list[i]=tmp_list[i];
}

/* Build the overlay diff text from an approval event.  Returns 1 on success.
 * Purely prepares display state; it never blocks the caller. */
static int tui_open_approval(int pane_idx, const puci_event_approval *req){
    if(!req) return 0;
    /* Show the *real* change first. A unified diff is the ground truth the
     * user must approve; display/args are only a fallback when there is no
     * diff to show. */
    size_t pos=0;
    const char *head = req->tool ? req->tool : "tool";
    size_t hl=strlen(head); if(pos+hl+1 < sizeof g_approval_diff){ memcpy(g_approval_diff+pos, head, hl); pos+=hl; g_approval_diff[pos++]='\n'; }
    pos = app_append(g_approval_diff, sizeof g_approval_diff, pos, req->diff);
    if(!req->diff) pos = app_append(g_approval_diff, sizeof g_approval_diff, pos, req->display?req->display:req->args_json);
    g_approval_diff[pos]=0;
    /* Explicit marker when a long diff is truncated by the fixed buffer, so
       the user is never silently approving a partial change.  Only append if
       there is headroom; otherwise overwrite the tail in place. */
    if(req->diff){
        size_t l = strlen(req->diff);
        size_t avail = sizeof g_approval_diff - 1;
        if(l > avail){
            static const char mark[] = "\n[+ .. truncated ..]\n";
            size_t mark_len = sizeof mark - 1;
            size_t keep = avail > mark_len ? avail - mark_len : 0;
            memcpy(g_approval_diff + keep, mark, mark_len);
            pos = keep + mark_len;
            g_approval_diff[pos]=0;
        }
    }
    /* upper bound for the j/k scroll region = number of lines */
    g_hunk_max=1;
    for(size_t i=0;i<pos;i++) if(g_approval_diff[i]=='\n') g_hunk_max++;
    if(g_hunk_max<1) g_hunk_max=1;
    g_hunk_off=0; g_approval_pending=1; g_approval_pane=pane_idx;
    g_approval_id=req->id; g_approval_feedback_mode=0;
    g_approval_feedback[0]=0; g_approval_feedback_len=0;
    extern void tui_render_frame(void);
    tui_render_frame();
    return 1;
}

// Lightweight inline markdown: **bold**, *italic*, `code`, # heading, - list, > quote
static int utf8_seq_width(const unsigned char *s); /* defined below; used here */
static void tb_print_styled(int x,int y, uintattr_t base_fg, uintattr_t base_bg, const char *s, int max_w);
static void tb_print_styled(int x,int y, uintattr_t base_fg, uintattr_t base_bg, const char *s, int max_w){
    if(!s) return;
    if(max_w<=0) return;
    int col=x;
    int bold=0, code=0;
    // heading detection at line start
    if(s[0]=='#' && (s[1]==' '||s[1]=='#')){
        base_fg = TB_YELLOW|TB_BOLD; // heading yellow
        int skip=0; while(s[skip]=='#') skip++; if(s[skip]==' ') skip++;
        s += skip;
    } else if(s[0]=='>' && s[1]==' '){
        base_fg = TB_CYAN;
        if(col < x+max_w){ tb_set_cell(col,y, 0x2502, TB_CYAN, base_bg); col++; }
        s+=2;
    } else if((s[0]=='-'||s[0]=='*') && s[1]==' '){
        base_fg = TB_WHITE;
        if(col < x+max_w){ tb_set_cell(col,y, 0x2022, base_fg, base_bg); col++; tb_set_cell(col,y,' ', base_fg, base_bg); col++; }
        s+=2;
    }
    /* Emit one UTF-8 code point into the cell buffer, advancing `col` by
       its display width so multi-byte text never misaligns the cursor. */
    while(*s && col < x+max_w){
        if(!code && s[0]=='`'){ code=!code; s+=1; continue; }
        if(!code && s[0]=='*' && s[1]=='*'){ bold=!bold; s+=2; continue; }
        if(!code && s[0]=='['){
            const char *end=strchr(s, ']');
            const char *par=end?strchr(end,'('):NULL;
            const char *par2=par?strchr(par,')'):NULL;
            if(end && par==end+1 && par2){
                const char *t=s+1;
                while(t<end && col<x+max_w){
                    uint32_t cp; int bl2=tb_utf8_char_to_unicode(&cp,t);
                    if(bl2<=0){ bl2=1; cp=(uint32_t)(unsigned char)t[0]; }
                    int w2=utf8_seq_width((const unsigned char*)t);
                    if(col+w2 > x+max_w) break;
                    tb_set_cell(col,y,cp, base_fg|TB_UNDERLINE, base_bg);
                    col+=w2; t+=(size_t)bl2;
                }
                s=par2+1;
                continue;
            }
        }
        uint32_t cp; int bl=tb_utf8_char_to_unicode(&cp, s);
        if(bl<=0){ bl=1; cp=(uint32_t)(unsigned char)s[0]; }
        int w=utf8_seq_width((const unsigned char*)s);
        if(col+w > x+max_w) break;
        uintattr_t fg=base_fg, bg=base_bg;
        if(bold) fg|=TB_BOLD;
        if(code){ fg=TB_BLACK; bg=TB_WHITE; }
        /* Wide glyphs occupy two cells; termbox fills the trailing cell. */
        tb_set_cell(col,y,cp, fg, bg);
        col+=w; s+=(size_t)bl;
    }
    // fill remainder
    for(int c=col;c<x+max_w;c++) tb_set_cell(c,y,' ', base_fg, base_bg);
}
/* Syntax-highlighted printer: runs the semantic line lexer (hl_line) over
 * `s` and emits each token span with a theme color. Falls back to plain
 * base_fg if the language is NONE or the lexer yields only one TEXT span.
 * Used only inside recognised code fences, where wrapping is already applied
 * by the caller, so the whole (already width-clamped) `s` is consumed. */
static void tb_print_hl(int x,int y,int max_w,HlLang lang,const char *s,uintattr_t base_fg,uintattr_t base_bg){
    if(!s || max_w<=0) return;
    HlSpan sp[HL_MAX_SPANS];
    int n=hl_line(lang, s, strlen(s), sp, HL_MAX_SPANS);
    if(n<=0){ tb_print_styled(x,y,base_fg,base_bg,s,max_w); return; }
    int col=x;
    for(int i=0;i<n && col<x+max_w;i++){
        size_t off=sp[i].off, ln=sp[i].len;
        if(off+ln > (size_t)strlen(s)) ln=(size_t)strlen(s)-off;
        if((int)off>=max_w) break;
        uintattr_t fg=base_fg;
        switch(sp[i].kind){
            case HL_TK_KEYWORD: fg=TB_CYAN|TB_BOLD; break;
            case HL_TK_STRING:  fg=TB_GREEN; break;
            case HL_TK_NUMBER:  fg=TB_MAGENTA; break;
            case HL_TK_COMMENT: fg=TB_GREEN|TB_DIM; break;
            case HL_TK_BOOL:    fg=TB_YELLOW; break;
            case HL_TK_HEADING: fg=TB_YELLOW|TB_BOLD; break;
            case HL_TK_DIFF_ADD:fg=TB_GREEN; break;
            case HL_TK_DIFF_DEL:fg=TB_RED; break;
            case HL_TK_DIFF_HUNK:fg=TB_CYAN; break;
            case HL_TK_MD_LINK: fg=TB_BLUE|TB_UNDERLINE; break;
            default: break;
        }
        const char *p=s+off;
        while(ln>0 && col<x+max_w){
            uint32_t cp; int bl=tb_utf8_char_to_unicode(&cp,p);
            if(bl<=0){ bl=1; cp=(uint32_t)(unsigned char)p[0]; }
            int w=utf8_seq_width((const unsigned char*)p);
            if(col+w > x+max_w) break;
            tb_set_cell(col,y,cp,fg,base_bg); col+=w;
            p+=(size_t)bl; ln-= (size_t)bl;
        }
    }
    /* the hl_line spans may not cover the tail (e.g. trailing space) */
    size_t consumed=0;
    for(int i=0;i<n;i++){ size_t e=sp[i].off+sp[i].len; if(e>consumed) consumed=e; }
    if(consumed < strlen(s) && col<x+max_w){
        const char *ch=s+consumed;
        while(col<x+max_w && *ch){
            uint32_t cp; int bl=tb_utf8_char_to_unicode(&cp,ch);
            if(bl<=0){ bl=1; cp=(uint32_t)(unsigned char)ch[0]; }
            int w=utf8_seq_width((const unsigned char*)ch);
            if(col+w > x+max_w) break;
            tb_set_cell(col,y,cp,base_fg,base_bg); col+=w; ch+=(size_t)bl;
        }
    }
    for(int c=col;c<x+max_w && c<x+max_w;c++) tb_set_cell(c,y,' ',base_fg,base_bg);
}
/* Recognize a Markdown code-fence line. Returns:
 *   -1 → not a fence
 *    0 → a closing fence (``` with no language)
 *    1 → an opening fence; *out_lang set to the normalized language */
static int md_fence_detect(const char *line, HlLang *out_lang){
    const char *p=line; while(*p==' ') p++;
    int ticks=0; while(p[ticks]=='`') ticks++;
    if(ticks<3) return -1;
    const char *rest=p+ticks;
    while(*rest==' ' || *rest=='\t') rest++;
    /* everything after must be a single language token, else treat as fence
       with an unknown/blank language (close or open with NONE) */
    int non_space=0;
    for(const char *q=rest;*q;q++) if(*q!=' ' && *q!='\t' && *q!='\r' && *q!='\n') { non_space++; break; }
    if(!non_space) return 0; /* closing */;
    /* opener: normalize the token after ``` (up to first space) */
    char tag[CTX_LANG_MAX]; size_t ti=0;
    while(*rest && *rest!=' ' && *rest!='\t' && ti+1<sizeof tag) tag[ti++]=(*rest++);
    tag[ti]=0;
    char alias[CTX_LANG_MAX];
    HlLang lang=md_lang_normalize(tag, alias, sizeof alias);
    if(out_lang) *out_lang=lang;
    return 1;
}
static void tb_draw_box(int r,int c,int h,int w,const char *title,int focused){
    uintattr_t fg = focused ? TB_CYAN|TB_BOLD : TB_WHITE;
    uintattr_t bg = TB_DEFAULT;
    tb_set_cell(c, r, 0x250C, fg, bg);
    for(int x=1;x<w-1;x++) tb_set_cell(c+x, r, 0x2500, fg, bg);
    tb_set_cell(c+w-1, r, 0x2510, fg, bg);
    if(title){
        char t[96]; snprintf(t,sizeof t," %.*s ", (int)(w-4), title);
        tb_print(c+2, r, fg, bg, t);
    }
    for(int y=1;y<h-1;y++){
        tb_set_cell(c, r+y, 0x2502, fg, bg);
        tb_set_cell(c+w-1, r+y, 0x2502, fg, bg);
    }
    tb_set_cell(c, r+h-1, 0x2514, fg, bg);
    for(int x=1;x<w-1;x++) tb_set_cell(c+x, r+h-1, 0x2500, fg, bg);
    tb_set_cell(c+w-1, r+h-1, 0x2518, fg, bg);
}
/* ---- UTF-8 width helpers ---------------------------------------------
 * All width math in the TUI must go through these so multi-byte UTF-8
 * (Turkish chars, emoji, combining marks) is measured in display columns,
 * never in bytes via strlen() alone. */

/* Display width of the first UTF-8 code point at s. Wide chars count 2;
 * combining/zero-width count 0. Termbox's tb_wcwidth returns 0/1/2 (or -1
 * clamped to 1) and tb_utf8_char_length gives the byte length. */
static int uchar_width(const char *s){
    static int warned=0;
    (void)warned;
    uint32_t cp=0;
    int bl = tb_utf8_char_to_unicode(&cp, s);
    if(bl<=0) return 1;
    int w = tb_wcwidth(cp);
    if(w<0)     return 1; /* control/unknown: treat as one cell */
    if(w>2)     return 2; /* emoji sequences are folded to 2 columns */
    return w;
}

/* Width (in display columns) of a whole UTF-8 string. */
static int uwidth(const char *s){
    if(!s) return 0;
    int col=0;
    while(*s){
        int w=uchar_width(s);
        s += tb_utf8_char_length(*s);
        col += w;
    }
    return col;
}

/* Advance s forward by whole code points until the written width would
 * exceed max_cols. Returns the number of BYTES consumed (so callers can
 * slice s[start..start+ret] into a visual row of <= max_cols columns).
 * Never splits a multi-byte sequence. */
static size_t uwidth_advance(const char *s, int max_cols){
    size_t nbytes=0;
    int col=0;
    while(*s && *s != '\n'){
        int w=uchar_width(s);
        if(col + w > max_cols) break;
        col += w;
        int bl=tb_utf8_char_length((char)*s);
        s += bl; nbytes += (size_t)bl;
    }
    return nbytes;
}

/* Number of bytes in a UTF-8 sequence given its lead byte, or 1 for
 * ASCII/continuation/lead bytes that are incomplete (validated by caller). */
static int utf8_seq_len(unsigned char lead){
    if(lead < 0x80) return 1;
    if((lead & 0xE0) == 0xC0) return 2;
    if((lead & 0xF0) == 0xE0) return 3;
    if((lead & 0xF8) == 0xF0) return 4;
    return 1; /* stale continuation byte (0x80..0xBF): skip it */
}

/* Validate a raw multi-byte sequence (no overlongs, no surrogates, up to
 * U+10FFFF). Returns 1 when `len` bytes at `s` form a valid code point and
 * all continuation bytes are well-formed. */
static int utf8_valid(const unsigned char *s, size_t len){
    if(len==1) return 1; /* ASCII */
    if(len==2){
        if(!(s[0]>=0xC2 && s[0]<=0xDF)) return 0;
    } else if(len==3){
        unsigned char a=s[0];
        if(a==0xE0){ if(!(s[1]>=0xA0 && s[1]<=0xBF)) return 0; }
        else if(a==0xED){ if(!(s[1]>=0x80 && s[1]<=0x9F)) return 0; }
        else if(a>=0xE1 && a<=0xEF){ if(!(s[1]>=0x80 && s[1]<=0xBF)) return 0; }
        else return 0;
    } else if(len==4){
        unsigned char a=s[0];
        if(a==0xF0){ if(!(s[1]>=0x90 && s[1]<=0xBF)) return 0; }
        else if(a>=0xF1 && a<=0xF3){ if(!(s[1]>=0x80 && s[1]<=0xBF)) return 0; }
        else if(a==0xF4){ if(!(s[1]>=0x80 && s[1]<=0x8F)) return 0; }
        else return 0;
    } else return 0;
    for(size_t i=1;i<len;i++) if((s[i]&0xC0)!=0x80) return 0;
    return 1;
}

/* Display column width of the first code point at s (for cursor motion). */
static int utf8_seq_width(const unsigned char *s){
    uint32_t cp=0;
    if(tb_utf8_char_to_unicode(&cp, (const char*)s)<=0) return 1;
    int w=tb_wcwidth(cp);
    if(w<=0) return 1;
    if(w>2) return 2;
    return w;
}

/* Byte length of the UTF-8 character that ENDS at index `end` (exclusive),
 * i.e. where the previous character begins. Used for backspace-by-char. */
static int utf8_prev_len(const unsigned char *s, size_t end){
    if(end==0) return 1;
    const unsigned char *p = s + end - 1;
    int lead=1;
    for(int i=0;i<3 && (size_t)i<end;i++){
        if((p[-i]&0xC0)!=0x80){ lead = i+1; break; }
    }
    return lead;
}

/* Byte length of the next visual row starting at s (fits `width` columns,
 * UTF-8 safe, prefers a trailing word boundary past the halfway mark).
 * Never splits a multi-byte sequence; returns >=1 when *s != 0. */
static size_t render_segment(const char *s, int width){
    size_t cut = uwidth_advance(s, width);
    if(cut==0) cut = 1;
    const char *q=s;
    size_t scan_col=0;
    size_t space_bytes=0;
    while(*q && *q!='\n'){
        int w=uchar_width(q);
        if((int)scan_col + w > width) break;
        scan_col += (size_t)w;
        if(*q==' '){ space_bytes = (size_t)(q - s) + 1; }
        q += tb_utf8_char_length(*q);
    }
    if(space_bytes && space_bytes*2 > (size_t)width){ cut = space_bytes; }
    return cut;
}

/* Visually wrap a logical line: walk UTF-8 code points up to `width`
 * columns per visual row. Mirrors the render slicing in the render loop. */
static int wrap_visual_count(const char *s, int width){
    if(width<=0) return 1;
    if(!s||!*s) return 1;
    int n=0; size_t pos=0; size_t len=strlen(s);
    while(pos < len){
        if(uwidth(s+pos) <= width){ n++; break; }
        size_t cut = render_segment(s+pos, width);
        n++; pos+=cut;
    }
    return n;
}

/* ── Semantic helpers for the conversation-first layout ──────────── */

/* Human-readable session status for the header, derived from data the TUI
   already tracks (no new library queries). */
static const char *tui_status_str(void){
    for(int i=0;i<g_npane && i<MAX_PANES;i++){
        if(i==g_focus && g_approval_pending && g_approval_pane==i) return "Approval";
        if(g_panes[i].streaming) return "Generating";
    }
    return "Ready";
}

static uintattr_t tui_status_color(void){
    for(int i=0;i<g_npane && i<MAX_PANES;i++) if(g_panes[i].streaming) return g_theme.warning;
    if(g_approval_pending) return g_theme.warning;
    return g_theme.success;
}

/* Render one pane's ring into rect (x,y)-(x+w-1,y+h-1) with a visual scroll
   indicator. Returns nothing; mutates only termbox cells. Semantic: block
   labels (YOU / PUCI / system dashes / tool / error) draw on the first
   visual row of each logical line. */
/* Render one pane's ring into rect (x,y)-(x+w-1,y+h-1) with a visual scroll
   indicator. Returns nothing; mutates only termbox cells. Semantic: block
   labels (YOU / PUCI / system dashes / tool / error) draw on the first
   visual row of each logical line.

   Scrollbar contract: when the pane overflows its viewport a dedicated
   right gutter column is reserved exclusively for the scrollbar track/thumb,
   and content is wrapped to exclude that gutter so a wide char or word is
   never overwritten by the scrollbar. The wrap cache is keyed on the content
   width AND scrollbar visibility so toggling the track invalidates it. */
static void layout_check(int x,int y,int w,int h,int rows,int cols,const char *what){
    int bad = (x<0) || (y<0) || (w<1) || (h<1) || (x+w>cols) || (y+h>rows) ? 1 : 0;
    if(bad){
        TRACE(TR_LAYOUT, (uint32_t)(((uint32_t)x&0xFF) | (((uint32_t)y)&0xFF)<<8 |
                                    (((uint32_t)w)&0xFF)<<16 | (((uint32_t)h)&0xFF)<<24));
        (void)what;
    }
}

static void tui_render_pane(Pane *p, int x, int y, int h, int w, int focused){
    int inner_h = h-2;
    int full_w = w-2;                 /* content region between box borders */
    if(inner_h<=0 || full_w<1) return;

    /* Is the pane scrollable? That decides whether we reserve a gutter. */
    int overflows;
    if(!p->layout_dirty && p->cached_inner_w==full_w && p->cached_scrollable==0 && p->cached_total_vis>=0){
        overflows = p->cached_total_vis > inner_h;
    } else {
        int tv=0;
        for(int k=0;k<p->ring.count;k++){
            int idx=(p->ring.head + k)%RING_LINES;
            tv += wrap_visual_count(p->ring.lines[idx], full_w);
        }
        overflows = tv > inner_h;
    }
    int scrollable = overflows ? 1 : 0;
    int inner_w = scrollable ? full_w-1 : full_w;  /* gutter reserved only when visible */
    if(inner_w<1) inner_w=1;

    int total_vis;
    if(!p->layout_dirty && p->cached_inner_w==inner_w && p->cached_scrollable==scrollable && p->cached_total_vis>=0){
        total_vis=p->cached_total_vis;
    } else {
        total_vis=0;
        for(int k=0;k<p->ring.count;k++){
            int idx=(p->ring.head + k)%RING_LINES;
            total_vis += wrap_visual_count(p->ring.lines[idx], inner_w);
        }
        p->cached_total_vis=total_vis;
        p->cached_inner_w=inner_w;
        p->cached_scrollable=scrollable;
        p->layout_dirty=0;
    }
    if(p->scroll_off > total_vis) p->scroll_off = total_vis;
    if(p->scroll_off <0) p->scroll_off=0;
    int start_vis = total_vis - inner_h - p->scroll_off;
    if(start_vis<0) start_vis=0;
    int cur_vis=0, rendered=0;
    /* Code-fence state carried across the contiguous assistant block so syntax
       highlighting applies to all rows of a recognized fence. */
    int blk_fence=0;
    HlLang blk_lang=HL_LANG_NONE;
    for(int k=0;k<p->ring.count && rendered<inner_h;k++){
        int idx=(p->ring.head + k)%RING_LINES;
        const char *s=p->ring.lines[idx];
        rtype_t t=p->ring.types[idx];
        int wrapped = wrap_visual_count(s, inner_w);
        /* Maintain code-fence state across the contiguous assistant block. */
        if(t!=RL_ASSISTANT){ blk_fence=0; blk_lang=HL_LANG_NONE; }
        else {
            HlLang fl=HL_LANG_NONE;
            int fr=md_fence_detect(s,&fl);
            if(fr==0){ blk_fence=0; blk_lang=HL_LANG_NONE; }
            else if(fr==1){ blk_fence=1; blk_lang=fl; }
        }
        int row_in_fence = blk_fence;
        int seg_off=0;
        for(int v=0; v<wrapped && rendered<inner_h; v++){
            if(cur_vis < start_vis){ cur_vis++;
                int rem2=(int)strlen(s)-seg_off;
                if(rem2>0){ int take2=(int)render_segment(s+seg_off, inner_w); if(take2<1||take2>rem2) take2=rem2; seg_off+=take2; }
                continue;
            }
            int row = y+1+rendered;
            int rem=(int)(strlen(s)-seg_off);
            if(rem<=0){
                for(int cc=x+1; cc<x+1+inner_w && cc<x+w-1; cc++) tb_set_cell(cc,row,' ',TB_DEFAULT,TB_DEFAULT);
                rendered++; cur_vis++;
                break;
            }
            int take;
            if(uwidth(s+seg_off) <= inner_w){ take=rem; }
            else { take=(int)render_segment(s+seg_off, inner_w); if(take<1||take>rem) take=rem; }
            char seg[RING_COLS]; memcpy(seg,s+seg_off,(size_t)take); seg[take]=0;
            seg_off+=take;
            uintattr_t fg=TB_WHITE;
            if(t==RL_USER) fg=g_theme.user;
            else if(t==RL_TOOL) fg=g_theme.tool;
            else if(t==RL_ERROR) fg=g_theme.error;
            if(v==0 && (t==RL_USER || t==RL_SYSTEM || t==RL_ERROR)){
                char lab[24];
                if(t==RL_USER) snprintf(lab,sizeof lab,"YOU  ");
                else if(t==RL_ERROR) snprintf(lab,sizeof lab,"ERR  ");
                else snprintf(lab,sizeof lab,"      ");
                tb_print(x+1, row, fg, TB_DEFAULT, lab);
                tb_print_styled(x+1+(int)strlen(lab)+0, row, fg, TB_DEFAULT, seg, inner_w);
            } else if(v==0 && t==RL_ASSISTANT){
                /* Assistant role heading, rendered exactly once per message:
                   on the first visual row of the first logical line of an
                   assistant block (contiguous RL_ASSISTANT entries). Wrapped
                   rows and the rest of the message are unlabelled; the model
                   name is omitted at narrow widths. */
                int prev_idx = (k>0) ? (int)((size_t)(p->ring.head + (size_t)k - 1) % RING_LINES) : -1;
                int first_of_block = (prev_idx<0) ? 1 : (p->ring.types[prev_idx] != RL_ASSISTANT);
                if(first_of_block && inner_w >= 6){
                    char head[96];
                    const char *mn = g_current_model[0]?g_current_model:"model";
                    int head_w = inner_w-24; if(head_w<0) head_w=0;
                    if(inner_w >= 24) snprintf(head,sizeof head,"PUCI \xC2\xB7 %.*s", head_w, mn);
                    else snprintf(head,sizeof head,"PUCI");
                    tb_print(x+1, row, g_theme.assistant, TB_DEFAULT, head);
                    int consumed = x+1 + (int)uwidth(head);
                    int gap = 2;                                   /* separation before content */
                    int rem_cells = x+1+inner_w - consumed - gap;
                    if(rem_cells>0 && seg[0]){
                        if(row_in_fence) tb_print_hl(consumed+gap, row, rem_cells, blk_lang, seg, fg, TB_DEFAULT);
                        else tb_print_styled(consumed+gap, row, fg, TB_DEFAULT, seg, rem_cells);
                    } else if(rem_cells>0) tb_set_cell(consumed+gap, row, ' ', TB_DEFAULT, TB_DEFAULT);
                } else {
                    if(row_in_fence) tb_print_hl(x+1,row,inner_w,blk_lang,seg,fg,TB_DEFAULT);
                    else tb_print_styled(x+1,row,fg,TB_DEFAULT,seg,inner_w);
                }
            } else if(t==RL_TOOL){
                if(v==0){ char lab[24]; snprintf(lab,sizeof lab,"[tool] "); tb_print(x+1,row,fg,TB_DEFAULT,lab); tb_print_styled(x+1+7,row,fg,TB_DEFAULT,seg,inner_w-7); }
                else tb_print_styled(x+1,row,fg,TB_DEFAULT,seg,inner_w);
            } else {
                tb_print_styled(x+1,row,fg,TB_DEFAULT,seg,inner_w);
            }
            rendered++; cur_vis++;
        }
    }
    /* Scrollbar: a track+thumb in the dedicated gutter column x+w-2, only
       when the pane actually overflows. Content never reaches this cell. */
    if(scrollable){
        int bar = inner_h;
        int den = (total_vis - inner_h) > 0 ? (total_vis - inner_h) : 1;
        int thumb = (bar * inner_h) / total_vis; if(thumb<1) thumb=1; if(thumb>bar) thumb=bar;
        int top_off = (start_vis < total_vis) ? start_vis : (total_vis-1);
        int pos = (top_off * (bar-thumb)) / den;
        if(pos<0) pos=0;
        if(pos>bar-thumb) pos=bar-thumb;
        int gx = x+w-2; /* rightmost content column, reserved for the track */
        for(int rr=0; rr<bar; rr++)
            tb_set_cell(gx, y+1+rr, 0x2591, (rr>=pos && rr<pos+thumb)?g_theme.accent:g_theme.muted, TB_DEFAULT);
    }
    /* Quiet empty-state hint when there are no messages yet. */
    if(p->ring.count==0 && inner_h>=3 && inner_w>=12){
        int ey=y+1;
        if(ey+2 <= y+inner_h){
            tb_print(x+1, ey, g_theme.accent, TB_DEFAULT, "PUCI");
            tb_print(x+1, ey+1, g_theme.muted,  TB_DEFAULT, "Type a message to begin.");
            tb_print(x+1, ey+2, g_theme.muted,  TB_DEFAULT, "/help for commands");
        }
    }
    /* Unread / new-lines badge: only when follow is disabled (scrolled up)
       and there are unseen lines stacked below the viewport. */
    if(!p->follow && p->unread>0){
        char nb[32]; snprintf(nb,sizeof nb,"  %d new \xE2\x86\x93  ", p->unread);
        tb_print(x+1, y, g_theme.warning, TB_DEFAULT, nb);
    }
    (void)focused;
}

void tui_render_frame(void){
    TRACE(TR_RENDER_BEG,0);
    pump_models_fetch();          /* merge a finished background model fetch */
    if(g_headless){
            // Headless simulation: mirror the conversation-first layout budget so
            // wrapped-visual counts match what the real renderer would produce.
            int cols=g_cols;
            int inner_w = cols-1-2; if(inner_w<1) inner_w=1;       /* single pane, no sidebar */
            int total_vis=0;
            for(int k=0;k<g_panes[g_focus].ring.count;k++){
                int idx=(g_panes[g_focus].ring.head + k)%RING_LINES;
                total_vis += wrap_visual_count(g_panes[g_focus].ring.lines[idx], inner_w);
            }
            g_panes[g_focus].cached_total_vis=total_vis;
            g_panes[g_focus].cached_inner_w=inner_w;
            g_panes[g_focus].layout_dirty=0;
        TRACE(TR_RENDER_END,0); TRACE(TR_PRESENT,0); return;
    }
    int rows=g_rows, cols=g_cols;
    tb_clear();
    long ctx_cur=0, ctx_bud=131072;
    if(g_npane>0 && g_panes[g_focus].sess){
        ctx_cur=(long)puci_session_context_bytes(g_panes[g_focus].sess);
        ctx_bud=(long)puci_session_get_context_budget(g_panes[g_focus].sess);
    }
    int pct = ctx_bud>0 ? (int)((ctx_cur*100)/ctx_bud) : 0;
    if(pct<0) pct=0;
    if(pct>100) pct=100;

    /* ---- Responsive sidebar width + multiline composer height -------- */
    int sidebar_w = 0;
    if(g_show_sidebar || g_mode==2){
        if(cols>=84) sidebar_w=30;
        else if(cols>=60) sidebar_w=24;
        else sidebar_w = (cols-4)>0?(cols-4):0;
    }
    int comp_w = cols-sidebar_w; if(comp_w<8) comp_w=8;
    int comp_avail = comp_w-4; if(comp_avail<1) comp_avail=1;

    int comp_h = 1;
    if(g_input_mode){
        int vis=comp_avail;
        int need = vis>0 ? (int)(uwidth(g_input)/vis) : 0;
        comp_h = need+1;
        if(comp_h<1) comp_h=1;
        if(comp_h>8) comp_h=8;
    }
    /* Composer rectangle is top border + comp_h visible rows + bottom border.
       Bottom border sits on rows-2 (footer owns rows-1); the top border is
       therefore at rows-3-comp_h and the conversation bottom border is one
       row above it, so the two boxes never overlap. */
    {
        int max_rows = rows-4;               /* leave footer + border rows */
        if(comp_h > max_rows) comp_h = max_rows>0?max_rows:0;
    }
    int cbh = comp_h + 2;                     /* full composer box height */
    int c_top=1, c_bot = rows-4-comp_h;       /* conversation bottom border */
    if(c_bot < c_top+1) c_bot=c_top+1;
    int comp_borderless = (rows < 5) ? 1 : 0; /* no room: compact borderless */
    /* Boundary ownership: header(1) · conversation(c_top..c_bot) · composer
       (c_bot+1 .. rows-2) · footer(rows-1). Validate each stays on-grid. */
    layout_check(0, 0, cols, 1, rows, cols, "header");
    layout_check(0, c_top, cols, c_bot-c_top+1, rows, cols, "conversation");
    layout_check(0, rows-1, cols, 1, rows, cols, "footer");
    if(!comp_borderless){
        layout_check(0, c_bot+1, cols, cbh, rows, cols, "composer");
    }

    if(rows<4 || cols<20){
        tb_print(1,1, g_theme.error, TB_DEFAULT, "Window too small - enlarge to at least 20x4");
        tb_hide_cursor();
        TRACE(TR_RENDER_END,0);
        TRACE(TR_PRESENT,0);
        tb_present();
        return;
    }

    const char *model = g_current_model[0]?g_current_model:getenv("PUCI_MODEL");
    if(!model || !model[0]) model="deepseek/deepseek-chat";
    const char *st = tui_status_str();
    uintattr_t sc = tui_status_color();

    /* ---- Header ------------------------------------------------------- */
    {
        char right[96];
        snprintf(right,sizeof right,"  \xE2\x97\x8F %s \xC2\xB7 Context %d%%", st, pct);
        int rw=(int)strlen(right);
        int lw = cols-1-rw; if(lw<16) lw=16;
        char left[320];
        if(g_show_debug){
            long rss=read_rss_kb(); long pss=read_pss_kb();
            snprintf(left,sizeof left," PUCI \xC2\xB7 %.*s \xC2\xB7 %s  (ctx %ldkB/%ldkB RSS %ld PSS %ld)",
                     lw>=0?(int)lw-16:0, g_panes[g_focus].title, model,
                     ctx_cur/1024, ctx_bud/1024, rss/1024, pss/1024);
        } else {
            snprintf(left,sizeof left," PUCI \xC2\xB7 %.*s \xC2\xB7 %s",
                     lw>=0?(int)lw-8:0, g_panes[g_focus].title, model);
        }
        for(int x=0;x<cols;x++) tb_set_cell(x,0,' ',TB_DEFAULT,TB_DEFAULT);
        tb_print(0,0, g_theme.text, TB_DEFAULT, left);
        tb_print(cols-1-(int)strlen(right), 0, sc, TB_DEFAULT, (const char*)right);
    }

    /* ---- Conversation: one focused pane fills available width -------- */
    if(g_split && g_npane>1 && sidebar_w==0){
        int per = cols/2; if(per<20) per=20; if(per>cols) per=cols;
        for(int i=0;i<g_npane && i<MAX_PANES;i++){
            int px=(i%2)*per;
            int pw=(i%2? cols-px: per); if(pw<1) pw=1;
            int ph=c_bot-c_top+1;
            tb_draw_box(c_top,px,ph,pw, g_panes[i].title, g_focus==i);
            tui_render_pane(&g_panes[i], px, c_top, ph, pw, g_focus==i);
        }
    } else {
        int cv_w = cols-sidebar_w; if(cv_w<1) cv_w=1;
        tb_draw_box(c_top,0, c_bot-c_top+1, cv_w, g_panes[g_focus].title, 1);
        tui_render_pane(&g_panes[g_focus], 0, c_top, c_bot-c_top+1, cv_w, 1);
    }

    /* ---- Session sidebar / drawer ------------------------------------ */
    if(sidebar_w>0){
        int sr=c_top, sc=cols-sidebar_w, sh=c_bot-c_top+1, sw=sidebar_w;
        char stitle[72];
        if(g_mode==2 && g_pick_filter[0]) snprintf(stitle,sizeof stitle,"Sessions /%.40s", g_pick_filter);
        else snprintf(stitle,sizeof stitle,"Sessions (%d) s", g_sess_count);
        tb_draw_box(sr,sc,sh,sw, stitle, g_mode==2);
        int cnt=(g_mode==2? g_pick_filtered_count : g_sess_count);
        int svis=sh-2; if(svis<1) svis=1;
        int stop=g_pick_sel-(svis-1); if(stop<0) stop=0;
        for(int i=stop; i<cnt && i<stop+svis; i++){
            uint32_t fg=(i==g_pick_sel && g_mode==2)? TB_BLACK:TB_WHITE;
            uint32_t bg=(i==g_pick_sel && g_mode==2)? TB_CYAN:TB_DEFAULT;
            int src=(g_mode==2? g_pick_filtered_idx[i] : i);
            for(int x=sc+1;x<sc+sw-1;x++) tb_set_cell(x, sr+1+(i-stop),' ',fg,bg);
            char ln[88]; snprintf(ln,sizeof ln,"%.12s %.10s", g_sess_list[src].id, g_sess_list[src].model);
            tb_print(sc+2, sr+1+(i-stop), fg, bg, ln);
        }
        if(cnt==0) tb_print(sc+2,sr+1, g_theme.warning, TB_DEFAULT, g_pick_filter[0]?"(no match)":"(no sessions)");
        if(g_mode==2) tb_print(sc+2, sr+sh-1, g_theme.accent, TB_DEFAULT, "type filter  Enter load");
    }

    /* ---- Multiline composer ------------------------------------------- */
    {
        int cb_top = rows-3-comp_h;   /* composer top border row */
        int ctop_in = cb_top+1;       /* first content row */
        if(comp_borderless){
            /* Compact borderless mode (very small height): draw the prompt on
               a single plain row; never write onto a border cell. */
            int rr = rows>1? rows-2 : 0;
            if(rr>=0){ for(int xx=0;xx<cols;xx++) tb_set_cell(xx,rr,' ',TB_DEFAULT,TB_DEFAULT);
                tb_print(1, rr, g_theme.muted, TB_DEFAULT,
                         g_input_mode ? g_input : "Message \xE2\x80\xA6 (Enter to compose)"); }
            tb_hide_cursor();
            goto composer_done;
        }
        tb_draw_box(cb_top, 0, cbh, comp_w, g_input_mode?"Input":"Composer", g_input_mode);
        for(int rr=0; rr<comp_h; rr++)
            for(int xx=1; xx<comp_w-1; xx++) tb_set_cell(xx, ctop_in+rr, ' ', TB_DEFAULT, TB_DEFAULT);
        if(g_input_mode){
            /* A pending paste occupies a collapsed chip row at the bottom of
               the composer; the typed text wraps above it. */
            int paste_rows = (int)(g_paste_len>0 ? 1u : 0u);
            int text_h = comp_h - paste_rows;
            if(text_h<1) text_h=1;
            /* wrap draft into visual rows, bottom-aligned */
            struct CLine { int off, len; } rows_s[16];
            int nrows=0; size_t pos=0;
            while(pos<g_input_len && nrows<16){
                rows_s[nrows].off=(int)pos;
                size_t adv=uwidth_advance(g_input+pos, comp_avail);
                if(adv==0) adv=1;
                if(pos+adv>g_input_len) adv=g_input_len-pos;
                rows_s[nrows].len=(int)adv;
                pos+=adv; nrows++;
            }
            if(nrows==0){ rows_s[0].off=0; rows_s[0].len=0; nrows=1; }
            int show_from = nrows<text_h? 0 : nrows-text_h;
            int shown = nrows-show_from; if(shown>text_h) shown=text_h;
            /* find the visual row that contains the cursor byte offset */
            int cstart=0;
            while(cstart<nrows){
                int r0=rows_s[cstart].off;
                int r1=(cstart+1<nrows)? rows_s[cstart+1].off : (int)g_input_len;
                if((int)g_input_curs>=r0 && (int)g_input_curs<=r1) break;
                cstart++;
            }
            if(cstart>=nrows) cstart=nrows-1;
            int cr_start = (int)g_input_curs - rows_s[cstart].off;
            if(cr_start<0) cr_start=0;
            if(cr_start>(int)rows_s[cstart].len) cr_start=(int)rows_s[cstart].len;
            /* column = 3 (prompt) + display width of the text within this
               wrapped row that precedes the cursor */
            int caret_col = 3;
            {
                char *pfx=calloc((size_t)cr_start+1,1);
                if(pfx){ memcpy(pfx, g_input+rows_s[cstart].off, (size_t)cr_start);
                    caret_col += (int)uwidth(pfx); free(pfx); }
            }
            int caret_row = ctop_in + (cstart-show_from);
            if(caret_row<ctop_in) caret_row=ctop_in;
            if(caret_row>ctop_in+text_h-1) caret_row=ctop_in+text_h-1;
            if(caret_col<comp_w-1) tb_set_cursor(caret_col, caret_row);
            for(int l=0;l<shown;l++){
                int r=ctop_in+l;
                char *ln=calloc((size_t)rows_s[show_from+l].len+1,1);
                if(ln){ memcpy(ln, g_input+rows_s[show_from+l].off, (size_t)rows_s[show_from+l].len);
                    tb_print(3, r, g_theme.text, TB_DEFAULT, ln); free(ln); }
            }
            /* collapsed paste chip */
            if(paste_rows>0){
                int r=ctop_in+text_h;
                for(int xx=1; xx<comp_w-1; xx++) tb_set_cell(xx,r,' ',TB_DEFAULT, g_theme.accent);
                char lbl[96]; paste_buf_label(lbl,sizeof lbl);
                for(int xx=1; xx<comp_w-1; xx++){} /* keep border clear */
                tb_print(2, r, g_theme.text, g_theme.accent, lbl);
            }
        } else {
            tb_print(1, ctop_in, g_theme.muted, TB_DEFAULT, "Message \xE2\x80\xA6 (Enter to compose)");
            tb_hide_cursor();
        }
    }
composer_done:
    ;

    /* ---- Footer ------------------------------------------------------- */
    /* Width-adaptive action list: always show complete reference only when
       they fit; never truncate a keybinding or a multi-byte label. */
    {
        const char *composer_active[] = {
            "Enter send", "Ctrl+J newline", "Ctrl+C cancel", "Esc close", NULL };
        const char *approval[] = {
            "y allow", "A always", "n deny", "j/k scroll", NULL };
        const char *wide[] = {
            "Enter prompt", "\xE2\x86\x91\xE2\x86\x93 scroll", "s sessions",
            "m model", "Tab pane", "q quit", NULL };
        const char *medium[] = {
            "Enter prompt", "\xE2\x86\x91/\xE2\x86\x93 scroll", "s sessions",
            "m model", "q quit", NULL };
        const char *narrow[] = {
            "Enter prompt", "? help", "q quit", NULL };
        const char **acts = NULL;
        if(g_input_mode) acts = composer_active;
        else if(g_approval_pending) acts = approval;
        else if(g_split && g_npane>1 && cols>=80) acts = wide;
        else if(cols>=64) acts = medium;
        else acts = narrow;
        char buf[512]; size_t b=0;
        buf[0]=0;
        for(int i=0; acts[i]; i++){
            const char *sep=(i==0)?"":"   \xC2\xB7   ";
            int need=(int)strlen(sep)+(int)strlen(acts[i]);
            if(b>0 && (int)b + need > cols-1) break;
            strcpy(buf+b, sep); b+=strlen(sep);
            strcpy(buf+b, acts[i]); b+=strlen(acts[i]);
        }
        for(int x=0;x<cols;x++) tb_set_cell(x,rows-1,' ',TB_DEFAULT,TB_DEFAULT);
        tb_print(0,rows-1, g_theme.muted, TB_DEFAULT, buf);
    }
    if(g_approval_pending){
        int oh=14, ow=68;
        int or_ = rows/2 - 7; if(or_<1) or_=1;
        int oc= cols/2 - 34; if(oc<0) oc=0;
        if(or_ >= rows-2) or_ = rows>3 ? rows-3 : 1;
        if(oc >= cols-2) oc = cols>3 ? cols-3 : 0;
        int maxh = rows - or_ - 1; if(maxh<3) maxh=3; if(oh>maxh) oh=maxh;
        int maxw = cols - oc - 1; if(maxw<8) maxw=8; if(ow>maxw) ow=maxw;
        for(int y=or_; y<or_+oh; y++) for(int x=oc; x<oc+ow; x++) tb_set_cell(x,y,' ', TB_WHITE, g_theme.surface);
        if(g_approval_feedback_mode){
            tb_draw_box(or_,oc,oh,ow,"Approval — deny with reason (Enter send, Esc cancel)",1);
        } else {
            tb_draw_box(or_,oc,oh,ow,"Approval — y allow  A always  n deny  d reason  j/k scroll  q deny",1);
        }
        char *copy=strdup(g_approval_diff); if(!copy){ /* allocation failure: skip overlay */ } else { char *save=NULL; char *tok=strtok_r(copy,"\n",&save); int l=0;
        int rows_avail = oh-3; if(rows_avail<1) rows_avail=1;
        while(tok){
            if(l>=g_hunk_off && l < g_hunk_off+rows_avail){
                int yy = or_+1+(l-g_hunk_off);
                if(yy >= or_+1 && yy < or_+oh-2){
                    uint32_t fg = TB_WHITE;
                    if(tok[0]=='+') fg = TB_GREEN|TB_BOLD;
                    else if(tok[0]=='-') fg = TB_RED|TB_BOLD;
                    char line[96]; snprintf(line,sizeof line,"%.*s", ow>2?ow-2:0, tok);
                    tb_print(oc+1, yy, fg, g_theme.surface, line);
                }
            }
            l++; tok=strtok_r(NULL,"\n",&save);
        }
        free(copy); }
        /* footer line: feedback prompt or scroll hint */
        int fy = or_+oh-2;
        if(fy >= or_+1 && fy < or_+oh-1){
            if(g_approval_feedback_mode){
                char fbuf[540];
                snprintf(fbuf,sizeof fbuf,"reason> %.510s", g_approval_feedback);
                tb_print(oc+1, fy, TB_BLACK, TB_YELLOW, fbuf);
            } else {
                char hbuf[64];
                if(g_hunk_max>1) snprintf(hbuf,sizeof hbuf,"%d/%d  j/k scroll", g_hunk_off+1, g_hunk_max);
                else snprintf(hbuf,sizeof hbuf,"%d/%d", g_hunk_off+1, g_hunk_max);
                tb_print(oc+1, fy, TB_CYAN, g_theme.surface, hbuf);
            }
        }
    }
    if(g_mode==3){
        int oh=16, ow=54;
        int or_ = rows/2 - 8; if(or_<1) or_=1;
        int oc= cols/2 - 27; if(oc<0) oc=0;
        if(or_ >= rows-2) or_ = rows>3 ? rows-3 : 1;
        if(oc >= cols-2) oc = cols>3 ? cols-3 : 0;
        int maxh = rows - or_ - 1; if(maxh<3) maxh=3; if(oh>maxh) oh=maxh;
        int maxw = cols - oc - 1; if(maxw<8) maxw=8; if(ow>maxw) ow=maxw;
        for(int y=or_; y<or_+oh; y++) for(int x=oc; x<oc+ow; x++) tb_set_cell(x,y,' ', TB_WHITE, g_theme.surface);
        char mtitle[64]; if(g_pick_filter[0]) snprintf(mtitle,sizeof mtitle,"Model /%.30s", g_pick_filter); else snprintf(mtitle,sizeof mtitle,"Model — Enter select (Ctrl-Y copy)");
        tb_draw_box(or_,oc,oh,ow,mtitle,1);
        /* Viewport: keep the selection row visible, scroll to follow it. */
        int vis = oh-2; if(vis<1) vis=1;
        int top = g_pick_sel - (vis-1); if(top<0) top=0;
        for(int i=top;i<g_pick_filtered_count && i<top+vis; i++){
            int src=g_pick_filtered_idx[i];
            int yy=or_+1+(i-top);
            if(yy < or_+oh-1){
                if(i==g_pick_sel){
                    for(int x=oc+1;x<oc+ow-1;x++) tb_set_cell(x,yy,' ', TB_BLACK, TB_CYAN);
                    tb_print(oc+2, yy, TB_BLACK, TB_CYAN, k_models[src]);
                } else {
                    tb_print(oc+2, yy, TB_WHITE, g_theme.surface, k_models[src]);
                }
            }
        }
        if(g_pick_filtered_count==0) tb_print(oc+2,or_+1, g_theme.warning, g_theme.surface, "(no match)");
        tb_print(oc+2,or_+oh-1, g_theme.accent, g_theme.surface, "type filter  Ctrl-Y copy  Esc cancel");
    }
    if(g_mode==2){
        // sessions picker overlay already via sidebar, but add help
        // footer overwrites
    }
    TRACE(TR_RENDER_END,0);
    TRACE(TR_PRESENT,0);
    tb_present();
    // present end could be another trace but keep single point
}

int puci_tui_available(void){ return 1; }
int puci_tui_should_launch(int argc, char **argv){
    /* Scan ALL args first so flag precedence is independent of ordering. */
    int want_tui=-1;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--no-tui")==0) want_tui=0;
        else if(strcmp(argv[i],"--tui")==0) want_tui=1;
    }
    if(want_tui>=0) return want_tui;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"run")==0) return 0;
        if(strcmp(argv[i],"doctor")==0) return 0;
        if(strcmp(argv[i],"--help")==0 || strcmp(argv[i],"-h")==0) return 0;
        if(strcmp(argv[i],"--version")==0) return 0;
    }
    if(argc>1) return 0; /* unknown positional args -> not an interactive TUI session */
    /* Only treat PUCI_NO_TUI as "disable" when it is set to a truthy value,
       so PUCI_NO_TUI=0 (asking for TUI) does not silently disable it. */
    { const char *nt=getenv("PUCI_NO_TUI"); if(nt && *nt && strcmp(nt,"0")!=0) return 0; }
    if(!getenv("PUCI_TUI_FORCE") && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) return 0;
    return 1;
}

static void stream_append_segment(Pane *p, const char *s, size_t n){
    if(n==0) return;
    int added_lines = 0;
    if(p->pending_nl > 0){
        /* (pending-1) blank separator lines, then the content on a fresh line.
           This preserves a\n\nb as "a", "", "b" instead of merging "b" onto
           the blank line. */
        int sep = p->pending_nl-1; if(sep>64) sep=64;
        for(int i=0; i < sep; i++){
            ring_push_typed(&p->ring, "", RL_ASSISTANT);
        }
        added_lines += sep;
        p->pending_nl = 0;
        ring_push_typed(&p->ring, "", RL_ASSISTANT);
        added_lines += 1;
        ring_append_to_last(&p->ring, s, n);
    } else {
        ring_append_to_last(&p->ring, s, n);
    }
    /* New output after follow is off is unseen: bank it for the badge. */
    if(!p->follow && added_lines>0) p->unread += added_lines;
}

static void pane_stream_begin(Pane *p){
    p->streaming=1; p->stream_len=0; p->stream_buf[0]=0;
    p->pending_nl=0; puci_ttg_reset(&p->ttg);
    ring_push_typed(&p->ring, "", RL_ASSISTANT);
}

/* Thin wrapper: strips TTG envelopes via the shared ttg module and keeps
 * tracing the completed-envelope latency (TTG metric). */
static size_t pane_ttg_filter(Pane *p, const char *txt, size_t len,
                              char *out, size_t outcap, int *trace_hits){
    char dig[64];
    size_t o = puci_ttg_filter(&p->ttg, txt, len, out, outcap, dig, sizeof dig, trace_hits);
    if(dig[0]){
        uint64_t sent = strtoull(dig, NULL, 10);
        if(sent > 0){
            uint64_t now = trace_now_mono_ns();
            int64_t ttg = (int64_t)(now - sent);
            if(ttg >= 0 && ttg < 1000000000LL){
                TRACE(TR_EVENT, (uint32_t)(10000 + ttg/1000000));
            }
        }
    }
    return o;
}

static void pane_stream_append(Pane *p, const char *txt, size_t len){
    if(!p->streaming){
        pane_stream_begin(p);
    }
    if(len==0) return;
    /* 1) Strip TTG envelopes at the stream boundary (never reaches Ring). */
    char filt[STREAM_BUF + 2];
    size_t flen;
    int dev;
    if(len <= STREAM_BUF){
        flen = pane_ttg_filter(p, txt, len, filt, sizeof filt, &dev);
    } else {
        size_t off=0; flen=0;
        while(off<len){
            size_t sub = len-off; if(sub>STREAM_BUF) sub=STREAM_BUF;
            char tmp[STREAM_BUF+2]; int th; char dig[256];
            size_t t = puci_ttg_filter(&p->ttg, txt+off, sub, tmp, sizeof tmp, dig, sizeof dig, &th);
            if(dig[0] && th>0){
                uint64_t sent = strtoull(dig, NULL, 10);
                if(sent>0){ uint64_t now=trace_now_mono_ns(); int64_t ttg=(int64_t)(now-sent);
                    if(ttg>=0 && ttg<1000000000LL) TRACE(TR_EVENT,(uint32_t)(10000+ttg/1000000)); }
            }
            memcpy(filt+flen, tmp, t); flen += t;
            off += sub;
        }
        dev = flen;
    }
    (void)dev;
    /* 2) Feed the filtered bytes through newline/CR splitting into the ring. */
    size_t off=0;
    while(off < flen){
        const char *nl = (const char*)memchr(filt+off, '\n', flen-off);
        size_t clen = nl ? (size_t)(nl - (filt+off)) : (flen - off);
        /* filter \r inside the segment (handle \r\n) */
        char tmp[RING_COLS]; size_t w=0;
        for(size_t k=0;k<clen && w<RING_COLS-1;k++) if(filt[off+k]!='\r') tmp[w++]=filt[off+k];
        if(w>0) stream_append_segment(p, tmp, w);
        if(nl){
            p->pending_nl += 1;
            off += clen + 1;
        } else {
            off += clen;
        }
    }
}

static void pane_stream_end(Pane *p){
    if(p->streaming){
        p->streaming=0;
        /* A TTG envelope that reached turn completion without its closing
           marker is not real instrumentation: flush any genuinely-partial
           content bytes (a lone <=3-byte marker is dropped). */
        char rest[PUCI_TTG_CARRY_MAX + 8];
        size_t n = puci_ttg_flush(&p->ttg, rest, sizeof rest);
        if(n > 0){
            pane_stream_append(p, rest, n);
        }
        /* Materialize trailing empty lines. */
        if(p->pending_nl > 0){
            int lim = p->pending_nl-1; if(lim>64) lim=64;
            for(int i=0; i < lim; i++){
                ring_push_typed(&p->ring, "", RL_ASSISTANT);
                if(!p->follow) p->unread += 1;
            }
            p->pending_nl=0;
        }
    }
}

int puci_tui_run(const puci_cli_params *params){
    trace_init();
    const char *cli_prompt = params ? params->prompt : NULL;
    int plan_mode = params ? params->plan_mode : 0;
    int headless=0;
    if(tb_init()!=0){
        if(getenv("PUCI_TUI_FORCE")){
            headless=1; g_headless=1;
            fprintf(stderr,"[tui] headless trace mode (tb_init failed)\n");
        } else { fprintf(stderr,"tui: tb_init failed\n"); return 1; }
    } else {
        g_tb_ok=1;
    }
    if(!headless){ tb_set_input_mode(TB_INPUT_ESC); tb_set_output_mode(TB_OUTPUT_NORMAL); }
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_winch; sigemptyset(&sa.sa_mask); sa.sa_flags=SA_RESTART; sigaction(SIGWINCH,&sa,&g_old_winch);
    /* SIGTERM/SIGHUP must restore the terminal on exit rather than leaving the
       shell in the alternate screen with a hidden cursor. We install a handler
       that sets g_terminate; the event loop then runs the normal, synchronous
       teardown (tb_shutdown restores alt-screen/cursor/input mode). */
    static struct sigaction ter;
    memset(&ter,0,sizeof ter); ter.sa_handler=on_terminate; sigemptyset(&ter.sa_mask); ter.sa_flags=SA_RESTART;
    sigaction(SIGTERM,&ter,NULL);
    sigaction(SIGHUP,&ter,NULL);
    g_npane=1; g_focus=0; g_mode=0;
    for(int i=0;i<MAX_PANES;i++){ memset(&g_panes[i],0,sizeof g_panes[i]); snprintf(g_panes[i].title,sizeof g_panes[i].title,"Agent %c",'A'+i); snprintf(g_panes[i].session_id,sizeof g_panes[i].session_id,"tui-%ld-%d-%d",(long)time(NULL),i,++g_sess_seq); g_panes[i].scroll_off=0; g_panes[i].layout_dirty=1; g_panes[i].cached_inner_w=-1; g_panes[i].cached_total_vis=-1; g_panes[i].follow=1; g_panes[i].unread=0; }
    puci_context *ctx=NULL;
    if(puci_context_create(NULL,&ctx)!=PUCI_OK){ tui_teardown(); trace_dump(); fprintf(stderr,"ctx fail\n"); return 1; }
    puci_provider_config *pcfg_ptr = puci_cli_provider("openai");
    if(pcfg_ptr && pcfg_ptr->model) snprintf(g_current_model,sizeof g_current_model,"%.255s",pcfg_ptr->model);
    if(!pcfg_ptr){ tui_teardown(); trace_dump(); fprintf(stderr,"tui: provider config failed\n"); puci_context_destroy(ctx); return 1; }
    puci_provider_config pcfg = *pcfg_ptr;
    puci_context_set_provider(ctx,&pcfg);
    for(int i=0;i<g_npane;i++){
        puci_session_options sopts={0}; sopts.provider=pcfg;
        sopts.system_prompt = plan_mode
            ? "You are puci, a fast C coding agent. Produce a concise implementation plan; do not edit files unless asked."
            : "You are puci, a fast C coding agent. Be concise and helpful.";
        sopts.approval_mode=PUCI_APPROVAL_MODE_EVENT;
        puci_session *s=NULL;
        puci_status _cs = puci_session_create(ctx,&sopts,&s);
        if(_cs==PUCI_OK){
            g_panes[i].sess=s;
            /* No startup keybinding tutorial in the transcript; initial help
               lives in the footer / empty-state view instead. */
        } else {
            char _em[180]; snprintf(_em,sizeof _em,"⚠ session init failed: %s", puci_status_string(_cs));
            ring_push_typed(&g_panes[i].ring, _em, RL_ERROR);
        }
    }
    if(!g_panes[0].sess){
        /* TUI is useless without at least one session; fail loudly now. */
        tui_teardown(); trace_dump();
        fprintf(stderr,"tui: could not create any session (provider misconfigured?)\n");
        puci_context_destroy(ctx);
        return 1;
    }
    load_sessions(); history_load(); models_init_static();
    // test hooks
    { const char *tf=getenv("PUCI_TUI_FILL_RING"); if(tf && *tf){ int n=atoi(tf); if(n>0){ for(int i=0;i<n;i++){ char ln[128]; snprintf(ln,sizeof ln,"prefill line %d Lorem ipsum dolor sit amet consectetur", i); ring_push_typed(&g_panes[0].ring, ln, RL_SYSTEM); } g_panes[0].layout_dirty=1; } } }
    { const char *pn=getenv("PUCI_TUI_PANES"); if(pn && *pn){ int v=atoi(pn); if(v>=1 && v<=MAX_PANES){ g_npane=v; for(int i=1;i<g_npane;i++){ if(!g_panes[i].sess){ puci_session_options sopts={0}; sopts.provider=pcfg; sopts.system_prompt="test"; sopts.approval_mode=PUCI_APPROVAL_MODE_EVENT; if(puci_session_create(ctx,&sopts,&g_panes[i].sess)==PUCI_OK){ ring_push_typed(&g_panes[i].ring,"— test pane —", RL_SYSTEM); } } } } } }
    get_winsize(&g_rows,&g_cols);
    if(cli_prompt){
        snprintf(g_input, sizeof g_input, "%.*s", (int)(sizeof g_input -1), cli_prompt);
        g_input_len=strlen(g_input); g_input_curs=g_input_len;
    }
    struct timespec last_gauge; clock_gettime(CLOCK_MONOTONIC,&last_gauge);
    struct timespec tui_start; clock_gettime(CLOCK_MONOTONIC,&tui_start);
    long exit_after_ms=0; { const char *ev=getenv("PUCI_TUI_EXIT_AFTER_MS"); if(ev) exit_after_ms=atol(ev); }
    int dirty=1;
    int running=1;
    while(running){
        if(g_terminate){ running=0; break; } /* SIGTERM/SIGHUP: clean teardown */
        if(g_winch){ g_winch=0; get_winsize(&g_rows,&g_cols); dirty=1; for(int _di=0;_di<MAX_PANES;_di++){ g_panes[_di].layout_dirty=1; g_panes[_di].cached_inner_w=-1; } }
        if(exit_after_ms>0){ struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now); long el=(now.tv_sec-tui_start.tv_sec)*1000 + (now.tv_nsec-tui_start.tv_nsec)/1000000; if(el>=exit_after_ms) running=0; }
        {
            /* Pump a finished background model fetch. Only when it installs a
               model change do we need to repaint. */
            if(pump_models_fetch()) dirty=1;
            /* Cheap-timeout repaint: force a frame only when there is live
               streaming, a visible dialog, or pending work. Skip it entirely
               for a fully static idle screen so we avoid a 2 Hz full redraw. */
            {
                int live=0;
                for(int _t=0;_t<g_npane;_t++) if(g_panes[_t].streaming) live=1;
                if(live || g_approval_pending || atomic_load(&g_live_done)){
                    struct timespec ng; clock_gettime(CLOCK_MONOTONIC,&ng);
                    long ge=(ng.tv_sec-last_gauge.tv_sec)*1000+(ng.tv_nsec-last_gauge.tv_nsec)/1000000;
                    if(ge>=100){ dirty=1; last_gauge=ng; }
                }
            }
        }
        if(dirty){ tui_render_frame(); dirty=0; }
        /* Dynamically sized fd array: never silently drop a session fd
           (a fixed cap could leave a panes polled never -> stalled). */
        size_t nf_cap = 1; /* room for stdin */
        for(int _t=0;_t<g_npane;_t++) if(g_panes[_t].sess){ size_t _c=0; puci_session_get_fds(g_panes[_t].sess,NULL,0,&_c); nf_cap += _c; }
        if(nf_cap > 65536) nf_cap = 65536;
        struct pollfd *fds = (struct pollfd*)malloc(nf_cap * sizeof(struct pollfd));
        if(!fds){ struct timespec _ts={0,30000000L}; nanosleep(&_ts,NULL); continue; }
        size_t nf=0;
        int pane_off[MAX_PANES]; int pane_cnt[MAX_PANES]; for(int _i=0;_i<MAX_PANES;_i++){ pane_off[_i]=-1; pane_cnt[_i]=0; }
        fds[nf].fd=STDIN_FILENO; fds[nf].events=POLLIN; nf++;
        int session_to=-1;
        for(int i=0;i<g_npane;i++) if(g_panes[i].sess){
            size_t cnt=0; puci_session_get_fds(g_panes[i].sess,NULL,0,&cnt);
            pane_off[i]=(int)nf; pane_cnt[i]=(int)cnt;
            if(cnt>0){
                if(nf+cnt > nf_cap) cnt = nf_cap - nf;
                if(cnt==0){ pane_cnt[i]=0; pane_off[i]=-1; }
                else {
                    size_t got=0; puci_session_get_fds(g_panes[i].sess,fds+nf, cnt, &got);
                    pane_cnt[i]=(int)got;
                    if(got==0) pane_off[i]=-1;
                    nf+=got;
                }
            } else { pane_off[i]=-1; pane_cnt[i]=0; }
            int tm=-1; puci_session_next_timeout_ms(g_panes[i].sess,&tm);
            if(tm>=0 && (session_to<0 || tm < session_to)) session_to=tm;
        }
        int gauge_due;
        {
            struct timespec ng2; clock_gettime(CLOCK_MONOTONIC,&ng2);
            long ge = (ng2.tv_sec-last_gauge.tv_sec)*1000 + (ng2.tv_nsec-last_gauge.tv_nsec)/1000000;
            gauge_due = (int)(500 - ge);
            if(gauge_due < 0) gauge_due = 0;
        }
        // streaming = responsive but not busy: 16ms, idle 250ms. Clamp 0->1ms to avoid 100% CPU spin.
        int is_streaming=0;
        for(int i=0;i<g_npane;i++) if(g_panes[i].streaming) is_streaming=1;
        int poll_to;
        if(is_streaming){
            poll_to = session_to>=0 ? (session_to < 20 ? session_to : 20) : 20;
            if(gauge_due < poll_to) poll_to = gauge_due;
            if(poll_to<0) poll_to=1;
        } else if(session_to >= 0){
            poll_to = session_to < gauge_due ? session_to : gauge_due;
            if(poll_to<0) poll_to=1;
        } else if(atomic_load(&g_live_inflight)){
            /* A background model fetch is running but publishes no fd; poll
               with a bounded tick so the finished result gets pumped. */
            poll_to = gauge_due;
            if(poll_to<1) poll_to=1;
        } else if(exit_after_ms > 0){
            /* Test hook only: needs a periodic wakeup to check the deadline. */
            poll_to = 250;
        } else {
            /* Fully idle, no deadlines: block until an event (input, signal)
               instead of busy-waking at 4 Hz. Zero wakeups while idle. */
            poll_to = -1;
        }
        if(poll_to < -1) poll_to = -1;
        if(poll_to == 0) poll_to = 1;
        int pr=poll(fds,(nfds_t)nf,poll_to);
        {
            int backlog=0;
            if(g_trace_enabled) { ioctl(STDIN_FILENO, FIONREAD, &backlog); }
            TRACE(TR_POLL_RET, (uint32_t)(pr<0?0:backlog));
        }
        if(pr<0){ free(fds); if(errno==EINTR) continue; else { struct timespec _ts={0,20000000L}; nanosleep(&_ts,NULL); continue; } }
        if(pr==0){
            /* A timeout is the idle case: only repaint on a cheap tick when
               there is actually something animated, never on a static idle
               screen (the previous gauge-driven 2 Hz redraw is gone). */
            {
                int live=0;
                for(int _t=0;_t<g_npane;_t++) if(g_panes[_t].streaming) live=1;
                if((live || g_approval_pending || atomic_load(&g_live_done))){
                    struct timespec ng3; clock_gettime(CLOCK_MONOTONIC,&ng3);
                    long ge2 = (ng3.tv_sec-last_gauge.tv_sec)*1000 + (ng3.tv_nsec-last_gauge.tv_nsec)/1000000;
                    if(ge2 >= 100) dirty=1;
                }
            }
        }
        if(pr>0 && (fds[0].revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL))){
            char buf[128]; ssize_t n=read(STDIN_FILENO,buf,sizeof buf);
            if(n<=0){ free(fds); running=0; break; }
            int had_key=0;
            for(ssize_t k=0;k<n;k++){
                unsigned char c=(unsigned char)buf[k];
                had_key=1;
                // ESC sequence handling (fragment-safe: arrow may arrive split across reads)
                if(c==27){
                    /* Bracketed paste markers: "\x1b[200~" opens, "\x1b[201~"
                       closes. Detect complete sequences present in this batch
                       and consume them, toggling g_paste so a pasted block is
                       treated as data rather than as command keys (q/m/s/y). */
                    if(k+6 < n){
                        if(memcmp(buf+k, "\x1b[200~", 6)==0){ g_paste=1; paste_buf_clear(); k+=5; continue; }
                        if(memcmp(buf+k, "\x1b[201~", 6)==0){ g_paste=0; k+=5; continue; }
                    }
                    char seq[8]={0}; int seq_len=0;
                    if(k+1 < n){ seq[seq_len++]=buf[++k]; }
                    else { struct pollfd _pf={STDIN_FILENO,POLLIN,0}; TRACE(TR_ESCWAIT,0); if(poll(&_pf,1,30)>0 && (_pf.revents&POLLIN)){ char _ch; if(read(STDIN_FILENO,&_ch,1)==1) seq[seq_len++]=_ch; } TRACE(TR_ESCWAIT,1); }
                    if(seq_len==1 && seq[0]=='['){
                        if(k+1 >= n){ struct pollfd _pf2={STDIN_FILENO,POLLIN,0}; TRACE(TR_ESCWAIT,2); if(poll(&_pf2,1,30)>0 && (_pf2.revents&POLLIN)){ char _ch; if(read(STDIN_FILENO,&_ch,1)==1) seq[seq_len++]=_ch; } else if(k+2 < n){ seq[seq_len++]=buf[++k]; } TRACE(TR_ESCWAIT,3); }
                        else seq[seq_len++]=buf[++k];
                        if(seq_len>=2 && (seq[1]=='5' || seq[1]=='6')){
                            if(seq_len==2){ struct pollfd _pf3={STDIN_FILENO,POLLIN,0}; TRACE(TR_ESCWAIT,4); if(poll(&_pf3,1,30)>0 && (_pf3.revents&POLLIN)){ char _ch; if(read(STDIN_FILENO,&_ch,1)==1) seq[seq_len++]=_ch; } else if(k+1 < n) seq[seq_len++]=buf[++k]; TRACE(TR_ESCWAIT,5); }
                        }
                    }
                    if(seq_len>=1 && seq[0]=='[' && seq_len>=2){
                        unsigned char code=(unsigned char)seq[1];
                        if(code=='A'){ if(g_input_mode) history_prev(); else if(g_mode==2||g_mode==3){ if(g_pick_sel>0) g_pick_sel--; } else { Pane *pp=&g_panes[g_focus]; pp->scroll_off+=2; pp->follow=0; } }
                        else if(code=='B'){ if(g_input_mode) history_next(); else if(g_mode==2||g_mode==3){ if(g_pick_sel < g_pick_filtered_count-1) g_pick_sel++; } else { Pane *pp=&g_panes[g_focus]; if(pp->scroll_off>0){ pp->scroll_off-=2; if(pp->scroll_off<=0){ pp->scroll_off=0; pp->follow=1; pp->unread=0; } } } }
                        else if(code=='C'){ if(g_input_mode && g_input_curs < g_input_len){ size_t el=utf8_seq_len((unsigned char)g_input[g_input_curs]); if(el+g_input_curs>g_input_len) el=g_input_len-g_input_curs; g_input_curs+=el; } }
                        else if(code=='D'){ if(g_input_mode && g_input_curs>0){ int dl=utf8_prev_len((const unsigned char*)g_input, g_input_curs); g_input_curs-=dl; } }
                        else if(code=='5' && seq_len>=3 && seq[2]=='~'){ if(g_input_mode) history_prev(); else { Pane *pp=&g_panes[g_focus]; pp->scroll_off+=10; pp->follow=0; } }
                        else if(code=='6' && seq_len>=3 && seq[2]=='~'){ if(g_input_mode) history_next(); else { Pane *pp=&g_panes[g_focus]; if(pp->scroll_off>0){ pp->scroll_off-=10; if(pp->scroll_off<0) pp->scroll_off=0; if(pp->scroll_off==0){ pp->follow=1; pp->unread=0; } } } }
                        else if(code=='H'){ if(g_input_mode) g_input_curs=0; else { Pane *pp=&g_panes[g_focus]; pp->scroll_off=9999; pp->follow=0; } }
                        else if(code=='F'){ if(g_input_mode) g_input_curs=g_input_len; else { Pane *pp=&g_panes[g_focus]; pp->scroll_off=0; pp->follow=1; pp->unread=0; } }
                    } else if(seq_len==0){
                        if(g_mode==2||g_mode==3){ g_mode=0; g_pick_filter[0]=0; g_show_sidebar=0; }
                        else if(g_input_mode){ g_input_len=0; g_input[0]=0; g_input_curs=0; g_input_mode=0; }
                    }
                    continue;
                }
                if(g_mode==2){ // session picker
                    if(c=='\n'||c=='\r'){
                        if(g_pick_sel < g_pick_filtered_count){
                            int src=g_pick_filtered_idx[g_pick_sel];
                            char id[128]; snprintf(id,sizeof id,"%s",g_sess_list[src].id);
                            Pane *pp=&g_panes[g_focus];
                            if(pp->sess){
                                /* Never load a session over an in-flight turn: cancel
                                   it first so library state isn't corrupted. */
                                if(pp->streaming) puci_session_cancel(pp->sess);
                                pp->streaming=0; pp->pending_nl=0; puci_ttg_reset(&pp->ttg); pp->scroll_off=0;
                                char loaded[128]; if(puci_cli_load_session(pp->sess, id, loaded,sizeof loaded)==0){
                                    ring_push_typed(&pp->ring, "— resumed session —", RL_SYSTEM);
                                    char msg[160]; snprintf(msg,sizeof msg,"Loaded %s (%s)", id, g_sess_list[src].model);
                                    ring_push_typed(&pp->ring, msg, RL_SYSTEM);
                                    snprintf(pp->title,sizeof pp->title,"%.20s", id);
                                    snprintf(pp->session_id,sizeof pp->session_id,"%.127s", id);
                                    pp->scroll_off=0;
                                } else {
                                    char _em[160]; snprintf(_em,sizeof _em,"load session failed: %s", id);
                                    ring_push_typed(&pp->ring, _em, RL_ERROR);
                                }
                            }
                        }
                        g_mode=0; g_pick_filter[0]=0; g_show_sidebar=0;
                    } else if(c==27 || (c=='q' && g_pick_filter[0]==0)){ g_mode=0; g_pick_filter[0]=0; g_show_sidebar=0; }
                    else if(c==25){ // Ctrl-Y copy
                        if(g_pick_sel < g_pick_filtered_count){
                            int src=g_pick_filtered_idx[g_pick_sel];
                            copy_to_clipboard(g_sess_list[src].id, strlen(g_sess_list[src].id));
                            char msg[160]; snprintf(msg,sizeof msg,"Copied %s", g_sess_list[src].id);
                            ring_push_typed(&g_panes[g_focus].ring, msg, RL_SYSTEM);
                        }
                    }
                    else if(c>=32 && c<127 && c!='\t'){
                        size_t fl=strlen(g_pick_filter);
                        if(fl+1 < sizeof g_pick_filter){ g_pick_filter[fl]=(char)c; g_pick_filter[fl+1]=0; rebuild_pick_filter(); }
                    } else if(c==127 || c==8){
                        size_t fl=strlen(g_pick_filter);
                        if(fl>0){ g_pick_filter[fl-1]=0; rebuild_pick_filter(); }
                    }
                    continue;
                }
                if(g_mode==3){ // model picker
                    if(c=='\n'||c=='\r'){
                        if(g_pick_sel < g_pick_filtered_count){
                            int src=g_pick_filtered_idx[g_pick_sel];
                            if(k_models[src]){
                                setenv("PUCI_MODEL", k_models[src], 1);
                                puci_status _mst = apply_model_to_panes(k_models[src]);
                                if(_mst==PUCI_OK){
                                    snprintf(g_current_model,sizeof g_current_model,"%.255s",k_models[src]);
                                    char msg[160]; snprintf(msg,sizeof msg,"Model → %s", k_models[src]);
                                    ring_push_typed(&g_panes[g_focus].ring, msg, RL_SYSTEM);
                                } else {
                                    char msg[200]; snprintf(msg,sizeof msg,"Model %s NOT applied (err %s)", k_models[src], puci_status_string(_mst));
                                    ring_push_typed(&g_panes[g_focus].ring, msg, RL_ERROR);
                                }
                            }
                        }
                        g_mode=0; g_pick_filter[0]=0;
                    } else if(c==27 || (c=='q' && g_pick_filter[0]==0)){ g_mode=0; g_pick_filter[0]=0; }
                    else if(c==25){ // Ctrl-Y copy
                        if(g_pick_sel < g_pick_filtered_count){
                            int src=g_pick_filtered_idx[g_pick_sel];
                            copy_to_clipboard(k_models[src], strlen(k_models[src]));
                            char msg[160]; snprintf(msg,sizeof msg,"Copied %s", k_models[src]);
                            ring_push_typed(&g_panes[g_focus].ring, msg, RL_SYSTEM);
                        }
                    }
                    else if(c>=32 && c<127 && c!='\t'){
                        size_t fl=strlen(g_pick_filter);
                        if(fl+1 < sizeof g_pick_filter){ g_pick_filter[fl]=(char)c; g_pick_filter[fl+1]=0; rebuild_pick_filter(); }
                    } else if(c==127 || c==8){
                        size_t fl=strlen(g_pick_filter);
                        if(fl>0){ g_pick_filter[fl-1]=0; rebuild_pick_filter(); }
                    }
                    continue;
                }
                if(g_input_mode){
                    if(c=='\r'){   /* Enter sends */
                        g_input[g_input_len]=0;
                        /* Slash commands only apply to a pure typed draft; a
                           pending paste always sends as a plain message. */
                        int is_slash=(g_paste_len==0 && g_input_len>0 && g_input[0]=='/');
                        if(g_input_len>0 || g_paste_len>0){
                            if(g_input_len>0) history_save_line(g_input);
                            if(is_slash){
                                if(strcmp(g_input,"/clear")==0){ Pane *pp=&g_panes[g_focus]; if(pp->sess) puci_session_cancel(pp->sess); pp->streaming=0; pp->pending_nl=0; puci_ttg_reset(&pp->ttg); pp->ring.count=0; pp->ring.head=0; pp->scroll_off=0; if(pp->sess) puci_session_clear(pp->sess); ring_push_typed(&pp->ring,"— cleared —", RL_SYSTEM); }
                                else if(strcmp(g_input,"/help")==0 || strcmp(g_input,"/?")==0){ ring_push_typed(&g_panes[g_focus].ring,"/clear /help /model [name] /sessions /new /compact /doctor /plan", RL_SYSTEM); ring_push_typed(&g_panes[g_focus].ring,"Keys: Enter send  Ctrl+J newline  Esc cancel  ↑↓ history/scroll  s sessions  m model  g/G top/bottom  q or Ctrl+C quit", RL_SYSTEM); }
                                else if(strncmp(g_input,"/model",6)==0 && (g_input[6]=='\0' || g_input[6]==' ')){ char *arg=g_input+6; while(*arg==' ') arg++; if(*arg){ setenv("PUCI_MODEL",arg,1); puci_status _mst=apply_model_to_panes(arg); if(_mst==PUCI_OK){ snprintf(g_current_model,sizeof g_current_model,"%.255s",arg); char msg[160]; snprintf(msg,sizeof msg,"Model → %.120s",arg); ring_push_typed(&g_panes[g_focus].ring,msg,RL_SYSTEM); } else { char _em[200]; snprintf(_em,sizeof _em,"Model %.120s NOT applied (err %s)", arg, puci_status_string(_mst)); ring_push_typed(&g_panes[g_focus].ring,_em,RL_ERROR); } } else { models_init_static(); picker_open(3); fetch_live_models(); if(g_mode==3) rebuild_pick_filter(); } }
                                else if(strcmp(g_input,"/sessions")==0 || strcmp(g_input,"/s")==0){ load_sessions(); picker_open(2); }
                                else if(strcmp(g_input,"/new")==0){ Pane *pp=&g_panes[g_focus]; if(pp->sess){ TRACE(TR_SAVE,1); puci_cli_save_session(pp->sess, pp->session_id); puci_session_cancel(pp->sess); puci_session_clear(pp->sess); } pp->streaming=0; pp->pending_nl=0; puci_ttg_reset(&pp->ttg); pp->ring.count=0; pp->ring.head=0; pp->scroll_off=0; snprintf(pp->session_id,sizeof pp->session_id,"tui-%ld-%d-%d",(long)time(NULL),g_focus,++g_sess_seq); ring_push_typed(&pp->ring,"— new session —", RL_SYSTEM); }
                                else if(strcmp(g_input,"/compact")==0){ Pane *pp=&g_panes[g_focus]; if(pp->sess){ size_t pr=0; puci_session_compact_context(pp->sess,&pr); char msg[96]; snprintf(msg,sizeof msg,"Compacted %zu bytes",pr); ring_push_typed(&pp->ring,msg,RL_SYSTEM); } }
                                else if(strcmp(g_input,"/doctor")==0){ puci_provider_config *pc=puci_cli_provider("openai"); char msg[256]; snprintf(msg,sizeof msg,"model=%s base=%s", pc?pc->model:"?", pc?pc->base_url:"?"); ring_push_typed(&g_panes[g_focus].ring,msg,RL_SYSTEM); }
                                else if(strcmp(g_input,"/plan")==0){ ring_push_typed(&g_panes[g_focus].ring,"plan mode: use --plan flag for now", RL_SYSTEM); }
                                else { char msg[200]; snprintf(msg,sizeof msg,"Unknown: %s (try /help)", g_input); ring_push_typed(&g_panes[g_focus].ring,msg,RL_ERROR); }
                            } else if(g_panes[g_focus].sess){
                                /* Build the exact draft: typed text + paste. We
                                   send byte-for-byte so the provider receives
                                   what the user actually assembled, and the
                                   transcript records the same. */
                                char *pl=build_send_payload();
                                if(!pl){
                                    ring_push_typed(&g_panes[g_focus].ring,"OOM building message", RL_ERROR);
                                } else {
                                    /* Show the paste as a collapsed card when it is
                                       multi-line; short pastes render inline. */
                                    if(g_paste_len>0){
                                        char lbl[96]; paste_buf_label(lbl,sizeof lbl);
                                        char card[160]; snprintf(card,sizeof card,"❯ %s%s%s",
                                            g_input, g_input_len>0?" ":"", lbl);
                                        ring_push_typed(&g_panes[g_focus].ring, card, RL_USER);
                                    } else {
                                        char u[2048]; snprintf(u,sizeof u,"❯ %s", g_input);
                                        ring_push_typed(&g_panes[g_focus].ring, u, RL_USER);
                                    }
                                    g_panes[g_focus].scroll_off=0;
                                    g_panes[g_focus].streaming=0;
                                    g_panes[g_focus].follow=1;   /* pin to bottom for the new turn */
                                    g_panes[g_focus].unread=0;
                                    { puci_status _st = puci_session_begin_turn(g_panes[g_focus].sess,pl); if(_st!=PUCI_OK){ char _em[160]; snprintf(_em,sizeof _em,"begin_turn failed: %s", puci_status_string(_st)); ring_push_typed(&g_panes[g_focus].ring,_em,RL_ERROR); } }
                                    free(pl);
                                }
                            }
                        }
                        g_input_len=0; g_input[0]=0; g_input_curs=0; g_input_mode=0;
                        paste_buf_clear();
                    } else if(c=='\n'){ /* Ctrl+J: insert a newline (multiline) */
                        if(g_input_len+1 < sizeof g_input){
                            memmove(g_input+g_input_curs+1, g_input+g_input_curs, g_input_len - g_input_curs +1);
                            g_input[g_input_curs++]='\n'; g_input_len++;
                        }
                    } else if(c==127 || c==8){
                        if(g_input_curs>0){
                            int dl=utf8_prev_len((const unsigned char*)g_input, g_input_curs);
                            if(dl<1) dl=1;
                            memmove(g_input+g_input_curs-dl, g_input+g_input_curs, g_input_len - g_input_curs +1);
                            g_input_len-=dl; g_input_curs-=dl;
                        }
                    } else if(c==21){ // Ctrl-U clear
                            g_input_len=0; g_input_curs=0; g_input[0]=0;
                            paste_buf_clear();
                        } else if(c==3){ // Ctrl-C: cancel the pending prompt
                            g_input_len=0; g_input[0]=0; g_input_curs=0; g_input_mode=0;
                            paste_buf_clear();
                        } else if(c>=32 && c!=127){ /* printable ASCII or UTF-8 lead byte */
                        int bl = (c>=0x80) ? utf8_seq_len(c) : 1;
                        if(c>=0x80 && bl>1){
                            /* Collect the full UTF-8 sequence. Continuation
                               bytes usually sit at k+1.. but may be split
                               across reads; pull them from the batch or read
                               the remainder directly from stdin. */
                            unsigned char seq[4]; seq[0]=c;
                            int have=1;
                            int need=bl-1;
                            while(have < bl && need>0){
                                if(k+have < n){ seq[have]=buf[k+have]; have++; }
                                else {
                                    char tmp; ssize_t rr=read(STDIN_FILENO,&tmp,1);
                                    if(rr==1){ seq[have]=(unsigned char)tmp; have++; }
                                    else break;
                                }
                                need--;
                            }
                            if(have!=bl || !utf8_valid(seq,(size_t)bl)){
                                /* Invalid: ignore the incomplete sequence. */
                                k += (have>0)?have-1:0;
                            } else {
                                if((size_t)(g_input_len+bl) < sizeof g_input){
                                    memmove(g_input+g_input_curs+bl, g_input+g_input_curs, g_input_len - g_input_curs +1);
                                    memcpy(g_input+g_input_curs, seq, (size_t)bl);
                                    g_input_curs += (size_t)bl; g_input_len += bl;
                                }
                                k += bl-1; /* skip continuation bytes */
                            }
                        } else {
                            /* printable ASCII */
                            if(g_input_len+1 < sizeof g_input){
                                memmove(g_input+g_input_curs+1, g_input+g_input_curs, g_input_len - g_input_curs +1);
                                g_input[g_input_curs++]=(char)c; g_input_len++;
                            }
                        }
                    }
                } else {
                    /* While inside a bracketed paste, pasted bytes are data: buffer them into
                       the paste block instead of acting on them as command
                       keys (q/m/s/... ) or navigation. Newlines are preserved
                       (that is the point of a multi-line paste); a single CR
                       from CRLF is stripped. A paste too large is refused
                       rather than silently truncated. */
                    if(g_paste){
                        if(c==8||c==127){ if(g_paste_len>0) g_paste_len--; }
                        else if(c==13){ /* CR of a CRLF pair: drop, keep LF */ }
                        else if(c==10 || (c>=0x20)){
                            if(paste_buf_append((const char*)&c,1)!=0){
                                ring_push_typed(&g_panes[g_focus].ring,
                                    "paste too large (refused before truncation)", RL_ERROR);
                            }
                        }
                        if(!g_input_mode){ g_input_mode=1; g_hist_pos=g_hist_count; g_hist_tmp[0]=0; }
                        continue;
                    }
                    if(g_approval_pending){
                        /* Modal approval overlay: all keys are scoped to the
                           decision. q/ESC deny (cancel semantics), y allow
                           once, A allow always, n deny, d deny-with-reason,
                           j/k scroll. The decision is resolved asynchronously
                           so the main poll() loop is never blocked. */
                        if(c==3){ /* Ctrl-C: cancel -> deny wins */
                            if(g_approval_pane>=0 && g_panes[g_approval_pane].sess){
                                TRACE(TR_APPROVAL,2);
                                puci_session_resolve_approval(g_panes[g_approval_pane].sess, g_approval_id, PUCI_APPROVAL_DENY);
                            }
                            g_approval_pending=0; g_approval_pane=-1;
                        }
                        else if(c==27){ /* ESC: deny (safety default) */
                            if(g_approval_pane>=0 && g_panes[g_approval_pane].sess){
                                TRACE(TR_APPROVAL,3);
                                puci_session_resolve_approval(g_panes[g_approval_pane].sess, g_approval_id, PUCI_APPROVAL_DENY);
                            }
                            g_approval_pending=0; g_approval_pane=-1;
                        }
                        else if(g_approval_feedback_mode){
                            /* Collecting a denial reason (safe printable
                               text only; no command keys accepted). */
                            if(c=='\n'||c=='\r'){
                                g_approval_feedback[g_approval_feedback_len]=0;
                                if(g_approval_pane>=0 && g_panes[g_approval_pane].sess){
                                    if(g_approval_feedback[0]) puci_session_set_approval_feedback(g_panes[g_approval_pane].sess, g_approval_feedback);
                                    TRACE(TR_APPROVAL,4);
                                    puci_session_resolve_approval(g_panes[g_approval_pane].sess, g_approval_id, PUCI_APPROVAL_DENY);
                                }
                                g_approval_pending=0; g_approval_pane=-1;
                            } else if(c==8||c==127){
                                if(g_approval_feedback_len>0) g_approval_feedback[--g_approval_feedback_len]=0;
                            } else if(c==27){
                                g_approval_feedback_mode=0; g_approval_feedback_len=0; g_approval_feedback[0]=0;
                            } else if(c>=32 && c<127){
                                if(g_approval_feedback_len+1 < sizeof g_approval_feedback){ g_approval_feedback[g_approval_feedback_len++]=(char)c; }
                            }
                        }
                        else if(c=='y'||c=='Y'){ /* allow once */
                            if(g_approval_pane>=0 && g_panes[g_approval_pane].sess){
                                TRACE(TR_APPROVAL,5);
                                puci_session_resolve_approval(g_panes[g_approval_pane].sess, g_approval_id, PUCI_APPROVAL_ALLOW_ONCE);
                            }
                            g_approval_pending=0; g_approval_pane=-1;
                        }
                        else if(c=='a'||c=='A'){ /* allow always for this tool */
                            if(g_approval_pane>=0 && g_panes[g_approval_pane].sess){
                                TRACE(TR_APPROVAL,6);
                                puci_session_resolve_approval(g_panes[g_approval_pane].sess, g_approval_id, PUCI_APPROVAL_ALLOW_ALWAYS);
                            }
                            g_approval_pending=0; g_approval_pane=-1;
                        }
                        else if(c=='n'||c=='N'||c=='q'){ /* deny (default policy) */
                            if(g_approval_pane>=0 && g_panes[g_approval_pane].sess){
                                TRACE(TR_APPROVAL,7);
                                puci_session_resolve_approval(g_panes[g_approval_pane].sess, g_approval_id, PUCI_APPROVAL_DENY);
                            }
                            g_approval_pending=0; g_approval_pane=-1;
                        }
                        else if(c=='d'||c=='D'){ /* deny with a reason */
                            g_approval_feedback_mode=1; g_approval_feedback_len=0; g_approval_feedback[0]=0;
                        }
                        else if(c=='j'){ if(g_hunk_off < g_hunk_max-1) g_hunk_off++; }
                        else if(c=='k'){ if(g_hunk_off>0) g_hunk_off--; }
                        continue;
                    }
                    if(c==3){ // Ctrl-C: cancel in-flight turn, else exit
                        Pane *pp=&g_panes[g_focus];
                        int cancelled=0;
                        if(pp->sess && pp->streaming){ puci_session_cancel(pp->sess); pp->streaming=0; cancelled=1; }
                        if(!cancelled){ running=0; break; }
                    }
                    else if(c=='q'){ running=0; break; }
                    else if(c=='\t'){ g_focus=(g_focus+1)%g_npane; }
                    else if(c=='s'){ if(g_mode==2){ g_mode=0; g_pick_filter[0]=0; g_show_sidebar=0; } else { load_sessions(); picker_open(2); } }
                    else if(c=='m'){ models_init_static(); picker_open(3); fetch_live_models(); if(g_mode==3) rebuild_pick_filter(); }
                    else if(c=='\n' || c=='\r'){ g_input_mode=1; g_input_len=0; g_input[0]=0; g_input_curs=0; g_hist_pos=g_hist_count; g_hist_tmp[0]=0; }
                    else if(c=='j'){ Pane *pp=&g_panes[g_focus]; if(pp->scroll_off>0){ pp->scroll_off-=1; if(pp->scroll_off<0) pp->scroll_off=0; if(pp->scroll_off==0){ pp->follow=1; pp->unread=0; } } }
                    else if(c=='k'){ Pane *pp=&g_panes[g_focus]; pp->scroll_off+=1; pp->follow=0; }
                    else if(c=='g'){ Pane *pp=&g_panes[g_focus]; pp->scroll_off=9999; pp->follow=0; }
                    else if(c=='G'){ Pane *pp=&g_panes[g_focus]; pp->scroll_off=0; pp->follow=1; pp->unread=0; }
                    else if(c=='?'){
                        ring_push_typed(&g_panes[g_focus].ring,"Keys: Enter prompt  ↑↓/j k/PgUpDn scroll  s sessions  m model  TAB pane  q or Ctrl+C quit  g/G top/bottom", RL_SYSTEM);
                    }
                }
            }
            if(had_key) dirty=1;
        }
        int had_event=0;
        for(int i=0;i<g_npane;i++) if(g_panes[i].sess){
            int _off=pane_off[i]; int _cnt=pane_cnt[i];
            const struct pollfd *_pfds=NULL; size_t _pcnt=0;
            if(_off>=1 && _cnt>0){ _pfds=fds+_off; _pcnt=(size_t)_cnt; }
            TRACE(TR_PROCESS, (uint32_t)i);
            puci_status _pst = puci_session_process(g_panes[i].sess, _pfds, _pcnt);
            if(_pst!=PUCI_OK){
                char _em[180]; snprintf(_em,sizeof _em,"⚠ session %d: %s", i, puci_status_string(_pst));
                ring_push_typed(&g_panes[i].ring, _em, RL_ERROR);
                had_event=1;
            }
            puci_event ev;
            while(puci_session_next_event(g_panes[i].sess,&ev)){
                had_event=1;
                TRACE(TR_EVENT, (uint32_t)ev.text_len);
                if(ev.kind==PUCI_EVENT_TOKEN_DELTA && ev.text){
                    pane_stream_append(&g_panes[i], ev.text, ev.text_len);
                } else if(ev.kind==PUCI_EVENT_REASONING_DELTA && ev.text){
                    /* Raw reasoning is hidden by default: it is not a normal
                       assistant reply. We only record that a reasoning phase
                       is happening so an opt-in "Thought for Ns" notice can be
                       shown; the raw bytes never reach the Ring. */
                    (void)ev.text;
                    Pane *p=&g_panes[i];
                    if(!p->thinking){
                        p->thinking=1;
                        p->think_start_ns=(int64_t)trace_now_mono_ns();
                    }
                } else if(ev.kind==PUCI_EVENT_TOOL_CALL_START){
                    pane_stream_end(&g_panes[i]);
                    char tmp[256]; snprintf(tmp,sizeof tmp,"🔧 %s %s", ev.payload.tool_call.name?ev.payload.tool_call.name:"?", ev.payload.tool_call.args_json?ev.payload.tool_call.args_json:"");
                    if(strlen(tmp)> 200) tmp[200]=0;
                    ring_push_typed(&g_panes[i].ring, tmp, RL_TOOL);
                    if(g_panes[i].scroll_off==0) { /* keep bottom */ }
                } else if(ev.kind==PUCI_EVENT_TOOL_CALL_END){
                    char tmp[512]; snprintf(tmp,sizeof tmp,"→ %.*s", (int)(ev.payload.tool_result.content? (int)strlen(ev.payload.tool_result.content):0), ev.payload.tool_result.content?ev.payload.tool_result.content:"");
                    if(strlen(tmp)> 400) { tmp[400]=0; strcat(tmp,"…"); }
                    ring_push_typed(&g_panes[i].ring, tmp, RL_TOOL);
                } else if(ev.kind==PUCI_EVENT_TURN_COMPLETE){
                    pane_stream_end(&g_panes[i]);
                    /* Compact, optional reasoning notice when a reasoning
                       phase ran — one line, never the raw reasoning text. */
                    if(g_panes[i].thinking){
                        int64_t now=(int64_t)trace_now_mono_ns();
                        int64_t ms = now - g_panes[i].think_start_ns;
                        if(ms < 0) ms = 0;
                        double s = (double)ms / 1e9;
                        char rn[96];
                        snprintf(rn,sizeof rn,"\xE2\x96\xB8 Thought for %.1fs", s);
                        ring_push_typed(&g_panes[i].ring, rn, RL_SYSTEM);
                        g_panes[i].thinking=0; g_panes[i].think_start_ns=0;
                    }
                    /* Completion is signalled by the header returning to
                       Ready — no "— done —" clutter in the transcript. */
                    // autosave to stable per-pane file
                    {
                        const char *sid = g_panes[i].session_id[0]?g_panes[i].session_id:"tui-autosave";
                        TRACE(TR_SAVE,0); puci_cli_save_session(g_panes[i].sess, sid);
                    }
                    if(g_panes[i].follow){ g_panes[i].unread=0; g_panes[i].scroll_off=0; }
                } else if(ev.kind==PUCI_EVENT_TURN_CANCELLED){
                    pane_stream_end(&g_panes[i]);
                    g_panes[i].thinking=0; g_panes[i].think_start_ns=0;
                    ring_push_typed(&g_panes[i].ring,"— cancelled —", RL_ERROR);
                } else if(ev.kind==PUCI_EVENT_APPROVAL_REQUEST){
                    /* Non-blocking: record the pending overlay; the main loop
                       keeps running (other panes stream, resize, timers). */
                    if(g_approval_pending){
                        /* Safety: a stale dialog's allow must never fire a
                           tool. Invalidate it explicitly, then show the new. */
                        if(g_approval_pane>=0 && g_panes[g_approval_pane].sess){
                            TRACE(TR_APPROVAL,1);
                            puci_session_resolve_approval(g_panes[g_approval_pane].sess,
                                                          g_approval_id,
                                                          PUCI_APPROVAL_DENY);
                        }
                        g_approval_pending=0;
                    }
                    tui_open_approval(i, &ev.payload.approval);
                    ring_push_typed(&g_panes[i].ring, "🔐 tool awaiting approval — y allow · A always · n deny", RL_SYSTEM);
                } else if(ev.kind==PUCI_EVENT_APPROVAL_RESOLVED){
                    if(g_approval_pending && g_approval_id==ev.payload.approval_resolved.id){
                        g_approval_pending=0; g_approval_pane=-1;
                    }
                } else if(ev.kind==PUCI_EVENT_ERROR && ev.text){
                    pane_stream_end(&g_panes[i]);
                    char tmp[512]; snprintf(tmp,sizeof tmp,"⚠ %.*s",(int)ev.text_len, ev.text);
                    ring_push_typed(&g_panes[i].ring, tmp, RL_ERROR);
                }
            }
        }
        if(had_event){ dirty=1; for(int _di=0;_di<g_npane;_di++) g_panes[_di].layout_dirty=1; }
        free(fds);
    }
    for(int i=0;i<MAX_PANES;i++) if(g_panes[i].sess) puci_session_destroy(g_panes[i].sess);
    puci_context_destroy(ctx);
    if(!headless) tui_teardown();
    trace_dump();
    return 0;
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#pragma GCC diagnostic pop
