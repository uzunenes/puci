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
#ifdef HAVE_CURL
#include <curl/curl.h>
#elif __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define HAVE_CURL 1
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif
#define MAX_PANES 4
#define RING_LINES 800
#define RING_COLS 4096
#define STREAM_BUF 16384

static volatile sig_atomic_t g_winch = 0;
static void on_winch(int s){ (void)s; g_winch=1; }

static void get_winsize(int *rows,int *cols){
    int w = tb_width(); int h = tb_height();
    if(w>0 && h>0){ *cols=w; *rows=h; return; }
    struct winsize ws; if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0 && ws.ws_col>0){ *rows=ws.ws_row; *cols=ws.ws_col; } else { *rows=24; *cols=80; }
}
static long read_rss_kb(void){
    FILE *f=fopen("/proc/self/status","r"); if(!f) return -1;
    char line[128]; long v=-1;
    while(fgets(line,sizeof line,f)){ if(strncmp(line,"VmRSS:",6)==0){ v=strtol(line+6,NULL,10); break; } }
    fclose(f); return v;
}
static long read_pss_kb(void){
    FILE *f=fopen("/proc/self/smaps_rollup","r"); if(!f) return read_rss_kb();
    char line[128]; long v=-1;
    while(fgets(line,sizeof line,f)){ if(strncmp(line,"Pss:",4)==0){ v=strtol(line+4,NULL,10); break; } }
    fclose(f); return v<0?read_rss_kb():v;
}

/* ── Ring with logical lines + role ── */
typedef enum { RL_USER, RL_ASSISTANT, RL_TOOL, RL_SYSTEM, RL_ERROR } rtype_t;
typedef struct Ring {
    char lines[RING_LINES][RING_COLS];
    rtype_t types[RING_LINES];
    int head; int count;
} Ring;
static void ring_push_typed(Ring *r,const char *s, rtype_t t){
    int idx=(r->head + r->count)%RING_LINES;
    size_t n = s ? strlen(s) : 0; if(n>=RING_COLS) n=RING_COLS-1;
    if(s) memcpy(r->lines[idx],s,n);
    r->lines[idx][n]=0;
    r->types[idx]=t;
    if(r->count<RING_LINES) r->count++; else r->head=(r->head+1)%RING_LINES;
}
static void ring_push(Ring *r,const char *s){ ring_push_typed(r,s,RL_SYSTEM); }
static void ring_append_to_last(Ring *r,const char *s, size_t len){
    if(r->count==0){ char tmp[RING_COLS]; size_t n=len<RING_COLS-1?len:RING_COLS-1; memcpy(tmp,s,n); tmp[n]=0; ring_push(r,tmp); return; }
    int idx=(r->head + r->count -1)%RING_LINES;
    size_t cur=strlen(r->lines[idx]);
    size_t avail=RING_COLS-1 - cur;
    size_t add=len<avail?len:avail;
    memcpy(r->lines[idx]+cur,s,add); r->lines[idx][cur+add]=0;
}

typedef struct Pane {
    puci_session *sess;
    Ring ring;
    char title[64];
    int streaming; // 1 while assistant streaming
    char stream_buf[STREAM_BUF];
    size_t stream_len;
    int scroll_off; // 0=bottom, >0 scrolled up (visual lines)
    int need_wrap_recalc;
} Pane;

static Pane g_panes[MAX_PANES];
static int g_npane=1;
static int g_focus=0;
static int g_show_sidebar=0;
static int g_approval_pending=0;
static char g_approval_diff[8192];
static int g_approval_result=0;
static int g_hunk_off=0;
static char g_input[2048];
static size_t g_input_len=0;
static size_t g_input_curs=0;
static int g_input_mode=0;
static int g_rows=24, g_cols=80;

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

