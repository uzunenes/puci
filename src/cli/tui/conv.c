/* conv.c — owned semantic conversation model (see conv.h). Depends only on
 * md.h (block parsing) plus the C library; never touches the terminal or the
 * visual Ring.
 */
#include "conv.h"
#include "md.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* --------------------------- helpers ------------------------------ */
static char *xdup(const char *s, size_t n){
    char *r=malloc(n+1); if(!r) return NULL;
    memcpy(r,s,n); r[n]=0; return r;
}
static size_t count_lines(const char *s, size_t n){
    size_t l=1; for(size_t i=0;i<n;i++) if(s[i]=='\n') l++;
    return l;
}
static void part_free(TuiContentPart *p){
    free(p->raw_source); p->raw_source=NULL; p->raw_len=0;
    free(p->language);   p->language=NULL;
    free(p->filename);   p->filename=NULL;
}
static void msg_free(TuiMessage *m){
    for(size_t i=0;i<m->part_count;i++) part_free(&m->parts[i]);
    free(m->parts); m->parts=NULL; m->part_count=m->part_cap=0;
    free(m->raw_source); m->raw_source=NULL; m->raw_len=m->raw_cap=0;
    free(m->plain); m->plain=NULL; m->plain_len=0;
}
static int msg_append_raw(TuiMessage *m, const char *data, size_t n);

void conv_init(TuiConversation *c){ memset(c,0,sizeof(*c)); }
void conv_free(TuiConversation *c){
    for(size_t i=0;i<c->count;i++) msg_free(&c->messages[i]);
    free(c->messages); memset(c,0,sizeof(*c));
}

uint64_t conv_max_id(TuiConversation *c){
    uint64_t mx=0;
    for(size_t i=0;i<c->count;i++){
        if(c->messages[i].id>mx) mx=c->messages[i].id;
        for(size_t k=0;k<c->messages[i].part_count;k++)
            if(c->messages[i].parts[k].id>mx) mx=c->messages[i].parts[k].id;
    }
    return mx;
}
static TuiContentPart *msg_new_part(TuiMessage *m){
    if(m->part_count>=m->part_cap){
        size_t nc=m->part_cap? m->part_cap*2 : 4;
        TuiContentPart *np=realloc(m->parts,nc*sizeof(*np));
        if(!np) return NULL;
        m->parts=np; m->part_cap=nc;
    }
    TuiContentPart *p=&m->parts[m->part_count++];
    memset(p,0,sizeof(*p));
    return p;
}

TuiMessage *conv_new_message(TuiConversation *c, TuiMessageRole role){
    if(c->count>=c->cap){
        size_t nc=c->cap? c->cap*2 : 8;
        TuiMessage *nm=realloc(c->messages,nc*sizeof(*nm));
        if(!nm) return NULL;
        c->messages=nm; c->cap=nc;
    }
    TuiMessage *m=&c->messages[c->count++];
    memset(m,0,sizeof(*m));
    m->id=conv_max_id(c)+1;
    m->role=role;
    c->version++;
    return m;
}

static int msg_append_raw(TuiMessage *m, const char *data, size_t n){
    if(!data || n==0) return 0;
    size_t need=m->raw_len+n;
    if(need+1>m->raw_cap){
        size_t nc=m->raw_cap? m->raw_cap*2 : 256;
        while(nc<need+1) nc*=2;
        char *np=realloc(m->raw_source,nc); if(!np) return -1;
        m->raw_source=np; m->raw_cap=nc;
    }
    memcpy(m->raw_source+m->raw_len,data,n);
    m->raw_len+=n; m->raw_source[m->raw_len]=0;
    m->content_version++;
    return 0;
}
int conv_append_raw(TuiConversation *c, TuiMessage *m, const char *data, size_t n){
    (void)c;
    return msg_append_raw(m,data,n);
}
int conv_append(TuiConversation *c, TuiMessage *m, const char *data, size_t n){
    (void)c;
    if(!m || !m->streaming) return -1;
    return msg_append_raw(m,data,n);
}
TuiMessage *conv_begin_assistant(TuiConversation *c){
    TuiMessage *m=conv_new_message(c,TUI_ROLE_ASSISTANT);
    if(!m) return NULL;
    m->streaming=true;
    return m;
}

