/* test_conv.c — unit tests for the owned semantic conversation model.
 * Covers: assistant raw ownership, code-part derivation + exact copy, raw
 * markdown exact copy, plain-text copy, multi-segment user ordering (exact
 * provider payload), and versioned JSON round-trip with hash equality.
 */
#include "conv.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail=0;
#define CK(cond) do{ if(!(cond)){ printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } }while(0)
static unsigned long long djb2(const unsigned char *s, size_t n){
    unsigned long long h=5381; for(size_t i=0;i<n;i++) h=((h<<5)+h)^(unsigned long long)s[i];
    return h;
}

static void t_assistant_owns_raw(void){
    TuiConversation c; conv_init(&c);
    TuiMessage *m=conv_begin_assistant(&c); CK(m!=NULL);
    const char *src="# Title\n\nBody **bold** and `code`.\n\n```python\ndef h():\n    return 1\n```\nTail.\n";
    conv_append(&c,m, src, strlen(src));
    conv_end_assistant(&c,m);
    CK(!m->streaming);
    CK(m->raw_len==strlen(src));
    CK(memcmp(m->raw_source,src,strlen(src))==0);
    TuiContentPart *code=NULL;
    for(size_t i=0;i<m->part_count;i++) if(m->parts[i].kind==TUI_PART_CODE) code=&m->parts[i];
    CK(code!=NULL);
    if(code){
        const char expect[]="def h():\n    return 1";   /* excludes trailing newline (md.h) */
        CK(code->raw_len==strlen(expect));
        CK(memcmp(code->raw_source,expect,strlen(expect))==0);
        CK(code->status==TUI_COMPLETE);
        size_t ol=0; char *cp=conv_part_copy(code,&ol); CK(cp!=NULL);
        if(cp){ CK(ol==strlen(expect) && memcmp(cp,expect,ol)==0);
                CK(djb2((const unsigned char*)cp,ol)==djb2((const unsigned char*)expect,strlen(expect))); free(cp); }
    }
    size_t rl=0; char *rc=conv_message_raw_copy(m,&rl); CK(rc!=NULL);
    if(rc){ CK(rl==m->raw_len && memcmp(rc,m->raw_source,rl)==0); free(rc); }
    conv_free(&c);
}

static void t_plain_copy(void){
    TuiConversation c; conv_init(&c);
    TuiMessage *m=conv_begin_assistant(&c);
    const char *src="# Title\n**bold** and `inline` and [link](http://x).\n\n```python\ndef f():\n    print(1)\n```\n";
    conv_append(&c,m,src,strlen(src));
    conv_end_assistant(&c,m);
    conv_message_plain(&c,m);
    char *pt=conv_message_plain_copy(m,NULL); CK(pt!=NULL);
    if(pt){
        CK(strstr(pt,"Title")!=NULL);
        CK(strstr(pt,"bold")!=NULL);
        CK(strstr(pt,"inline")!=NULL);
        CK(strstr(pt,"link")!=NULL);
        CK(strstr(pt,"def f():")!=NULL);
        CK(strstr(pt,"print(1)")!=NULL);
        CK(strstr(pt,"```")==NULL);
        CK(strstr(pt,"**")==NULL);
        free(pt);
    }
    conv_free(&c);
}

/* Phase 2: text A + paste A + text B + paste B + text C -> exact order. */
static void t_multi_segment_order(void){
    TuiConversation c; conv_init(&c);
    const char *t1="Inspect this Python:\n";
    const char *pa="print(1)\n";
    const char *t2="Compare with JSON:\n";
    const char *pb="{\"ok\": true}\n";
    const char *t3="Focus on errors.\n";
    TuiMessage *m=conv_user_message(&c,t1,strlen(t1));
    conv_user_add_paste(&c,m,pa,strlen(pa),"python");
    conv_append_raw(&c,m,t2,strlen(t2));
    conv_user_add_paste(&c,m,pb,strlen(pb),NULL);
    conv_append_raw(&c,m,t3,strlen(t3));
    /* expected provider payload: text then "\n"+paste each, then text */
    char expect[512]; size_t e=0;
    memcpy(expect+e,t1,strlen(t1)); e+=strlen(t1);
    memcpy(expect+e,"\n",1); e+=1; memcpy(expect+e,pa,strlen(pa)); e+=strlen(pa);
    memcpy(expect+e,t2,strlen(t2)); e+=strlen(t2);
    memcpy(expect+e,"\n",1); e+=1; memcpy(expect+e,pb,strlen(pb)); e+=strlen(pb);
    memcpy(expect+e,t3,strlen(t3)); e+=strlen(t3);
    CK(m->raw_len==e);
    CK(memcmp(m->raw_source,expect,e)==0);
    CK(djb2((const unsigned char*)m->raw_source,m->raw_len)==djb2((const unsigned char*)expect,e));
    /* pastes are individually copyable */
    CK(m->part_count>=3); /* 2 pastes + 1 text part */
    conv_free(&c);
}

static void t_roundtrip(void){
    TuiConversation c; conv_init(&c);
    TuiMessage *u=conv_user_message(&c,"selam",5);
    const char *paste="line1\nline2\n"; conv_user_add_paste(&c,u,paste,strlen(paste),"python");
    TuiMessage *a=conv_begin_assistant(&c);
    const char *md="Hey!\n\n```json\n{\"a\":1}\n```\n";
    conv_append(&c,a,md,strlen(md));
    conv_end_assistant(&c,a);
    unsigned long long uh=djb2((const unsigned char*)u->raw_source,u->raw_len);
    unsigned long long ah=djb2((const unsigned char*)a->raw_source,a->raw_len);
    size_t jl=0; char *json=conv_serialize(&c,&jl); CK(json!=NULL);

    TuiConversation d; conv_init(&d);
    int rc=conv_deserialize(&d,json,jl); CK(rc==0);
    if(rc==0){
        CK(d.count==2);
        if(d.count>=2){
            CK(djb2((const unsigned char*)d.messages[0].raw_source,d.messages[0].raw_len)==uh);
            CK(djb2((const unsigned char*)d.messages[1].raw_source,d.messages[1].raw_len)==ah);
        }
    }
    free(json);
    conv_free(&c); conv_free(&d);
}

int main(void){
    t_assistant_owns_raw();
    t_plain_copy();
    t_multi_segment_order();
    t_roundtrip();
    if(g_fail){ printf("%d FAILURE(S)\n",g_fail); return 1; }
    printf("test_conv: all ok\n");
    return 0;
}
