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
    TR_POLL_RET=0, TR_PROCESS, TR_EVENT, TR_RENDER_BEG, TR_RENDER_END, TR_PRESENT, TR_PROCFS, TR_SAVE, TR_ESCWAIT
} trace_phase_t;
static const char *trace_phase_name[] = {"POLL_RET","PROCESS","EVENT","RENDER_BEG","RENDER_END","PRESENT","PROCFS","SAVE","ESCWAIT"};
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
        const char *pn = r->phase < 9 ? trace_phase_name[r->phase] : "UNKNOWN";
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
static struct sigaction g_old_winch;
static void on_winch(int s){ (void)s; g_winch=1; }
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
    int cached_inner_w; // to invalidate on resize
    char stream_buf[STREAM_BUF];
    size_t stream_len;
    int scroll_off; // 0=bottom, >0 scrolled up (visual lines)
    int pending_nl;
    int need_wrap_recalc;
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
static int g_approval_pending=0;
static char g_approval_diff[262144];
static int g_approval_result=0;
static int g_hunk_off=0;
static int g_hunk_max=0;
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
static int g_rows=24, g_cols=80;
static char g_current_model[256]="";

static int g_sess_seq=0;
#define HIST_MAX 200
static char g_hist[HIST_MAX][2048];
static int g_hist_count=0;
static int g_hist_pos=0; // navigation index
static char g_hist_tmp[2048];

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

/* Detached worker thread for the model fetch. Writes only its own private
 * buffers, then signals g_live_done. The main thread pumps the result. */
