/* ===========================================================================================
 *  tl_host.c -- the Teensy model core (shared/tl_core.c) running on the PC, for ONE purpose:
 *  proving it computes exactly what shared/model_q.c computes. PC speed means nothing here.
 *
 *  The board is simulated as the Teensy has it: weights read from the .gguf file on every token,
 *  and a separate store standing in for the PSRAM that is only reachable through read/write calls.
 *  The store is filled with 0xA5 first, because real PSRAM powers up with garbage and nothing may
 *  depend on it being zero.
 *
 *  BUILD (from firmware/bench-one/tests, vendored gcc on PATH -- see .claude/skills/run-bench-one):
 *    gcc -O3 -fopenmp -std=c11 -Wall -Wextra -I../shared -o tl_host.exe tl_host.c \
 *        ../shared/tl_core.c ../shared/gguf_dot.c ../shared/gguf_bits.c -lm
 *  RUN
 *    tl_host.exe <model.gguf> "prompt" [n_gen] > out.txt       (TL_PSRAM_MB=48 by default)
 *  Compare with tl_ref.exe, which prints the same lines from model_q.c.
 * ======================================================================================== */
#include "tl_core.h"
#include "tl_plat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
  #include <windows.h>
#else
  #include <time.h>
#endif

static FILE    *F;
static uint64_t FS;
static uint8_t *PS;
static uint32_t PS_SIZE;

int plat_sd_read(uint64_t off, void *dst, uint32_t n)
{
#if defined(_WIN32)
    if (_fseeki64(F, (long long)off, SEEK_SET)) return -1;
#else
    if (fseeko(F, (off_t)off, SEEK_SET)) return -1;
#endif
    return fread(dst, 1, n, F) == n ? 0 : -1;
}
/* No DMA here, so the "overlap" is a stand-in that decides how many of the pending rows run before the read:
 * TL_OVERLAP_STEPS unset or -1 = all of them, N >= 0 = at most N, -2 = a different count every read (0..4).
 * The core finishes the rest, so every setting must print the same lines -- which tests that resumption. */
static long g_ov_calls;
int plat_sd_read_overlap(uint64_t off, void *dst, uint32_t n, int (*work)(void *), void *ctx)
{
    static int mode = -3;
    if (mode == -3) mode = getenv("TL_OVERLAP_STEPS") ? atoi(getenv("TL_OVERLAP_STEPS")) : -1;
    if (work) {
        const long lim = mode == -1 ? -1 : mode == -2 ? (g_ov_calls++ % 5) : mode;
        for (long i = 0; (lim < 0 || i < lim) && work(ctx); i++) {}
    }
    {   /* tl_plat.h: the whole sectors around the read are the core's spare room, which a board may overwrite
         * (the Teensy reads them in one DMA command). Spoil them here, so a core that kept anything there
         * answers differently on the PC, and check the promised alignment. */
        uint8_t *d = (uint8_t *)dst;
        const uint32_t pre = (uint32_t)(off % 512u), end = (pre + n + 511u) & ~511u;
        if ((uintptr_t)(d - pre) % 32u) { fprintf(stderr, "plat_sd_read_overlap: dst - off%%512 not 32-aligned\n"); exit(9); }
        memset(d - pre, 0xA5, pre);
        memset(d + n, 0x5A, end - pre - n);
    }
    return plat_sd_read(off, dst, n);
}
uint64_t plat_sd_size(void) { return FS; }
uint32_t plat_ps_size(void) { return PS_SIZE; }
int plat_ps_read(uint32_t a, void *d, uint32_t n)
{
    if ((uint64_t)a + n > PS_SIZE) return -1;
    memcpy(d, PS + a, n);
    return 0;
}
int plat_ps_write(uint32_t a, const void *s, uint32_t n)
{
    if ((uint64_t)a + n > PS_SIZE) return -1;
    memcpy(PS + a, s, n);
    return 0;
}
double plat_now(void)
{
#if defined(_WIN32)
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}
void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
void plat_emit(const char *s, int n) { fwrite(s, 1, (size_t)n, stderr); }