int conv_appendf(TuiConversation *c, TuiMessage *m, const char *fmt, ...){
    (void)c;
    if(!m || !m->streaming) return -1;
    va_list ap; va_start(ap,fmt);
    char small[256]; va_list aq; va_copy(aq,ap);
    int n=vsnprintf(small,sizeof small,fmt,ap); va_end(ap);
    if(n<0){ va_end(aq); return -1; }
    if((size_t)n<sizeof small){ va_end(aq); return msg_append_raw(m,small,(size_t)n); }
    char *big=malloc((size_t)n+1); if(!big){ va_end(aq); return -1; }
    vsnprintf(big,(size_t)n+1,fmt,aq); va_end(aq);
    int rc=msg_append_raw(m,big,(size_t)n);
    free(big); return rc;
}

/* Build derived TUI_CONTENT_PART_CODE / TEXT parts from raw_source markdown. */
static void conv_parse_parts(TuiConversation *c, TuiMessage *m){
    for(size_t i=0;i<m->part_count;i++) part_free(&m->parts[i]);
    free(m->parts); m->parts=NULL; m->part_count=m->part_cap=0;

    MdDoc d=md_parse(m->raw_source, m->raw_len, m->streaming);
    int has_code=0;
    for(size_t i=0;i<d.count;i++){
        if(d.blocks[i].type!=MD_BLOCK_CODE || !d.blocks[i].is_fence) continue;
        size_t bo=d.blocks[i].body_off, bl=d.blocks[i].body_len;
        if(bo+bl>m->raw_len) bl=m->raw_len-bo;
        TuiContentPart *p=msg_new_part(m); if(!p) break;
        p->id=conv_max_id(c)+1;
        p->kind=TUI_PART_CODE;
        if(d.blocks[i].lang_str[0]){
            char alias[32];
            md_lang_normalize(d.blocks[i].lang_str, alias, sizeof alias);
            p->language=xdup(alias, strlen(alias));
        }
        p->raw_source=xdup(m->raw_source+bo, bl);
        p->raw_len=bl;
        p->line_count=count_lines(p->raw_source, bl);
        p->has_newline_ends=(bl>0 && (m->raw_source[bo+bl-1]=='\n'));
        p->status = d.blocks[i].closed ? TUI_COMPLETE : TUI_PARTIAL_STREAM;
        has_code=1;
    }
    /* Nonempty, fully-complete message with no fenced code: allow a single
       synthesized TEXT part so parts[] mirrors the message (still owned). */
    if(!has_code && m->raw_len>0){
        TuiContentPart *p=msg_new_part(m);
        if(p){
            p->id=conv_max_id(c)+1;
            p->kind=TUI_PART_TEXT;
            p->raw_source=xdup(m->raw_source,m->raw_len);
            p->raw_len=m->raw_len;
            p->line_count=count_lines(p->raw_source,m->raw_len);
            p->status=TUI_COMPLETE;
        }
    }
}

int conv_end_assistant(TuiConversation *c, TuiMessage *m){
    if(!m || !m->streaming) return -1;
    m->streaming=false; m->cancelled=false; m->failed=false;
    conv_parse_parts(c,m);
    return 0;
}
int conv_cancel_assistant(TuiConversation *c, TuiMessage *m){
    if(!m || !m->streaming) return -1;
    m->streaming=false; m->cancelled=true;
    conv_parse_parts(c,m);
    return 0;
}

