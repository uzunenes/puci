/* test_ttg.c — standalone streaming tests for the TTG-envelope filter.
 *
 * Verifies that the \xE2\x81\xA3 <digits> \xE2\x81\xA3 instrumentation
 * envelope is removed at every possible split position and that genuine
 * content is preserved, without ever going out of bounds.
 *
 * Compiled directly against src/cli/tui/ttg.c (dependency-free).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ttg.h"

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)
#define TEST_END()                                                          \
    do {                                                                    \
        if (g_failures) {                                                   \
            fprintf(stderr, "%d/%d ttg checks failed\n", g_failures, g_checks); \
            return EXIT_FAILURE;                                            \
        }                                                                   \
        printf("ok (%d ttg checks)\n", g_checks);                           \
        return EXIT_SUCCESS;                                                \
    } while (0)

/* A TTG-wrapped token: E2 81 A3 <digits> E2 81 A3 <text> */
static size_t payload(char *out, size_t cap, const char *digits, const char *text){
    (void)cap;
    size_t o = 0;
    const unsigned char m[3] = {0xE2, 0x81, 0xA3};
    for(int i=0;i<3;i++) out[o++]=(char)m[i];
    size_t dl = strlen(digits);
    memcpy(out+o, digits, dl); o += dl;
    for(int i=0;i<3;i++) out[o++]=(char)m[i];
    size_t tl = strlen(text);
    memcpy(out+o, text, tl); o += tl;
    return o;
}

/* Feed raw in `chunk`-sized pieces; returns filtered bytes, sets *hits. */
static size_t feed(puci_ttg_state *st, const char *raw, size_t rl, size_t chunk,
                   char *out, size_t outcap, int *hits){
    (void)outcap;
    size_t o=0; *hits=0; size_t pos=0;
    while(pos < rl){
        size_t n = rl-pos; if(n>chunk) n=chunk;
        char tmp[1024]; int h;
        size_t t = puci_ttg_filter(st, raw+pos, n, tmp, sizeof tmp, NULL,0,&h);
        *hits += h;
        memcpy(out+o, tmp, t); o += t;
        pos += n;
    }
    return o;
}

/* One scenario driven at every split position 1..rl. */
static void run_all_splits(const char *digits, const char *text){
    char raw[512];
    size_t rl = payload(raw, sizeof raw, digits, text);
    size_t tlen = strlen(text);
    for(size_t chunk=1; chunk<=rl && chunk < 600; chunk++){
        puci_ttg_state st; memset(&st,0,sizeof st);
        char out[1024]; int hits=0;
        size_t o = feed(&st, raw, rl, chunk, out, sizeof out, &hits);
        CHECK(hits == 1);
        CHECK(o <= rl);
        CHECK(o == tlen);
        if(o == tlen) CHECK(memcmp(out, text, tlen) == 0);
        if(o != tlen)
            fprintf(stderr, "  digits=[%s] split=%zu got=%zu want=%zu out=[%.*s]\n",
                    digits, chunk, o, tlen, (int)o, out);
    }
}

int main(void){
    /* 1. short digits/ascii, every split position. */
    run_all_splits("1234", "tok0");
    /* 2. long digits, every split. */
    run_all_splits("123456789012345678901234", "token");
    /* 3. non-ASCII token text after the envelope (Turkish + CJK + emoji). */
    /* expected non-ascii text (the test above only checks byte-for-byte). */
    run_all_splits("42", "çğıöşü 你好 🙂");

    /* 4. multiple wrapped tokens in one chunk processed at every split. */
    {
        char raw[512]; size_t rl=0;
        char a[80]; size_t al = payload(a,sizeof a,"111","alpha ");
        char b[80]; size_t bl = payload(b,sizeof b,"222","beta ");
        char c[80]; size_t cl = payload(c,sizeof c,"333","gamma");
        memcpy(raw+rl,a,al); rl+=al;
        memcpy(raw+rl,b,bl); rl+=bl;
        memcpy(raw+rl,c,cl); rl+=cl;
        const char *exp = "alpha beta gamma";
        for(size_t chunk=1; chunk<=rl; chunk++){
            char out[1024]; int hits=0;
            puci_ttg_state st; memset(&st,0,sizeof st);
            size_t o = feed(&st, raw, rl, chunk, out, sizeof out, &hits);
            CHECK(hits == 3);
            CHECK(o == strlen(exp));
            if(o == strlen(exp)) CHECK(memcmp(out, exp, o) == 0);
            if(o != strlen(exp))
                fprintf(stderr,"  multile split=%zu got=%zu want=%zu out=[%.*s]\n",
                        chunk, o, strlen(exp), (int)o, out);
        }
    }

    /* 5. malformed: marker + digit but no closing marker. Held in carry;
       once a non-digit arrives it is not an envelope and the whole prefix
       is spilled as ordinary text. */
    {
        const unsigned char m[3] = {0xE2,0x81,0xA3};
        char raw[4];
        for(int i=0;i<3;i++) raw[i]=(char)m[i];
        raw[3]='9';
        char out[64]; int hits=0;
        puci_ttg_state st; memset(&st,0,sizeof st);
        size_t o = puci_ttg_filter(&st, raw, 4, out, sizeof out, NULL,0,&hits);
        CHECK(hits == 0);
        CHECK(o == 0);      /* held in carry, not yet an envelope */
        CHECK(st.len == 4);
        /* A following ordinary char terminates the false envelope: spill. */
        char exp[5];
        for(int i=0;i<3;i++) exp[i]=(char)m[i];
        exp[3]='9'; exp[4]='x';
        char out2[64];
        size_t o2 = puci_ttg_filter(&st, "x", 1, out2, sizeof out2, NULL,0,&hits);
        CHECK(hits == 0);
        CHECK(o2 == 5 && memcmp(out2,exp,5)==0);
    }

    /* 6. incomplete marker at EOF: carried then dropped at flush. */
    {
        const unsigned char m[3] = {0xE2,0x81,0xA3};
        char raw[3];
        for(int i=0;i<3;i++) raw[i]=(char)m[i];
        char out[16]; int hits=0;
        puci_ttg_state st; memset(&st,0,sizeof st);
        size_t o = puci_ttg_filter(&st, raw, 3, out, sizeof out, NULL,0,&hits);
        CHECK(o == 0);
        CHECK(st.len == 3);
        char flush[8];
        CHECK(puci_ttg_flush(&st, flush, sizeof flush) == 0); /* dropped */
    }

    /* 7. an ordinary U+2063 not forming a valid envelope is preserved text. */
    {
        const unsigned char m[3] = {0xE2,0x81,0xA3};
        char raw[6];
        for(int i=0;i<3;i++) raw[i]=(char)m[i];
        raw[3]='h'; raw[4]='i'; raw[5]=0;
        char out[16]; int hits=0;
        puci_ttg_state st; memset(&st,0,sizeof st);
        size_t o = puci_ttg_filter(&st, raw, 6, out, sizeof out, NULL,0,&hits);
        CHECK(hits == 0);
        CHECK(o == 6 && memcmp(out, raw, 6)==0);
    }

    TEST_END();
}