/* The same escaping in tl_ref.c, so the two outputs can be compared byte for byte. */
static void esc(const char *s, int n, char *o, int omax)
{
    int k = 0;
    for (int i = 0; i < n && k < omax - 5; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c >= 0x20 && c < 0x7F) o[k++] = (char)c;
        else k += snprintf(o + k, (size_t)(omax - k), "\\x%02X", c);
    }
    o[k] = 0;
}

/* one position of one sequence, in tl_ref.exe's line format, to that sequence's file */
static void multi_line(void *ctx, int seq, int pos, int32_t fed, int32_t t1, float l1, int32_t t2, float l2)
{
    FILE **so = (FILE **)ctx;
    char txt[256], e[1024];
    const int k = tl_decode(t1, txt, (int)sizeof txt - 1);
    esc(txt, k, e, (int)sizeof e);
    fprintf(so[seq], "step %d pos %d fed %d -> top1 %d %.9g top2 %d %.9g text \"%s\"\n",
            pos, pos, (int)fed, (int)t1, l1, (int)t2, l2, e);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: tl_host <model.gguf> \"prompt\" [n_gen]\n"); return 1; }
    const int n_gen = argc > 3 ? atoi(argv[3]) : 8;
    F = fopen(argv[1], "rb");
    if (!F) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
#if defined(_WIN32)
    _fseeki64(F, 0, SEEK_END); FS = (uint64_t)_ftelli64(F);
#else
    fseeko(F, 0, SEEK_END); FS = (uint64_t)ftello(F);