/* --------------------------- user + tool messages -------------------- */
TuiMessage *conv_user_message(TuiConversation *c, const char *text, size_t text_len){
    TuiMessage *m=conv_new_message(c,TUI_ROLE_USER);
    if(!m) return NULL;
    if(text && text_len) msg_append_raw(m,text,text_len);
    conv_parse_parts(c,m);
    return m;
}
TuiContentPart *conv_user_add_paste(TuiConversation *c, TuiMessage *m,
                                    const char *data, size_t n, const char *lang){
    if(!m || m->role!=TUI_ROLE_USER) return NULL;
    /* Ordering contract (Phase 2): the user message raw_source is the exact
       provider payload = text before, then a separating newline, then the
       paste body, in the order the segments were added. */
    if(m->raw_len>0) msg_append_raw(m,"\n",1);
    if(data && n) msg_append_raw(m,data,n);
    TuiContentPart *p=msg_new_part(m); if(!p) return NULL;
    p->id=conv_max_id(c)+1;
    p->kind=TUI_PART_PASTE;
    if(lang && lang[0]) p->language=xdup(lang,strlen(lang));
    p->raw_source=data? xdup(data,n) : NULL;
    p->raw_len=n;
    p->line_count=count_lines(data?data:"", n);
    p->has_newline_ends=(n>0 && data[n-1]=='\n');
    p->status=TUI_COMPLETE;
    return p;
}
TuiContentPart *conv_message_add_file(TuiConversation *c, TuiMessage *m,
                                      const char *filename, const char *lang,
                                      const char *data, size_t n, bool truncated){
    TuiContentPart *p=msg_new_part(m); if(!p) return NULL;
    p->id=conv_max_id(c)+1;
    p->kind=TUI_PART_FILE;
    if(filename) p->filename=xdup(filename,strlen(filename));
    if(lang && lang[0]) p->language=xdup(lang,strlen(lang));
    p->raw_source=xdup(data,n); p->raw_len=n;
    p->line_count=count_lines(data,n);
    p->has_newline_ends=(n>0 && data[n-1]=='\n');
    p->status=truncated?TUI_PARTIAL_UPSTREAM:TUI_COMPLETE;
    return p;
}
TuiContentPart *conv_message_add_tool(TuiConversation *c, TuiMessage *m,
                                      const char *data, size_t n){
    TuiContentPart *p=msg_new_part(m); if(!p) return NULL;
    p->id=conv_max_id(c)+1;
    p->kind=TUI_PART_TOOL_OUTPUT;
    p->raw_source=xdup(data,n); p->raw_len=n;
    p->line_count=count_lines(data,n);
    p->has_newline_ends=(n>0 && data[n-1]=='\n');
    p->status=TUI_COMPLETE;
    return p;
}

/* --------------------------- copy ------------------------------------ */
char *conv_part_copy(const TuiContentPart *p, size_t *out_len){
    if(!p){ if(out_len) *out_len=0; return NULL; }
    char *s=xdup(p->raw_source?p->raw_source:"", p->raw_len);
    if(out_len) *out_len=s?p->raw_len:0;
    return s;
}
char *conv_message_raw_copy(const TuiMessage *m, size_t *out_len){
    if(!m){ if(out_len)*out_len=0; return NULL; }
    char *s=xdup(m->raw_source?m->raw_source:"", m->raw_len);
    if(out_len) *out_len=s?m->raw_len:0;
    return s;
}

/* plain text: strip common inline fences, bullets markers become '-', keep
 * code bodies verbatim at correct byte ranges when we have part spans. */
/* Minimal deterministic Markdown->plain-text for copy. Operates on
 * raw_source, never on the cell grid. Handles headings, emphasis, inline code,
 * links, ordered/unordered lists, blockquotes, fenced code (verbatim), and
 * horizontal rules. Unknown constructs degrade to their inner text. */