#ifdef HAVE_CURL
struct MemBuf { char *data; size_t len; size_t cap; };
static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata){
    struct MemBuf *m=(struct MemBuf*)userdata;
    size_t n=size*nmemb;
    if(m->len + n +1 > m->cap){
        size_t ncap = m->cap ? m->cap*2 : 8192;
        while(ncap < m->len + n +1) ncap*=2;
        if(ncap > 4*1024*1024) ncap=4*1024*1024;
        if(m->len + n +1 > ncap) n = ncap - m->len -1;
        char *nd=(char*)realloc(m->data, ncap);
        if(!nd) return 0;
        m->data=nd; m->cap=ncap;
    }
    memcpy(m->data + m->len, ptr, n);
    m->len+=n; m->data[m->len]=0;
    return n;
}
static int fetch_live_models(void){
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
    char tmp_buf[MAX_MODELS][128];
    int tmp_cnt=0;
    while((p=strstr(p,"\"id\"")) && tmp_cnt<MAX_MODELS){
        p+=4;
        while(*p && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p==':')) p++;
        if(*p!='"'){ continue; }
        p++;
        const char *q=strchr(p,'"');
        if(!q) break;
        size_t idlen=(size_t)(q-p);
        if(idlen>0 && idlen < sizeof tmp_buf[0]){
            if(memchr(p,' ', idlen) || memchr(p,'\n', idlen)){
                p=q+1; continue;
            }
            char id[128]; memcpy(id, p, idlen); id[idlen]=0;
            int dup=0; for(int i=0;i<tmp_cnt;i++) if(strcmp(tmp_buf[i],id)==0) dup=1;
            if(!dup){
                snprintf(tmp_buf[tmp_cnt], sizeof tmp_buf[tmp_cnt], "%.127s", id);
                tmp_cnt++;
            }
        }
        p=q+1;
    }
    free(mem.data);
    if(tmp_cnt>0){
        for(int i=0;i<tmp_cnt;i++){
            snprintf(g_model_buf[i], sizeof g_model_buf[i], "%.127s", tmp_buf[i]);
            k_models[i]=g_model_buf[i];
        }
        k_model_count=tmp_cnt;
        k_models[k_model_count]=NULL;
        return 1;
    }
    return 0;
}
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
static int g_pick_filtered_idx[MAX_SESS_LIST+32];
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
            if(strcasestr_contains(g_sess_list[i].id, g_pick_filter) || strcasestr_contains(g_sess_list[i].model, g_pick_filter)){
                g_pick_filtered_idx[g_pick_filtered_count++]=i;
            }
        }
    } else if(g_mode==3){
        for(int i=0;k_models[i];i++){
            if(strcasestr_contains(k_models[i], g_pick_filter)){
                g_pick_filtered_idx[g_pick_filtered_count++]=i;
            }
        }
    }
    if(g_pick_sel >= g_pick_filtered_count) g_pick_sel = g_pick_filtered_count>0 ? g_pick_filtered_count-1 : 0;
    if(g_pick_sel <0) g_pick_sel=0;
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
    char dir[1024]; if(puci_cli_get_sessions_dir(dir,sizeof dir)!=0) return;
    DIR *d=opendir(dir); if(!d) return;
    g_sess_count=0;
    struct dirent *ent;
    while((ent=readdir(d))!=NULL && g_sess_count<MAX_SESS_LIST){
        size_t len=strlen(ent->d_name); if(len<6 || strcmp(ent->d_name+len-5,".json")!=0) continue;
        char full[2048]; snprintf(full,sizeof full,"%s/%s",dir,ent->d_name);
        struct stat st; if(stat(full,&st)!=0) continue;
        sess_entry_t *e=&g_sess_list[g_sess_count];
        snprintf(e->id,sizeof e->id,"%.*s",(int)(len-5),ent->d_name);
        e->mtime=st.st_mtime;
        // peek model/preview
        FILE *f=fopen(full,"r"); e->model[0]=0; e->preview[0]=0;
        if(f){
            char buf[4096]; size_t rd=fread(buf,1,sizeof buf-1,f); buf[rd]=0; fclose(f);
            char *p=strstr(buf,"\"model\":\""); if(p){ p+=9; char *q=strchr(p,'"'); if(q){ size_t n=(size_t)(q-p); if(n>=sizeof e->model) n=sizeof e->model-1; memcpy(e->model,p,n); e->model[n]=0; }}
            p=strstr(buf,"\"content\":\""); if(p){ p+=11; char *q=strchr(p,'"'); if(q){ size_t n=(size_t)(q-p); if(n>=sizeof e->preview) n=sizeof e->preview-1; memcpy(e->preview,p,n); e->preview[n]=0; }}
            if(!e->model[0]) snprintf(e->model,sizeof e->model,"-");
            if(!e->preview[0]) snprintf(e->preview,sizeof e->preview,"(empty)");
        }
        g_sess_count++;
    }
    closedir(d);
    // sort by mtime desc (simple bubble)
    for(int i=0;i<g_sess_count;i++) for(int j=i+1;j<g_sess_count;j++) if(g_sess_list[j].mtime > g_sess_list[i].mtime){ sess_entry_t tmp=g_sess_list[i]; g_sess_list[i]=g_sess_list[j]; g_sess_list[j]=tmp; }
}