static char g_live[MAX_MODELS][128];
static int  g_live_cnt = 0;
static _Atomic int g_live_inflight = 0;
static _Atomic int g_live_done     = 0;
static void *models_fetch_thread(void *arg){
    (void)arg;
    int e=0;
    g_live_cnt = fetch_models_into(g_live, MAX_MODELS, &e);
    atomic_store(&g_live_done, 1);
    atomic_store(&g_live_inflight, 0);
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

/* Called every frame from the renderer: if a background fetch finished,
 * install the fresh list into the picker globals. */
static void pump_models_fetch(void){
    if(!atomic_load(&g_live_done)) return;
    if(g_live_cnt>0) models_install(g_live, g_live_cnt);
    g_live_cnt=0;
    atomic_store(&g_live_done, 0);
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
static void copy_to_clipboard(const char *text){
    if(!text||!*text) return;
    const char *cmds[] = {"wl-copy 2>/dev/null","xclip -selection clipboard 2>/dev/null","xsel --clipboard --input 2>/dev/null","pbcopy 2>/dev/null",NULL};
    for(int i=0;cmds[i];i++){
        FILE *f=popen(cmds[i],"w");
        if(f){ fwrite(text,1,strlen(text),f); int rc=pclose(f); if(rc==0) return; }
    }
    char b64out[8192]; size_t out=0;
    static const char b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len=strlen(text);
    for(size_t i=0;i<len;i+=3){
        unsigned a=(unsigned char)text[i];
        unsigned b=(i+1<len)?(unsigned char)text[i+1]:0;
        unsigned c=(i+2<len)?(unsigned char)text[i+2]:0;
        unsigned triple=(a<<16)|(b<<8)|c;
        if(out+4<sizeof b64out){
            b64out[out++]=b64[(triple>>18)&63];
            b64out[out++]=b64[(triple>>12)&63];
            b64out[out++]= (i+1<len)?b64[(triple>>6)&63]:'=';
            b64out[out++]= (i+2<len)?b64[triple&63]:'=';
        }
    }
    b64out[out]=0;
    char osc[8300]; snprintf(osc,sizeof osc,"\033]52;c;%.8190s\007",b64out);
    { ssize_t _w=write(STDOUT_FILENO,osc,strlen(osc)); (void)_w; }
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

static puci_approval tui_approval_cb(const puci_approval_request *req, void *ud){
    (void)ud;
    if(!req) return PUCI_APPROVAL_DENY;
    /* Show the *real* change first. A unified diff (write_file over an existing
     * file) is the ground truth the user must approve; display/args are only a
     * fallback when there is no diff to show. */
    size_t pos=0;
    const char *head = req->tool ? req->tool : "tool";
    size_t hl=strlen(head); if(pos+hl+1 < sizeof g_approval_diff){ memcpy(g_approval_diff+pos, head, hl); pos+=hl; g_approval_diff[pos++]='\n'; }
    pos = app_append(g_approval_diff, sizeof g_approval_diff, pos, req->diff);
    if(!req->diff) pos = app_append(g_approval_diff, sizeof g_approval_diff, pos, req->display?req->display:req->args_json);
    g_approval_diff[pos]=0;
    /* upper bound for the j/k scroll region = number of lines */
    g_hunk_max=1;
    for(size_t i=0;i<pos;i++) if(g_approval_diff[i]=='\n') g_hunk_max++;
    if(g_hunk_max<1) g_hunk_max=1;
    g_approval_pending=1; g_hunk_off=0;
    int cancel_fd = req->cancel?req->cancel->fd:-1;
    extern void tui_render_frame(void);
    int need_redraw=1;
    while(g_approval_pending){
        if(req->cancel && req->cancel->flag && *req->cancel->flag) { g_approval_pending=0; return PUCI_APPROVAL_DENY; }
        /* Render once up front and only re-render when something changes, so an
           idle approval prompt doesn't repaint the whole screen every poll. */
        if(need_redraw){ tui_render_frame(); need_redraw=0; }
        struct pollfd pf[2]; int nf=1;
        pf[0].fd=STDIN_FILENO; pf[0].events=POLLIN;
        if(cancel_fd>=0){ pf[1].fd=cancel_fd; pf[1].events=POLLIN; nf=2; }
        int pr=poll(pf,(nfds_t)nf,300);
        if(pr<0){ if(errno==EINTR) continue; else { struct timespec _ts={0,50000000L}; nanosleep(&_ts,NULL); continue; } }
        if(pr==0){ need_redraw=1; continue; }
        if(nf==2 && (pf[1].revents&POLLIN)) { g_approval_pending=0; return PUCI_APPROVAL_DENY; }
        if(pf[0].revents&POLLIN){
            char c; ssize_t n=read(STDIN_FILENO,&c,1);
            if(n==1){
                if(c=='y'||c=='Y'){ g_approval_result=1; g_approval_pending=0; return PUCI_APPROVAL_ALLOW; }
                if(c=='n'||c=='N'||c=='q'||c==27){ g_approval_result=-1; g_approval_pending=0; return PUCI_APPROVAL_DENY; }
                if(c=='j'){ if(g_hunk_off < g_hunk_max-1) g_hunk_off++; need_redraw=1; }
                if(c=='k' && g_hunk_off>0){ g_hunk_off--; need_redraw=1; }
            }
        }
        if(g_winch){ g_winch=0; get_winsize(&g_rows,&g_cols); need_redraw=1; }
    }
    return g_approval_result==1?PUCI_APPROVAL_ALLOW:PUCI_APPROVAL_DENY;
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
static void tb_draw_gauge(int r,int c,int w,long cur,long max,const char *label){
    char bar[512];
    if(w > g_cols) w = g_cols;           /* never draw wider than the terminal */
    if(w > (int)sizeof bar - 1) w = (int)sizeof bar - 1;
    char info[40]; snprintf(info,sizeof info," %ld/%ld",cur,max);
    int infolen=(int)strlen(info);
    int llen = label ? (int)strlen(label) : 0;
    /* Budget: label + '[' + bar + ']' + trailing info must fit within w. */
    int bar_w = w - llen - 2 - infolen;
    if(bar_w < 1){ return; }             /* not enough room to render a bar */
    int fill = max>0? (int)((cur*(long)bar_w)/max):0; if(fill<0) fill=0; if(fill>bar_w) fill=bar_w;
    int pos=0;
    if(label){ for(int i=0;label[i] && pos<(int)sizeof bar-1;i++) bar[pos++]=label[i]; }
    if(pos<(int)sizeof bar-1) bar[pos++]='[';
    for(int i=0;i<bar_w && pos<(int)sizeof bar-1;i++) bar[pos++]= i<fill ? '#' : '-';
    if(pos<(int)sizeof bar-1) bar[pos++]=']';
    bar[pos]=0;
    tb_print(c, r, TB_WHITE, TB_DEFAULT, bar);
    tb_print(c+pos, r, TB_WHITE, TB_DEFAULT, info);
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

void tui_render_frame(void){
    TRACE(TR_RENDER_BEG,0);
    pump_models_fetch();          /* merge a finished background model fetch */
    if(g_headless){
        // Headless simulation: still compute layout CPU but skip terminal draws
        int rows=g_rows, cols=g_cols;
        int usable_rows = rows-3; if(usable_rows<1) usable_rows=1;
        int pane_h = usable_rows/2; if(pane_h<1) pane_h=1;
        int pane_w = (cols-3)/2; if(pane_w<1) pane_w=1;
        for(int i=0;i<g_npane && i<MAX_PANES;i++){
            int inner_h = pane_h-2; int inner_w = pane_w-2; if(inner_h<=0||inner_w<=0) continue;
            int total_vis;
            if(!g_panes[i].layout_dirty && g_panes[i].cached_inner_w==inner_w && g_panes[i].cached_total_vis>=0) total_vis=g_panes[i].cached_total_vis;
            else { total_vis=0; for(int k=0;k<g_panes[i].ring.count;k++){ int idx=(g_panes[i].ring.head + k)%RING_LINES; total_vis += wrap_visual_count(g_panes[i].ring.lines[idx], inner_w); } g_panes[i].cached_total_vis=total_vis; g_panes[i].cached_inner_w=inner_w; g_panes[i].layout_dirty=0; }
            // simulate scan without tb draws
            (void)total_vis;
        }
        TRACE(TR_RENDER_END,0); TRACE(TR_PRESENT,0); return;
    }
    int rows=g_rows, cols=g_cols;
    tb_clear();
    long rss=read_rss_kb(); long pss=read_pss_kb();
    long ctx_cur=0, ctx_bud=131072;
    if(g_npane>0 && g_panes[0].sess){ ctx_cur=(long)puci_session_context_bytes(g_panes[0].sess); ctx_bud=(long)puci_session_get_context_budget(g_panes[0].sess); }
    char hdr[320];
    snprintf(hdr,sizeof hdr," PUCI v0.5.0 — Swarm %d/%d — %ld kB (PSS %ld) — %ld/%ld ctx ", g_npane, MAX_PANES, rss, pss, ctx_cur, ctx_bud);
    for(int x=0;x<cols;x++) tb_set_cell(x, 0, ' ', TB_WHITE|TB_BOLD, TB_BLUE);
    tb_print(0, 0, TB_WHITE|TB_BOLD, TB_BLUE, hdr);
    // model indicator top-right
    {
        const char *m = g_current_model[0]?g_current_model:getenv("PUCI_MODEL"); if(!m||!*m) m="deepseek/deepseek-chat";
        char mod[64]; snprintf(mod,sizeof mod," %.61s ", m);
        int ml=(int)strlen(mod);
        int mx=cols-ml-1; if(mx>0) tb_print(mx,0, TB_WHITE, TB_BLUE, mod);
    }

    int usable_rows = rows-3;
    if(usable_rows<4) usable_rows=4;
    int pane_h = usable_rows/2; if(pane_h<4) pane_h=4;
    int pane_w = (cols-3)/2; if(pane_w<20) pane_w=20;

    for(int i=0;i<g_npane && i<MAX_PANES;i++){
        int pr = 1 + (i/2)*pane_h;
        int pc = (i%2)*(pane_w+1);
        if(g_show_sidebar && i>=2) continue;
        int is_focus = (g_focus==i && g_mode==0);
        tb_draw_box(pr, pc, pane_h, pane_w, g_panes[i].title, is_focus);
        int inner_h = pane_h-2; int inner_w = pane_w-2;
        if(inner_h<=0||inner_w<=0) continue;
        int total_vis;
        if(!g_panes[i].layout_dirty && g_panes[i].cached_inner_w==inner_w && g_panes[i].cached_total_vis>=0){
            total_vis=g_panes[i].cached_total_vis;
        } else {
            total_vis=0;
            for(int k=0;k<g_panes[i].ring.count;k++){
                int idx=(g_panes[i].ring.head + k)%RING_LINES;
                total_vis += wrap_visual_count(g_panes[i].ring.lines[idx], inner_w);
            }
            g_panes[i].cached_total_vis=total_vis;
            g_panes[i].cached_inner_w=inner_w;
            g_panes[i].layout_dirty=0;
        }
        // scroll_off is visual lines above bottom
        if(g_panes[i].scroll_off > total_vis) g_panes[i].scroll_off = total_vis;
        if(g_panes[i].scroll_off <0) g_panes[i].scroll_off=0;
        int start_vis = total_vis - inner_h - g_panes[i].scroll_off;
        if(start_vis<0) start_vis=0;
        int cur_vis=0;
        int rendered=0;
        for(int k=0;k<g_panes[i].ring.count && rendered<inner_h;k++){
            int idx=(g_panes[i].ring.head + k)%RING_LINES;
            const char *s=g_panes[i].ring.lines[idx];
            rtype_t t=g_panes[i].ring.types[idx];
            int wrapped = wrap_visual_count(s, inner_w);
            int seg_off=0;
            for(int w=0; w<wrapped && rendered<inner_h; w++){
                if(cur_vis < start_vis){ cur_vis++; /* still advance seg_off */ 
                    int rem2=(int)strlen(s)-seg_off;
                    if(rem2>0){ int take2=(int)render_segment(s+seg_off, inner_w); if(take2<1||take2>rem2) take2=rem2; seg_off += take2; }
                    continue;
                }
                int rem=(int)(strlen(s)-seg_off);
                if(rem<=0){
                    /* Empty logical line: it still counts as one visual row
                       (wrap_visual_count returns 1), so render a blank spacer
                       row to keep scroll math consistent. */
                    uint32_t fg=TB_WHITE;
                    if(t==RL_USER) fg=TB_CYAN|TB_BOLD;
                    else if(t==RL_TOOL) fg=TB_YELLOW;
                    else if(t==RL_ERROR) fg=TB_RED|TB_BOLD;
                    tb_print(pc+1, pr+1+rendered, fg, TB_DEFAULT, "");
                    rendered++; cur_vis++;
                    break;
                }
                int take;
                if(uwidth(s+seg_off) <= inner_w){ take = rem; }
                else { take = (int)render_segment(s+seg_off, inner_w); if(take<1||take>rem) take=rem; }
                char seg[ RING_COLS ];
                memcpy(seg, s+seg_off, (size_t)take); seg[take]=0;
                seg_off += take;
                // trim trailing spaces for visual
                uint32_t fg=TB_WHITE;
                if(t==RL_USER) fg=TB_CYAN|TB_BOLD;
                else if(t==RL_TOOL) fg=TB_YELLOW;
                else if(t==RL_ERROR) fg=TB_RED|TB_BOLD;
                else if(t==RL_ASSISTANT) fg=TB_WHITE;
                // prefix for continuation lines
                // remove leading spaces
                if(t==RL_ASSISTANT || t==RL_TOOL){
                    tb_print_styled(pc+1, pr+1+rendered, fg, TB_DEFAULT, seg, inner_w);
                } else {
                    tb_print(pc+1, pr+1+rendered, fg, TB_DEFAULT, seg);
                }
                rendered++; cur_vis++;
            }
        }
        // scroll indicator
        if(total_vis > inner_h){
            char ind[16]; snprintf(ind,sizeof ind," %d%% ", (int)((start_vis*100)/ (total_vis - inner_h +1)));
            tb_print(pc+pane_w-6, pr, TB_WHITE, TB_BLUE, ind);
        }
    }
    if(g_show_sidebar || g_mode==2){
        int sr=1, sc= cols-26, sh= usable_rows, sw=25;
        if(sc<0) sc=0;
        if(sw>cols) sw=cols;
        char stitle[64]; if(g_mode==2 && g_pick_filter[0]) snprintf(stitle,sizeof stitle,"Sessions /%.18s", g_pick_filter); else snprintf(stitle,sizeof stitle,"Sessions (%d/%d) s", g_pick_filtered_count, g_sess_count);
        tb_draw_box(sr,sc,sh,sw,stitle, g_mode==2);
        int cnt = (g_mode==2? g_pick_filtered_count : g_sess_count);
        /* Viewport: keep the highlighted row visible in the sidebar. */
        int svis = sh-2; if(svis<1) svis=1;
        int stop = g_pick_sel - (svis-1); if(stop<0) stop=0;
        for(int i=stop;i<cnt && i<stop+svis;i++){
            uint32_t fg = (i==g_pick_sel && g_mode==2)? TB_BLACK:TB_WHITE;
            uint32_t bg = (i==g_pick_sel && g_mode==2)? TB_CYAN:TB_DEFAULT;
            int src = (g_mode==2? g_pick_filtered_idx[i] : i);
            char ln[64]; snprintf(ln,sizeof ln,"%.12s %.10s", g_sess_list[src].id, g_sess_list[src].model);
            // highlight
            for(int x=sc+1;x<sc+sw-1;x++) tb_set_cell(x,sr+1+(i-stop),' ',fg,bg);
            tb_print(sc+2, sr+1+(i-stop), fg, bg, ln);
        }
        if(cnt==0) tb_print(sc+2,sr+1, TB_YELLOW, TB_DEFAULT, g_pick_filter[0]?"(no match)":"(no sessions)");
        if(g_mode==2) tb_print(sc+2,sr+sh-1, TB_CYAN, TB_DEFAULT, "type filter  Ctrl-Y copy  Enter load");
    }
    tb_draw_gauge(rows-2, 0, cols, ctx_cur, ctx_bud, "Context");
    const char *footer_normal="[Enter] prompt  [↑/↓] scroll  [PgUp/Dn]  [s] sessions  [m] model  [TAB] pane  [q] quit";
    const char *footer_input="[Enter] send  [Esc] cancel  [←→] cursor  [m] model in picker";
    const char *foot = g_input_mode ? footer_input : footer_normal;
    tb_print(0, rows-1, TB_WHITE, TB_DEFAULT, foot);
    // input line overlay
    if(g_input_mode){
        char prompt[2600]; snprintf(prompt,sizeof prompt,"❯ %s", g_input);
        // clear line
        for(int x=0;x<cols;x++) tb_set_cell(x, rows-1, ' ', TB_BLACK, TB_WHITE);
        tb_print(0, rows-1, TB_BLACK, TB_WHITE, prompt);
        // cursor (display column, not byte offset, so multi-byte glyphs place
                // the caret correctly)
                char _curs_buf[256]; size_t _cl=g_input_curs; if(_cl>255) _cl=255;
                memcpy(_curs_buf,g_input,_cl); _curs_buf[_cl]=0;
                int cx = 2 + uwidth(_curs_buf);
                if(cx<0) cx=0;
                if(cx>=cols) cx=cols-1;
                tb_set_cursor(cx, rows-1);
    } else {
        tb_hide_cursor();
    }
    if(g_approval_pending){
        int oh=14, ow=68;
        int or_ = rows/2 - 7; if(or_<1) or_=1;
        int oc= cols/2 - 34; if(oc<0) oc=0;
        if(or_ >= rows-2) or_ = rows>3 ? rows-3 : 1;
        if(oc >= cols-2) oc = cols>3 ? cols-3 : 0;
        int maxh = rows - or_ - 1; if(maxh<3) maxh=3; if(oh>maxh) oh=maxh;
        int maxw = cols - oc - 1; if(maxw<8) maxw=8; if(ow>maxw) ow=maxw;
        for(int y=or_; y<or_+oh; y++) for(int x=oc; x<oc+ow; x++) tb_set_cell(x,y,' ', TB_WHITE, TB_BLUE);
        tb_draw_box(or_,oc,oh,ow,"Approval — y=allow n=deny j/k scroll",1);
        char *copy=strdup(g_approval_diff); if(!copy){ /* allocation failure: skip overlay */ } else { char *save=NULL; char *tok=strtok_r(copy,"\n",&save); int l=0;
        while(tok){
            if(l>=g_hunk_off && l < g_hunk_off+oh-2){
                int yy = or_+1+(l-g_hunk_off);
                if(yy >= or_+1 && yy < or_+oh-1){
                    uint32_t fg = TB_WHITE;
                    if(tok[0]=='+') fg = TB_GREEN|TB_BOLD;
                    else if(tok[0]=='-') fg = TB_RED|TB_BOLD;
                    char line[96]; snprintf(line,sizeof line,"%.*s", ow>2?ow-2:0, tok);
                    tb_print(oc+1, yy, fg, TB_BLUE, line);
                }
            }
            l++; tok=strtok_r(NULL,"\n",&save);
        }
        free(copy); }
    }
    if(g_mode==3){
        int oh=16, ow=54;
        int or_ = rows/2 - 8; if(or_<1) or_=1;
        int oc= cols/2 - 27; if(oc<0) oc=0;
        if(or_ >= rows-2) or_ = rows>3 ? rows-3 : 1;
        if(oc >= cols-2) oc = cols>3 ? cols-3 : 0;
        int maxh = rows - or_ - 1; if(maxh<3) maxh=3; if(oh>maxh) oh=maxh;
        int maxw = cols - oc - 1; if(maxw<8) maxw=8; if(ow>maxw) ow=maxw;
        for(int y=or_; y<or_+oh; y++) for(int x=oc; x<oc+ow; x++) tb_set_cell(x,y,' ', TB_WHITE, TB_BLUE);
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
                    tb_print(oc+2, yy, TB_WHITE, TB_BLUE, k_models[src]);
                }
            }
        }
        if(g_pick_filtered_count==0) tb_print(oc+2,or_+1, TB_YELLOW, TB_BLUE, "(no match)");
        tb_print(oc+2,or_+oh-1, TB_CYAN, TB_BLUE, "type filter  Ctrl-Y copy  Esc cancel");
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
    if(p->pending_nl > 0){
        /* (pending-1) blank separator lines, then the content on a fresh line.
           This preserves a\n\nb as "a", "", "b" instead of merging "b" onto
           the blank line. */
        for(int i=0; i < p->pending_nl-1 && i < 64; i++){
            ring_push_typed(&p->ring, "", RL_ASSISTANT);
        }
        p->pending_nl = 0;
        ring_push_typed(&p->ring, "", RL_ASSISTANT);
        ring_append_to_last(&p->ring, s, n);
    } else {
        ring_append_to_last(&p->ring, s, n);
    }
}

static void pane_stream_begin(Pane *p){
    p->streaming=1; p->stream_len=0; p->stream_buf[0]=0; p->pending_nl=0;
    ring_push_typed(&p->ring, "", RL_ASSISTANT);
}

static void pane_stream_append(Pane *p, const char *txt, size_t len){
    if(!p->streaming){
        pane_stream_begin(p);
    }
    // TTG sentinel detection: look for U+2063 (E2 81 A3) markers
    const unsigned char *end = (const unsigned char *)txt + len;
    const unsigned char *a = (const unsigned char *)txt;
    /* Scan within bounds and only accept full 3-byte sentinels. */
    while ((size_t)(end - a) >= 6) {
        if (*a == 0xE2 && memcmp(a, "\xE2\x81\xA3", 3) == 0) {
            const unsigned char *b = a + 3; /* start of digits */
            /* Find the closing sentinel (E2 81 A3), fully validated. */
            const unsigned char *c = b;
            while ((size_t)(end - c) >= 6) {
                if (*c == 0xE2 && memcmp(c, "\xE2\x81\xA3", 3) == 0) break;
                c++;
            }
            if ((size_t)(end - c) >= 6 && (size_t)(c - a) < 64) {
                /* Digits run from b (after first sentinel) to c (second sentinel). */
                size_t dlen = (size_t)(c - b);
                if (dlen > 0 && dlen < 24) {
                    char num[24];
                    memcpy(num, b, dlen); num[dlen] = 0;
                    uint64_t sent = strtoull(num, NULL, 10);
                    if (sent > 0) {
                        uint64_t now = trace_now_mono_ns();
                        int64_t ttg = (int64_t)(now - sent);
                        if (ttg >= 0 && ttg < 1000000000LL) {
                            TRACE(TR_EVENT, (uint32_t)(10000 + ttg / 1000000));
                        }
                    }
                }
                a = c + 3;
                continue;
            }
        }
        a++;
    }
    size_t add = len < STREAM_BUF - p->stream_len -1 ? len : STREAM_BUF - p->stream_len -1;
    memcpy(p->stream_buf + p->stream_len, txt, add); p->stream_len+=add; p->stream_buf[p->stream_len]=0;
    size_t off=0;
    while(off < len){
        const char *nl = (const char*)memchr(txt+off, '\n', len-off);
        size_t clen = nl ? (size_t)(nl - (txt+off)) : (len - off);
        // filter \r inside the segment (handle \r\n)
        char tmp[RING_COLS]; size_t w=0;
        for(size_t k=0;k<clen && w<RING_COLS-1;k++) if(txt[off+k]!='\r') tmp[w++]=txt[off+k];
        if(w>0) stream_append_segment(p, tmp, w);
        if(nl){
            // newline seen: record a pending line break (counted, so consecutive
            // newlines yield consecutive blank logical lines).
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
        /* Materialize trailing empty lines. P=1 trailing newline after content
           closes the current line (no blank); each extra newline emits a blank. */
        if(p->pending_nl > 0){
            for(int i=0; i < p->pending_nl-1 && i < 64; i++){
                ring_push_typed(&p->ring, "", RL_ASSISTANT);
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
    g_npane=1; g_focus=0; g_mode=0;
    for(int i=0;i<MAX_PANES;i++){ memset(&g_panes[i],0,sizeof g_panes[i]); snprintf(g_panes[i].title,sizeof g_panes[i].title,"Agent %c",'A'+i); snprintf(g_panes[i].session_id,sizeof g_panes[i].session_id,"tui-%ld-%d-%d",(long)time(NULL),i,++g_sess_seq); g_panes[i].scroll_off=0; g_panes[i].layout_dirty=1; g_panes[i].cached_inner_w=-1; g_panes[i].cached_total_vis=-1; }
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
        sopts.approve_cb=tui_approval_cb; sopts.approve_userdata=NULL;
        puci_session *s=NULL;
        puci_status _cs = puci_session_create(ctx,&sopts,&s);
        if(_cs==PUCI_OK){
            g_panes[i].sess=s;
            ring_push_typed(&g_panes[i].ring,"— PUCI TUI v0.5 — Enter prompt, ↑↓ scroll, s sessions, m model, q quit —", RL_SYSTEM);
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
    { const char *pn=getenv("PUCI_TUI_PANES"); if(pn && *pn){ int v=atoi(pn); if(v>=1 && v<=MAX_PANES){ g_npane=v; for(int i=1;i<g_npane;i++){ if(!g_panes[i].sess){ puci_session_options sopts={0}; sopts.provider=pcfg; sopts.system_prompt="test"; sopts.approve_cb=tui_approval_cb; if(puci_session_create(ctx,&sopts,&g_panes[i].sess)==PUCI_OK){ ring_push_typed(&g_panes[i].ring,"— test pane —", RL_SYSTEM); } } } } } }
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
        if(g_winch){ g_winch=0; get_winsize(&g_rows,&g_cols); dirty=1; for(int _di=0;_di<MAX_PANES;_di++){ g_panes[_di].layout_dirty=1; g_panes[_di].cached_inner_w=-1; } }
        if(exit_after_ms>0){ struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now); long el=(now.tv_sec-tui_start.tv_sec)*1000 + (now.tv_nsec-tui_start.tv_nsec)/1000000; if(el>=exit_after_ms) running=0; }
        {
            struct timespec ng; clock_gettime(CLOCK_MONOTONIC,&ng);
            long ge = (ng.tv_sec-last_gauge.tv_sec)*1000 + (ng.tv_nsec-last_gauge.tv_nsec)/1000000;
            if(ge >= 500){ dirty=1; last_gauge=ng; }
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
        } else poll_to = gauge_due < 250 ? gauge_due : 250;
        if(poll_to < 0) poll_to = 1;
        if(poll_to < 1) poll_to = 1;
        int pr=poll(fds,(nfds_t)nf,poll_to);
        {
            int backlog=0;
            if(g_trace_enabled) { ioctl(STDIN_FILENO, FIONREAD, &backlog); }
            TRACE(TR_POLL_RET, (uint32_t)(pr<0?0:backlog));
        }
        if(pr<0){ free(fds); if(errno==EINTR) continue; else { struct timespec _ts={0,20000000L}; nanosleep(&_ts,NULL); continue; } }
        if(pr==0){
            struct timespec ng3; clock_gettime(CLOCK_MONOTONIC,&ng3);
            long ge2 = (ng3.tv_sec-last_gauge.tv_sec)*1000 + (ng3.tv_nsec-last_gauge.tv_nsec)/1000000;
            if(ge2 >= 500) dirty=1;
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
                        if(memcmp(buf+k, "\x1b[200~", 6)==0){ g_paste=1; k+=5; continue; }
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
                        if(code=='A'){ if(g_input_mode) history_prev(); else if(g_mode==2||g_mode==3){ if(g_pick_sel>0) g_pick_sel--; } else { Pane *pp=&g_panes[g_focus]; pp->scroll_off+=2; } }
                        else if(code=='B'){ if(g_input_mode) history_next(); else if(g_mode==2||g_mode==3){ if(g_pick_sel < g_pick_filtered_count-1) g_pick_sel++; } else { Pane *pp=&g_panes[g_focus]; if(pp->scroll_off>0) pp->scroll_off-=2; } }
                        else if(code=='C'){ if(g_input_mode && g_input_curs < g_input_len){ size_t el=utf8_seq_len((unsigned char)g_input[g_input_curs]); if(el+g_input_curs>g_input_len) el=g_input_len-g_input_curs; g_input_curs+=el; } }
                        else if(code=='D'){ if(g_input_mode && g_input_curs>0){ int dl=utf8_prev_len((const unsigned char*)g_input, g_input_curs); g_input_curs-=dl; } }
                        else if(code=='5' && seq_len>=3 && seq[2]=='~'){ if(g_input_mode) history_prev(); else { Pane *pp=&g_panes[g_focus]; pp->scroll_off+=10; } }
                        else if(code=='6' && seq_len>=3 && seq[2]=='~'){ if(g_input_mode) history_next(); else { Pane *pp=&g_panes[g_focus]; pp->scroll_off-=10; if(pp->scroll_off<0) pp->scroll_off=0; } }
                        else if(code=='H'){ if(g_input_mode) g_input_curs=0; else { Pane *pp=&g_panes[g_focus]; pp->scroll_off=9999; } }
                        else if(code=='F'){ if(g_input_mode) g_input_curs=g_input_len; else { Pane *pp=&g_panes[g_focus]; pp->scroll_off=0; } }
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
                                pp->streaming=0; pp->pending_nl=0; pp->scroll_off=0;
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
                            copy_to_clipboard(g_sess_list[src].id);
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
                            copy_to_clipboard(k_models[src]);
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
                    if(c=='\n'||c=='\r'){
                        g_input[g_input_len]=0;
                        if(g_input_len>0){
                            history_save_line(g_input);
                            if(g_input[0]=='/'){
                                if(strcmp(g_input,"/clear")==0){ Pane *pp=&g_panes[g_focus]; if(pp->sess) puci_session_cancel(pp->sess); pp->streaming=0; pp->pending_nl=0; pp->ring.count=0; pp->ring.head=0; pp->scroll_off=0; if(pp->sess) puci_session_clear(pp->sess); ring_push_typed(&pp->ring,"— cleared —", RL_SYSTEM); }
                                else if(strcmp(g_input,"/help")==0 || strcmp(g_input,"/?")==0){ ring_push_typed(&g_panes[g_focus].ring,"/clear /help /model [name] /sessions /new /compact /doctor /plan", RL_SYSTEM); ring_push_typed(&g_panes[g_focus].ring,"Keys: Enter send  Esc cancel  ↑↓ history/scroll  s sessions  m model  g/G top/bottom", RL_SYSTEM); }
                                else if(strncmp(g_input,"/model",6)==0 && (g_input[6]=='\0' || g_input[6]==' ')){ char *arg=g_input+6; while(*arg==' ') arg++; if(*arg){ setenv("PUCI_MODEL",arg,1); puci_status _mst=apply_model_to_panes(arg); if(_mst==PUCI_OK){ snprintf(g_current_model,sizeof g_current_model,"%.255s",arg); char msg[160]; snprintf(msg,sizeof msg,"Model → %.120s",arg); ring_push_typed(&g_panes[g_focus].ring,msg,RL_SYSTEM); } else { char _em[200]; snprintf(_em,sizeof _em,"Model %.120s NOT applied (err %s)", arg, puci_status_string(_mst)); ring_push_typed(&g_panes[g_focus].ring,_em,RL_ERROR); } } else { models_init_static(); picker_open(3); fetch_live_models(); if(g_mode==3) rebuild_pick_filter(); } }
                                else if(strcmp(g_input,"/sessions")==0 || strcmp(g_input,"/s")==0){ load_sessions(); picker_open(2); }
                                else if(strcmp(g_input,"/new")==0){ Pane *pp=&g_panes[g_focus]; if(pp->sess){ TRACE(TR_SAVE,1); puci_cli_save_session(pp->sess, pp->session_id); puci_session_cancel(pp->sess); puci_session_clear(pp->sess); } pp->streaming=0; pp->pending_nl=0; pp->ring.count=0; pp->ring.head=0; pp->scroll_off=0; snprintf(pp->session_id,sizeof pp->session_id,"tui-%ld-%d-%d",(long)time(NULL),g_focus,++g_sess_seq); ring_push_typed(&pp->ring,"— new session —", RL_SYSTEM); }
                                else if(strcmp(g_input,"/compact")==0){ Pane *pp=&g_panes[g_focus]; if(pp->sess){ size_t pr=0; puci_session_compact_context(pp->sess,&pr); char msg[96]; snprintf(msg,sizeof msg,"Compacted %zu bytes",pr); ring_push_typed(&pp->ring,msg,RL_SYSTEM); } }
                                else if(strcmp(g_input,"/doctor")==0){ puci_provider_config *pc=puci_cli_provider("openai"); char msg[256]; snprintf(msg,sizeof msg,"model=%s base=%s", pc?pc->model:"?", pc?pc->base_url:"?"); ring_push_typed(&g_panes[g_focus].ring,msg,RL_SYSTEM); }
                                else if(strcmp(g_input,"/plan")==0){ ring_push_typed(&g_panes[g_focus].ring,"plan mode: use --plan flag for now", RL_SYSTEM); }
                                else { char msg[200]; snprintf(msg,sizeof msg,"Unknown: %s (try /help)", g_input); ring_push_typed(&g_panes[g_focus].ring,msg,RL_ERROR); }
                            } else if(g_panes[g_focus].sess){
                                char u[2048]; snprintf(u,sizeof u,"❯ %s", g_input);
                                ring_push_typed(&g_panes[g_focus].ring, u, RL_USER);
                                g_panes[g_focus].scroll_off=0;
                                g_panes[g_focus].streaming=0;
                                { puci_status _st = puci_session_begin_turn(g_panes[g_focus].sess,g_input); if(_st!=PUCI_OK){ char _em[160]; snprintf(_em,sizeof _em,"begin_turn failed: %s", puci_status_string(_st)); ring_push_typed(&g_panes[g_focus].ring,_em,RL_ERROR); } }
                            }
                        }
                        g_input_len=0; g_input[0]=0; g_input_curs=0; g_input_mode=0;
                    } else if(c==127 || c==8){
                        if(g_input_curs>0){
                            int dl=utf8_prev_len((const unsigned char*)g_input, g_input_curs);
                            if(dl<1) dl=1;
                            memmove(g_input+g_input_curs-dl, g_input+g_input_curs, g_input_len - g_input_curs +1);
                            g_input_len-=dl; g_input_curs-=dl;
                        }
                    } else if(c==21){ // Ctrl-U clear
                        g_input_len=0; g_input_curs=0; g_input[0]=0;
                    } else if(c==3){ // Ctrl-C: cancel the pending prompt
                        g_input_len=0; g_input[0]=0; g_input_curs=0; g_input_mode=0;
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
                    /* While inside a bracketed paste, pasted bytes are data:
                       buffer them into the input line instead of acting on
                       them as command keys (q/m/s/... ) or navigation. */
                    if(g_paste){
                        if(c==8||c==127){ if(g_input_len>0){ g_input[--g_input_len]=0; g_input_curs=g_input_len; } }
                        else if(c!='\n' && c!='\r' && c>=0x20){
                            if(g_input_len+1 < sizeof g_input){ g_input[g_input_len++]=(char)c; g_input[g_input_len]=0; g_input_curs=g_input_len; }
                        }
                        if(!g_input_mode){ g_input_mode=1; g_hist_pos=g_hist_count; g_hist_tmp[0]=0; }
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
                    else if(c=='j'){ Pane *pp=&g_panes[g_focus]; if(pp->scroll_off>0){ pp->scroll_off-=1; if(pp->scroll_off<0) pp->scroll_off=0; }}
                    else if(c=='k'){ Pane *pp=&g_panes[g_focus]; pp->scroll_off+=1; }
                    else if(c=='g'){ Pane *pp=&g_panes[g_focus]; pp->scroll_off=9999; }
                    else if(c=='G'){ Pane *pp=&g_panes[g_focus]; pp->scroll_off=0; }
                    else if(c=='?'){
                        ring_push_typed(&g_panes[g_focus].ring,"Keys: Enter prompt  ↑↓/PgUpDn scroll  s sessions  m model  TAB pane  q quit  g/G top/bottom", RL_SYSTEM);
                    }
                    if(g_approval_pending){
                        if(c=='y'||c=='Y'){ g_approval_result=1; g_approval_pending=0; }
                        else if(c=='n'||c=='N'){ g_approval_result=-1; g_approval_pending=0; }
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
                    // Coalesce all reasoning deltas into ONE "thinking" line instead
                    // of flooding the 800-line ring with per-token entries.
                    Ring *r=&g_panes[i].ring;
                    const char *prefix="[think] ";
                    const int pfxlen=8;
                    int last = r->count>0 ? (r->head + r->count -1)%RING_LINES : -1;
                    int is_think = last>=0 && strncmp(r->lines[last], prefix, pfxlen)==0 ? 1 : 0;
                    if(is_think){
                        ring_append_to_last(r, ev.text, ev.text_len);
                    } else {
                        char tmp[512]; snprintf(tmp,sizeof tmp,"%s%.*s", prefix, (int)(ev.text_len<480?ev.text_len:480), ev.text);
                        ring_push_typed(r, tmp, RL_SYSTEM);
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
                    ring_push_typed(&g_panes[i].ring,"— done —", RL_SYSTEM);
                    // autosave to stable per-pane file
                    {
                        const char *sid = g_panes[i].session_id[0]?g_panes[i].session_id:"tui-autosave";
                        TRACE(TR_SAVE,0); puci_cli_save_session(g_panes[i].sess, sid);
                    }
                    g_panes[i].scroll_off=0;
                } else if(ev.kind==PUCI_EVENT_TURN_CANCELLED){
                    pane_stream_end(&g_panes[i]);
                    ring_push_typed(&g_panes[i].ring,"— cancelled —", RL_ERROR);
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
