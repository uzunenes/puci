/* hl.c — small, deterministic terminal syntax highlighter. See hl.h.
 * No termbox dependency; unit-testable in isolation.
 */
#include "hl.h"

#include <string.h>

/* ---- span writer -------------------------------------------------------- */
typedef struct { HlSpan *sp; int n, cap; } SpOut;

static void sp_add(SpOut *o, HlToken k, size_t off, size_t len){
    if(len==0) return;
    if(o->n && o->n<o->cap && o->sp[o->n-1].kind==k &&
       o->sp[o->n-1].off + o->sp[o->n-1].len == off){
        o->sp[o->n-1].len += len;      /* coalesce contiguous same-kind */
        return;
    }
    if(o->n>=o->cap) return;
    o->sp[o->n].kind = k;
    o->sp[o->n].off  = off;
    o->sp[o->n].len  = len;
    o->n++;
}

/* ---- character classes --------------------------------------------------- */
static int is_id0(unsigned char c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'; }
static int is_idc(unsigned char c){ return is_id0(c)||(c>='0'&&c<='9'); }
static int is_digit(unsigned char c){ return c>='0'&&c<='9'; }

/* boundary-checked membership in a space-delimited string list */
static int in_words(const char *list, const char *s, size_t n){
    const char *p=list;
    while(*p){
        while(*p==' ') p++;
        if(!*p) break;
        size_t w=0; while(p[w] && p[w]!=' ') w++;
        if(w==n && memcmp(p,s,n)==0) return 1;
        p += w;
    }
    return 0;
}

/* keyword set per family */
static const char *kw_list(HlLang L){
    switch(L){
      case HL_LANG_C: case HL_LANG_CPP: case HL_LANG_RUST: case HL_LANG_GO:
      case HL_LANG_JS: case HL_LANG_TS:
        return "if else for while do return break continue switch case default "
               "struct enum union typedef static extern const class public private "
               "int char long short float double void unsigned signed true false";
      case HL_LANG_PYTHON:
        return "def class return if elif else pass import from as try except "
               "finally raise with while for and or not in is lambda yield";
      case HL_LANG_SHELL:
        return "if then else fi while for do done case esac return local export echo";
      case HL_LANG_JSON:
        return "true false null";
      default:
        return "";
    }
}

static const char *bool_list(void){ return "true false null True False None"; }

static int lang_hash(HlLang L){
    return L==HL_LANG_PYTHON||L==HL_LANG_SHELL||L==HL_LANG_YAML||L==HL_LANG_TOML;
}
static int lang_slash(HlLang L){
    return L==HL_LANG_C||L==HL_LANG_CPP||L==HL_LANG_JS||L==HL_LANG_TS||L==HL_LANG_RUST||L==HL_LANG_GO;
}

/* ---- code-family scanner ------------------------------------------------- */
static void scan_code(HlLang L, const char *line, size_t len, SpOut *o){
    size_t i=0;
    while(i<len){
        unsigned char c=(unsigned char)line[i];
        if(c==' '||c=='\t'){ i++; continue; }

        if(c=='#' && lang_hash(L)){ sp_add(o,HL_TK_COMMENT,i,len-i); return; }
        if(c=='/' && i+1<len && line[i+1]=='/' && lang_slash(L)){
            sp_add(o,HL_TK_COMMENT,i,len-i); return;
        }
        if(c=='"' || c=='\'' || (L==HL_LANG_SHELL && c=='`')){
            size_t j=i+1; int esc=0;
            while(j<len){
                if(esc){ esc=0; j++; continue; }
                if(line[j]=='\\'){ esc=1; j++; continue; }
                if(line[j]==c){ j++; break; }
                if(line[j]=='\n') break;
                j++;
            }
            sp_add(o,HL_TK_STRING,i,j-i); i=j; continue;
        }
        if(is_digit(c)){
            size_t j=i;
            while(j<len && (is_digit((unsigned char)line[j])||line[j]=='.'||line[j]=='_')) j++;
            sp_add(o,HL_TK_NUMBER,i,j-i); i=j; continue;
        }
        if(is_id0(c)){
            size_t j=i; while(j<len && is_idc((unsigned char)line[j])) j++;
            HlToken k=in_words(kw_list(L), line+i, j-i) ? HL_TK_KEYWORD : HL_TK_TEXT;
            if(in_words(bool_list(), line+i, j-i)) k=HL_TK_BOOL;
            sp_add(o,k,i,j-i); i=j; continue;
        }
        sp_add(o,HL_TK_PUNCT,i,1); i++;
    }
}

/* ---- markdown line ------------------------------------------------------- */
static void scan_markdown(HlLang L, const char *line, size_t len, SpOut *o){
    (void)L;
    size_t i=0;
    if(len>0 && line[0]=='#'){
        size_t j=0; while(j<len && j<6 && line[j]=='#') j++;
        sp_add(o,HL_TK_HEADING,0,j); i=j;
    } else if(len>=3 && (line[0]=='-'||line[0]=='*'||line[0]=='_') &&
              (size_t)strspn(line,"-*_")==len){
        sp_add(o,HL_TK_PUNCT,0,len); return;
    } else if(len>=1 && (line[0]=='-'||line[0]=='*'||line[0]=='+') && (len==1||line[1]==' ')){
        sp_add(o,HL_TK_PUNCT,0,1); i=1;
    } else if(len>0 && line[0]=='>'){
        sp_add(o,HL_TK_PUNCT,0,1); i=1;
    }
    /* inline `code` spans */
    while(i<len){
        if(line[i]=='`'){
            size_t j=i+1; while(j<len && line[j]!='`') j++;
            size_t e = (j<len)?(j-i+1):(j-i);
            sp_add(o,HL_TK_STRING,i,e); i = (j<len)?(j+2):len;
            continue;
        }
        if(line[i]=='[' || line[i]==']' || line[i]=='(' || line[i]==')' ||
           line[i]=='*' || line[i]=='_'){
            sp_add(o,HL_TK_PUNCT,i,1); i++; continue;
        }
        i++;
    }
}

/* ---- diff line ------------------------------------------------------------ */
static void scan_diff(const char *line, size_t len, SpOut *o){
    /* diff: whole-line classification by leading marker */
    size_t j=0;
    if(len>0){
        if(line[0]=='+'){ sp_add(o,HL_TK_DIFF_ADD,0,len); return; }
        if(line[0]=='-'){ sp_add(o,HL_TK_DIFF_DEL,0,len); return; }
        if(line[0]=='@'){ for(j=0;j<len && line[j]!='\n';j++){}
                          sp_add(o,HL_TK_DIFF_HUNK,0,len); return; }
    }
    (void)j;
    sp_add(o,HL_TK_TEXT,0,len);
}

int hl_line(HlLang lang, const char *line, size_t len, HlSpan *out, int cap){
    if(!line || cap<=0) return 0;
    SpOut o; o.sp=out; o.n=0; o.cap=cap;
    switch(lang){
      case HL_LANG_DIFF: scan_diff(line,len,&o); break;
      case HL_LANG_MARKDOWN: scan_markdown(lang,line,len,&o); break;
      default: scan_code(lang,line,len,&o); break;
    }
    return o.n;
}