static puci_approval tui_approval_cb(const puci_approval_request *req, void *ud){
    (void)ud;
    if(!req) return PUCI_APPROVAL_DENY;
    snprintf(g_approval_diff,sizeof g_approval_diff,"%s\n%s", req->tool?req->tool:"tool", req->display?req->display:(req->diff?req->diff:req->args_json?req->args_json:""));
    g_approval_pending=1; g_hunk_off=0;
    int cancel_fd = req->cancel?req->cancel->fd:-1;
    extern void tui_render_frame(void);
    while(g_approval_pending){
        if(req->cancel && req->cancel->flag && *req->cancel->flag) { g_approval_pending=0; return PUCI_APPROVAL_DENY; }
        tui_render_frame();
        struct pollfd pf[2]; int nf=1;
        pf[0].fd=STDIN_FILENO; pf[0].events=POLLIN;
        if(cancel_fd>=0){ pf[1].fd=cancel_fd; pf[1].events=POLLIN; nf=2; }
        int pr=poll(pf,(nfds_t)nf,100);
        if(pr<0 && errno==EINTR) continue;
        if(nf==2 && (pf[1].revents&POLLIN)) { g_approval_pending=0; return PUCI_APPROVAL_DENY; }
        if(pf[0].revents&POLLIN){
            char c; ssize_t n=read(STDIN_FILENO,&c,1);
            if(n==1){
                if(c=='y'||c=='Y'){ g_approval_result=1; g_approval_pending=0; return PUCI_APPROVAL_ALLOW; }
                if(c=='n'||c=='N'||c=='q'||c==27){ g_approval_result=-1; g_approval_pending=0; return PUCI_APPROVAL_DENY; }
                if(c=='j') g_hunk_off++;
                if(c=='k' && g_hunk_off>0) g_hunk_off--;
            }
        }
        if(g_winch){ g_winch=0; get_winsize(&g_rows,&g_cols); }
    }
    return g_approval_result==1?PUCI_APPROVAL_ALLOW:PUCI_APPROVAL_DENY;
}