static void md_to_plain(const char *src, size_t len, char *out, size_t cap){
    if(!cap) return;
    size_t o=0; size_t i=0;
    int in_code=0;
    /* buf for one logical line (NUL-safe up to \n) */
    while(i<len && o+1<cap){
        /* collect one physical line */
        size_t ls=i; size_t le=i;
        while(le<len && src[le]!='\n') le++;
        size_t ll=le-ls;
        /* trim leading/trailing whitespace for decision, keep content span */
        size_t start=ls; while(start<le && (src[start]==' '||src[start]=='\t')) start++;
        if(ll==0){
            if(o+2<cap){ out[o++]='\n'; } o+=0; i=le+1; continue;
        }
        /* fence detection at line start (after spaces) */
        {
            int ticks=0; size_t q=start;
            while(q<le && src[q]=='`'){ ticks++; q++; }
            int is_fence=(ticks>=3 ? (q==le || src[q]==' '||src[q]=='\t') : 0);
            if(is_fence){
                in_code = !in_code; /* toggles regardless of lang token */
                if(o+1<cap) out[o++]='\n';
                i=le+1; continue;
            }
        }
        if(in_code){
            /* verbatim code body (strip one trailing CR, keep newline) */
            size_t bl=ll;
            if(bl>0 && src[le-1]=='\r') bl--;
            if(o+bl+1<cap){ memcpy(out+o,src+ls,bl); o+=bl; out[o++]='\n'; }
            i=le+1; continue;
        }
        /* strip a leading blockquote marker */
        {
            size_t st=start;
            if(st<le && src[st]=='>'){ st++; if(st<le && src[st]==' ') st++; }
            /* strip leading "### " heading markers */
            while(st<le && (src[st]=='#') && (st-start<6)) st++;
            if(st-start>=1 && st<le && src[st]==' ') st++;
            /* list marker */
            if(st<le && (src[st]=='-'||src[st]=='*'||src[st]=='+') && st+1<le && src[st+1]==' '){
                if(o+1<cap) out[o++]='-';
                st+=2;
            } else if(st<le && src[st]>='0'&&src[st]<='9'){
                size_t t2=st; while(t2<le && src[t2]>='0'&&src[t2]<='9') t2++;
                if(t2<le && (src[t2]=='.'||src[t2]==')') && (t2+1<le && src[t2+1]==' ')){ st=t2+2; }
            }
            /* if entirely a heading ('####' then end) keep a newline */
            if(st>=le){ if(o+1<cap) out[o++]='\n'; i=le+1; continue; }
            /* copy the line body stripping inline decoration */
            while(st<le && o+1<cap){
                char c=src[st];
                if(c=='`'){ /* inline code: copy inner verbatim */
                    st++;
                    while(st<le && src[st]!='`' && o+1<cap) out[o++]=src[st++];
                    if(st<le) st++; /* closing backtick */
                    continue;
                }
                if(c=='['){ /* link [text](url) */
                    size_t e2=st+1;
                    while(e2<le && src[e2]!=']' ) e2++;
                    if(e2<le){ /* could be link */
                        size_t paren=e2+1;
                        while(paren<le && src[paren]!='(' && src[paren]!='\n') paren++;
                        if(paren<le && src[paren]=='('){
                            while(st+1<e2 && o+1<cap){ out[o++]=src[++st]; }
                            /* consume through ')' */
                            while(st<le && src[st]!=')') st++;
                            if(st<le) st++;
                            continue;
                        }
                    }
                }
                if(c=='*'||c=='_'){
                    /* toggle bold/italic (eat markers) */
                    if(st+1<le && (src[st+1]=='*'||src[st+1]=='_')){ st+=2; }
                    else st++;
                    continue;
                }
                out[o++]=c;
                if(c=='\r'){ /* normalize CRLF: drop CR, LF appended below */ out[o-1]='\n'; }
                st++;
                if(c=='\r' && st<le && src[st]=='\n') st++;
            }
            out[o++]='\n';
        }
        i=le+1;
    }
    if(o>0 && out[o-1]=='\n') o--; /* trim a single trailing newline for copy */
    if(o<cap) out[o]=0;
}

