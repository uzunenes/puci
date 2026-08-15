/* tui.c - Featherweight POSIX ANSI TUI (v0.5.0)
 * Single-thread poll ring: STDIN + N*session fds, dirty rendering 2Hz gauges, no ncurses.
 * Views: header, 2x2 swarm grid, gauges, diff overlay, sidebar, footer.
 */
#include "tui.h"
#include <puci/puci.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

static void twrite(const void *buf, size_t n){ ssize_t r=write(STDOUT_FILENO,buf,n); (void)r; }
#define MAX_PANES 4
#define RING_LINES 500
#define RING_COLS 256

static volatile sig_atomic_t g_winch = 0;
static void on_winch(int s){ (void)s; g_winch=1; }

static struct termios g_orig_term;
static int g_raw=0;
static void restore_term(void){
    if(g_raw){ tcsetattr(STDIN_FILENO,TCSANOW,&g_orig_term); g_raw=0; }
    const char *seq="\x1b[?1049l\x1b[?25h";
    twrite(seq,strlen(seq));
}
static int enable_raw(void){
    if(!isatty(STDIN_FILENO)) return -1;
    if(tcgetattr(STDIN_FILENO,&g_orig_term)!=0) return -1;
    struct termios raw=g_orig_term;
    raw.c_lflag &= (tcflag_t)~(ECHO|ICANON|ISIG|IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON|ICRNL|BRKINT|INPCK|ISTRIP);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN]=0; raw.c_cc[VTIME]=0;
    if(tcsetattr(STDIN_FILENO,TCSANOW,&raw)!=0) return -1;
    g_raw=1; atexit(restore_term);
    return 0;
}
static void enter_alt(void){
    const char *seq="\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l";
    twrite(seq,strlen(seq));
}
static void ansi_goto(int r,int c, char *buf,size_t cap){ snprintf(buf,cap,"\x1b[%d;%dH",r,c); }

