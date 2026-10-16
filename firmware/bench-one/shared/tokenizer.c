/* tokenizer.c -- see tokenizer.h. Byte-level BPE with the vocabulary taken from the model file. */

#include "tokenizer.h"
#include "pretok_qwen2.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------------
 *  hash table: string -> int
 *
 *  Open addressing, power-of-two capacity, linear probing. 151k entries in a 262k table is a 58%
 *  load factor, which for linear probing is comfortable. Nothing is ever deleted, so there are no
 *  tombstones to reason about.
 * ------------------------------------------------------------------------------------------ */

struct tok_map {
    const char **key;
    int32_t     *val;
    uint32_t     cap;    /* power of two */
};

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ull; }
    return h;
}

static tok_map *map_new(uint32_t want)
{
    uint32_t cap = 16;
    while (cap < want * 2u) cap <<= 1;
    tok_map *m = (tok_map *)calloc(1, sizeof(tok_map));
    if (!m) return NULL;
    m->cap = cap;
    m->key = (const char **)calloc(cap, sizeof(char *));
    m->val = (int32_t *)calloc(cap, sizeof(int32_t));
    if (!m->key || !m->val) { free(m->key); free(m->val); free(m); return NULL; }
    return m;
}

static void map_free(tok_map *m)
{
    if (!m) return;
    free(m->key);
    free(m->val);
    free(m);
}

/* First writer wins. The vocabulary has no duplicates, but if a file ever did, keeping the lower id
 * matches what the reference implementation does when it builds its map in order. */
static void map_put(tok_map *m, const char *k, int32_t v)
{
    uint32_t i = (uint32_t)(fnv1a(k) & (m->cap - 1));
    while (m->key[i]) {
        if (strcmp(m->key[i], k) == 0) return;
        i = (i + 1) & (m->cap - 1);
    }
    m->key[i] = k;
    m->val[i] = v;
}

static int32_t map_get(const tok_map *m, const char *k, int32_t dflt)
{
    uint32_t i = (uint32_t)(fnv1a(k) & (m->cap - 1));
    while (m->key[i]) {
        if (strcmp(m->key[i], k) == 0) return m->val[i];
        i = (i + 1) & (m->cap - 1);
    }
    return dflt;
}

/* ---------------------------------------------------------------------------------------------
 *  the byte permutation
 * ------------------------------------------------------------------------------------------ */

/* GPT-2's bytes_to_unicode, built rather than tabulated so the rule is visible: every byte that is
 * already printable keeps its own codepoint, and the 68 that are not get pushed to 256 and upward in
 * byte order. Those three ASCII and Latin-1 ranges are the definition, not a heuristic. */
static void build_byte_map(tokenizer_t *tk)
{
    int used[256] = { 0 };
    for (int i = 0; i < 512; i++) tk->cp_byte[i] = 0xFFFF;

    for (int b = '!'; b <= '~'; b++) used[b] = 1;
    for (int b = 0xA1; b <= 0xAC; b++) used[b] = 1;
    for (int b = 0xAE; b <= 0xFF; b++) used[b] = 1;

    for (int b = 0; b < 256; b++) {
        if (used[b]) {
            tk->byte_cp[b] = (uint16_t)b;
            tk->cp_byte[b] = (uint16_t)b;
        }
    }
    int n = 0;
    for (int b = 0; b < 256; b++) {
        if (!used[b]) {
            const uint16_t cp = (uint16_t)(256 + n);
            tk->byte_cp[b] = cp;
            tk->cp_byte[cp] = (uint16_t)b;
            n++;
        }
    }
}

/* UTF-8 of a codepoint below 0x800, which is every codepoint the permutation produces. */
static int cp_to_utf8(uint16_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
}

