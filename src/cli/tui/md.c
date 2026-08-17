/* md.c — small, bounded Markdown block parser for the PUCI TUI.
 * See md.h. No termbox dependency; unit-testable in isolation.
 */
#include "md.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ------------------------- language normalization ---------------------- */

static const struct { const char *alias; HlLang lang; } k_langs[] = {
    { "c",     HL_LANG_C },        { "h",       HL_LANG_C },
    { "cpp",   HL_LANG_CPP },      { "cxx",     HL_LANG_CPP },
    { "cc",    HL_LANG_CPP },      { "hpp",     HL_LANG_CPP }, { "hxx", HL_LANG_CPP },
    { "python",HL_LANG_PYTHON },   { "py",      HL_LANG_PYTHON },
    { "json",  HL_LANG_JSON },
    { "markdown",HL_LANG_MARKDOWN},{ "md",      HL_LANG_MARKDOWN },
    { "sh",    HL_LANG_SHELL },    { "bash",    HL_LANG_SHELL }, { "shell", HL_LANG_SHELL },
    { "javascript", HL_LANG_JS },  { "js",      HL_LANG_JS },
    { "typescript", HL_LANG_TS },  { "ts",      HL_LANG_TS },
    { "yaml",  HL_LANG_YAML },     { "yml",     HL_LANG_YAML },
    { "toml",  HL_LANG_TOML },
    { "diff",  HL_LANG_DIFF },     { "patch",   HL_LANG_DIFF },
    { "rust",  HL_LANG_RUST },     { "rs",      HL_LANG_RUST },
    { "go",    HL_LANG_GO },
};

static void copy_alias(char *dst, size_t cap, const char *src){
    size_t i=0;
    for(i=0;src[i] && i+1<cap;i++) dst[i]=src[i];
    dst[i]=0;
}

static void lang_lower(const char *tag, char *out, size_t cap){
    if(!out || cap==0) return;
    out[0]=0;
    if(!tag) return;
    while(*tag==' '||*tag=='\t') tag++;
    size_t L=strlen(tag);
    while(L>0 && (tag[L-1]=='\r'||tag[L-1]=='\n'||tag[L-1]==' '||tag[L-1]=='\t')) L--;
    size_t i=0;
    for(i=0;i<L && i+1<cap;i++){
        char c=tag[i];
        out[i]=(c>='A'&&c<='Z')?(char)(c-'A'+'a'):c;
    }
    out[i]=0;
}

HlLang md_lang_normalize(const char *tag, char *alias, size_t cap){
    char low[48];
    if(alias && cap) alias[0]=0;
    lang_lower(tag, low, sizeof low);
    if(alias && cap && low[0]) copy_alias(alias, cap, low);
    for(size_t k=0;k<sizeof(k_langs)/sizeof(k_langs[0]);k++)
        if(strcmp(low, k_langs[k].alias)==0) return k_langs[k].lang;
    return HL_LANG_NONE;
}

HlLang md_lang_from_filename(const char *path){
    if(!path) return HL_LANG_NONE;
    const char *dot=strrchr(path,'.');
    if(!dot || dot==path || dot[1]==0) return HL_LANG_NONE;
    const char *e=dot+1;
    char low[16]; size_t i=0;
    for(i=0;e[i] && i+1<sizeof low;i++){
        char c=e[i]; low[i]=(c>='A'&&c<='Z')?(char)(c-'A'+'a'):c;
    }
    low[i]=0;
    char alias[32];
    return md_lang_normalize(low, alias, sizeof alias);
}

/* ------------------------------- parser -------------------------------- */

typedef struct { const char *buf; size_t len, pos; } Lines;

static bool lines_next(Lines *it, const char **start, size_t *len){
    if(it->pos >= it->len) return false;
    *start = it->buf + it->pos;
    size_t e=it->pos;
    while(e<it->len && it->buf[e]!='\n') e++;
    *len = e - it->pos;
    if(*len>0 && (*start)[*len-1]=='\r') (*len)--;
    it->pos = e+1;
    return true;
}

static size_t run_len(const char *s, size_t n, char ch){
    size_t i=0; while(i<n && s[i]==ch) i++; return i;
}
static bool all_run(const char *s, size_t n, char ch, size_t req){
    return n>=req && run_len(s,n,ch)==n;
}
/* A line consisting only of the given char delimiters. */
static bool is_fence_close(const char *c, size_t cn, char ch, size_t open_run){
    if(cn < open_run || c[0]!=ch) return false;
    /* the first char is ch; ensure at least fmin identical then only ws */
    size_t r=run_len(c,cn,c[0]);
    if(r < open_run) return false;
    for(size_t j=r;j<cn;j++) if(!(c[j]==' '||c[j]=='\t')) return false;
    return true;
}