#endif
    const char *mb = getenv("TL_PSRAM_MB");
    PS_SIZE = (uint32_t)((mb ? atoi(mb) : 48) * 1024u * 1024u);
    PS = (uint8_t *)malloc(PS_SIZE);
    if (!PS) { fprintf(stderr, "no memory for the PSRAM stand-in\n"); return 1; }
    memset(PS, 0xA5, PS_SIZE);

    tl_info_t in;
    char err[200];
    const double t0 = plat_now();
    if (tl_open(&in, err, sizeof err)) { fprintf(stderr, "tl_open: %s\n", err); return 1; }
    fprintf(stderr, "open %.2f s: %d layers dim %d hidden %d heads %d/%d head_dim %d vocab %d merges %u max_seq %d\n",
            plat_now() - t0, in.n_layer, in.dim, in.hidden, in.n_heads, in.n_kv, in.head_dim, in.vocab,
            in.n_merges, in.max_seq);
    fprintf(stderr, "params %.0f  file %llu bytes  read per token %llu bytes\n", in.params,
            (unsigned long long)in.file_bytes, (unsigned long long)in.sd_bytes_per_token);
    fprintf(stderr, "PSRAM used %u of %u: tokenizer %u, gains+biases %u, cache %u\n",
            in.ps_used, PS_SIZE, in.ps_tokenizer, in.ps_small, in.ps_kv);

    /* TL_TOKREC=path: the same, for records separated by NUL bytes (so a record can hold newlines) */
    if (getenv("TL_TOKREC")) {
        FILE *tf = fopen(getenv("TL_TOKREC"), "rb");
        if (!tf) { fprintf(stderr, "cannot open %s\n", getenv("TL_TOKREC")); return 1; }
        static char rb[1 << 16], back[1 << 18];
        static int32_t tids[1 << 16];
        int ln = 0, c, k = 0, rt_bad = 0;
        do {
            c = fgetc(tf);
            if (c == 0 || (c == EOF && k)) {
                rb[k] = 0;
                ln++;
                const int m = tl_encode(rb, tids, 1 << 16);
                printf("L%d %d:", ln, m);
                for (int i = 0; i < m; i++) printf(" %d", (int)tids[i]);
                printf("\n");
                int bl = 0;
                for (int i = 0; i < m && bl < (int)sizeof back - 64; i++) bl += tl_decode(tids[i], back + bl, (int)sizeof back - bl - 1);
                back[bl] = 0;
                if (strcmp(back, rb)) rt_bad++;
                k = 0;
            } else if (c != EOF && k < (int)sizeof rb - 1) rb[k++] = (char)c;
        } while (c != EOF);
        fclose(tf);
        fprintf(stderr, "tokrec: %d records, %d round-trip failures\n", ln, rt_bad);
        return 0;
    }
    /* TL_TOKFILE=path: tokenizer parity. Every line (with its newline) encoded by the PSRAM tokenizer, ids
     * printed, and decoded back; tl_ref.exe prints the same from shared/tokenizer.c. */
    if (getenv("TL_TOKFILE")) {
        FILE *tf = fopen(getenv("TL_TOKFILE"), "rb");
        if (!tf) { fprintf(stderr, "cannot open %s\n", getenv("TL_TOKFILE")); return 1; }
        static char lb[65536], back[262144];
        static int32_t tids[65536];
        int ln = 0, rt_bad = 0;
        while (fgets(lb, sizeof lb, tf)) {
            ln++;
            const int k = tl_encode(lb, tids, 65536);
            printf("L%d %d:", ln, k);
            for (int i = 0; i < k; i++) printf(" %d", (int)tids[i]);
            printf("\n");
            int bl = 0;
            for (int i = 0; i < k && bl < (int)sizeof back - 64; i++) bl += tl_decode(tids[i], back + bl, (int)sizeof back - bl - 1);
            back[bl] = 0;
            if (strcmp(back, lb)) rt_bad++;
        }
        fclose(tf);
        fprintf(stderr, "tokfile: %d lines, %d round-trip failures\n", ln, rt_bad);
        return 0;
    }

    /* TL_MULTI=file: every line of the file is a prompt (\n \t \\ unescaped as the board does), all of them
     * answered together through tl_multi_pass, n_gen each. Sequence i's lines go to <TL_MULTI_OUT>i.txt in
     * exactly tl_ref.exe's format, so each diffs against tl_ref.exe run on that prompt alone. */
    if (getenv("TL_MULTI")) {
        FILE *pf = fopen(getenv("TL_MULTI"), "rb");
        if (!pf) { fprintf(stderr, "cannot open %s\n", getenv("TL_MULTI")); return 1; }
        const char *outp = getenv("TL_MULTI_OUT") ? getenv("TL_MULTI_OUT") : "multi_";
        static char lines[8][4096];
        static int32_t sids[8][1024];
        static FILE *so[8];
        tl_seq_t sq[8];
        int ns = 0;
        while (ns < 8 && fgets(lines[ns], sizeof lines[ns], pf)) {
            char *s = lines[ns];
            s[strcspn(s, "\r\n")] = 0;
            if (!*s) continue;
            char *o = s;                                   /* the board's unescape, character for character */
            for (char *p = s; *p; p++) {
                if (*p == '\\' && p[1]) {
                    p++;
                    if (*p == 'n') *o++ = '\n';
                    else if (*p == 't') *o++ = '\t';
                    else if (*p == '\\') *o++ = '\\';
                    else { *o++ = '\\'; *o++ = *p; }
                } else *o++ = *p;
            }
            *o = 0;
            const int k = tl_encode(s, sids[ns], 1024 - n_gen - 1);
            if (k <= 0) { fprintf(stderr, "prompt %d did not encode\n", ns); return 1; }
            sq[ns].ids = sids[ns]; sq[ns].n_prompt = k; sq[ns].n_gen = n_gen;
            char fn[512];
            snprintf(fn, sizeof fn, "%s%d.txt", outp, ns);
            so[ns] = fopen(fn, "wb");
            if (!so[ns]) { fprintf(stderr, "cannot write %s\n", fn); return 1; }
            fprintf(so[ns], "prompt ids:");
            for (int i = 0; i < k; i++) fprintf(so[ns], " %d", (int)sids[ns][i]);
            fprintf(so[ns], "\n");
            ns++;
        }
        fclose(pf);
        if (getenv("TL_MULTI_SHARE")) tl_multi_share(atoi(getenv("TL_MULTI_SHARE")));   /* default on */
        if (tl_multi_begin(sq, ns)) { fprintf(stderr, "multi: %s\n", tl_last_error()); return 1; }
        for (int i = 0; i < ns; i++)
            if (sq[i].donor >= 0) fprintf(stderr, "  prompt %d copies its first %d positions from prompt %d\n", i, sq[i].share, sq[i].donor);
        int passes = 0, fed = 0, r;
        while ((r = tl_multi_pass(sq, ns, multi_line, so)) > 0) { passes++; fed += r; fprintf(stderr, "  pass %d: %d positions\n", passes, r); }
        if (r < 0) { fprintf(stderr, "multi: %s\n", tl_last_error()); return 1; }
        int solo = 0;
        for (int i = 0; i < ns; i++) {
            fclose(so[i]);
            solo += (sq[i].n_prompt + tl_batch() - 1) / tl_batch() + (sq[i].fed - sq[i].n_prompt);
        }
        fprintf(stderr, "multi: %d prompts, %d passes over the weights for %d positions (the same prompts one at a time "
                "with the batched prompt: %d passes)\n", ns, passes, fed, solo);
        return 0;
    }

    int32_t ids[512];
    const int n = tl_encode(argv[2], ids, 512);
    if (n <= 0) { fprintf(stderr, "prompt did not encode\n"); return 1; }
    printf("prompt ids:");
    for (int i = 0; i < n; i++) printf(" %d", (int)ids[i]);
    printf("\n");

    char txt[256], e[1024], gen[8192];
    int gl = 0;
    int32_t next = 0;

    /* TL_LOOKUP=1: prompt-lookup decoding. The prompt goes through tl_prefill; then each pass feeds the pending
     * token plus a draft copied from the context (the tokens that followed the most recent earlier occurrence
     * of the last 3, 2 or 1 tokens), and keeps the draft only as far as the model's own greedy choice agrees.
     * Every kept position's logits come from tl_prefill, which is identical to tl_forward, so the lines printed
     * must be identical to tl_ref.exe; only the number of passes changes. TL_DRAFT caps the draft length. */
    if (getenv("TL_LOOKUP") && atoi(getenv("TL_LOOKUP"))) {
        static int32_t ctx[4096], o1[64], o2[64], feed[64], draft[64];
        static float f1[64], f2[64];
        const int B = tl_batch();
        int kmax = getenv("TL_DRAFT") ? atoi(getenv("TL_DRAFT")) : B - 1;
        if (kmax > B - 1) kmax = B - 1;
        if (kmax < 0) kmax = 0;
        int passes = 0, drafted = 0, accepted = 0, stop = 0;
        memcpy(ctx, ids, (size_t)n * sizeof(int32_t));
#define PRINT_STEP(q, f, a1, b1, a2, b2) do { \
            const int kk = tl_decode((a1), txt, (int)sizeof txt - 1); esc(txt, kk, e, (int)sizeof e); \
            printf("step %d pos %d fed %d -> top1 %d %.9g top2 %d %.9g text \"%s\"\n", (q), (q), (int)(f), (int)(a1), (b1), (int)(a2), (b2), e); \
            if ((q) >= n - 1 && gl + kk < (int)sizeof gen) { memcpy(gen + gl, txt, (size_t)kk); gl += kk; } \
            if ((q) >= n - 1 && (a1) == in.eos) stop = 1; } while (0)
        for (int p0 = 0; p0 < n && !stop; p0 += B) {
            const int k = (n - p0) < B ? (n - p0) : B;
            if (tl_prefill(ids + p0, k, p0, o1, f1, o2, f2)) { fprintf(stderr, "prefill: %s\n", tl_last_error()); return 1; }
            passes++;
            for (int j = 0; j < k; j++) PRINT_STEP(p0 + j, ids[p0 + j], o1[j], f1[j], o2[j], f2[j]);
            next = o1[k - 1];
        }
        int pos = n;
        while (!stop && pos < n + n_gen) {
            ctx[pos] = next;
            int lim = n + n_gen - pos - 1;
            if (lim > kmax) lim = kmax;
            int k = 0;
            for (int m = 3; m >= 1 && !k && lim > 0; m--) {          /* most recent earlier match of the last m */
                if (pos + 1 < m + 1) continue;
                const int32_t *suf = ctx + pos + 1 - m;
                for (int s = pos - m; s >= 0 && !k; s--) {
                    if (memcmp(ctx + s, suf, (size_t)m * sizeof(int32_t))) continue;
                    while (k < lim && s + m + k <= pos) { draft[k] = ctx[s + m + k]; k++; }
                }
            }
            feed[0] = next;
            for (int i = 0; i < k; i++) feed[i + 1] = draft[i];
            if (tl_prefill(feed, k + 1, pos, o1, f1, o2, f2)) { fprintf(stderr, "prefill: %s\n", tl_last_error()); return 1; }
            passes++;
            drafted += k;
            int j = 0;
            PRINT_STEP(pos, feed[0], o1[0], f1[0], o2[0], f2[0]);
            while (!stop && j < k && o1[j] == draft[j]) {
                j++;
                accepted++;
                PRINT_STEP(pos + j, feed[j], o1[j], f1[j], o2[j], f2[j]);
            }
            for (int i = 0; i < j; i++) ctx[pos + 1 + i] = draft[i];
            next = o1[j];
            pos += j + 1;
        }