// Lightweight inline markdown: **bold**, *italic*, `code`, # heading, - list, > quote
static void tb_print_styled(int x,int y, uintattr_t base_fg, uintattr_t base_bg, const char *s, int max_w) __attribute__((unused));
static void tb_print_styled(int x,int y, uintattr_t base_fg, uintattr_t base_bg, const char *s, int max_w){
    if(!s) return;
    int col=x;
    int bold=0, code=0;
    // heading detection at line start
    if(s[0]=='#' && (s[1]==' '||s[1]=='#')){
        base_fg = TB_YELLOW|TB_BOLD; // heading yellow
        int skip=0; while(s[skip]=='#') skip++; if(s[skip]==' ') skip++;
        // skip markers for display but keep visual offset?
        s += skip;
    } else if(s[0]=='>' && s[1]==' '){
        base_fg = TB_CYAN;
        // draw quote bar
        if(col < x+max_w){ tb_set_cell(col,y, 0x2502, TB_CYAN, base_bg); col++; }
        s+=2;
    } else if((s[0]=='-'||s[0]=='*') && s[1]==' '){
        base_fg = TB_WHITE;
        if(col < x+max_w){ tb_set_cell(col,y, 0x2022, base_fg, base_bg); col++; tb_set_cell(col,y,' ', base_fg, base_bg); col++; }
        s+=2;
    }
    for(int i=0; s[i] && col < x+max_w; ){
        if(!code && s[i]=='`'){
            code=!code; i++; continue;
        }
        if(!code && s[i]=='*' && s[i+1]=='*'){
            bold=!bold; i+=2; continue;
        }
        // link [text](url) -> underline text
        if(!code && s[i]=='['){
            char *end = strchr(s+i, ']');
            char *par = end ? strchr(end, '(') : NULL;
            char *par2 = par ? strchr(par, ')') : NULL;
            if(end && par==end+1 && par2){
                // render text inside [] underlined
                for(int k=i+1;k<(int)(end-(s));k++){
                    if(col>=x+max_w) break;
                    tb_set_cell(col,y, (uint32_t)(unsigned char)s[k], base_fg|TB_UNDERLINE, base_bg); col++;
                }
                i = (int)(par2 - s) +1;
                continue;
            }
        }
        uintattr_t fg = base_fg;
        uintattr_t bg = base_bg;
        if(bold) fg |= TB_BOLD;
        if(code){ fg = TB_BLACK; bg = TB_WHITE; }
        // utf8: for ascii, one cell; termbox handles utf8 via tb_print but we do per-char
        tb_set_cell(col,y, (uint32_t)(unsigned char)s[i], fg, bg);
        col++; i++;
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
    char bar[512]; int fill = max>0? (int)((cur* (long)(w-14))/max):0; if(fill<0) fill=0; if(fill>w-14) fill=w-14;
    int pos=snprintf(bar,sizeof bar,"%s [",label); if(pos<0) pos=0; if(pos> (int)sizeof bar - 24) pos=(int)sizeof bar -24;
    for(int i=0;i<w-14;i++) bar[pos++]= i<fill ? 0x2588 : 0x2591; // full/shade block fallback to #/-
    // Use ascii for portability: replace blocks with #/-
    for(int i=0;i<w-14;i++) bar[pos - (w-14) + i] = (i<fill?'#':'-');
    bar[pos++]=']'; bar[pos]=0;
    tb_print(c, r, TB_WHITE, TB_DEFAULT, bar);
    char info[40]; snprintf(info,sizeof info," %ld/%ld",cur,max);
    tb_print(c+pos, r, TB_WHITE, TB_DEFAULT, info);
}

/* Word-wrap one logical line into visual lines */
static int wrap_visual_count(const char *s, int width){
    if(!s||!*s) return 1;
    int n=0; int len=(int)strlen(s);
    int pos=0;
    while(pos < len){
        if(len - pos <= width){ n++; break; }
        int cut=width;
        // try word boundary
        int last_sp=-1;
        for(int i=0;i<width;i++) if(s[pos+i]==' ') last_sp=i;
        if(last_sp> width/2) cut=last_sp+1;
        n++; pos+=cut;
    }
    return n;
}

void tui_render_frame(void){
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
        const char *m = getenv("PUCI_MODEL"); if(!m) m="deepseek/deepseek-chat";
        char mod[64]; snprintf(mod,sizeof mod," %s ", m);
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
        // compute total visual lines
        int total_vis=0;
        for(int k=0;k<g_panes[i].ring.count;k++){
            int idx=(g_panes[i].ring.head + k)%RING_LINES;
            total_vis += wrap_visual_count(g_panes[i].ring.lines[idx], inner_w);
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
                    int take2 = rem2 < inner_w ? rem2 : inner_w;
                    if(rem2 > inner_w){
                        int last_sp=-1;
                        for(int j=0;j<take2;j++) if(s[seg_off+j]==' ') last_sp=j;
                        if(last_sp> inner_w/2) take2=last_sp+1;
                    }
                    seg_off += take2;
                    continue;
                }
                int rem=(int)strlen(s)-seg_off;
                if(rem<=0) break;
                int take = rem < inner_w ? rem : inner_w;
                if(rem > inner_w){
                    int last_sp=-1;
                    for(int j=0;j<take;j++) if(s[seg_off+j]==' ') last_sp=j;
                    if(last_sp> inner_w/2) take=last_sp+1;
                }
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
                tb_print(pc+1, pr+1+rendered, fg, TB_DEFAULT, seg);
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
        for(int i=0;i<cnt && i<sh-2;i++){
            uint32_t fg = (i==g_pick_sel && g_mode==2)? TB_BLACK:TB_WHITE;
            uint32_t bg = (i==g_pick_sel && g_mode==2)? TB_CYAN:TB_DEFAULT;
            int src = (g_mode==2? g_pick_filtered_idx[i] : i);
            char ln[64]; snprintf(ln,sizeof ln,"%.12s %.10s", g_sess_list[src].id, g_sess_list[src].model);
            // highlight
            for(int x=sc+1;x<sc+sw-1;x++) tb_set_cell(x,sr+1+i,' ',fg,bg);
            tb_print(sc+2, sr+1+i, fg, bg, ln);
        }
        if(cnt==0) tb_print(sc+2,sr+1, TB_YELLOW, TB_DEFAULT, g_pick_filter[0]?"(no match)":"(no sessions)");
        if(g_mode==2) tb_print(sc+2,sr+sh-1, TB_CYAN, TB_DEFAULT, "type filter  y copy  Enter load");
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
        // cursor
        int cx = 2 + (int)g_input_curs;
        if(cx>=cols) cx=cols-1;
        tb_set_cursor(cx, rows-1);
        tb_set_cursor(2 + (int)g_input_curs, g_rows-1);
    } else {
        tb_hide_cursor();
    }
    if(g_approval_pending){
        int oh=14, ow=68;
        int or_ = rows/2 - 7; if(or_<1) or_=1;
        int oc= cols/2 - 34; if(oc<0) oc=0;
        if(or_+oh >= rows) oh = rows - or_ -1;
        if(oc+ow >= cols) ow = cols - oc -1;
        for(int y=or_; y<or_+oh; y++) for(int x=oc; x<oc+ow; x++) tb_set_cell(x,y,' ', TB_WHITE, TB_BLUE);
        tb_draw_box(or_,oc,oh,ow,"Approval — y=allow n=deny j/k scroll",1);
        char *copy=strdup(g_approval_diff); char *tok=strtok(copy,"\n"); int l=0;
        while(tok && l<oh-2){
            if(l>=g_hunk_off){
                int yy = or_+1+(l-g_hunk_off);
                if(yy >= or_+1 && yy < or_+oh-1){
                    uint32_t fg = TB_WHITE;
                    if(tok[0]=='+') fg = TB_GREEN|TB_BOLD;
                    else if(tok[0]=='-') fg = TB_RED|TB_BOLD;
                    char line[96]; snprintf(line,sizeof line,"%.*s", ow-2, tok);
                    tb_print(oc+1, yy, fg, TB_BLUE, line);
                }
            }
            l++; tok=strtok(NULL,"\n");
        }
        free(copy);
    }
    if(g_mode==3){
        int oh=16, ow=54;
        int or_ = rows/2 - 8; if(or_<1) or_=1;
        int oc= cols/2 - 27; if(oc<0) oc=0;
        for(int y=or_; y<or_+oh; y++) for(int x=oc; x<oc+ow; x++) tb_set_cell(x,y,' ', TB_WHITE, TB_BLUE);
        char mtitle[64]; if(g_pick_filter[0]) snprintf(mtitle,sizeof mtitle,"Model /%.30s", g_pick_filter); else snprintf(mtitle,sizeof mtitle,"Model — Enter select (y copy)");
        tb_draw_box(or_,oc,oh,ow,mtitle,1);
        for(int i=0;i<g_pick_filtered_count && i<oh-2; i++){
            int src=g_pick_filtered_idx[i];
            int yy=or_+1+i;
            if(i==g_pick_sel){
                for(int x=oc+1;x<oc+ow-1;x++) tb_set_cell(x,yy,' ', TB_BLACK, TB_CYAN);
                tb_print(oc+2, yy, TB_BLACK, TB_CYAN, k_models[src]);
            } else {
                tb_print(oc+2, yy, TB_WHITE, TB_BLUE, k_models[src]);
            }
        }
        if(g_pick_filtered_count==0) tb_print(oc+2,or_+1, TB_YELLOW, TB_BLUE, "(no match)");
        tb_print(oc+2,or_+oh-1, TB_CYAN, TB_BLUE, "type filter  y copy  Esc cancel");
    }
    if(g_mode==2){
        // sessions picker overlay already via sidebar, but add help
        // footer overwrites
    }
    tb_present();
}

