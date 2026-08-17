/* test_content.c — unit tests for the owned-content module (src/cli/tui/
 * content.{h,c}) and the bounded Markdown parser (md.{h,c}). These modules are
 * dependency-free (no termbox) so they are linked standalone here.
 *
 * Assertions cover:
 *   - paste block ownership & metadata
 *   - composer segment ordering and exact serialization
 *   - CRLF -> LF policy on text; paste emitted byte-for-byte
 *   - collapsed paste label (width-limited, no raw body)
 *   - markdown fence => exact code body and language tag
 */
#include "content.h"
#include "md.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;

#define CK(cond) do{ if(!(cond)){ \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } }while(0)

/* deterministic simple hash so we can compare original vs delivered bytes */
static unsigned long long djb2(const unsigned char *s, size_t n){
    unsigned long long h=5381;
    for(size_t i=0;i<n;i++) h = ((h<<5)+h) ^ (unsigned long long)s[i];
    return h;
}

static void t_paste(void){
    const char *code =
        "def load_config(path):\n"
        "    with open(path) as f:\n"
        "        return json.load(f)\n";
    TuiPasteBlock *pb = ctx_paste_new(code, strlen(code), "python");
    CK(pb != NULL);
    CK(pb && pb->line_count == 3);
    CK(pb && pb->ends_with_newline == true);
    CK(pb && pb->byte_len == strlen(code));
    CK(pb && strcmp(pb->language_hint,"python")==0);
    CK(pb && memcmp(pb->data, code, strlen(code))==0);
    /* label does NOT contain the body */
    char label[128];
    ctx_paste_label(pb, label, sizeof label, 80);
    CK(strstr(label, "load_config")==NULL);
    CK(strstr(label, "pasted")!=NULL);
    CK(strstr(label, "3")!=NULL);
    ctx_paste_free(pb);

    /* empty / NUL policy: n==0 rejected */
    TuiPasteBlock *empty = ctx_paste_new("",0,NULL);
    CK(empty==NULL);
    /* 1 line, no trailing newline */
    TuiPasteBlock *one = ctx_paste_new("hello",5,NULL);
    CK(one && one->line_count==1 && one->ends_with_newline==false);
    ctx_paste_free(one);
}

static void t_compose(void){
    Composer c; ctx_composer_init(&c);
    const char *a="Review this:\n", *b="\nFocus on ownership.";
    TuiPasteBlock *pb = ctx_paste_new("line1\nline2\nline3\nline4\n", 24, "py");
    /* text, paste, text ordering */
    CK(ctx_composer_insert_text(&c, 0, a, strlen(a)));
    CK(ctx_composer_insert_paste(&c, 1, pb));
    CK(ctx_composer_insert_text(&c, 2, b, strlen(b)));
    size_t olen=0;
    char *out = ctx_composer_serialize(&c, &olen);
    CK(out != NULL);
    if(out){
        /* Build expected: a + full paste body + b */
        char expect[512]; size_t e=0;
        memcpy(expect+e, a, strlen(a)); e+=strlen(a);
        memcpy(expect+e, pb->data, pb->byte_len); e+=pb->byte_len;
        memcpy(expect+e, b, strlen(b)); e+=strlen(b);
        CK(olen == (size_t)e);
        CK((size_t)e<sizeof expect && memcmp(out,expect,(size_t)e)==0);
        /* Hash equality: the exact paste bytes must appear in the serialized
           payload. Find the paste body in `out` and confirm the copy kept the
           original source hash (identical bytes), i.e. the paste was emitted
           byte-for-byte, not abbreviated to the collapsed label. */
        {
            const unsigned char *found_ptr = NULL;
            for(size_t p=0;p+pb->byte_len<=olen && !found_ptr;p++)
                if(memcmp(out+p, pb->data, pb->byte_len)==0) found_ptr=(const unsigned char*)out+p;
            CK(found_ptr != NULL);
            if(found_ptr)
                CK(djb2(found_ptr, pb->byte_len) == djb2((const unsigned char*)pb->data, pb->byte_len));
        }
        free(out);
    }
    ctx_composer_free(&c);
    ctx_paste_free(pb);
}

static void t_compose_crlf(void){
    Composer c; ctx_composer_init(&c);
    const char *txt = "a\r\nb\nc";
    CK(ctx_composer_insert_text(&c,0,txt,strlen(txt)));
    size_t n; char *out=ctx_composer_serialize(&c,&n);
    CK(out && strcmp(out,"a\nb\nc")==0 && n==5);
    free(out); ctx_composer_free(&c);
}

static void t_md(void){
    const char *md =
        "# Title\n"
        "Some prose.\n"
        "\n"
        "```python\n"
        "print(\"hello\")\n"
        "```\n";
    MdDoc d = md_parse(md, strlen(md), false);
    CK(d.count>=3);
    int found=0;
    for(size_t i=0;i<d.count;i++){
        if(d.blocks[i].is_fence && d.blocks[i].lang==HL_LANG_PYTHON && d.blocks[i].closed){
            /* Code body excludes the trailing newline of the last line (matches
               the copy acceptance: "print(\"hello\")" with no trailing LF). */
            CK(d.blocks[i].body_len == strlen("print(\"hello\")"));
            CK(strlen("print(\"hello\")")<256);
            CK(memcmp(md+d.blocks[i].body_off,"print(\"hello\")",strlen("print(\"hello\")"))==0);
            found++;
        }
    }
    CK(found>=1);

    /* unclosed fence is not closed */
    const char *un =
        "```c\n"
        "int x;\n";
    MdDoc u = md_parse(un, strlen(un), false);
    size_t fcnt=0; for(size_t i=0;i<u.count;i++) if(u.blocks[i].is_fence) fcnt++;
    CK(fcnt>=1);
    for(size_t i=0;i<u.count;i++) if(u.blocks[i].is_fence) CK(u.blocks[i].closed==false);

    /* lang normalization + filename inference */
    char alias[32];
    CK(md_lang_normalize("Python", alias, sizeof alias)==HL_LANG_PYTHON);
    CK(strcmp(alias,"python")==0);
    CK(md_lang_from_filename("/a/b/main.c")==HL_LANG_C);
    CK(md_lang_from_filename("config.yaml")==HL_LANG_YAML);
    CK(md_lang_from_filename("x.unknown")==HL_LANG_NONE);
}

int main(void){
    t_paste();
    t_compose();
    t_compose_crlf();
    t_md();
    if(g_fail){ printf("%d FAILURE(S)\n", g_fail); return 1; }
    printf("test_content: all ok\n");
    return 0;
}
