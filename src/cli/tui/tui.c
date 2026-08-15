/* tui.c - Featherweight POSIX ANSI TUI (v0.5.0) — hybrid termbox2 cell renderer
 * Master poll ring: STDIN + N*session fds. termbox2 used SOLELY as double-
 * buffered cell renderer (tb_clear/tb_set_cell/tb_print/tb_present). No
 * tb_poll_event — poll() remains master to avoid starving session fds.
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif
#define MAX_PANES 4
#define RING_LINES 500
#define RING_COLS 256

static volatile sig_atomic_t g_winch = 0;
static void on_winch(int s){ (void)s; g_winch=1; }

static void get_winsize(int *rows,int *cols){
    // Prefer termbox2 dimensions if initialized, else ioctl
    int w = tb_width();
    int h = tb_height();
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

typedef struct Ring {
    char lines[RING_LINES][RING_COLS];
    int head; int count;
} Ring;
static void ring_push(Ring *r,const char *s){
    int idx=(r->head + r->count)%RING_LINES;
    { size_t _n = s ? strlen(s) : 0; if(_n>=RING_COLS) _n=RING_COLS-1; if(s){ memcpy(r->lines[idx],s,_n); r->lines[idx][_n]=0; } else r->lines[idx][0]=0; }
    if(r->count<RING_LINES) r->count++; else r->head=(r->head+1)%RING_LINES;
}

typedef struct Pane {
    puci_session *sess;
    Ring ring;
    int focused;
    char title[64];
} Pane;

static Pane g_panes[MAX_PANES];
static int g_npane=1;
static int g_focus=0;
static int g_show_sidebar=0;
static int g_approval_pending=0;
static char g_approval_diff[4096];
static int g_approval_result=0;
static int g_hunk_off=0;
static char g_input[512];
static size_t g_input_len=0;
static int g_input_mode=0;
static int g_rows=24, g_cols=80;

static puci_approval tui_approval_cb(const puci_approval_request *req, void *ud){
    (void)ud;
    if(!req) return PUCI_APPROVAL_DENY;
    snprintf(g_approval_diff,sizeof g_approval_diff,"%s\n%s", req->tool?req->tool:"tool", req->display?req->display:(req->diff?req->diff:req->args_json?req->args_json:""));
    g_approval_pending=1;
    g_hunk_off=0;
    int cancel_fd = req->cancel?req->cancel->fd:-1;
    // Approval is blocking: render overlay and poll keys + cancel
    // Ensure screen shows overlay immediately
    extern void tui_render_frame(void);
    // We'll render via direct call if rows/cols known
    while(g_approval_pending){
        if(req->cancel && req->cancel->flag && *req->cancel->flag) { g_approval_pending=0; return PUCI_APPROVAL_DENY; }
        // render overlay with current frame
        // Use global rows/cols
        // Defer to main loop's render? For now poll and handle keys here; overlay will be drawn by main loop's render when we set dirty,
        // but since we're blocking main loop, we render here directly
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif
static void tb_draw_box(int r,int c,int h,int w,const char *title,int focused){
    uintattr_t fg = focused ? TB_CYAN|TB_BOLD : TB_WHITE;
    uintattr_t bg = TB_DEFAULT;
    // top border
    tb_set_cell(c, r, 0x250C, fg, bg); // ┌
    for(int x=1;x<w-1;x++) tb_set_cell(c+x, r, 0x2500, fg, bg); // ─
    tb_set_cell(c+w-1, r, 0x2510, fg, bg); // ┐
    if(title){
        char t[64]; snprintf(t,sizeof t," %.*s ", (int)(w-4), title);
        tb_print(c+2, r, fg, bg, t);
    }
    for(int y=1;y<h-1;y++){
        tb_set_cell(c, r+y, 0x2502, fg, bg); // │
        tb_set_cell(c+w-1, r+y, 0x2502, fg, bg);
    }
    tb_set_cell(c, r+h-1, 0x2514, fg, bg); // └
    for(int x=1;x<w-1;x++) tb_set_cell(c+x, r+h-1, 0x2500, fg, bg);
    tb_set_cell(c+w-1, r+h-1, 0x2518, fg, bg); // ┘
}

static void tb_draw_gauge(int r,int c,int w,long cur,long max,const char *label){
    char bar[512]; int fill = max>0? (int)((cur* (long)(w-12))/max):0; if(fill<0) fill=0; if(fill>w-12) fill=w-12;
    int pos=snprintf(bar,sizeof bar,"%s [",label); if(pos<0) pos=0; if(pos> (int)sizeof bar - 20) pos=(int)sizeof bar -20;
    for(int i=0;i<w-12;i++) bar[pos++]= i<fill ? '#' : '-';
    bar[pos++]=']'; bar[pos]=0;
    tb_print(c, r, TB_WHITE, TB_DEFAULT, bar);
    char info[32]; snprintf(info,sizeof info," %ld/%ld",cur,max);
    tb_print(c+pos, r, TB_WHITE, TB_DEFAULT, info);
}

void tui_render_frame(void){
    int rows=g_rows, cols=g_cols;
    tb_clear();
    long rss=read_rss_kb(); long pss=read_pss_kb();
    long ctx_cur=0, ctx_bud=131072;
    if(g_npane>0 && g_panes[0].sess){ ctx_cur=(long)puci_session_context_bytes(g_panes[0].sess); ctx_bud=(long)puci_session_get_context_budget(g_panes[0].sess); }
    char hdr[256];
    snprintf(hdr,sizeof hdr," PUCI v0.5.0 ─ Swarm %d/%d ─ RSS %ld kB (PSS %ld) ─ Context %ld/%ld ", g_npane, MAX_PANES, rss, pss, ctx_cur, ctx_bud);
    // header bar full width blue bg
    for(int x=0;x<cols;x++) tb_set_cell(x, 0, ' ', TB_WHITE|TB_BOLD, TB_BLUE);
    tb_print(0, 0, TB_WHITE|TB_BOLD, TB_BLUE, hdr);
    int usable_rows = rows-3; // header + gauge + footer
    if(usable_rows<4) usable_rows=4;
    int pane_h = usable_rows/2; if(pane_h<4) pane_h=4;
    int pane_w = (cols-3)/2; if(pane_w<20) pane_w=20;
    for(int i=0;i<g_npane && i<MAX_PANES;i++){
        int pr = 1 + (i/2)*pane_h;
        int pc = (i%2)*(pane_w+1);
        if(g_show_sidebar && i>=2) continue;
        tb_draw_box(pr, pc, pane_h, pane_w, g_panes[i].title, g_focus==i);
        int inner_h = pane_h-2; int inner_w = pane_w-2;
        int start = g_panes[i].ring.count>inner_h? g_panes[i].ring.count-inner_h:0;
        for(int l=0;l<inner_h && start+l<g_panes[i].ring.count;l++){
            int idx=(g_panes[i].ring.head + start + l)%RING_LINES;
            char line[RING_COLS]; snprintf(line,sizeof line,"%.*s", inner_w, g_panes[i].ring.lines[idx]);
            uintattr_t fg = TB_WHITE;
            if(line[0]=='+') fg = TB_GREEN;
            else if(line[0]=='-') fg = TB_RED;
            tb_print(pc+1, pr+1+l, fg, TB_DEFAULT, line);
        }
    }
    if(g_show_sidebar){
        int sr=1, sc= cols-22, sh= usable_rows, sw=21;
        if(sc<0) sc=0;
        if(sw>cols) sw=cols;
        tb_draw_box(sr,sc,sh,sw,"Sessions",0);
        for(int i=0;i<g_npane;i++){
            char ln[32]; snprintf(ln,sizeof ln,"%s %s", g_focus==i?"▸":" ", g_panes[i].title);
            tb_print(sc+2, sr+1+i, TB_WHITE, TB_DEFAULT, ln);
        }
    }
    tb_draw_gauge(rows-2, 0, cols, ctx_cur, ctx_bud, "Context");
    const char *footer="[y] Approve  [n] Deny  [j/k] Hunk  [TAB] Focus  [s] Sidebar  [q] Quit  [Enter] Prompt";
    tb_print(0, rows-1, TB_WHITE, TB_DEFAULT, footer);
    if(g_input_mode){
        char prompt[600]; snprintf(prompt,sizeof prompt,"> %.*s", (int)g_input_len, g_input);
        // overwrite footer with input line
        tb_print(0, rows-1, TB_BLACK, TB_WHITE, prompt);
        // also ensure trailing clear by filling rest with bg
        int len = (int)strlen(prompt);
        for(int x=len;x<cols;x++) tb_set_cell(x, rows-1, ' ', TB_BLACK, TB_WHITE);
    }
    if(g_approval_pending){
        int oh=12, ow=60;
        int or_ = rows/2 - 6; if(or_<0) or_=0;
        int oc= cols/2 - 30; if(oc<0) oc=0;
        if(or_+oh >= rows) oh = rows - or_ -1;
        if(oc+ow >= cols) ow = cols - oc -1;
        // overlay bg fill
        for(int y=or_; y<or_+oh; y++) for(int x=oc; x<oc+ow; x++) tb_set_cell(x,y,' ', TB_WHITE, TB_BLUE);
        tb_draw_box(or_,oc,oh,ow,"Approval — y/n, j/k scroll",1);
        char *copy=strdup(g_approval_diff); char *tok=strtok(copy,"\n"); int l=0;
        while(tok && l<oh-2){
            if(l>=g_hunk_off){
                int yy = or_+1+(l-g_hunk_off);
                if(yy >= or_+1 && yy < or_+oh-1){
                    uintattr_t fg = TB_WHITE;
                    if(tok[0]=='+') fg = TB_GREEN;
                    else if(tok[0]=='-') fg = TB_RED;
                    char line[64]; snprintf(line,sizeof line,"%.*s", ow-2, tok);
                    tb_print(oc+1, yy, fg, TB_BLUE, line);
                }
            }
            l++; tok=strtok(NULL,"\n");
        }
        free(copy);
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

int puci_tui_run(const puci_cli_params *params){
    (void)params;
    // termbox2 init replaces raw/alt-screen handling
    if(tb_init()!=0){ fprintf(stderr,"tui: tb_init failed\n"); return 1; }
    tb_set_input_mode(TB_INPUT_ESC);
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_winch; sigemptyset(&sa.sa_mask); sa.sa_flags=SA_RESTART; sigaction(SIGWINCH,&sa,NULL);
    g_npane=1; g_focus=0;
    for(int i=0;i<MAX_PANES;i++){ memset(&g_panes[i],0,sizeof g_panes[i]); snprintf(g_panes[i].title,sizeof g_panes[i].title,"Agent %c",'A'+i); }
    puci_context *ctx=NULL;
    if(puci_context_create(NULL,&ctx)!=PUCI_OK){ tb_shutdown(); fprintf(stderr,"ctx fail\n"); return 1; }
    puci_provider_config pcfg={0};
    const char *key=getenv("PUCI_API_KEY"); if(!key||!*key) key=getenv("OPENROUTER_API_KEY");
    const char *base=getenv("PUCI_BASE_URL"); if(!base){ if(key && getenv("OPENROUTER_API_KEY")) base="https://openrouter.ai/api/v1"; else base="https://api.openai.com/v1"; }
    const char *model=getenv("PUCI_MODEL"); if(!model){ if(key && getenv("OPENROUTER_API_KEY")) model="anthropic/claude-3.5-sonnet"; else model="gpt-4o-mini"; }
    pcfg.name="openai"; pcfg.base_url=base; pcfg.api_key=key; pcfg.model=model; pcfg.timeout_ms=60000;
    puci_context_set_provider(ctx,&pcfg);
    for(int i=0;i<g_npane;i++){
        puci_session_options sopts={0}; sopts.provider=pcfg; sopts.system_prompt="You are puci, a fast C coding agent. Be concise.";
        sopts.approve_cb=tui_approval_cb; sopts.approve_userdata=NULL;
        puci_session *s=NULL;
        if(puci_session_create(ctx,&sopts,&s)==PUCI_OK){ g_panes[i].sess=s; ring_push(&g_panes[i].ring,"— ready — type prompt, Enter to send —"); }
    }
    ring_push(&g_panes[g_focus].ring,"PUCI TUI v0.5.0 — TAB focus, s sidebar, q quit, Enter prompt");
    g_input_len=0; g_input_mode=0; g_input[0]=0;
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
        int poll_to;
        if(session_to >= 0) poll_to = session_to < gauge_due ? session_to : gauge_due;
        else poll_to = gauge_due < 250 ? gauge_due : 250;
        if(poll_to < 0) poll_to = 250;
        int pr=poll(fds,(nfds_t)nf,poll_to);
        if(pr<0 && errno==EINTR) continue;
        if(pr==0){
            struct timespec ng3; clock_gettime(CLOCK_MONOTONIC,&ng3);
            long ge2 = (ng3.tv_sec-last_gauge.tv_sec)*1000 + (ng3.tv_nsec-last_gauge.tv_nsec)/1000000;
            if(ge2 >= 500) dirty=1;
            continue;
        }
        if(fds[0].revents&POLLIN){
            char buf[64]; ssize_t n=read(STDIN_FILENO,buf,sizeof buf);
            int had_key=0;
            for(ssize_t k=0;k<n;k++){
                char c=buf[k];
                had_key=1;
                if(g_input_mode){
                    if(c=='\n'||c=='\r'){
                        g_input[g_input_len]=0;
                        if(g_input_len>0 && g_panes[g_focus].sess){
                            ring_push(&g_panes[g_focus].ring,g_input);
                            puci_session_begin_turn(g_panes[g_focus].sess,g_input);
                        }
                        g_input_len=0; g_input[0]=0; g_input_mode=0;
                    } else if(c==27 || c==3){ g_input_len=0; g_input[0]=0; g_input_mode=0; }
                    else if(c==127 || c==8){ if(g_input_len>0){ g_input_len--; g_input[g_input_len]=0; } }
                    else if(g_input_len+1<sizeof g_input && c>=32) { g_input[g_input_len++]=c; g_input[g_input_len]=0; }
                } else {
                    if(c=='q' || c==3){ running=0; break; }
                    if(c=='\t'){ g_focus=(g_focus+1)%g_npane; }
                    if(c=='s'){ g_show_sidebar=!g_show_sidebar; }
                    if(c=='\n' || c=='\r'){ g_input_mode=1; g_input_len=0; g_input[0]=0; ring_push(&g_panes[g_focus].ring,"> "); }
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
                    char tmp[RING_COLS]; snprintf(tmp,sizeof tmp,"%.*s", (int)ev.text_len, ev.text);
                    ring_push(&g_panes[i].ring, tmp);
                } else if(ev.kind==PUCI_EVENT_TOOL_CALL_START){
                    char tmp[128]; snprintf(tmp,sizeof tmp,"[tool] %s", ev.payload.tool_call.name?ev.payload.tool_call.name:"?");
                    ring_push(&g_panes[i].ring, tmp);
                } else if(ev.kind==PUCI_EVENT_TURN_COMPLETE){
                    ring_push(&g_panes[i].ring,"— done —");
                } else if(ev.kind==PUCI_EVENT_ERROR && ev.text){
                    char tmp[256]; snprintf(tmp,sizeof tmp,"error: %.*s",(int)ev.text_len, ev.text);
                    ring_push(&g_panes[i].ring, tmp);
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