static void compute_plain(TuiConversation *c, TuiMessage *m){
    (void)c;
    free(m->plain); m->plain=NULL; m->plain_len=0;
    if(!m->raw_source){ m->plain=xdup("",0); m->plain_len=0; return; }
    /* conservative rough estimate so the output always fits */
    size_t cap=m->raw_len+8;
    char *out=malloc(cap?cap:1); if(!out) return;
    /* note: md_to_plain may need up to ~raw_len; we pass cap and clip */
    md_to_plain(m->raw_source,m->raw_len,out,cap);
    m->plain=out;
    m->plain_len=strlen(out);
}
char *conv_message_plain_copy(const TuiMessage *m, size_t *out_len){
    if(!m){ if(out_len)*out_len=0; return NULL; }
    if(!m->plain){ /* caller must have run conv_message_plain */ }
    char *s=xdup(m->plain?m->plain:m->raw_source?m->raw_source:"", m->plain?m->plain_len:(m->raw_len));
    if(out_len)*out_len=s?(m->plain?m->plain_len:m->raw_len):0;
    return s;
}
void conv_message_plain(TuiConversation *c, TuiMessage *m){ compute_plain(c,m); }

/* --------------------------- JSON sidecar (Phase 9) ----------------- */
typedef struct { char *p; size_t len, cap; } JBuf;
static void jb_add(JBuf *j, const char *s, size_t n){
    if(j->len+n+1>j->cap){ size_t nc=j->cap?j->cap*2:256; while(nc<j->len+n+1) nc*=2;
        char *np=realloc(j->p,nc); if(!np) return; j->p=np; j->cap=nc; }
    if(n) memcpy(j->p+j->len,s,n);
    j->len+=n; j->p[j->len]=0;
}
static void jb_q(JBuf *j, const char *s, size_t n){
    jb_add(j,"\"",1);
    for(size_t i=0;i<n;i++){ unsigned char ch=(unsigned char)s[i];
        switch(ch){
            case '"': jb_add(j,"\\\"",2); break;
            case '\\': jb_add(j,"\\\\",2); break;
            case '\n': jb_add(j,"\\n",2); break;
            case '\r': jb_add(j,"\\r",2); break;
            case '\t': jb_add(j,"\\t",2); break;
            default:
                if(ch<0x20){ char b[8]; snprintf(b,sizeof b,"\\u%04x",ch); jb_add(j,b,strlen(b)); }
                else jb_add(j,(const char*)&ch,1);
                break;
        }
    }
    jb_add(j,"\"",1);
}
static const char *msg_kind_name(TuiPartKind k){
    switch(k){ case TUI_PART_TEXT:return "text"; case TUI_PART_PASTE:return "paste";
        case TUI_PART_CODE:return "code"; case TUI_PART_TOOL_OUTPUT:return "tool";
        case TUI_PART_FILE:return "file"; } return "text";
}
static TuiPartKind msg_kind_from(const char *k){
    if(!k) return TUI_PART_TEXT;
    if(strcmp(k,"paste")==0) return TUI_PART_PASTE;
    if(strcmp(k,"code")==0) return TUI_PART_CODE;
    if(strcmp(k,"tool")==0) return TUI_PART_TOOL_OUTPUT;
    if(strcmp(k,"file")==0) return TUI_PART_FILE;
    return TUI_PART_TEXT;
}
static const char *msg_role(TuiMessageRole r){
    switch(r){ case TUI_ROLE_USER:return "user"; case TUI_ROLE_ASSISTANT:return "assistant";
    case TUI_ROLE_SYSTEM:return "system"; case TUI_ROLE_TOOL:return "tool";
    case TUI_ROLE_ERROR:return "error"; } return "user";
}
static TuiMessageRole msg_role_from(const char *r){
    if(!r) return TUI_ROLE_USER;
    if(strcmp(r,"assistant")==0) return TUI_ROLE_ASSISTANT;
    if(strcmp(r,"system")==0) return TUI_ROLE_SYSTEM;
    if(strcmp(r,"tool")==0) return TUI_ROLE_TOOL;
    if(strcmp(r,"error")==0) return TUI_ROLE_ERROR;
    return TUI_ROLE_USER;
}
static TuiPartStatus msg_status_from(const char *s){
    if(!s) return TUI_COMPLETE;
    if(strcmp(s,"partial_stream")==0) return TUI_PARTIAL_STREAM;
    if(strcmp(s,"partial_upstream")==0) return TUI_PARTIAL_UPSTREAM;
    if(strcmp(s,"partial_hardlimit")==0) return TUI_PARTIAL_HARDLIMIT;
    if(strcmp(s,"collapsed")==0) return TUI_COLLAPSED;
    return TUI_COMPLETE;
}

