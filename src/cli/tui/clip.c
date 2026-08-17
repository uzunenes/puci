/* clip.c — safe clipboard access (Phase 5).
 * See clip.h for the guarantees. Backend command strings are constant; copied
 * bytes are streamed to the process stdin. A backend that fails is skipped;
 * if every backend fails we fall back to OSC 52 subject to an explicit size
 * policy, refusing oversized payloads rather than truncating them.
 */
#include "clip.h"
#include "content.h" /* CTX_OSC52_MAX_PAYLOAD */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_test_sink; /* test hook path */
static int g_osc52_fallback=false;

/* Try all platform backends feeding data into the returned pipe. Returns true
 * if any backend reported a clean success (stdin fully consumed, exit 0). */
/* PHP-style guard: a missing backend must not deliver SIGPIPE because we write
 * to a pipe whose child exited instantly. Ignore SIGPIPE for the write loop and
 * restore the previous disposition afterward. */
static int clip_backend(const char *data, size_t len){
    static const char *const cmds[] = {
        "wl-copy",
        "xclip -selection clipboard",
        "xsel --clipboard --input",
        "pbcopy",
        NULL,
    };
    void (*old)(int) = signal(SIGPIPE, SIG_IGN);
    for(size_t i=0; cmds[i]; i++){
        FILE *f=popen(cmds[i],"w");
        if(!f) continue;
        size_t wrote=fwrite(data,1,len,f);
        int rc=pclose(f);
        if(wrote==len && rc==0){ signal(SIGPIPE, old); return 0; }
    }
    signal(SIGPIPE, old);
    return 1;
}

int clip_copy(const char *data, size_t len){
    if(!data) data="";
    if(!len) return 1;

    if(g_test_sink && *g_test_sink){
        FILE *f=fopen(g_test_sink,"wb");
        if(!f) return 1;
        int ok=(fwrite(data,1,len,f)==len);
        if(fclose(f)!=0) ok=0;
        return ok?0:1;
    }

    if(!g_osc52_fallback && clip_backend(data,len)==0){
        return 0;
    }
    g_osc52_fallback=true; /* remember: no OS clipboard, stick to OSC 52 */

    /* Size policy: refuse payloads that cannot fit the OSC 52 cap rather than
     * silently truncating. */
    if(len > clip_osc52_max()) return 2;

    /* base64 payload. */
    size_t bcap = len + (len/3) + 8;
    char *b64 = malloc(bcap ? bcap : 1);
    if(!b64) return 1;
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o=0;
    for(size_t i=0;i<len && o+4<=bcap-1;i+=3){
        unsigned a=(unsigned char)data[i];
        unsigned b=(i+1<len)?(unsigned char)data[i+1]:0;
        unsigned c=(i+2<len)?(unsigned char)data[i+2]:0;
        unsigned t=(a<<16)|(b<<8)|c;
        b64[o++]=B64[(t>>18)&0x3F];
        b64[o++]=B64[(t>>12)&0x3F];
        b64[o++]= (i+1<len)?B64[(t>>6)&0x3F]:'=';
        b64[o++]= (i+2<len)?B64[t&0x3F]:'=';
    }
    b64[o]='\0';
    /* Build and emit the OSC 52 sequence. */
    size_t blen=strlen(b64), cap2=blen+16;
    char *osc=malloc(cap2); if(!osc){ free(b64); return 1; }
    char *p=osc;
    if((size_t)(p-osc)<cap2) *p++='\033';
    if((size_t)(p-osc)<cap2) *p++=']';
    if((size_t)(p-osc)<cap2) *p++='5';
    if((size_t)(p-osc)<cap2) *p++='2';
    if((size_t)(p-osc)<cap2) *p++=';';
    if((size_t)(p-osc)<cap2) *p++='c';
    if((size_t)(p-osc)<cap2) *p++=';';
    size_t put=blen; if((size_t)(p-osc)+put<cap2){ memcpy(p,b64,put); p+=put; } else put=0;
    if((size_t)(p-osc)<cap2) *p++='\007';
    size_t total=(size_t)(p-osc);
    ssize_t w=write(STDOUT_FILENO,osc,total);
    free(b64); free(osc);
    return (w==(ssize_t)total)?0:1;
}

void clip_set_test_sink(const char *path){ g_test_sink=path; }

size_t clip_osc52_max(void){ return CTX_OSC52_MAX_PAYLOAD; }
