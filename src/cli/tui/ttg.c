/* ttg.c — streaming TTG-envelope strip (see ttg.h). */
#include "ttg.h"

#include <string.h>

size_t puci_ttg_filter(puci_ttg_state *st, const char *txt, size_t len,
                       char *out, size_t outcap,
                       char *dig, size_t digcap, int *hits){
    size_t o = 0;
    if(hits) *hits = 0;
    if(dig && digcap > 0) dig[0] = 0;

    /* Fold the carry from the previous call into a working buffer, then
       append the freshly-arrived bytes. Filtering runs over the whole buf
       so envelope markers split across a chunk boundary are re-joined. */
    char buf[PUCI_TTG_ENV_CAP + 4096];
    size_t bl = 0;
    for(int i = 0; i < st->len; i++) buf[bl++] = st->carry[i];
    st->len = 0;
    if(bl + len > sizeof buf) len = sizeof buf - bl;
    memcpy(buf + bl, txt, len);
    bl += len;

    size_t i = 0;
    const unsigned char *B = (const unsigned char *)buf;

    while(i < bl){
        if(B[i] != 0xE2){
            if(o < outcap) out[o++] = buf[i];
            i++;
            continue;
        }
        size_t rem = bl - i;
        int is_marker = 0;
        if(rem >= 3 && B[i]==0xE2 && B[i+1]==0x81 && B[i+2]==0xA3) is_marker = 1;
        else if(rem < 3){
            /* Only part of the leading 3 bytes is present: carry them. */
            for(size_t k=i; k<bl; k++) st->carry[st->len++] = (char)buf[k];
            break;
        }
        if(!is_marker){
            /* An ordinary 0xE2-led UTF-8 char (e.g. EUR = E2 82 AC): emit 3 bytes. */
            if(rem < 3){ for(size_t k=i;k<bl;k++) st->carry[st->len++]=(char)buf[k]; break; }
            size_t copy = 3;
            if(o + copy <= outcap){ for(size_t k=0;k<copy;k++) out[o++]=buf[i+k]; }
            i += copy;
            continue;
        }
        /* Envelope start. Look ahead: digits then the closing E2 81 A3. */
        size_t digit_start = i + 3;
        size_t j = digit_start;
        while(j < bl && B[j] >= '0' && B[j] <= '9') j++;
        size_t digits = j - digit_start;
        if(digits == 0 || digits > 48){
            /* Either there are no digits, or there are too many. If the
               digit-run breadth came to the end of the buffer and fewer bytes
               than the envelope cap remain, a real envelope may still be
               arriving (marker complete, digits split to the next chunk):
               carry rather than emit. Otherwise it is an ordinary marker with
               no digits, so spill it as text. */
            size_t at_end = (j >= bl);
            int fit_cap = ((size_t)(bl - i) <= PUCI_TTG_ENV_CAP);
            if(digits == 0 && at_end && fit_cap && (size_t)st->len + (bl - i) <= PUCI_TTG_CARRY_MAX){
                for(size_t k=i;k<bl;k++) st->carry[st->len++]=(char)buf[k];
                break;
            }
            if(rem < 3){ for(size_t k=i;k<bl;k++) st->carry[st->len++]=(char)buf[k]; break; }
            size_t copy = (rem>=3)?3:rem;
            for(size_t k=0;k<copy;k++) if(o<outcap) out[o++]=buf[i+k];
            i += copy;
            continue;
        }
        if(j + 2 < bl && B[j]==0xE2 && B[j+1]==0x81 && B[j+2]==0xA3){
            /* Complete envelope: consume [i .. j+3). */
            if(dig && digcap > 0){
                size_t n = digits < digcap ? digits : (digcap-1);
                memcpy(dig, buf+digit_start, n);
                dig[n] = 0;
            }
            i = j + 3;
            if(hits) (*hits)++;
            continue;
        }
        /* Digits present but the closing marker is not complete. Distinguish:
           - If the run stopped at a definite non-(digit|marker) byte, the
             "envelope" is surely malformed: spill the marker+digits as text
             and continue (don't consume that terminator byte).
           - Otherwise (end-of-buffer or a partial E2.. marker) it may still
             complete on a later chunk: carry the [i..bl) prefix. */
        if(j < bl && B[j] != 0xE2){
            /* Definitively not an envelope: a non-digit, non-marker byte
               follows the digit run (a lone E2 that could begin a closing
               marker, even if partial, is treated as carryable below). */
            while(i < j){ if(o < outcap) out[o++] = buf[i++]; else i++; }
            continue;
        }
        if((bl - i) <= PUCI_TTG_ENV_CAP){
            for(size_t k=i; k<bl; k++) st->carry[st->len++] = (char)buf[k];
            break;
        }
        while(i < bl && o < outcap){ out[o++] = buf[i++]; }
        break;
    }
    return o;
}

void puci_ttg_reset(puci_ttg_state *st){
    st->len = 0;
}

size_t puci_ttg_flush(puci_ttg_state *st, char *out, size_t outcap){
    size_t o = 0;
    if(st->len > 3){
        size_t n = (size_t)st->len;
        if(n > outcap) n = outcap;
        memcpy(out, st->carry, n);
        o = n;
    }
    st->len = 0;
    return o;
}