static int utf8_to_cp(const char *s, uint16_t *cp)
{
    const unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) {
        *cp = (uint16_t)(((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F));
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        *cp = (uint16_t)(((c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) |
                         ((unsigned char)s[2] & 0x3F));
        return 3;
    }
    *cp = 0xFFFF;
    return 1;
}

/* ---------------------------------------------------------------------------------------------
 *  setup
 * ------------------------------------------------------------------------------------------ */

int tokenizer_init(tokenizer_t *tk, gguf_t *g)
{
    memset(tk, 0, sizeof(*tk));
    build_byte_map(tk);

    const gguf_kv *v = gguf_find(g, "tokenizer.ggml.tokens");
    const gguf_kv *m = gguf_find(g, "tokenizer.ggml.merges");
    const gguf_kv *ty = gguf_find(g, "tokenizer.ggml.token_type");
    if (!v || !v->sv) { snprintf(g->err, sizeof(g->err), "no tokenizer.ggml.tokens"); return -1; }

    tk->n_vocab = (uint32_t)v->n;
    tk->piece = v->sv;
    tk->type = (ty && ty->av) ? (int32_t *)ty->av : NULL;
    tk->n_merges = m ? (uint32_t)m->n : 0;
    tk->merge = m ? m->sv : NULL;
    tk->bos = (int32_t)gguf_int(g, "tokenizer.ggml.bos_token_id", -1);
    tk->eos = (int32_t)gguf_int(g, "tokenizer.ggml.eos_token_id", -1);
    tk->pre_qwen2 = strcmp(gguf_str(g, "tokenizer.ggml.pre", ""), "qwen2") == 0;

    tk->vmap = map_new(tk->n_vocab);
    if (!tk->vmap) { snprintf(g->err, sizeof(g->err), "out of memory"); return -1; }
    for (uint32_t i = 0; i < tk->n_vocab; i++) map_put(tk->vmap, tk->piece[i], (int32_t)i);

    if (tk->n_merges) {
        tk->mmap_ = map_new(tk->n_merges);
        if (!tk->mmap_) { snprintf(g->err, sizeof(g->err), "out of memory"); return -1; }
        /* Rank IS the index: earlier merges were learned first and take priority. */
        for (uint32_t i = 0; i < tk->n_merges; i++) map_put(tk->mmap_, tk->merge[i], (int32_t)i);
    }
    return 0;
}

void tokenizer_free(tokenizer_t *tk)
{
    map_free(tk->vmap);
    map_free(tk->mmap_);
    memset(tk, 0, sizeof(*tk));
}

/* ---------------------------------------------------------------------------------------------
 *  decode
 * ------------------------------------------------------------------------------------------ */

int tokenizer_decode(const tokenizer_t *tk, int32_t id, char *out, int max)
{
    if (id < 0 || (uint32_t)id >= tk->n_vocab) return 0;
    const char *p = tk->piece[id];
    int n = 0;
    while (*p && n < max) {
        uint16_t cp;
        p += utf8_to_cp(p, &cp);
        const uint16_t b = (cp < 512) ? tk->cp_byte[cp] : 0xFFFF;
        /* A codepoint outside the permutation means this piece is not byte-level encoded -- true of
         * control tokens like <|im_start|>, whose text is literal. Emit it as-is. */
        if (b == 0xFFFF) {
            char tmp[4];
            const int k = cp_to_utf8(cp, tmp);
            for (int i = 0; i < k && n < max; i++) out[n++] = tmp[i];
        } else {
            out[n++] = (char)(unsigned char)b;
        }
    }
    return n;
}

/* ---------------------------------------------------------------------------------------------
 *  encode
 * ------------------------------------------------------------------------------------------ */

static int is_letter(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}
static int is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* One pre-tokenizer chunk starting at s, returning its length in bytes. See the header for what this
 * approximates and where it can differ. */
static int chunk_len(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!p[0]) return 0;

    /* Contractions are their own chunk, and they are matched BEFORE the letter rule so that the
     * apostrophe does not become punctuation and split the word in three. */
    if (p[0] == '\'') {
        static const char *c1[] = { "'s", "'t", "'re", "'ve", "'m", "'ll", "'d" };
        for (size_t i = 0; i < sizeof(c1) / sizeof(c1[0]); i++) {
            const size_t l = strlen(c1[i]);
            if (strncmp(s, c1[i], l) == 0) return (int)l;
        }
    }

    int i = 0;
    /* A single leading space belongs to the word that follows it, which is why " the" is one token
     * and why a tokenizer that strips whitespace first produces worse results at the same speed. */
    if (p[0] == ' ' && p[1] && !is_space(p[1])) i = 1;

    if (is_letter(p[i])) { while (is_letter(p[i])) i++; return i; }
    if (is_digit(p[i]))  { while (is_digit(p[i]))  i++; return i; }
    if (p[i] && !is_space(p[i])) {
        while (p[i] && !is_space(p[i]) && !is_letter(p[i]) && !is_digit(p[i])) i++;
        return i ? i : 1;
    }

    /* Whitespace run. A run that ends in a non-space keeps its last space for that word, matching
     * the reference's negative lookahead. */
    i = 0;
    while (is_space(p[i])) i++;
    if (i > 1 && p[i] && !is_space(p[i])) i--;
    return i ? i : 1;
}