char *conv_serialize(const TuiConversation *c, size_t *out_len){
    JBuf j={0};
    jb_add(&j,"{\"tui_content_version\":1,\"messages\":[",(sizeof("{\"tui_content_version\":1,\"messages\":[")-1));
    for(size_t mi=0;mi<c->count;mi++){
        const TuiMessage *m=&c->messages[mi];
        if(mi) jb_add(&j,",",1);
        jb_add(&j,"{\"id\":",6);
        char nb[32]; snprintf(nb,sizeof nb,"%llu,",(unsigned long long)m->id);
        jb_add(&j,nb,strlen(nb));
        jb_add(&j,"\"role\":",(sizeof("\"role\":")-1));
        const char *rn=msg_role(m->role); jb_q(&j,rn,strlen(rn));
        jb_add(&j,",\"raw_source\":",14);
        jb_q(&j,m->raw_source?m->raw_source:"",m->raw_len);
        if(m->part_count){
            jb_add(&j,",\"parts\":[",10);
            for(size_t pi=0;pi<m->part_count;pi++){
                const TuiContentPart *p=&m->parts[pi];
                if(pi) jb_add(&j,",",1);
                jb_add(&j,"{\"kind\":",8);
                const char *kn=msg_kind_name(p->kind); jb_q(&j,kn,strlen(kn));
                if(p->language){ jb_add(&j,",\"language\":",12); jb_q(&j,p->language,strlen(p->language)); }
                if(p->filename){ jb_add(&j,",\"filename\":",12); jb_q(&j,p->filename,strlen(p->filename)); }
                const char *st=p->status==TUI_PARTIAL_STREAM?"partial_stream":
                             p->status==TUI_PARTIAL_UPSTREAM?"partial_upstream":
                             p->status==TUI_PARTIAL_HARDLIMIT?"partial_hardlimit":
                             p->status==TUI_COLLAPSED?"collapsed":"complete";
                jb_add(&j,",\"status\":",10); jb_q(&j,st,strlen(st));
                jb_add(&j,",\"source\":",10); jb_q(&j,p->raw_source?p->raw_source:"",p->raw_len);
                jb_add(&j,"}",1);
            }
            jb_add(&j,"]",1);
        }
        jb_add(&j,"}",1);
    }
    jb_add(&j,"]}",2);
    if(out_len) *out_len=j.len;
    return j.p;
}

/* Minimal JSON parser for the sidecar schema above. Bounds-checked,
 * dependency-free. Only reads string / number / object / array values; unknown
 * fields are skipped. On any structural error it stops and returns nonzero. */
