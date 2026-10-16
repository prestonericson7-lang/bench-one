/* ===========================================================================================
 *  pretok_qwen2.h -- the Qwen2 pre-tokenizer: where one BPE chunk ends and the next begins
 *
 *  The file says tokenizer.ggml.pre = "qwen2". That names this regex, from the model's tokenizer.json:
 *
 *    (?i:'s|'t|'re|'ve|'m|'ll|'d) | [^\r\n\p{L}\p{N}]?\p{L}+ | \p{N} | ?[^\s\p{L}\p{N}]+[\r\n]*
 *                                 | \s*[\r\n]+ | \s+(?!\S) | \s+
 *
 *  implemented here directly, alternative by alternative in the regex's own order (the first alternative
 *  that matches wins, each one greedy). It replaced GPT-2's rules, which the tokenizer used until
 *  2026-09-27 and which split 18,765 of 27,055 corpus lines differently from llama.cpp on this very file:
 *  GPT-2 lets only a SPACE lead a word (" foo"), Qwen2 lets any one non-letter do it ("-S", "_proto",
 *  ".ai"); GPT-2 ends a punctuation run at a newline, Qwen2 keeps the newlines (")\n" is one token).
 *
 *  Classes are Unicode's, as llama.cpp uses them: every code point is exactly one of whitespace (checked
 *  first: ASCII 9-13 and 32, and the White_Space code points above ASCII), letter (L*), number (N*),
 *  or other. ASCII is classified inline; above it, unicode_ln.h's range tables. A byte that is not valid
 *  UTF-8 is one "other" code point, as llama.cpp turns it into U+FFFD.
 *
 *  Used by shared/tokenizer.c and shared/tl_core.c, which must split identically.
 * ======================================================================================== */
#ifndef PRETOK_QWEN2_H
#define PRETOK_QWEN2_H

#include <stdint.h>
#include "unicode_ln.h"

enum { PT_OTHER = 0, PT_WS = 1, PT_LETTER = 2, PT_NUMBER = 3 };

static int pt_in(const uint32_t (*r)[2], int n, uint32_t c)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (c < r[m][0]) hi = m - 1;
        else if (c > r[m][1]) lo = m + 1;
        else return 1;
    }
    return 0;
}

static int pt_class(uint32_t c)
{
    if (c < 0x80) {
        if (c == ' ' || (c >= 9 && c <= 13)) return PT_WS;
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') return PT_LETTER;
        if (c >= '0' && c <= '9') return PT_NUMBER;
        return PT_OTHER;
    }
    if (c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
        c == 0x202F || c == 0x205F || c == 0x3000) return PT_WS;
    if (pt_in(UNI_L, (int)(sizeof UNI_L / sizeof UNI_L[0]), c)) return PT_LETTER;
    if (pt_in(UNI_N, (int)(sizeof UNI_N / sizeof UNI_N[0]), c)) return PT_NUMBER;
    return PT_OTHER;
}

/* one code point at s (n > 0 bytes left): its length in bytes, the value in *c */
static int pt_cp(const unsigned char *s, int n, uint32_t *c)
{
    const unsigned char b = s[0];
    int len;
    uint32_t v;
    if (b < 0x80) { *c = b; return 1; }
    else if ((b & 0xE0) == 0xC0) { len = 2; v = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { len = 3; v = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { len = 4; v = b & 0x07; }
    else { *c = 0xFFFD; return 1; }
    if (len > n) { *c = 0xFFFD; return 1; }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *c = 0xFFFD; return 1; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    *c = v;
    return len;
}

static int pt_lower(uint32_t c) { return (c >= 'A' && c <= 'Z') ? (int)(c | 0x20) : (int)c; }

/* Length in bytes of the chunk that starts at s, n > 0 bytes long. n is the end of the text or of the
 * fragment before a special token: llama.cpp pre-tokenizes each fragment on its own, so "(?!\S)" and
 * the runs stop there. */
static int pt_qwen2_len(const unsigned char *s, int n)
{
    uint32_t c0, c;
    const int l0 = pt_cp(s, n, &c0);
    const int k0 = pt_class(c0);
    int i, l;

    /* (?i:'s|'t|'re|'ve|'m|'ll|'d) */
    if (c0 == '\'' && n > 1) {
        const int a = pt_lower(s[1]);
        if (a == 's' || a == 't' || a == 'm' || a == 'd') return 2;
        if (n > 2) {
            const int b = pt_lower(s[2]);
            if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l')) return 3;
        }
    }
    /* [^\r\n\p{L}\p{N}]?\p{L}+ -- the optional lead taken if a letter follows it, else no lead */
    i = -1;
    if (c0 != '\r' && c0 != '\n' && k0 != PT_LETTER && k0 != PT_NUMBER && l0 < n) {
        l = pt_cp(s + l0, n - l0, &c);
        if (pt_class(c) == PT_LETTER) i = l0 + l;
    }
    if (i < 0 && k0 == PT_LETTER) i = l0;
    if (i >= 0) {
        while (i < n) {
            l = pt_cp(s + i, n - i, &c);
            if (pt_class(c) != PT_LETTER) break;
            i += l;
        }
        return i;
    }
    /* \p{N} -- one digit */
    if (k0 == PT_NUMBER) return l0;
    /*  ?[^\s\p{L}\p{N}]+[\r\n]* */
    i = -1;
    if (c0 == ' ' && n > 1) {
        l = pt_cp(s + 1, n - 1, &c);
        if (pt_class(c) == PT_OTHER) i = 1;
    }
    if (i < 0 && k0 == PT_OTHER) i = 0;
    if (i >= 0) {
        while (i < n) {
            l = pt_cp(s + i, n - i, &c);
            if (pt_class(c) != PT_OTHER) break;
            i += l;
        }
        while (i < n && (s[i] == '\r' || s[i] == '\n')) i++;
        return i;
    }
    /* c0 is whitespace. \s*[\r\n]+ : through the last CR or LF of the whitespace run */
    {
        int last_nl = -1, last_start = 0;
        i = 0;
        while (i < n) {
            l = pt_cp(s + i, n - i, &c);
            if (pt_class(c) != PT_WS) break;
            if (c == '\r' || c == '\n') last_nl = i;
            last_start = i;
            i += l;
        }
        if (last_nl >= 0) return last_nl + 1;
        /* \s+(?!\S) : the whole run at the end, else all of it but its last character */
        if (i >= n) return i;
        if (last_start > 0) return last_start;
        /* \s+ */
        return i;
    }
}

#endif