int puci_tui_available(void){ return 1; }
int puci_tui_should_launch(int argc, char **argv){
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--no-tui")==0) return 0;
        if(strcmp(argv[i],"--tui")==0) return 1;
        if(strcmp(argv[i],"run")==0) return 0;
        if(strcmp(argv[i],"doctor")==0) return 0;
        if(strcmp(argv[i],"--help")==0) return 0;
        if(strcmp(argv[i],"-h")==0) return 0;
        if(strcmp(argv[i],"--version")==0) return 0;
    }
    if(getenv("PUCI_NO_TUI")) return 0;
    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return 0;
    if(argc>1) return 0;
    return 1;
}

static void pane_stream_begin(Pane *p){
    p->streaming=1; p->stream_len=0; p->stream_buf[0]=0;
    ring_push_typed(&p->ring, "", RL_ASSISTANT);
}

static void pane_stream_append(Pane *p, const char *txt, size_t len){
    if(!p->streaming){
        pane_stream_begin(p);
    }
    // append to stream_buf and to last ring line
    size_t add = len < STREAM_BUF - p->stream_len -1 ? len : STREAM_BUF - p->stream_len -1;
    memcpy(p->stream_buf + p->stream_len, txt, add); p->stream_len+=add; p->stream_buf[p->stream_len]=0;
    // append to last logical line (coalesce)
    ring_append_to_last(&p->ring, txt, len);
    // keep scroll at bottom if user was at bottom
    if(p->scroll_off==0) p->scroll_off=0; // auto stays
}

static void pane_stream_end(Pane *p){
    if(p->streaming){
        p->streaming=0;
        // if empty streaming line, remove? keep as is
    }
}