typedef struct { const char *s; size_t len; size_t i; int err; } Jp;
static void jp_ws(Jp *p){ while(p->i<p->len){ char c=p->s[p->i]; if(c==' '||c=='\t'||c=='\n'||c=='\r') p->i++; else break; } }
static int jp_peek(Jp *p, char c){ jp_ws(p); return p->i<p->len && p->s[p->i]==c; }
static int jp_accept(Jp *p, char c){ jp_ws(p); if(p->i<p->len && p->s[p->i]==c){ p->i++; return 1;} return 0; }
/* parse a JSON string, returning owned bytes (or NULL on error) */
static char *jp_string(Jp *p){
    jp_ws(p);
    if(p->i>=p->len || p->s[p->i]!='"'){ p->err=1; return NULL; }
    p->i++;
    size_t cap=32, n=0; char *out=malloc(cap); if(!out){ p->err=1; return NULL; }
    while(p->i<p->len){
        char c=p->s[p->i++];
        if(c=='"'){ out[n]=0; return out; }
        if(c=='\\'){
            if(p->i>=p->len){ p->err=1; free(out); return NULL; }
            char e=p->s[p->i++];
            char r=0;
            switch(e){ case 'n':r='\n';break; case 't':r='\t';break; case 'r':r='\r';break;
                case '"':r='"';break; case '\\':r='\\';break; case '/':r='/';break; default:break; }
            if(r) c=r;
            else if(e=='u'){
                /* skip the 4 hex digits (we do not need the code point) */
                if(p->i+4>p->len){ p->err=1; free(out); return NULL; }
                p->i+=4; continue;
            } else { p->err=1; free(out); return NULL; }
        }
        if(n+2>cap){ size_t nc=cap?cap*2:64; char *np=realloc(out,nc); if(!np){ p->err=1; free(out); return NULL; } out=np; cap=nc; }
        out[n++]=(char)c;
    }
    p->err=1; free(out); return NULL;
}
/* skip an arbitrary JSON value at the current position (does not validate deeply) */
static void jp_skip(Jp *p){
    jp_ws(p);
    if(p->i>=p->len) return;
    char c=p->s[p->i];
    if(c=='"'){ p->i++; while(p->i<p->len){ char d=p->s[p->i++]; if(d=='"')break; if(d=='\\'&&p->i<p->len)p->i++; } return; }
    if(c=='{'){ p->i++; jp_ws(p); if(jp_accept(p,'}'))return; for(;;){ char *k=jp_string(p); free(k); if(p->err||!jp_accept(p,':')){p->err=1;return;} jp_skip(p); jp_ws(p); if(jp_accept(p,','))continue; jp_accept(p,'}'); return; } }
    if(c=='['){ p->i++; jp_ws(p); if(jp_accept(p,']'))return; for(;;){ jp_skip(p); if(p->err)return; jp_ws(p); if(jp_accept(p,','))continue; jp_accept(p,']'); return; } }
    /* number / true / false / null */
    while(p->i<p->len && p->s[p->i]!=',' && p->s[p->i]!='}' && p->s[p->i]!=']' && p->s[p->i]!='\n') p->i++;
}