/* BPE over one chunk, any length. Symbols start as single bytes (mapped through the permutation) and
 * merge pairwise, lowest rank first (leftmost on a tie), until no pair is in the merge list.
 *
 * A SYMBOL IS A SPAN of one buffer holding the whole chunk, byte-level encoded, not a copy in a fixed
 * slot. Until 2026-09-27 each symbol was a 64-byte slot, with a comment saying no piece came near that;
 * 201 pieces of this vocabulary are 64 bytes or longer (the longest 256, a merge line 257), and a merge
 * that would have reached 64 ended ALL merging for the chunk -- a comment rule of dashes came out as
 * eight tokens where llama.cpp makes three. And a chunk was capped at 256 bytes, so a longer run of '='
 * lost its tail. Spans have no ceiling; the rank of every adjacent pair is kept and only the two pairs
 * a merge touches are looked up again, so a long run costs a lookup per merge, not a scan of lookups.
 *
 * KEY_MAX bounds a lookup key. No merge line is longer than 257 bytes and every symbol is a single byte
 * or a vocabulary piece (a merge whose result is not a piece is refused), so a longer key cannot match
 * anything and is simply not looked up. */
#define KEY_MAX 1024

static int32_t pair_rank(const tokenizer_t *tk, const char *buf, const int *st, int i, char *key)
{
    const int la = st[i + 1] - st[i], lb = st[i + 2] - st[i + 1];
    if (!tk->mmap_ || la + lb + 2 > KEY_MAX) return -1;
    /* The merge list is keyed on the two pieces separated by a space, which is unambiguous because a
     * literal space is never a piece -- it is U+0120 after the permutation. */
    memcpy(key, buf + st[i], (size_t)la);
    key[la] = ' ';
    memcpy(key + la + 1, buf + st[i + 1], (size_t)lb);
    key[la + 1 + lb] = 0;
    return map_get(tk->mmap_, key, -1);
}