int puci_tui_run(const puci_cli_params *params){
    (void)params;
    if(tb_init()!=0){ fprintf(stderr,"tui: tb_init failed\n"); return 1; }
    tb_set_input_mode(TB_INPUT_ESC);
    tb_set_output_mode(TB_OUTPUT_NORMAL);
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_winch; sigemptyset(&sa.sa_mask); sa.sa_flags=SA_RESTART; sigaction(SIGWINCH,&sa,NULL);
    g_npane=1; g_focus=0; g_mode=0;
    for(int i=0;i<MAX_PANES;i++){ memset(&g_panes[i],0,sizeof g_panes[i]); snprintf(g_panes[i].title,sizeof g_panes[i].title,"Agent %c",'A'+i); g_panes[i].scroll_off=0; }
    puci_context *ctx=NULL;
    if(puci_context_create(NULL,&ctx)!=PUCI_OK){ tb_shutdown(); fprintf(stderr,"ctx fail\n"); return 1; }
    puci_provider_config *pcfg_ptr = puci_cli_provider("openai");
    if(!pcfg_ptr){ tb_shutdown(); fprintf(stderr,"tui: provider config failed\n"); puci_context_destroy(ctx); return 1; }
    puci_provider_config pcfg = *pcfg_ptr;
    puci_context_set_provider(ctx,&pcfg);
    for(int i=0;i<g_npane;i++){
        puci_session_options sopts={0}; sopts.provider=pcfg; sopts.system_prompt="You are puci, a fast C coding agent. Be concise and helpful.";
        sopts.approve_cb=tui_approval_cb; sopts.approve_userdata=NULL;
        puci_session *s=NULL;
        if(puci_session_create(ctx,&sopts,&s)==PUCI_OK){
            g_panes[i].sess=s;
            // load history if resume? for now empty
            ring_push_typed(&g_panes[i].ring,"— PUCI TUI v0.5 — Enter prompt, ↑↓ scroll, s sessions, m model, q quit —", RL_SYSTEM);
        }
    }
    load_sessions(); history_load(); models_init_static();
    get_winsize(&g_rows,&g_cols);
    struct timespec last_gauge; clock_gettime(CLOCK_MONOTONIC,&last_gauge);
    int dirty=1;
    int running=1;
    while(running){
        if(g_winch){ g_winch=0; get_winsize(&g_rows,&g_cols); dirty=1; }
        {
            struct timespec ng; clock_gettime(CLOCK_MONOTONIC,&ng);
            long ge = (ng.tv_sec-last_gauge.tv_sec)*1000 + (ng.tv_nsec-last_gauge.tv_nsec)/1000000;
            if(ge >= 500){ dirty=1; last_gauge=ng; }
        }
        if(dirty){ tui_render_frame(); dirty=0; }
        struct pollfd fds[32]; size_t nf=0;
        fds[nf].fd=STDIN_FILENO; fds[nf].events=POLLIN; nf++;
        int session_to=-1;
        for(int i=0;i<g_npane;i++) if(g_panes[i].sess){
            size_t cnt=0; puci_session_get_fds(g_panes[i].sess,NULL,0,&cnt);
            if(cnt>0){
                if(nf+cnt>32) cnt=32-nf;
                size_t got=0; puci_session_get_fds(g_panes[i].sess,fds+nf, cnt, &got); nf+=got;
            }
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
        // streaming = responsive but not busy: 16ms, idle 250ms. Clamp 0->5ms to avoid 100% CPU spin
        int is_streaming=0;
        for(int i=0;i<g_npane;i++) if(g_panes[i].streaming) is_streaming=1;
        int poll_to;
        if(is_streaming){
            poll_to = session_to>=0 ? (session_to < 16 ? session_to : 16) : 16;
            if(gauge_due < poll_to) poll_to = gauge_due;
            if(poll_to==0) poll_to=5;
        } else if(session_to >= 0){
            poll_to = session_to < gauge_due ? session_to : gauge_due;
            if(poll_to==0) poll_to=5;
        } else poll_to = gauge_due < 250 ? gauge_due : 250;
        if(poll_to < 0) poll_to = 5;
        if(poll_to==0) poll_to=5;
        int pr=poll(fds,(nfds_t)nf,poll_to);
        if(pr<0 && errno==EINTR) continue;
        if(pr==0){
            struct timespec ng3; clock_gettime(CLOCK_MONOTONIC,&ng3);
            long ge2 = (ng3.tv_sec-last_gauge.tv_sec)*1000 + (ng3.tv_nsec-last_gauge.tv_nsec)/1000000;
            if(ge2 >= 500) dirty=1;
            continue;
        }
        if(fds[0].revents&POLLIN){
            char buf[128]; ssize_t n=read(STDIN_FILENO,buf,sizeof buf);
            int had_key=0;
            for(ssize_t k=0;k<n;k++){
                unsigned char c=(unsigned char)buf[k];
                had_key=1;
                // ESC sequence handling (fragment-safe: arrow may arrive split across reads)
                if(c==27){
                    char seq[8]={0}; int seq_len=0;
                    if(k+1 < n){ seq[seq_len++]=buf[++k]; }
                    else { struct pollfd _pf={STDIN_FILENO,POLLIN,0}; if(poll(&_pf,1,30)>0 && (_pf.revents&POLLIN)){ char _ch; if(read(STDIN_FILENO,&_ch,1)==1) seq[seq_len++]=_ch; } }
                    if(seq_len==1 && seq[0]=='['){
                        if(k+1 >= n){ struct pollfd _pf2={STDIN_FILENO,POLLIN,0}; if(poll(&_pf2,1,30)>0 && (_pf2.revents&POLLIN)){ char _ch; if(read(STDIN_FILENO,&_ch,1)==1) seq[seq_len++]=_ch; } else if(k+2 < n){ seq[seq_len++]=buf[++k]; } }
                        else seq[seq_len++]=buf[++k];
                        if(seq_len>=2 && (seq[1]=='5' || seq[1]=='6')){
                            if(seq_len==2){ struct pollfd _pf3={STDIN_FILENO,POLLIN,0}; if(poll(&_pf3,1,30)>0 && (_pf3.revents&POLLIN)){ char _ch; if(read(STDIN_FILENO,&_ch,1)==1) seq[seq_len++]=_ch; } else if(k+1 < n) seq[seq_len++]=buf[++k]; }
                        }
                    }
                    if(seq_len>=1 && seq[0]=='[' && seq_len>=2){
                        unsigned char code=(unsigned char)seq[1];
                        if(code=='A'){ if(g_input_mode) history_prev(); else if(g_mode==2||g_mode==3){ if(g_pick_sel>0) g_pick_sel--; } else { Pane *pp=&g_panes[g_focus]; pp->scroll_off+=2; } }
                        else if(code=='B'){ if(g_input_mode) history_next(); else if(g_mode==2||g_mode==3){ int lim=(g_mode==2?g_pick_filtered_count:g_pick_filtered_count); if(g_mode==3 && lim==0){ int mcnt=0; while(k_models[mcnt]) mcnt++; lim=mcnt; } if(lim==0){ int mcnt=0; while(k_models[mcnt]) mcnt++; lim=(g_mode==3?mcnt:g_sess_count); } if(g_pick_sel < lim-1) g_pick_sel++; } else { Pane *pp=&g_panes[g_focus]; if(pp->scroll_off>0) pp->scroll_off-=2; } }
                        else if(code=='C'){ if(g_input_mode && g_input_curs < g_input_len) g_input_curs++; }
                        else if(code=='D'){ if(g_input_mode && g_input_curs>0) g_input_curs--; }
                        else if(code=='5' && seq_len>=3 && seq[2]=='~'){ if(g_input_mode) history_prev(); else { Pane *pp=&g_panes[g_focus]; pp->scroll_off+=10; } }
                        else if(code=='6' && seq_len>=3 && seq[2]=='~'){ if(g_input_mode) history_next(); else { Pane *pp=&g_panes[g_focus]; pp->scroll_off-=10; if(pp->scroll_off<0) pp->scroll_off=0; } }
                        else if(code=='H'){ if(g_input_mode) g_input_curs=0; else { Pane *pp=&g_panes[g_focus]; pp->scroll_off=9999; } }
                        else if(code=='F'){ if(g_input_mode) g_input_curs=g_input_len; else { Pane *pp=&g_panes[g_focus]; pp->scroll_off=0; } }
                    } else if(seq_len==0){
                        if(g_mode==2||g_mode==3){ g_mode=0; g_pick_filter[0]=0; }
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
                                char loaded[128]; if(puci_cli_load_session(pp->sess, id, loaded,sizeof loaded)==0){
                                    ring_push_typed(&pp->ring, "— resumed session —", RL_SYSTEM);
                                    char msg[160]; snprintf(msg,sizeof msg,"Loaded %s (%s)", id, g_sess_list[src].model);
                                    ring_push_typed(&pp->ring, msg, RL_SYSTEM);
                                    snprintf(pp->title,sizeof pp->title,"%.20s", id);
                                    pp->scroll_off=0;
                                }
                            }
                        }
                        g_mode=0; g_pick_filter[0]=0;
                    } else if(c=='q'||c==27){ g_mode=0; g_pick_filter[0]=0; }
                    else if(c=='y' || c=='Y'){
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
                    else if(c=='j' && g_pick_sel < g_pick_filtered_count-1) g_pick_sel++;
                    else if(c=='k' && g_pick_sel>0) g_pick_sel--;
                    continue;
                }
                if(g_mode==3){ // model picker
                    if(c=='\n'||c=='\r'){
                        if(g_pick_sel < g_pick_filtered_count){
                            int src=g_pick_filtered_idx[g_pick_sel];
                            if(k_models[src]){
                                setenv("PUCI_MODEL", k_models[src], 1);
                                FILE *f=fopen("/tmp/puci_model_sel","w"); if(f){ fprintf(f,"%s",k_models[src]); fclose(f); }
                                char msg[160]; snprintf(msg,sizeof msg,"Model → %s", k_models[src]);
                                ring_push_typed(&g_panes[g_focus].ring, msg, RL_SYSTEM);
                            }
                        }
                        g_mode=0; g_pick_filter[0]=0;
                    } else if(c=='q'||c==27){ g_mode=0; g_pick_filter[0]=0; }
                    else if(c=='y' || c=='Y'){
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
                    else if(c=='j' && g_pick_sel < g_pick_filtered_count-1) g_pick_sel++;
                    else if(c=='k' && g_pick_sel>0) g_pick_sel--;
                    continue;
                }
                if(g_input_mode){
                    if(c=='\n'||c=='\r'){
                        g_input[g_input_len]=0;
                        if(g_input_len>0){
                            history_save_line(g_input);
                            if(g_input[0]=='/'){
                                if(strcmp(g_input,"/clear")==0){ Pane *pp=&g_panes[g_focus]; pp->ring.count=0; pp->ring.head=0; pp->scroll_off=0; ring_push_typed(&pp->ring,"— cleared —", RL_SYSTEM); }
                                else if(strcmp(g_input,"/help")==0 || strcmp(g_input,"/?")==0){ ring_push_typed(&g_panes[g_focus].ring,"/clear /help /model [name] /sessions /new /compact /doctor /plan", RL_SYSTEM); ring_push_typed(&g_panes[g_focus].ring,"Keys: Enter send  Esc cancel  ↑↓ history/scroll  s sessions  m model  g/G top/bottom", RL_SYSTEM); }
                                else if(strncmp(g_input,"/model",6)==0){ char *arg=g_input+6; while(*arg==' ') arg++; if(*arg){ setenv("PUCI_MODEL",arg,1); char msg[160]; snprintf(msg,sizeof msg,"Model → %.120s",arg); ring_push_typed(&g_panes[g_focus].ring,msg,RL_SYSTEM); } else { models_init_static(); fetch_live_models(); g_pick_filter[0]=0; rebuild_pick_filter(); g_pick_sel=0; g_mode=3; } }
                                else if(strcmp(g_input,"/sessions")==0 || strcmp(g_input,"/s")==0){ load_sessions(); g_pick_filter[0]=0; rebuild_pick_filter(); g_pick_sel=0; g_mode=2; g_show_sidebar=1; }
                                else if(strcmp(g_input,"/new")==0){ Pane *pp=&g_panes[g_focus]; pp->ring.count=0; pp->ring.head=0; pp->scroll_off=0; ring_push_typed(&pp->ring,"— new session —", RL_SYSTEM); char sid[64]; snprintf(sid,sizeof sid,"tui-%ld",(long)time(NULL)); if(pp->sess) puci_cli_save_session(pp->sess,sid); }
                                else if(strcmp(g_input,"/compact")==0){ Pane *pp=&g_panes[g_focus]; if(pp->sess){ size_t pr=0; puci_session_compact_context(pp->sess,&pr); char msg[96]; snprintf(msg,sizeof msg,"Compacted %zu bytes",pr); ring_push_typed(&pp->ring,msg,RL_SYSTEM); } }
                                else if(strcmp(g_input,"/doctor")==0){ puci_provider_config *pc=puci_cli_provider("openai"); char msg[256]; snprintf(msg,sizeof msg,"model=%s base=%s", pc?pc->model:"?", pc?pc->base_url:"?"); ring_push_typed(&g_panes[g_focus].ring,msg,RL_SYSTEM); }
                                else if(strcmp(g_input,"/plan")==0){ ring_push_typed(&g_panes[g_focus].ring,"plan mode: use --plan flag for now", RL_SYSTEM); }
                                else { char msg[200]; snprintf(msg,sizeof msg,"Unknown: %s (try /help)", g_input); ring_push_typed(&g_panes[g_focus].ring,msg,RL_ERROR); }
                            } else if(g_panes[g_focus].sess){
                                char u[2048]; snprintf(u,sizeof u,"❯ %s", g_input);
                                ring_push_typed(&g_panes[g_focus].ring, u, RL_USER);
                                g_panes[g_focus].scroll_off=0;
                                g_panes[g_focus].streaming=0;
                                puci_session_begin_turn(g_panes[g_focus].sess,g_input);
                            }
                        }
                        g_input_len=0; g_input[0]=0; g_input_curs=0; g_input_mode=0;
                    } else if(c==127 || c==8){
                        if(g_input_curs>0){
                            memmove(g_input+g_input_curs-1, g_input+g_input_curs, g_input_len - g_input_curs +1);
                            g_input_len--; g_input_curs--;
                        }
                    } else if(c==21){ // Ctrl-U clear
                        g_input_len=0; g_input_curs=0; g_input[0]=0;
                    } else if(c>=32 && c<127){
                        if(g_input_len+1 < sizeof g_input){
                            memmove(g_input+g_input_curs+1, g_input+g_input_curs, g_input_len - g_input_curs +1);
                            g_input[g_input_curs++]= (char)c; g_input_len++;
                        }
                    }
                } else {
                    if(c=='q'){ running=0; break; }
                    else if(c=='\t'){ g_focus=(g_focus+1)%g_npane; }
                    else if(c=='s'){ load_sessions(); g_pick_filter[0]=0; rebuild_pick_filter(); g_pick_sel=0; g_mode=2; g_show_sidebar=1; }
                    else if(c=='m'){ models_init_static(); fetch_live_models(); g_pick_filter[0]=0; rebuild_pick_filter(); g_pick_sel=0; g_mode=3; }
                    else if(c=='\n' || c=='\r'){ g_input_mode=1; g_input_len=0; g_input[0]=0; g_input_curs=0; }
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
            puci_session_process(g_panes[i].sess, fds+1, nf-1);
            puci_event ev;
            while(puci_session_next_event(g_panes[i].sess,&ev)){
                had_event=1;
                if(ev.kind==PUCI_EVENT_TOKEN_DELTA && ev.text){
                    pane_stream_append(&g_panes[i], ev.text, ev.text_len);
                } else if(ev.kind==PUCI_EVENT_REASONING_DELTA && ev.text){
                    // show reasoning dim
                    char tmp[512]; snprintf(tmp,sizeof tmp,"[think] %.*s", (int)(ev.text_len<400?ev.text_len:400), ev.text);
                    // don't coalesce reasoning, push as separate
                    ring_push_typed(&g_panes[i].ring, tmp, RL_SYSTEM);
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
                    // autosave
                    {
                        char sid[64]; snprintf(sid,sizeof sid,"tui-%ld", (long)time(NULL));
                        puci_cli_save_session(g_panes[i].sess, sid);
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
        if(had_event) dirty=1;
    }
    for(int i=0;i<MAX_PANES;i++) if(g_panes[i].sess) puci_session_destroy(g_panes[i].sess);
    puci_context_destroy(ctx);
    tb_shutdown();
    return 0;
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#pragma GCC diagnostic pop