int conv_deserialize(TuiConversation *c, const char *json, size_t len){
    Jp p={json,len,0,0};
    if(!jp_accept(&p,'{')) return -1;
    while(!jp_peek(&p,'}')){
        size_t kn=0; char kbuf[64];
        if(!jp_accept(&p,'"')) return -2;
        while(p.i<p.len && p.s[p.i]!='"' && kn+1<sizeof(kbuf)) kbuf[kn++]=p.s[p.i++];
        if(!jp_accept(&p,'"')) return -2;
        kbuf[kn]=0;
        if(!jp_accept(&p,':')) return -2;
        if(strcmp(kbuf,"messages")==0){
            if(!jp_accept(&p,'[')) return -3;
            while(!jp_peek(&p,']')){
                if(p.err) return -4;
                TuiMessage *m=conv_new_message(c,TUI_ROLE_USER);
                if(!m) return -5;
                jp_accept(&p,','); /* consume comma between messages */
                if(!jp_accept(&p,'{')) return -3;
                while(!jp_peek(&p,'}')){
                    jp_accept(&p,','); /* consume comma between fields */
                    size_t k2=0; char k2b[64];
                    if(!jp_accept(&p,'"')){ p.err=1; break; }
                    while(p.i<p.len && p.s[p.i]!='"' && k2<sizeof(k2b)-1) k2b[k2++]=p.s[p.i++];
                    if(!jp_accept(&p,'"')){ p.err=1; break; }
                    k2b[k2]=0;
                    if(!jp_accept(&p,':')){ p.err=1; break; }
                    if(strcmp(k2b,"role")==0){
                        char *v=jp_string(&p); if(!v) p.err=1; else { m->role=msg_role_from(v); free(v); }
                    } else if(strcmp(k2b,"raw_source")==0){
                        char *v=jp_string(&p); if(!v) p.err=1; else { msg_append_raw(m,v,strlen(v)); free(v); }
                    } else if(strcmp(k2b,"parts")==0){
                        if(!jp_accept(&p,'[')){ p.err=1; break; }
                        while(!jp_peek(&p,']')){
                            if(p.err) break;
                            jp_accept(&p,','); /* comma between parts */
                            if(!jp_accept(&p,'{')){ p.err=1; break; }
                            TuiPartKind kind=TUI_PART_TEXT; TuiPartStatus status=TUI_COMPLETE;
                            char *lang=NULL,*file=NULL,*src=NULL;
                            while(!jp_peek(&p,'}')){
                                jp_accept(&p,',');
                                size_t k3=0; char k3b[64];
                                if(!jp_accept(&p,'"')){ p.err=1; break; }
                                while(p.i<p.len && p.s[p.i]!='"' && k3<sizeof(k3b)-1) k3b[k3++]=p.s[p.i++];
                                if(!jp_accept(&p,'"')){ p.err=1; break; }
                                k3b[k3]=0;
                                if(!jp_accept(&p,':')){ p.err=1; break; }
                                if(strcmp(k3b,"kind")==0){ char *v=jp_string(&p); if(!v)p.err=1;else{kind=msg_kind_from(v);free(v);} }
                                else if(strcmp(k3b,"language")==0){ char *v=jp_string(&p); if(!v)p.err=1;else{free(lang);lang=strdup(v);free(v);} }
                                else if(strcmp(k3b,"filename")==0){ char *v=jp_string(&p); if(!v)p.err=1;else{free(file);file=strdup(v);free(v);} }
                                else if(strcmp(k3b,"status")==0){ char *v=jp_string(&p); if(!v)p.err=1;else{status=msg_status_from(v);free(v);} }
                                else if(strcmp(k3b,"source")==0){ char *v=jp_string(&p); if(!v)p.err=1;else{free(src);src=strdup(v);free(v);} }
                                else jp_skip(&p);
                                if(p.err) break;
                            }
                            jp_accept(&p,'}');
                            if(!p.err && m && kind!=TUI_PART_TEXT){
                                TuiContentPart *part=msg_new_part(m);
                                if(part){
                                    part->id=conv_max_id(c)+1;
                                    part->kind=kind;
                                    part->status=status;
                                    part->language=lang; lang=NULL;
                                    part->filename=file; file=NULL;
                                    part->raw_source=src; src=NULL;
                                    part->raw_len=part->raw_source?strlen(part->raw_source):0;
                                    part->line_count=count_lines(part->raw_source?part->raw_source:"",part->raw_len);
                                    part->has_newline_ends=(part->raw_len>0 && part->raw_source[part->raw_len-1]=='\n');
                                }
                            }
                            free(lang); free(file); free(src);
                        }
                        jp_accept(&p,']');
                    } else jp_skip(&p);
                    if(p.err) break;
                }
                jp_accept(&p,'}');
            }
            if(!jp_accept(&p,']')) return -6;
        } else jp_skip(&p);
        if(p.err) return -2;
        if(!jp_peek(&p,',')) break;
        if(!jp_accept(&p,',')) break;
    }
    jp_accept(&p,'}');
    return 0;
}