static int bpe_chunk(const tokenizer_t *tk, const char *s, int len, int32_t *out, int max, int n_out)
{
    char *buf  = (char *)malloc((size_t)len * 2 + 1);       /* each byte is 1 or 2 bytes of UTF-8 */
    int  *st   = (int *)malloc(((size_t)len + 1) * sizeof(int));  /* symbol i is buf[st[i] .. st[i+1]) */
    int  *rank = (int *)malloc(((size_t)len + 1) * sizeof(int));  /* rank of pair (i, i+1), -1 none   */
    char  key[KEY_MAX];
    int   nsym = 0, bl = 0;
    if (!buf || !st || !rank) { free(buf); free(st); free(rank); return -1; }

    for (int i = 0; i < len; i++) {
        st[nsym++] = bl;
        bl += cp_to_utf8(tk->byte_cp[(unsigned char)s[i]], buf + bl);
    }
    st[nsym] = bl;
    for (int i = 0; i + 1 < nsym; i++) rank[i] = pair_rank(tk, buf, st, i, key);

    for (;;) {
        int best = -1, best_rank = 0x7FFFFFFF;
        for (int i = 0; i + 1 < nsym; i++)
            if (rank[i] >= 0 && rank[i] < best_rank) { best_rank = rank[i]; best = i; }
        if (best < 0) break;
        /* A merge whose result is not in the vocabulary cannot be emitted, so treat it as absent
         * rather than producing an id of -1 that would index out of the embedding table. */
        const int lm = st[best + 2] - st[best];
        if (lm >= KEY_MAX) break;
        memcpy(key, buf + st[best], (size_t)lm);
        key[lm] = 0;
        if (map_get(tk->vmap, key, -1) < 0) break;
        for (int i = best + 1; i < nsym; i++) st[i] = st[i + 1];
        for (int i = best + 1; i + 1 < nsym - 1; i++) rank[i] = rank[i + 1];
        nsym--;
        if (best > 0) rank[best - 1] = pair_rank(tk, buf, st, best - 1, key);
        if (best + 1 < nsym) rank[best] = pair_rank(tk, buf, st, best, key);
    }

    for (int i = 0; i < nsym; i++) {
        const int l = st[i + 1] - st[i];
        int32_t id = -1;
        if (l < KEY_MAX) {
            memcpy(key, buf + st[i], (size_t)l);
            key[l] = 0;
            id = map_get(tk->vmap, key, -1);
        }
        if (id < 0) continue;          /* unreachable for byte-level: every single byte is a token */
        if (n_out >= max) { n_out = -1; break; }
        out[n_out++] = id;
    }
    free(buf); free(st); free(rank);
    return n_out;
}

/* The special token written literally at s, if there is one: the text from s up to the first '>' is a
 * token the file marks control (3) or user-defined (4) -- <|im_start|>, <|im_end|>, <tool_call> ... every
 * one of them in this vocabulary starts with '<' and ends at its only '>'. Returns its id, or -1. */
static int32_t special_at(const tokenizer_t *tk, const char *s, int *len)
{
    if (s[0] != '<' || !tk->type) return -1;
    const char *e = strchr(s + 1, '>');
    if (!e || e - s + 1 >= 64) return -1;
    char buf[64];
    const int l = (int)(e - s + 1);
    memcpy(buf, s, (size_t)l);
    buf[l] = 0;
    const int32_t id = map_get(tk->vmap, buf, -1);
    if (id < 0 || (tk->type[id] != 3 && tk->type[id] != 4)) return -1;
    *len = l;
    return id;
}

int tokenizer_encode(const tokenizer_t *tk, const char *text, int32_t *out, int max)
{
    int n = 0;
    const char *s = text;

    while (*s) {
        /* Special tokens are literal text in the prompt and are split out FIRST, wherever they are, as
         * llama.cpp does: "<|im_start|>" run through BPE would become eight useless pieces and the model
         * would never see the turn boundary it was trained on. Until 2026-09-27 they were matched only
         * where a chunk began, so "colors.<|im_end|>" lost its end-of-turn to the punctuation run ".<|". */
        int l;
        const int32_t sid = special_at(tk, s, &l);
        if (sid >= 0) {
            if (n >= max) return -1;
            out[n++] = sid;
            s += l;
            continue;
        }
        /* the fragment up to the next special token (or the end) is pre-tokenized on its own */
        int frag = 0;
        while (s[frag] && !(s[frag] == '<' && special_at(tk, s + frag, &l) >= 0)) frag++;
        while (frag > 0) {
            int len = tk->pre_qwen2 ? pt_qwen2_len((const unsigned char *)s, frag) : chunk_len(s);
            if (len > frag) len = frag;
            if (len <= 0) return n;
            n = bpe_chunk(tk, s, len, out, max, n);
            if (n < 0) return -1;
            s += len;
            frag -= len;
        }
    }
    return n;
}

int tokenizer_roundtrip(const tokenizer_t *tk, const char *text)
{
    int32_t ids[4096];
    const int n = tokenizer_encode(tk, text, ids, 4096);
    if (n < 0) return -1;

    char buf[16384];
    int k = 0;
    for (int i = 0; i < n; i++) k += tokenizer_decode(tk, ids[i], buf + k, (int)sizeof(buf) - k - 1);
    buf[k] = 0;
    return strcmp(buf, text) == 0 ? 0 : -1;
}