#undef PRINT_STEP
        gen[gl] = 0;
        fprintf(stderr, "lookup: %d passes over the weights for %d positions (prompt %d), %d draft tokens proposed, %d accepted\n",
                passes, pos, n, drafted, accepted);
        fprintf(stderr, "\n%s%s\n", argv[2], gen);
        return 0;
    }
    /* TL_PREFILL=1: the prompt goes through tl_prefill, tl_batch() positions per pass over the weights. The
     * lines printed must be identical to the per-token run and to tl_ref.exe. */
    const int use_prefill = getenv("TL_PREFILL") && atoi(getenv("TL_PREFILL"));
    static int32_t pt1[512], pt2[512];
    static float pl1[512], pl2[512];
    if (use_prefill) {
        for (int p0 = 0; p0 < n; p0 += tl_batch()) {
            const int k = (n - p0) < tl_batch() ? (n - p0) : tl_batch();
            tl_stats_reset();
            if (tl_prefill(ids + p0, k, p0, pt1 + p0, pl1 + p0, pt2 + p0, pl2 + p0)) { fprintf(stderr, "prefill: %s\n", tl_last_error()); return 1; }
            tl_stats_t st;
            tl_stats(&st);
            fprintf(stderr, "  prefill positions %d..%d: %.2f s, SD %.1f MB\n", p0, p0 + k - 1, st.t_total, st.sd_bytes / 1048576.0);
        }
    }
    for (int step = 0; step < n + n_gen; step++) {
        const int32_t fed = step < n ? ids[step] : next;
        int32_t t1, t2;
        float l1, l2;
        tl_stats_reset();
        if (use_prefill && step < n) { t1 = pt1[step]; l1 = pl1[step]; t2 = pt2[step]; l2 = pl2[step]; }
        else if (tl_forward(fed, step, &t1, &l1, &t2, &l2)) { fprintf(stderr, "forward: %s\n", tl_last_error()); return 1; }
        const int k = tl_decode(t1, txt, (int)sizeof txt - 1);
        esc(txt, k, e, (int)sizeof e);
        printf("step %d pos %d fed %d -> top1 %d %.9g top2 %d %.9g text \"%s\"\n",
               step, step, (int)fed, (int)t1, l1, (int)t2, l2, e);
        fflush(stdout);
        tl_stats_t st;
        tl_stats(&st);
        fprintf(stderr, "  step %d: %.2f s, SD %.1f MB, PSRAM read %.1f KB written %.1f KB\n", step, st.t_total,
                st.sd_bytes / 1048576.0, st.ps_read / 1024.0, st.ps_written / 1024.0);
        if (step >= n - 1 && gl + k < (int)sizeof gen) { memcpy(gen + gl, txt, (size_t)k); gl += k; }
        next = t1;
        if (step >= n - 1 && t1 == in.eos) break;
    }
    gen[gl] = 0;
    fprintf(stderr, "\n%s%s\n", argv[2], gen);
    return 0;
}