MdDoc md_parse(const char *src, size_t len, bool streaming){
    MdDoc d; memset(&d,0,sizeof d);
    if(!src || len==0) return d;
    Lines it; it.buf=src; it.len=len; it.pos=0;
    const char *lstart; size_t llen;
    bool in_fence=false;
    char fence_char=0; size_t fence_len=0;
    size_t fence_open=0;                 /* d.blocks[] index of the open fence */
    (void)streaming;                     /* an unclosed fence stays closed=false */
    while(lines_next(&it,&lstart,&llen)){
        const char *l=lstart; size_t n=llen;
        size_t ind=0; while(ind<n && ind<3 && (l[ind]==' '||l[ind]=='\t')) ind++;
        const char *c=l+ind; size_t cn=n-ind;

        if(in_fence){
            if(cn>0 && is_fence_close(c,cn,fence_char,fence_len)){
                d.blocks[fence_open].closed = true;
                in_fence=false;
                continue;
            }
            size_t line_end = (size_t)(lstart - src) + llen;
            if(line_end > d.blocks[fence_open].body_off)
                d.blocks[fence_open].body_len = line_end - d.blocks[fence_open].body_off;
            continue;
        }

        if(d.count>=MD_MAX_BLOCKS){ d.truncated=true; break; }
        if(cn==0) continue;              /* blank line terminates a paragraph */

        if(cn>=3 && (c[0]=='`'||c[0]=='~')){
            size_t fr=run_len(c,cn,c[0]);
            if(fr>=3){
                MdBlock *b=&d.blocks[d.count];
                b->type=MD_BLOCK_CODE; b->is_fence=true; b->closed=false;
                {   /* language tag: trimmed remainder of the fence line */
                    size_t tagcap=sizeof b->lang_str;
                    size_t ts=0;
                    size_t a=fr, z=cn;
                    while(a<z && (c[a]==' '||c[a]=='\t')) a++;
                    while(z>a && (c[z-1]==' '||c[z-1]=='\t'||c[z-1]=='\r')) z--;
                    for(size_t k=a;k<z && ts+1<tagcap;k++) b->lang_str[ts++]=c[k];
                    b->lang_str[ts]=0;
                    {   /* normalize from a separate temp so read/write alias
                           cannot clobber the source tag. */
                        char tmp[32]; copy_alias(tmp,sizeof tmp,b->lang_str[0]?b->lang_str:"");
                        b->lang=md_lang_normalize(tmp[0]?tmp:NULL, b->lang_str, sizeof b->lang_str);
                    }
                }
                b->body_off = it.pos;   /* first byte after the fence line */
                b->body_len=0;
                fence_open=d.count;
                d.count++;
                if(d.count>=MD_MAX_BLOCKS){ d.truncated=true; break; }
                in_fence=true; fence_char=c[0]; fence_len=fr;
                continue;
            }
        }
        if(c[0]=='#'){
            size_t h=0; while(h<cn && h<6 && c[h]=='#') h++;
            if(h>=1 && (h==cn || c[h]==' '||c[h]=='\t')){
                d.blocks[d.count].type=MD_BLOCK_HEADING; d.count++; continue;
            }
        }
        if(cn>=1 && c[0]=='>'){
            d.blocks[d.count].type=MD_BLOCK_QUOTE; d.count++; continue;
        }
        if(cn>=1 && (c[0]=='-'||c[0]=='_'||c[0]=='*'||c[0]=='=')){
            /* horizontal rule if the whole line is a run of one such char
               of length >=3 */
            char ch=c[0];
            if(cn>=3 && all_run(c,cn,ch,3)){
                d.blocks[d.count].type=MD_BLOCK_HR; d.count++; continue;
            }
        }
        if((c[0]=='-'||c[0]=='+'||c[0]=='*') &&
           cn>=1 && (cn==1 || (cn>1 && (c[1]==' '||c[1]=='\t')))){
            d.blocks[d.count].type=MD_BLOCK_ULIST; d.count++; continue;
        }
        if(cn>=1 && c[0]>='0' && c[0]<='9'){
            bool ord=false;
            for(size_t j=1;j<cn && j<4;j++){
                if(c[j]=='.' || c[j]==')'){ ord=true; break; }
                if(c[j]<'0'||c[j]>'9') break;
            }
            if(ord){ d.blocks[d.count].type=MD_BLOCK_OLIST; d.count++; continue; }
        }
        d.blocks[d.count].type=MD_BLOCK_PARAGRAPH; d.count++;
    }
    return d;
}