static void get_winsize(int *rows,int *cols){
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

static puci_approval tui_approval_cb(const puci_approval_request *req, void *ud){
    (void)ud;
    if(!req) return PUCI_APPROVAL_DENY;
    snprintf(g_approval_diff,sizeof g_approval_diff,"%s\n%s", req->tool?req->tool:"tool", req->display?req->display:(req->diff?req->diff:req->args_json?req->args_json:""));
    g_approval_pending=1;
    g_hunk_off=0;
    int cancel_fd = req->cancel?req->cancel->fd:-1;
    while(g_approval_pending){
        if(req->cancel && req->cancel->flag && *req->cancel->flag) return PUCI_APPROVAL_DENY;
        struct pollfd pf[2]; int nf=1;
        pf[0].fd=STDIN_FILENO; pf[0].events=POLLIN;
        if(cancel_fd>=0){ pf[1].fd=cancel_fd; pf[1].events=POLLIN; nf=2; }
        int pr=poll(pf,(nfds_t)nf,100);
        if(pr<0 && errno==EINTR) continue;
        if(nf==2 && (pf[1].revents&POLLIN)) return PUCI_APPROVAL_DENY;
        if(pf[0].revents&POLLIN){
            char c; ssize_t n=read(STDIN_FILENO,&c,1);
            if(n==1){
                if(c=='y'||c=='Y'){ g_approval_result=1; g_approval_pending=0; return PUCI_APPROVAL_ALLOW; }
                if(c=='n'||c=='N'||c=='q'||c==27){ g_approval_result=-1; g_approval_pending=0; return PUCI_APPROVAL_DENY; }
                if(c=='j') g_hunk_off++;
                if(c=='k' && g_hunk_off>0) g_hunk_off--;
            }
        }
    }
    return g_approval_result==1?PUCI_APPROVAL_ALLOW:PUCI_APPROVAL_DENY;
}

static void draw_box(int r,int c,int h,int w,const char *title,int focused){
    char seq[32];
    ansi_goto(r,c,seq,sizeof seq); twrite(seq,strlen(seq));
    twrite( focused?"\x1b[1;36m":"\x1b[2m", focused?7:4);
    twrite("┌",3);
    for(int i=0;i<w-2;i++) twrite("─",3);
    twrite("┐",3);
    twrite("\x1b[0m",4);
    if(title){
        char t[64]; { size_t _n=strlen(title); if(_n>58) _n=58; t[0]=32; memcpy(t+1,title,_n); t[1+_n]=32; t[2+_n]=0; }
        ansi_goto(r,c+2,seq,sizeof seq); twrite(seq,strlen(seq));
        twrite(t,strlen(t));
    }
    for(int i=1;i<h-1;i++){
        ansi_goto(r+i,c,seq,sizeof seq); twrite(seq,strlen(seq));
        twrite("│",3);
        ansi_goto(r+i,c+w-1,seq,sizeof seq); twrite(seq,strlen(seq));
        twrite("│",3);
    }
    ansi_goto(r+h-1,c,seq,sizeof seq); twrite(seq,strlen(seq));
    twrite("└",3);
    for(int i=0;i<w-2;i++) twrite("─",3);
    twrite("┘",3);
}
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
static void draw_gauge(int r,int c,int w,long cur,long max,const char *label){
    char seq[32]; ansi_goto(r,c,seq,sizeof seq); twrite(seq,strlen(seq));
    char bar[256]; int fill = max>0? (int)((cur* (long)(w-12))/max):0; if(fill<0) fill=0; if(fill>w-12) fill=w-12;
    int pos=snprintf(bar,sizeof bar,"%s [",label); if(pos<0) pos=0;
    for(int i=0;i<w-12;i++) bar[pos++]= i<fill ? '#' : '-';
    bar[pos++]=']'; bar[pos]=0;
    twrite(bar,strlen(bar));
    char info[32]; snprintf(info,sizeof info," %ld/%ld",cur,max);
    twrite(info,strlen(info));
    twrite("\x1b[K",3);
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static void render_frame(int rows,int cols){
    twrite("\x1b[H",3);
    long rss=read_rss_kb(); long pss=read_pss_kb();
    long ctx_cur=0, ctx_bud=131072;
    if(g_npane>0 && g_panes[0].sess){ ctx_cur=(long)puci_session_context_bytes(g_panes[0].sess); ctx_bud=(long)puci_session_get_context_budget(g_panes[0].sess); }
    char hdr[256];
    snprintf(hdr,sizeof hdr," PUCI v0.5.0 ─ Swarm %d/%d ─ RSS %ld kB (PSS %ld) ─ Context %ld/%ld ", g_npane, MAX_PANES, rss, pss, ctx_cur, ctx_bud);
    char seq[32]; ansi_goto(1,1,seq,sizeof seq); twrite(seq,strlen(seq));
    twrite("\x1b[1;44;37m",10); twrite(hdr,strlen(hdr)); twrite("\x1b[0m",4); twrite("\x1b[K",3);
    int usable_rows = rows-4;
    int pane_h = usable_rows/2; if(pane_h<4) pane_h=4;
    int pane_w = (cols-3)/2; if(pane_w<20) pane_w=20;
    int grid_cols= g_show_sidebar?1:2; (void)grid_cols;
    for(int i=0;i<g_npane && i<MAX_PANES;i++){
        int pr = 2 + (i/2)*pane_h;
        int pc = 1 + (i%2)*(pane_w+1);
        if(g_show_sidebar && i>=2) continue;
        draw_box(pr,pc,pane_h,pane_w,g_panes[i].title, g_focus==i);
        int inner_h = pane_h-2; int inner_w = pane_w-2;
        int start = g_panes[i].ring.count>inner_h? g_panes[i].ring.count-inner_h:0;
        for(int l=0;l<inner_h && start+l<g_panes[i].ring.count;l++){
            int idx=(g_panes[i].ring.head + start + l)%RING_LINES;
            ansi_goto(pr+1+l, pc+1, seq, sizeof seq); twrite(seq,strlen(seq));
            char line[RING_COLS]; snprintf(line,sizeof line,"%-*.*s", inner_w, inner_w, g_panes[i].ring.lines[idx]);
            if(line[0]=='+') twrite("\x1b[32m",5);
            else if(line[0]=='-') twrite("\x1b[31m",5);
            twrite(line,strlen(line));
            twrite("\x1b[K",3);
            twrite("\x1b[0m",4);
        }
        // clear remaining inner lines
        for(int l=g_panes[i].ring.count; l<inner_h; l++){
            ansi_goto(pr+1+l, pc+1, seq, sizeof seq); twrite(seq,strlen(seq));
            twrite("\x1b[K",3);
        }
    }
    if(g_show_sidebar){
        int sr=2, sc= cols-22, sh= usable_rows, sw=21;
        draw_box(sr,sc,sh,sw,"Sessions",0);
        for(int i=0;i<g_npane;i++){
            ansi_goto(sr+1+i, sc+2, seq,sizeof seq); twrite(seq,strlen(seq));
            char ln[32]; snprintf(ln,sizeof ln,"%s %s", g_focus==i?"▸":" ", g_panes[i].title);
            twrite(ln,strlen(ln)); twrite("\x1b[K",3);
        }
    }
    draw_gauge(rows-2,1, cols-2, ctx_cur, ctx_bud, "Context");
    ansi_goto(rows,1,seq,sizeof seq); twrite(seq,strlen(seq));
    const char *footer="[y] Approve  [n] Deny  [j/k] Hunk  [TAB] Focus  [s] Sidebar  [q] Quit  [Enter] Prompt";
    twrite("\x1b[2m",4); twrite(footer,strlen(footer)); twrite("\x1b[0m",4); twrite("\x1b[K",3);
    if(g_approval_pending){
        int or_= rows/2 - 6, oc= cols/2 - 30, oh=12, ow=60;
        if(or_<1) or_=1;
        if(oc<1) oc=1;
        draw_box(or_,oc,oh,ow,"Approval — y/n, j/k scroll",1);
        char *copy=strdup(g_approval_diff); char *tok=strtok(copy,"\n"); int l=0;
        while(tok && l<oh-2){
            if(l>=g_hunk_off){
                ansi_goto(or_+1+(l-g_hunk_off), oc+1, seq,sizeof seq); twrite(seq,strlen(seq));
                if(tok[0]=='+') twrite("\x1b[32m",5);
                else if(tok[0]=='-') twrite("\x1b[31m",5);
                char line[64]; snprintf(line,sizeof line,"%-*.*s", ow-2, ow-2, tok);
                twrite(line,strlen(line)); twrite("\x1b[K",3); twrite("\x1b[0m",4);
            }
            l++; tok=strtok(NULL,"\n");
        }
        free(copy);
    }
    // ensure cursor out of way
    ansi_goto(rows, cols, seq, sizeof seq); twrite(seq,strlen(seq));
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
    if(enable_raw()!=0){ fprintf(stderr,"tui: raw mode failed\n"); return 1; }
    enter_alt();
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_winch; sigemptyset(&sa.sa_mask); sa.sa_flags=SA_RESTART; sigaction(SIGWINCH,&sa,NULL);
    g_npane=1; g_focus=0;
    for(int i=0;i<MAX_PANES;i++){ memset(&g_panes[i],0,sizeof g_panes[i]); snprintf(g_panes[i].title,sizeof g_panes[i].title,"Agent %c",'A'+i); }
    puci_context *ctx=NULL;
    if(puci_context_create(NULL,&ctx)!=PUCI_OK){ restore_term(); fprintf(stderr,"ctx fail\n"); return 1; }
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
    char input[512]; size_t ilen=0; int input_mode=0;
    int rows, cols; get_winsize(&rows,&cols);
    struct timespec last_gauge; clock_gettime(CLOCK_MONOTONIC,&last_gauge);
    int dirty=1;
    int running=1;
    while(running){
        if(g_winch){ g_winch=0; get_winsize(&rows,&cols); dirty=1; }
        {
            struct timespec ng; clock_gettime(CLOCK_MONOTONIC,&ng);
            long ge = (ng.tv_sec-last_gauge.tv_sec)*1000 + (ng.tv_nsec-last_gauge.tv_nsec)/1000000;
            if(ge >= 500){ dirty=1; last_gauge=ng; }
        }
        if(dirty){ render_frame(rows,cols); dirty=0; if(input_mode){ char seq[32]; ansi_goto(rows-1,1,seq,sizeof seq); twrite(seq,strlen(seq)); twrite("\x1b[7m",4); twrite("> ",2); twrite(input,ilen); twrite("\x1b[K",3); twrite("\x1b[0m",4); } }
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
                if(input_mode){
                    if(c=='\n'||c=='\r'){
                        input[ilen]=0;
                        if(ilen>0 && g_panes[g_focus].sess){
                            ring_push(&g_panes[g_focus].ring,input);
                            puci_session_begin_turn(g_panes[g_focus].sess,input);
                        }
                        ilen=0; input_mode=0;
                    } else if(c==27 || c==3){ ilen=0; input_mode=0; }
                    else if(c==127 || c==8){ if(ilen>0) ilen--; }
                    else if(ilen+1<sizeof input && c>=32) input[ilen++]=c;
                } else {
                    if(c=='q' || c==3){ running=0; break; }
                    if(c=='\t'){ g_focus=(g_focus+1)%g_npane; }
                    if(c=='s'){ g_show_sidebar=!g_show_sidebar; }
                    if(c=='\n' || c=='\r'){ input_mode=1; ilen=0; ring_push(&g_panes[g_focus].ring,"> "); }
                    // y/n handled via approval poll; also allow here if pending
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
    restore_term();
    return 0;
}
