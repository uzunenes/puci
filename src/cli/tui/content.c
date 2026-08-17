/* content.c - owned structured content for the PUCI TUI.
 * See content.h for the ownership model. This module has no dependency on
 * termbox or the rest of the TUI so it can be unit-tested in isolation.
 */
#include "content.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------------
 * Coarse UTF-8 display width. Full wcwidth is intentionally not linked;
 * the ranges cover CJK, wide punctuation and emoji; combining marks are
 * width 0 and everything else is width 1.
 * ------------------------------------------------------------------ */
static int u8_seq(unsigned char c){
    if(c < 0x80) return 1;
    if((c & 0xE0) == 0xC0) return 2;
    if((c & 0xF0) == 0xE0) return 3;
    if((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static uint32_t u8_cp(const unsigned char *p, size_t rem){
    if(rem==1) return p[0];
    if(rem==2) return ((uint32_t)(p[0]&0x1F)<<6) | (uint32_t)(p[1]&0x3F);
    if(rem==3) return ((uint32_t)(p[0]&0x0F)<<12) | ((uint32_t)(p[1]&0x3F)<<6) | (uint32_t)(p[2]&0x3F);
    return ((uint32_t)(p[0]&0x07)<<18) | ((uint32_t)(p[1]&0x3F)<<12) |
           ((uint32_t)(p[2]&0x3F)<<6) | (uint32_t)(p[3]&0x3F);
}

static int u8_cp_width(uint32_t cp){
    if(cp==0) return 0;
    if((cp>=0x0300 && cp<=0x036F) || (cp>=0x1AB0 && cp<=0x1AFF) ||
       (cp>=0x1DC0 && cp<=0x1DFF) || (cp>=0x20D0 && cp<=0x20FF) ||
       (cp>=0xFE20 && cp<=0xFE2F)) return 0;
    if((cp>=0x1100 && cp<=0x115F) || (cp>=0x2E80 && cp<=0xA4CF) ||
       (cp>=0xAC00 && cp<=0xD7A3) || (cp>=0xF900 && cp<=0xFAFF) ||
       (cp>=0xFE30 && cp<=0xFE4F) || (cp>=0xFF00 && cp<=0xFF60) ||
       (cp>=0xFFE0 && cp<=0xFFE6) || (cp>=0x1F300 && cp<=0x1FAFF)) return 2;
    if(cp==0x200B || cp==0x200D || cp==0xFEFF || cp==0x2063) return 0;
    return 1;
}

static int u8_width(const unsigned char *s, size_t n){
    int w=0; size_t i=0;
    while(i<n){
        unsigned char c=s[i];
        int seq=u8_seq(c);
        size_t rem=((size_t)seq>(size_t)(n-i))?(size_t)(n-i):(size_t)seq;
        if(rem==0) rem=1;
        w += u8_cp_width(u8_cp(s+i, rem));
        i += rem;
    }
    return w;
}

static int u8_width_str(const char *s){
    return s ? u8_width((const unsigned char*)s, strlen(s)) : 0;
}

/* ============================ Paste block ================================ */

static void norm_lang(char *dst, size_t cap, const char *src){
    if(!src || !*src){ if(cap) dst[0]=0; return; }
    size_t w=0;
    for(const char *p=src; *p && w+1<cap; p++){
        if(*p=='\n'||*p=='\r'||*p=='`'||*p==' '||*p=='\t') break;
        dst[w++]=(char)(unsigned char)*p;
    }
    dst[w]=0;
}

static void human_bytes(size_t n, char *buf, size_t cap){
    double v=(double)n;
    const char *unit="B";
    if(v>=1024.0){ v/=1024.0; unit="KiB"; if(v>=1024.0){ v/=1024.0; unit="MiB"; } }
    if(unit[0]=='B') snprintf(buf,cap,"%zu B", n);
    else snprintf(buf,cap,"%.1f %s", v, unit);
}

TuiPasteBlock *ctx_paste_new(const char *data, size_t n, const char *lang_hint){
    static uint64_t g_paste_id = 1;
    if(!data || n==0 || n > CTX_PASTE_MAX_BYTES) return NULL;
    TuiPasteBlock *pb=(TuiPasteBlock*)calloc(1, sizeof *pb);
    if(!pb) return NULL;
    pb->data=(char*)malloc(n+1);
    if(!pb->data){ free(pb); return NULL; }
    memcpy(pb->data, data, n);
    pb->data[n]=0;
    pb->id = g_paste_id++;
    pb->byte_len = n;
    if(data[n-1]=='\n') pb->ends_with_newline = true;
    size_t lc=1;
    for(size_t i=0;i<n;i++) if(data[i]=='\n') lc++;
    pb->line_count = pb->ends_with_newline ? (lc-1u) : lc;
    if(pb->line_count==0) pb->line_count=1;
    norm_lang(pb->language_hint, sizeof pb->language_hint, lang_hint);
    return pb;
}

void ctx_paste_free(TuiPasteBlock *pb){
    if(!pb) return;
    free(pb->data);
    free(pb);
}

static const char *label_best(const TuiPasteBlock *pb, char *buf, size_t cap, int width){
    char sz[32]; human_bytes(pb->byte_len, sz, sizeof sz);
    char core[200];
    if(pb->line_count<=1)
        snprintf(core,sizeof core,"[pasted 1 line · %s]", sz);
    else if(pb->language_hint[0])
        snprintf(core,sizeof core,"[pasted lines 1–%zu · %s · %s]", pb->line_count, pb->language_hint, sz);
    else
        snprintf(core,sizeof core,"[pasted lines 1–%zu · %s]", pb->line_count, sz);
    if(u8_width_str(core)<=width){ snprintf(buf,cap,"%s",core); return buf; }
    if(pb->language_hint[0]){
        snprintf(core,sizeof core,"[pasted %zu ln · %s]", pb->line_count, pb->language_hint);
        if(u8_width_str(core)<=width){ snprintf(buf,cap,"%s",core); return buf; }
    }
    snprintf(core,sizeof core,"[pasted %zu ln]", pb->line_count);
    if(u8_width_str(core)<=width){ snprintf(buf,cap,"%s",core); return buf; }
    snprintf(buf,cap,"%s",core);
    return buf;
}

void ctx_paste_label(const TuiPasteBlock *pb, char *buf, size_t cap, int width){
    if(!pb || cap==0){ if(cap) buf[0]=0; return; }
    char core[200];
    label_best(pb, core, sizeof core, width);
    if(u8_width_str(core)<=width){ snprintf(buf,cap,"%s",core); return; }
    size_t o=0; int w=0;
    const unsigned char *s=(const unsigned char*)core;
    size_t n=strlen(core);
    while(o<n){
        unsigned char c=s[o]; int seq=u8_seq(c);
        size_t rem=((size_t)seq>(size_t)(n-o))?(n-o):(size_t)seq; if(rem==0) rem=1;
        int cw=u8_cp_width(u8_cp(s+o,rem));
        if(w+cw>width) break;
        w+=cw; o+=rem;
    }
    size_t keep=o+1<cap ? o : (cap>0?cap-1:0);
    memcpy(buf, core, keep);
    buf[keep]=0;
}

/* ============================ Composer ================================= */

bool ctx_composer_init(Composer *c){
    if(!c) return false;
    c->segs=NULL; c->seg_n=c->seg_cap=0; c->total_bytes=0;
    return true;
}

void ctx_composer_free(Composer *c){
    if(!c) return;
    for(size_t i=0;i<c->seg_n;i++)
        if(c->segs[i].kind==COMPOSER_SEG_TEXT) free(c->segs[i].text);
    free(c->segs);
    memset(c,0,sizeof *c);
}

static bool seg_ensure(Composer *c, size_t need){
    if(need <= c->seg_cap) return true;
    size_t nc=c->seg_cap?c->seg_cap*2u:8u;
    while(nc<need) nc*=2u;
    ComposerSegment *ns=(ComposerSegment*)realloc(c->segs, nc*sizeof *ns);
    if(!ns) return false;
    c->segs=ns; c->seg_cap=nc;
    return true;
}

static bool seg_insert(Composer *c, size_t pos, const ComposerSegment *seg){
    if(pos>c->seg_n) return false;
    if(!seg_ensure(c, c->seg_n+1u)) return false;
    size_t i=c->seg_n;
    while(i>pos){ c->segs[i]=c->segs[i-1]; i--; }
    c->segs[pos]=*seg;
    c->seg_n++;
    return true;
}

bool ctx_composer_insert_text(Composer *c, size_t pos, const char *s, size_t n){
    if(!c || (n&&!s)) return false;
    if(pos>c->seg_n) return false;
    if(c->total_bytes + n > CTX_DRAFT_MAX_BYTES) return false;
    if(pos<c->seg_n && c->segs[pos].kind==COMPOSER_SEG_TEXT){
        ComposerSegment *seg=&c->segs[pos];
        char *nb=(char*)malloc(seg->text_len+n+1);
        if(!nb) return false;
        if(seg->text_len) memcpy(nb, seg->text, seg->text_len);
        if(n) memcpy(nb+seg->text_len, s, n);
        seg->text_len+=n; nb[seg->text_len]=0;
        free(seg->text); seg->text=nb;
        c->total_bytes+=n;
        return true;
    }
    char *buf=(char*)malloc(n+1);
    if(!buf) return false;
    if(n) memcpy(buf,s,n);
    buf[n]=0;
    ComposerSegment cs; cs.kind=COMPOSER_SEG_TEXT; cs.text=buf; cs.text_len=n; cs.paste=NULL;
    if(!seg_insert(c, c->seg_n, &cs)){ free(buf); return false; }
    c->total_bytes+=n;
    return true;
}

bool ctx_composer_insert_paste(Composer *c, size_t pos, TuiPasteBlock *pb){
    if(!c || !pb) return false;
    if(pos>c->seg_n) return false;
    if(c->total_bytes + pb->byte_len > CTX_DRAFT_MAX_BYTES) return false;
    ComposerSegment cs; cs.kind=COMPOSER_SEG_PASTE; cs.text=NULL; cs.text_len=0; cs.paste=pb;
    if(!seg_insert(c,pos,&cs)) return false;
    c->total_bytes += pb->byte_len;
    return true;
}

char *ctx_composer_serialize(Composer *c, size_t *out_len){
    if(out_len) *out_len=0;
    if(!c) return NULL;
    size_t need=1;
    for(size_t i=0;i<c->seg_n;i++){
        ComposerSegment *seg=&c->segs[i];
        if(seg->kind==COMPOSER_SEG_TEXT){
            for(size_t k=0;k<seg->text_len;k++){
                need++;
                if(seg->text[k]=='\r' && k+1<seg->text_len && seg->text[k+1]=='\n') k++;
            }
        } else if(seg->paste){
            need += seg->paste->byte_len;
        }
    }
    char *out=(char*)malloc(need);
    if(!out) return NULL;
    size_t o=0;
    for(size_t i=0;i<c->seg_n;i++){
        ComposerSegment *seg=&c->segs[i];
        if(seg->kind==COMPOSER_SEG_TEXT){
            for(size_t k=0;k<seg->text_len;k++){
                if(seg->text[k]=='\r'){
                    if(k+1<seg->text_len && seg->text[k+1]=='\n') continue;
                    out[o++]='\n';
                } else out[o++]=seg->text[k];
            }
        } else if(seg->paste){
            memcpy(out+o, seg->paste->data, seg->paste->byte_len);
            o += seg->paste->byte_len;
        }
    }
    if(o+1>need) o=need-1;
    out[o]=0;
    if(out_len) *out_len=o;
    return out;
}
