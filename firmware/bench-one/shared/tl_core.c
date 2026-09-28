/* ===========================================================================================
 *  tl_core.c -- see tl_core.h. A GGUF transformer with streamed weights and external memory.
 * ===========================================================================================
 *
 *  WHERE EVERYTHING LIVES (Qwen2.5-Coder-3B: dim 2048, hidden 11008, 16 heads, 2 kv heads, 36 layers)
 *
 *    storage (SD)   every weight matrix and the embedding table, 1.93 GB, read once per token in blocks
 *                   of whole rows (ROW, 64 KB); the tied embedding is also the output head, read in full
 *    PSRAM          tokenizer: 151,936 pieces + 151,387 merges + two open-addressing maps (~8 MB)
 *                   every norm gain and bias, 36 layers (~0.9 MB)
 *                   the attention cache, int8 + one float scale per head per position (19 KB/position)
 *    on-chip RAM    the buffers listed below
 *
 *  STATIC RAM, and why each is that size (defaults, this model):
 *    X XB XB2 Q         4 x 2048 floats        32 KB   activations, exactly model_q's x xb xb2 q
 *    KB VB              2 x 512 floats          4 KB   this position's key and value
 *    HB HB2             2 x 11008 floats       86 KB   feed-forward hidden (big, OCRAM on the Teensy)
 *    XQ XS XSUM         11008 + 2 x 345        14 KB   the int8 activation, its per-32 scales and sums
 *    ATT                16 heads x 2048 pos   128 KB   attention scores for every head (OCRAM)
 *    ROW                64 KB                  64 KB   one block of weight rows (OCRAM)
 *    KC/VC + scales     64 pos x 512 + 64 x 8  34 KB   one block of the cache pulled from PSRAM
 *    G_A G_F BQ BK BV   2048 x 3 + 512 x 2     28 KB   this layer's gains and biases, from PSRAM
 *    SYM + misc         256 x 64 + small       ~20 KB  the tokenizer's merge workspace, parse buffer
 *    total                                    ~410 KB
 *
 *  THE ARITHMETIC IS model_q.c's FAST PATH, COPIED, NOT RE-DERIVED. rmsnorm, softmax, rope and kv_store
 *  are verbatim; matvec quantizes the input once per matrix and takes the Q4_K presum kernel exactly when
 *  model_matvec_rows does; q/k/v share one quantization and use gguf_dot_q like matvec_group. The only
 *  reorderings are ones that do not touch a floating-point sum: attention scores for all heads are
 *  computed per block of positions (each score is the same dot in the same order), and the value sum for
 *  each head still runs over positions in ascending order.
 * ======================================================================================== */
/* Round like the PC does. GCC on ARM fuses a*b+c into one fused multiply-add by default, which rounds
 * once where the PC's separate multiply and add round twice; that alone moves logits in the last bits and
 * can flip a near-tie (the France run has one at step 14, margin 0.006). gguf_dot.c and gguf_bits.c carry
 * the same pragma since 2026-09-27 (v5 had 23 fused operations there), and expf/powf/sinf/cosf come from
 * tl_math.h instead of each machine's C library, so the Teensy and tests/tl_ref.exe are meant to agree to
 * the last bit; the board suite checks that rather than assuming it. */
#if defined(__arm__) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("fp-contract=off")
#endif
#include "tl_core.h"
#include "tl_plat.h"
#include "gguf.h"
#include "pretok_qwen2.h"
#include "tl_math.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---- limits ------------------------------------------------------------------------------ */
#ifndef TL_MAX_DIM
#define TL_MAX_DIM     2048
#endif
#ifndef TL_MAX_HIDDEN
#define TL_MAX_HIDDEN  11008
#endif
#ifndef TL_MAX_KVDIM
#define TL_MAX_KVDIM   512
#endif
#ifndef TL_MAX_KVH
#define TL_MAX_KVH     8
#endif
#ifndef TL_MAX_HEADS
#define TL_MAX_HEADS   16
#endif
#ifndef TL_MAX_LAYERS
#define TL_MAX_LAYERS  40
#endif
#ifndef TL_MAX_SEQ
#define TL_MAX_SEQ     2048
#endif
#define TL_ROWBUF      TL_PIPEBUF
#define TL_KCHUNK      64         /* cache positions pulled from PSRAM per read in attention */
#define TL_KEYMAX      1024       /* longest tokenizer string accepted                        */
#define TL_MAX_SPECIAL 64         /* control and user-defined tokens remembered from the file  */
#define TL_MAX_CHUNK   2048       /* longest pre-tokenizer chunk BPE takes whole (a prompt line
                                     on the board is at most 2047 bytes, so it never splits one) */

/* Big buffers go to OCRAM on the Teensy 4.1 (the core's DMAMEM section); anywhere on the PC. */
#if defined(__IMXRT1062__)
  #define TL_BIG __attribute__((section(".dmabuffers"), used, aligned(32)))
#else
  #define TL_BIG __attribute__((aligned(32)))
#endif

/* ---- error and statistics ---------------------------------------------------------------- */
static char        g_err[160];
static tl_stats_t  S;

const char *tl_last_error(void) { return g_err; }
void tl_stats_reset(void) { memset(&S, 0, sizeof S); }
void tl_stats(tl_stats_t *s)
{
    *s = S;
    s->t_compute = S.t_total - S.t_sd - S.t_ps;
    if (s->t_compute < 0) s->t_compute = 0;
}

/* TWO ROW BUFFERS, IN RAM THAT IS NOT CACHED. While block i+1 of a matrix comes off the card into one, block i
 * is worked from the other (pipe_read). On the Teensy the card fills them by DMA, and they are plain arrays
 * in RAM1 (DTCM) because a DMA transfer into cached OCRAM next to SdFat's CPU-copied partial sectors would
 * leave stale bytes in the shared cache line. 24 KB each: a 16-row half of a gate/up block (18 KB) fits.
 * Each has one sector more: a block goes at its file offset's place within a sector (pipe_at), so a board may
 * read the whole sectors around it in one piece (tl_plat.h, plat_sd_read_overlap). */
#ifndef TL_PIPEBUF
#define TL_PIPEBUF     (24u * 1024u)
#endif
#define TL_SECTOR      512u
static uint8_t PIPE[2][TL_PIPEBUF + TL_SECTOR] __attribute__((aligned(32)));
#define ROW (PIPE[0])
static uint8_t *pipe_at(int buf, uint64_t off) { return PIPE[buf] + (uint32_t)(off % TL_SECTOR); }

static int sd_read(uint64_t off, void *dst, uint32_t n)
{
    const double t = plat_now();
    const int r = plat_sd_read(off, dst, n);
    S.t_sd += plat_now() - t;
    S.sd_bytes += n;
    if (r) snprintf(g_err, sizeof g_err, "SD read failed at %llu (+%lu)", (unsigned long long)off, (unsigned long)n);
    return r;
}

/* ---- the card and the arithmetic at the same time -------------------------------------------------------------
 * A block is a run of whole rows in one PIPE half, and the arithmetic for it is one call per row, in order.
 * pipe_read() reads the next block into the other half while the platform runs the pending block's rows
 * (plat_sd_read_overlap calls blk_step during the transfer, as often as it can); whatever rows it did not reach
 * are run right after, before the next block starts. Every row is still worked exactly once and in the same
 * order as before, so the arithmetic -- and every result bit -- is unchanged; only when it happens moves.
 * S.t_sd counts only the card time the pass waited for; S.t_hidden the arithmetic done while reading. */
typedef struct {
    const uint8_t *rows;
    uint32_t rb;
    int r0, k, j;                                         /* rows r0..r0+k-1; j is the next to work */
    void (*row)(void *c, const uint8_t *p, int r);       /* the arithmetic for row r */
    void (*done)(void *c, int r0, int k);                /* after the block's last row, or NULL */
    void *c;
} tl_blk;

static int g_in_read;

static int blk_step(void *ctx)
{
    tl_blk *b = (tl_blk *)ctx;
    if (b->j >= b->k) return 0;
    const double t = g_in_read ? plat_now() : 0.0;
    b->row(b->c, b->rows + (size_t)b->j * b->rb, b->r0 + b->j);
    b->j++;
    if (b->j == b->k && b->done) b->done(b->c, b->r0, b->k);
    if (g_in_read) S.t_hidden += plat_now() - t;
    return b->j < b->k;
}

/* read n bytes at off into dst while working `pending` (may be NULL); then finish `pending` */
static int pipe_read(uint64_t off, void *dst, uint32_t n, tl_blk *pending)
{
    const double t = plat_now(), h0 = S.t_hidden;
    g_in_read = 1;
    const int r = plat_sd_read_overlap(off, dst, n, pending && pending->j < pending->k ? blk_step : 0, pending);
    g_in_read = 0;
    S.t_sd += (plat_now() - t) - (S.t_hidden - h0);
    S.sd_bytes += n;
    if (r) { snprintf(g_err, sizeof g_err, "SD read failed at %llu (+%lu)", (unsigned long long)off, (unsigned long)n); return r; }
    if (pending) while (blk_step(pending)) {}
    return 0;
}

/* One job of a pipeline: a read, and the rows it brings. next() fills the next job, 0 when there is none. */
typedef struct {
    uint64_t off;
    uint32_t rb;
    int r0, k;
    void (*row)(void *c, const uint8_t *p, int r);
    void (*done)(void *c, int r0, int k);
    void *c;
} tl_job;
typedef int (*tl_next_fn)(void *it, tl_job *j);

static int pipeline(tl_next_fn next, void *it)
{
    tl_job j;
    if (!next(it, &j)) return 0;
    int buf = 0;
    if (pipe_read(j.off, pipe_at(buf, j.off), j.rb * (uint32_t)j.k, NULL)) return -1;
    tl_blk cur = { pipe_at(buf, j.off), j.rb, j.r0, j.k, 0, j.row, j.done, j.c };
    for (;;) {
        tl_job nj;
        if (!next(it, &nj)) { while (blk_step(&cur)) {} return 0; }
        buf ^= 1;
        if (pipe_read(nj.off, pipe_at(buf, nj.off), nj.rb * (uint32_t)nj.k, &cur)) return -1;
        const tl_blk nb = { pipe_at(buf, nj.off), nj.rb, nj.r0, nj.k, 0, nj.row, nj.done, nj.c };
        cur = nb;
    }
}

/* rows per block of a matrix: as many as a PIPE half holds, and an even number when a row is not a multiple of
 * 4 bytes (Q6_K rows of 11008 are 9,030), so every block starts 4-aligned -- a misaligned start makes SdFat's
 * DMA path fall back to one command per 512-byte sector */
static int pipe_rows(uint32_t rb)
{
    int per = (int)(TL_PIPEBUF / rb);
    if (rb % 4 && per > 1) per &= ~1;
    return per;
}

static uint32_t g_ps_size;
static int ps_read(uint32_t a, void *dst, uint32_t n)
{
    if (!n) return 0;
    if ((uint64_t)a + n > g_ps_size) {
        snprintf(g_err, sizeof g_err, "PSRAM read outside the store: %lu+%lu > %lu", (unsigned long)a, (unsigned long)n, (unsigned long)g_ps_size);
        return -1;
    }
    const double t = plat_now();
    const int r = plat_ps_read(a, dst, n);
    S.t_ps += plat_now() - t;
    S.ps_read += n;
    if (r) snprintf(g_err, sizeof g_err, "PSRAM read failed at %lu (+%lu)", (unsigned long)a, (unsigned long)n);
    return r;
}
static int ps_write(uint32_t a, const void *src, uint32_t n)
{
    if (!n) return 0;
    if ((uint64_t)a + n > g_ps_size) {
        snprintf(g_err, sizeof g_err, "PSRAM write outside the store: %lu+%lu > %lu", (unsigned long)a, (unsigned long)n, (unsigned long)g_ps_size);
        return -1;
    }
    const double t = plat_now();
    const int r = plat_ps_write(a, src, n);
    S.t_ps += plat_now() - t;
    S.ps_written += n;
    if (r) snprintf(g_err, sizeof g_err, "PSRAM write failed at %lu (+%lu)", (unsigned long)a, (unsigned long)n);
    return r;
}

/* ---- the model --------------------------------------------------------------------------- */
typedef struct {
    uint32_t type, rows, cols, row_bytes;
    uint64_t off;             /* ABSOLUTE file offset of row 0 */
    int      present;
} tten_t;

typedef struct {
    tten_t   wq, wk, wv, wo, wg, wu, wd;
    tten_t   n_attn, n_ffn, bq, bk, bv;          /* small float tensors, copied to PSRAM */
    uint32_t ps_attn, ps_ffn, ps_bq, ps_bk, ps_bv;
} tlayer_t;

static struct {
    int      n_layer, dim, hidden, n_heads, n_kv, head_dim, q_dim, kv_dim, vocab, max_seq;
    float    eps, rope_base;
    tlayer_t L[TL_MAX_LAYERS];
    tten_t   embd, onorm, out;
    int      tied;
    uint32_t ps_onorm;
    uint64_t data_start;
    /* tokenizer, all in PSRAM */
    uint32_t n_vocab, n_merges;
    uint32_t p_off, p_blob, m_off, m_blob;       /* offset tables have n+1 entries */
    uint32_t vmap, vcap, mmap_, mcap;            /* slot = id+1 / rank+1, 0 = empty */
    int32_t  bos, eos;
    int      pre_qwen2;                          /* tokenizer.ggml.pre is "qwen2"     */
    int32_t  spec[TL_MAX_SPECIAL];               /* token_type 3 (control) or 4 (user-defined) */
    int      n_spec;
    /* attention cache in PSRAM */
    uint32_t k_base, v_base, ks_base, vs_base;
    uint32_t ps_used;
    int      open;
} M;

/* ---- on-chip buffers ---------------------------------------------------------------------- */
static float   X[TL_MAX_DIM], XB[TL_MAX_DIM], XB2[TL_MAX_DIM], Q[TL_MAX_DIM];
static float   KB[TL_MAX_KVDIM], VB[TL_MAX_KVDIM];
#ifndef TL_BATCH
#define TL_BATCH       8          /* prompt positions per pass over the weights in tl_prefill */
#endif

/* ONE ARENA, TWO LAYOUTS.
 *
 * The per-token path needs the feed-forward hidden twice (HB, HB2: 86 KB). The batched prompt path needs,
 * for up to TL_BATCH positions at once, the residual stream, the quantized input of the matrix being
 * streamed, keys and values, and the feed-forward output -- kept QUANTIZED per 32 rather than as floats,
 * which is exactly what the down projection would have made of it -- sharing space with the positions'
 * query vectors, because the two are never alive at the same time. The two paths never run together, so
 * they share one block of OCRAM, and a board can borrow it between passes (tl_scratch). */
typedef struct {
    float   x[TL_BATCH][TL_MAX_DIM];
    int8_t  xq[TL_BATCH][TL_MAX_DIM];
    float   xs[TL_BATCH][TL_MAX_DIM / 32 + 1];
    int32_t xsum[TL_BATCH][TL_MAX_DIM / 32 + 1];
    float   k[TL_BATCH][TL_MAX_KVDIM], v[TL_BATCH][TL_MAX_KVDIM];
    float   g[TL_BATCH][32], u[TL_BATCH][32];
    union {
        float q[TL_BATCH][TL_MAX_DIM];
        struct {
            int8_t  hq[TL_BATCH][TL_MAX_HIDDEN];
            float   hs[TL_BATCH][TL_MAX_HIDDEN / 32 + 1];
            int32_t hsum[TL_BATCH][TL_MAX_HIDDEN / 32 + 1];
        } h;
    } c;
} tl_batch_t;
typedef struct { float hb[TL_MAX_HIDDEN], hb2[TL_MAX_HIDDEN]; } tl_tok_t;
static union { tl_batch_t b; tl_tok_t t; } AR TL_BIG;
#define HB  (AR.t.hb)
#define HB2 (AR.t.hb2)
#define PB  (AR.b)

void *tl_scratch(uint32_t *bytes) { if (bytes) *bytes = (uint32_t)sizeof AR; return &AR; }
int   tl_batch(void) { return TL_BATCH; }
static int8_t  XQ[TL_MAX_HIDDEN];
static float   XS[TL_MAX_HIDDEN / 32 + 1];
static int32_t XSUM[TL_MAX_HIDDEN / 32 + 1];
static float   ATT[TL_MAX_HEADS * TL_MAX_SEQ] TL_BIG;

static int8_t  KC[TL_KCHUNK * TL_MAX_KVDIM];
static float   KCS[TL_KCHUNK * TL_MAX_KVH];
static int8_t  KQ8[TL_MAX_KVDIM], VQ8[TL_MAX_KVDIM];
static float   KSC[TL_MAX_KVH], VSC[TL_MAX_KVH];
static float   G_A[TL_MAX_DIM], G_F[TL_MAX_DIM], BQ[TL_MAX_DIM], BK[TL_MAX_KVDIM], BV[TL_MAX_KVDIM];

/* ================================================================================================
 *  GGUF, read sequentially through a 4 KB window
 * ============================================================================================== */
static struct { uint64_t base; uint32_t len, i; uint8_t b[4096]; } R;
static uint64_t g_fsize;

static int r_fill(void)
{
    const uint64_t nb = R.base + R.len;
    if (nb >= g_fsize) { snprintf(g_err, sizeof g_err, "model file truncated at %llu", (unsigned long long)nb); return -1; }
    const uint64_t left = g_fsize - nb;
    const uint32_t n = left < sizeof R.b ? (uint32_t)left : (uint32_t)sizeof R.b;
    if (sd_read(nb, R.b, n)) return -1;
    R.base = nb; R.len = n; R.i = 0;
    return 0;
}
static int r_bytes(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        if (R.i == R.len && r_fill()) return -1;
        uint32_t k = R.len - R.i;
        if (k > n) k = n;
        memcpy(d, R.b + R.i, k);
        R.i += k; d += k; n -= k;
    }
    return 0;
}
static int r_skip(uint64_t n)
{
    const uint64_t avail = R.len - R.i;
    if (n <= avail) { R.i += (uint32_t)n; return 0; }
    const uint64_t pos = R.base + R.len + (n - avail);
    if (pos > g_fsize) { snprintf(g_err, sizeof g_err, "skip past end of file"); return -1; }
    R.base = pos; R.len = 0; R.i = 0;
    return 0;
}
static uint64_t r_tell(void) { return R.base + R.i; }
static int r_u32(uint32_t *v) { return r_bytes(v, 4); }
static int r_u64(uint64_t *v) { return r_bytes(v, 8); }

/* Scalar metadata, kept small: every non-array key under 72 characters. */
#define NSCAL 128
static struct { char key[72]; int64_t i; double f; char s[40]; } SC[NSCAL];
static int nsc;

static int64_t sc_int(const char *key, int64_t d)
{
    for (int k = 0; k < nsc; k++) if (!strcmp(SC[k].key, key)) return SC[k].i;
    return d;
}
static double sc_flt(const char *key, double d)
{
    for (int k = 0; k < nsc; k++) if (!strcmp(SC[k].key, key)) return SC[k].f;
    return d;
}
static const char *sc_str(const char *key)
{
    for (int k = 0; k < nsc; k++) if (!strcmp(SC[k].key, key)) return SC[k].s;
    return NULL;
}
static char g_arch[40];
static int64_t arch_int(const char *suffix, int64_t d)
{
    char k[96]; snprintf(k, sizeof k, "%s.%s", g_arch, suffix); return sc_int(k, d);
}
static double arch_flt(const char *suffix, double d)
{
    char k[96]; snprintf(k, sizeof k, "%s.%s", g_arch, suffix); return sc_flt(k, d);
}

static size_t scalar_width(uint32_t t)
{
    switch (t) {
    case GGUF_U8: case GGUF_I8: case GGUF_BOOL: return 1;
    case GGUF_U16: case GGUF_I16:               return 2;
    case GGUF_U32: case GGUF_I32: case GGUF_F32:return 4;
    case GGUF_U64: case GGUF_I64: case GGUF_F64:return 8;
    default:                                    return 0;
    }
}

/* One scalar, into a table slot (or discarded when slot is NULL). Same conversions as shared/gguf.c. */
static int r_scalar(uint32_t t, int64_t *iv, double *fv, char *sv, int svlen)
{
    switch (t) {
    case GGUF_U8:  { uint8_t  v; if (r_bytes(&v,1)) return -1; *iv = v; return 0; }
    case GGUF_I8:  { int8_t   v; if (r_bytes(&v,1)) return -1; *iv = v; return 0; }
    case GGUF_BOOL:{ uint8_t  v; if (r_bytes(&v,1)) return -1; *iv = v ? 1 : 0; return 0; }
    case GGUF_U16: { uint16_t v; if (r_bytes(&v,2)) return -1; *iv = v; return 0; }
    case GGUF_I16: { int16_t  v; if (r_bytes(&v,2)) return -1; *iv = v; return 0; }
    case GGUF_U32: { uint32_t v; if (r_bytes(&v,4)) return -1; *iv = v; return 0; }
    case GGUF_I32: { int32_t  v; if (r_bytes(&v,4)) return -1; *iv = v; return 0; }
    case GGUF_U64: { uint64_t v; if (r_bytes(&v,8)) return -1; *iv = (int64_t)v; return 0; }
    case GGUF_I64: { int64_t  v; if (r_bytes(&v,8)) return -1; *iv = v; return 0; }
    case GGUF_F32: { float    v; if (r_bytes(&v,4)) return -1; *fv = v; *iv = (int64_t)v; return 0; }
    case GGUF_F64: { double   v; if (r_bytes(&v,8)) return -1; *fv = v; *iv = (int64_t)v; return 0; }
    case GGUF_STR: {
        uint64_t n;
        if (r_u64(&n) || n > (1u << 28)) return -1;
        if (sv && svlen > 0) {
            const uint64_t keep = n < (uint64_t)(svlen - 1) ? n : (uint64_t)(svlen - 1);
            if (r_bytes(sv, (uint32_t)keep)) return -1;
            sv[keep] = 0;
            return r_skip(n - keep);
        }
        return r_skip(n);
    }
    default: return -1;
    }
}

/* ---- PSRAM allocation ----------------------------------------------------------------------- */
static uint32_t g_ps_cur;
static int ps_alloc(uint32_t n, uint32_t *addr)
{
    g_ps_cur = (g_ps_cur + 3u) & ~3u;
    if ((uint64_t)g_ps_cur + n > g_ps_size) {
        snprintf(g_err, sizeof g_err, "PSRAM full: need %lu more bytes at %lu of %lu", (unsigned long)n, (unsigned long)g_ps_cur, (unsigned long)g_ps_size);
        return -1;
    }
    *addr = g_ps_cur;
    g_ps_cur += n;
    return 0;
}
static int ps_zero(uint32_t a, uint32_t n)
{
    memset(ROW, 0, TL_ROWBUF);
    while (n) {
        const uint32_t k = n < TL_ROWBUF ? n : TL_ROWBUF;
        if (ps_write(a, ROW, k)) return -1;
        a += k; n -= k;
    }
    return 0;
}

/* A GGUF string array streamed into PSRAM as an offset table (n+1 entries, relative to the blob) and a
 * blob of the bytes. Strings are cut at the first NUL, exactly where shared/tokenizer.c's C strings end. */
static int stream_strings(uint64_t n, uint32_t *off_tab, uint32_t *blob, uint32_t *count)
{
    if (n > 4000000u) { snprintf(g_err, sizeof g_err, "string array of %llu entries", (unsigned long long)n); return -1; }
    if (ps_alloc((uint32_t)(n + 1) * 4u, off_tab)) return -1;
    *blob = (g_ps_cur + 3u) & ~3u;
    g_ps_cur = *blob;
    static uint32_t ob[256];
    static uint8_t  bb[4096];
    uint32_t no = 0, nbb = 0, rel = 0, oaddr = *off_tab;
    uint32_t baddr = *blob;
    static char s[TL_KEYMAX];
    for (uint64_t j = 0; j < n; j++) {
        uint64_t len;
        if (r_u64(&len) || len > (1u << 28)) { snprintf(g_err, sizeof g_err, "bad string %llu", (unsigned long long)j); return -1; }
        if (len >= TL_KEYMAX) { snprintf(g_err, sizeof g_err, "tokenizer string %llu is %llu bytes (limit %d)", (unsigned long long)j, (unsigned long long)len, TL_KEYMAX - 1); return -1; }
        if (r_bytes(s, (uint32_t)len)) return -1;
        s[len] = 0;
        const uint32_t cl = (uint32_t)strlen(s);
        ob[no++] = rel;
        if (no == 256) { if (ps_write(oaddr, ob, 1024)) return -1; oaddr += 1024; no = 0; }
        for (uint32_t k = 0; k < cl; ) {
            uint32_t c = cl - k;
            if (c > sizeof bb - nbb) c = (uint32_t)sizeof bb - nbb;
            memcpy(bb + nbb, s + k, c); nbb += c; k += c;
            if (nbb == sizeof bb) {
                if ((uint64_t)baddr + nbb > g_ps_size) { snprintf(g_err, sizeof g_err, "PSRAM full while storing strings"); return -1; }
                if (ps_write(baddr, bb, nbb)) return -1;
                baddr += nbb; nbb = 0;
            }
        }
        rel += cl;
    }
    ob[no++] = rel;                                   /* the n+1'th entry: end of the last string */
    if (ps_write(oaddr, ob, no * 4u)) return -1;
    if (nbb) {
        if ((uint64_t)baddr + nbb > g_ps_size) { snprintf(g_err, sizeof g_err, "PSRAM full while storing strings"); return -1; }
        if (ps_write(baddr, bb, nbb)) return -1;
    }
    g_ps_cur = *blob + rel;
    *count = (uint32_t)n;
    return 0;
}

/* ================================================================================================
 *  the tokenizer: shared/tokenizer.c with its maps and strings in PSRAM
 * ============================================================================================== */
static uint16_t byte_cp[256], cp_byte[512];

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ull; }
    return h;
}

/* String k of a table -> buf (NUL terminated). Returns its length or -1. */
static int ps_string(uint32_t off_tab, uint32_t blob, uint32_t k, char *buf, int max)
{
    uint32_t o[2];
    if (ps_read(off_tab + 4u * k, o, 8)) return -1;
    const uint32_t len = o[1] - o[0];
    if ((int)len >= max) { snprintf(g_err, sizeof g_err, "stored string %lu too long", (unsigned long)k); return -1; }
    if (ps_read(blob + o[0], buf, len)) return -1;
    buf[len] = 0;
    return (int)len;
}

/* map_get/map_put from tokenizer.c: linear probing from fnv1a & (cap-1), first writer wins. A slot holds
 * value+1 and the key is the string with that index, so a probe reads the slot and then the string. */
static int32_t map_get(uint32_t map, uint32_t cap, uint32_t off_tab, uint32_t blob, const char *k, int32_t dflt)
{
    static char cand[TL_KEYMAX];
    uint32_t i = (uint32_t)(fnv1a(k) & (cap - 1));
    for (;;) {
        uint32_t slot;
        if (ps_read(map + 4u * i, &slot, 4)) return dflt;
        if (!slot) return dflt;
        if (ps_string(off_tab, blob, slot - 1, cand, TL_KEYMAX) < 0) return dflt;
        if (!strcmp(cand, k)) return (int32_t)(slot - 1);
        i = (i + 1) & (cap - 1);
    }
}
static int map_put(uint32_t map, uint32_t cap, uint32_t off_tab, uint32_t blob, const char *k, uint32_t v)
{
    static char cand[TL_KEYMAX];
    uint32_t i = (uint32_t)(fnv1a(k) & (cap - 1));
    for (;;) {
        uint32_t slot;
        if (ps_read(map + 4u * i, &slot, 4)) return -1;
        if (!slot) { slot = v + 1; return ps_write(map + 4u * i, &slot, 4); }
        if (ps_string(off_tab, blob, slot - 1, cand, TL_KEYMAX) < 0) return -1;
        if (!strcmp(cand, k)) return 0;
        i = (i + 1) & (cap - 1);
    }
}

static int build_map(uint32_t n, uint32_t off_tab, uint32_t blob, uint32_t *map, uint32_t *capo)
{
    uint32_t cap = 16;
    while (cap < n * 2u) cap <<= 1;                  /* tokenizer.c's map_new */
    if (ps_alloc(cap * 4u, map)) return -1;
    if (ps_zero(*map, cap * 4u)) return -1;
    *capo = cap;
    /* Walk the strings in index order, in blocks, so building costs one sequential pass plus the probes. */
    static uint32_t ob[257];
    static char s[TL_KEYMAX];
    for (uint32_t j0 = 0; j0 < n; j0 += 256) {
        const uint32_t k = (n - j0) < 256 ? (n - j0) : 256;
        if (ps_read(off_tab + 4u * j0, ob, (k + 1) * 4u)) return -1;
        for (uint32_t j = 0; j < k; j++) {
            const uint32_t len = ob[j + 1] - ob[j];
            if (len >= TL_KEYMAX) { snprintf(g_err, sizeof g_err, "string %lu too long", (unsigned long)(j0 + j)); return -1; }
            if (ps_read(blob + ob[j], s, len)) return -1;
            s[len] = 0;
            if (map_put(*map, cap, off_tab, blob, s, j0 + j)) return -1;
        }
    }
    return 0;
}

static void build_byte_map(void)
{
    int used[256] = { 0 };
    for (int i = 0; i < 512; i++) cp_byte[i] = 0xFFFF;
    for (int b = '!'; b <= '~'; b++) used[b] = 1;
    for (int b = 0xA1; b <= 0xAC; b++) used[b] = 1;
    for (int b = 0xAE; b <= 0xFF; b++) used[b] = 1;
    for (int b = 0; b < 256; b++) if (used[b]) { byte_cp[b] = (uint16_t)b; cp_byte[b] = (uint16_t)b; }
    int n = 0;
    for (int b = 0; b < 256; b++) {
        if (!used[b]) {
            const uint16_t cp = (uint16_t)(256 + n);
            byte_cp[b] = cp;
            cp_byte[cp] = (uint16_t)b;
            n++;
        }
    }
}
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
    if ((c & 0xE0) == 0xC0) { *cp = (uint16_t)(((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F)); return 2; }
    if ((c & 0xF0) == 0xE0) {
        *cp = (uint16_t)(((c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) | ((unsigned char)s[2] & 0x3F));
        return 3;
    }
    *cp = 0xFFFF;
    return 1;
}

static int32_t vget(const char *k) { return map_get(M.vmap, M.vcap, M.p_off, M.p_blob, k, -1); }
static int32_t mget(const char *k) { return M.n_merges ? map_get(M.mmap_, M.mcap, M.m_off, M.m_blob, k, -1) : -1; }

int tl_decode(int32_t id, char *out, int max)
{
    static char piece[TL_KEYMAX];
    if (id < 0 || (uint32_t)id >= M.n_vocab) return 0;
    if (ps_string(M.p_off, M.p_blob, (uint32_t)id, piece, TL_KEYMAX) < 0) return 0;
    const char *p = piece;
    int n = 0;
    while (*p && n < max) {
        uint16_t cp;
        p += utf8_to_cp(p, &cp);
        const uint16_t b = (cp < 512) ? cp_byte[cp] : 0xFFFF;
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

static int is_letter(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80; }
static int is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int chunk_len(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!p[0]) return 0;
    if (p[0] == '\'') {
        static const char *c1[] = { "'s", "'t", "'re", "'ve", "'m", "'ll", "'d" };
        for (size_t i = 0; i < sizeof(c1) / sizeof(c1[0]); i++) {
            const size_t l = strlen(c1[i]);
            if (strncmp(s, c1[i], l) == 0) return (int)l;
        }
    }
    int i = 0;
    if (p[0] == ' ' && p[1] && !is_space(p[1])) i = 1;
    if (is_letter(p[i])) { while (is_letter(p[i])) i++; return i; }
    if (is_digit(p[i]))  { while (is_digit(p[i]))  i++; return i; }
    if (p[i] && !is_space(p[i])) {
        while (p[i] && !is_space(p[i]) && !is_letter(p[i]) && !is_digit(p[i])) i++;
        return i ? i : 1;
    }
    i = 0;
    while (is_space(p[i])) i++;
    if (i > 1 && p[i] && !is_space(p[i])) i--;
    return i ? i : 1;
}

/* BPE over one chunk: shared/tokenizer.c's bpe_chunk with the maps in PSRAM -- symbols are spans of one
 * buffer, every adjacent pair's rank is kept and only the two pairs a merge touches are looked up again.
 * See tokenizer.c for why (pieces up to 256 bytes; a 64-byte slot silently stopped the merging). */
static char     BB[2 * TL_MAX_CHUNK + 1];
static uint16_t BST[TL_MAX_CHUNK + 1];
static int32_t  BRK[TL_MAX_CHUNK + 1];
static char     BKEY[TL_KEYMAX];

static int32_t pair_rank(int i)
{
    const int la = BST[i + 1] - BST[i], lb = BST[i + 2] - BST[i + 1];
    if (la + lb + 2 > TL_KEYMAX) return -1;
    memcpy(BKEY, BB + BST[i], (size_t)la);
    BKEY[la] = ' ';
    memcpy(BKEY + la + 1, BB + BST[i + 1], (size_t)lb);
    BKEY[la + 1 + lb] = 0;
    return mget(BKEY);
}

static int bpe_chunk(const char *s, int len, int32_t *out, int max, int n_out)
{
    int nsym = 0, bl = 0;
    for (int i = 0; i < len && nsym < TL_MAX_CHUNK; i++) {
        BST[nsym++] = (uint16_t)bl;
        bl += cp_to_utf8(byte_cp[(unsigned char)s[i]], BB + bl);
    }
    BST[nsym] = (uint16_t)bl;
    for (int i = 0; i + 1 < nsym; i++) BRK[i] = pair_rank(i);
    for (;;) {
        int best = -1, best_rank = 0x7FFFFFFF;
        for (int i = 0; i + 1 < nsym; i++)
            if (BRK[i] >= 0 && BRK[i] < best_rank) { best_rank = BRK[i]; best = i; }
        if (best < 0) break;
        const int lm = BST[best + 2] - BST[best];
        if (lm >= TL_KEYMAX) break;
        memcpy(BKEY, BB + BST[best], (size_t)lm);
        BKEY[lm] = 0;
        if (vget(BKEY) < 0) break;
        for (int i = best + 1; i < nsym; i++) BST[i] = BST[i + 1];
        for (int i = best + 1; i + 1 < nsym - 1; i++) BRK[i] = BRK[i + 1];
        nsym--;
        if (best > 0) BRK[best - 1] = pair_rank(best - 1);
        if (best + 1 < nsym) BRK[best] = pair_rank(best);
    }
    for (int i = 0; i < nsym; i++) {
        const int l = BST[i + 1] - BST[i];
        int32_t id = -1;
        if (l < TL_KEYMAX) {
            memcpy(BKEY, BB + BST[i], (size_t)l);
            BKEY[l] = 0;
            id = vget(BKEY);
        }
        if (id < 0) continue;
        if (n_out >= max) return -1;
        out[n_out++] = id;
    }
    return n_out;
}

/* A special token written literally at s (tokenizer.c's special_at): the text up to the first '>' is a
 * token the file marks control or user-defined. Returns its id, or -1. */
static int32_t special_at(const char *s, int *len)
{
    if (s[0] != '<' || !M.n_spec) return -1;
    const char *e = strchr(s + 1, '>');
    if (!e || e - s + 1 >= 64) return -1;
    char buf[64];
    const int l = (int)(e - s + 1);
    memcpy(buf, s, (size_t)l);
    buf[l] = 0;
    const int32_t id = vget(buf);
    if (id < 0) return -1;
    for (int k = 0; k < M.n_spec; k++)
        if (M.spec[k] == id) { *len = l; return id; }
    return -1;
}

int tl_encode(const char *text, int32_t *out, int max)
{
    if (!M.open) return -1;
    int n = 0;
    const char *s = text;
    while (*s) {
        int l;
        const int32_t sid = special_at(s, &l);
        if (sid >= 0) {
            if (n >= max) return -1;
            out[n++] = sid;
            s += l;
            continue;
        }
        int frag = 0;
        while (s[frag] && !(s[frag] == '<' && special_at(s + frag, &l) >= 0)) frag++;
        while (frag > 0) {
            int len = M.pre_qwen2 ? pt_qwen2_len((const unsigned char *)s, frag) : chunk_len(s);
            if (len > frag) len = frag;
            if (len <= 0) return n;
            for (int o = 0; o < len; o += TL_MAX_CHUNK) {
                n = bpe_chunk(s + o, (len - o) < TL_MAX_CHUNK ? (len - o) : TL_MAX_CHUNK, out, max, n);
                if (n < 0) return -1;
            }
            s += len;
            frag -= len;
        }
    }
    return n;
}

/* ================================================================================================
 *  opening the model
 * ============================================================================================== */
static const struct { uint32_t blk, bytes; } TYPES[16] = {
    { 1, 4 }, { 1, 2 }, { 32, 18 }, { 32, 20 }, { 0, 0 }, { 0, 0 }, { 32, 22 }, { 32, 24 },
    { 32, 34 }, { 32, 36 }, { 256, 84 }, { 256, 110 }, { 256, 144 }, { 256, 176 }, { 256, 210 }, { 256, 292 }
};

static int set_tensor(tten_t *t, uint32_t type, const uint64_t *dims, uint32_t nd, uint64_t off)
{
    if (type >= 16 || !TYPES[type].blk) { snprintf(g_err, sizeof g_err, "tensor type %lu unsupported", (unsigned long)type); return -1; }
    t->type = type;
    t->cols = (uint32_t)dims[0];
    uint64_t rows = nd > 1 ? dims[1] : 1;
    if (nd > 2 && dims[2] > 1) rows *= dims[2];
    t->rows = (uint32_t)rows;
    if (t->cols % TYPES[type].blk) { snprintf(g_err, sizeof g_err, "%lu columns not a multiple of block", (unsigned long)t->cols); return -1; }
    t->row_bytes = (t->cols / TYPES[type].blk) * TYPES[type].bytes;
    t->off = off;                 /* relative for now; data_start added once it is known */
    t->present = 1;
    return 0;
}

static int assign_tensor(const char *name, uint32_t type, const uint64_t *dims, uint32_t nd, uint64_t off)
{
    if (!strcmp(name, "token_embd.weight"))  return set_tensor(&M.embd, type, dims, nd, off);
    if (!strcmp(name, "output_norm.weight")) return set_tensor(&M.onorm, type, dims, nd, off);
    if (!strcmp(name, "output.weight"))      return set_tensor(&M.out, type, dims, nd, off);
    if (strncmp(name, "blk.", 4)) return 0;
    const char *p = name + 4;
    int l = 0;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') l = l * 10 + (*p++ - '0');
    if (*p++ != '.') return 0;
    if (l >= TL_MAX_LAYERS) { snprintf(g_err, sizeof g_err, "layer %d over TL_MAX_LAYERS", l); return -1; }
    tlayer_t *L = &M.L[l];
    static const struct { const char *s; size_t o; } K[] = {
        { "attn_q.weight",      offsetof(tlayer_t, wq) },     { "attn_k.weight",   offsetof(tlayer_t, wk) },
        { "attn_v.weight",      offsetof(tlayer_t, wv) },     { "attn_output.weight", offsetof(tlayer_t, wo) },
        { "ffn_gate.weight",    offsetof(tlayer_t, wg) },     { "ffn_up.weight",   offsetof(tlayer_t, wu) },
        { "ffn_down.weight",    offsetof(tlayer_t, wd) },     { "attn_norm.weight", offsetof(tlayer_t, n_attn) },
        { "ffn_norm.weight",    offsetof(tlayer_t, n_ffn) },  { "attn_q.bias",     offsetof(tlayer_t, bq) },
        { "attn_k.bias",        offsetof(tlayer_t, bk) },     { "attn_v.bias",     offsetof(tlayer_t, bv) },
    };
    for (size_t k = 0; k < sizeof K / sizeof K[0]; k++)
        if (!strcmp(p, K[k].s)) return set_tensor((tten_t *)((char *)L + K[k].o), type, dims, nd, off);
    /* Anything else in a block means a model this runtime does not implement -- refuse rather than run
     * a different network: Qwen3's q/k norms, mixture-of-experts tensors, and so on. */
    snprintf(g_err, sizeof g_err, "unsupported tensor %s", name);
    return -1;
}

/* A small float tensor (a gain or bias) from the file into PSRAM, converted to float the way
 * gguf_read_f32 does. Bounded: dim floats at most. */
static int small_to_ps(const tten_t *t, uint32_t n, uint32_t *addr)
{
    if (!t->present) { *addr = 0; return 0; }
    if (t->rows * t->cols != n || n > TL_MAX_DIM) { snprintf(g_err, sizeof g_err, "small tensor of %lu elements, expected %lu", (unsigned long)(t->rows * t->cols), (unsigned long)n); return -1; }
    if (t->type != GGML_F32 && t->type != GGML_F16) { snprintf(g_err, sizeof g_err, "small tensor type %lu", (unsigned long)t->type); return -1; }
    const uint32_t nb = t->row_bytes * t->rows;
    if (nb > TL_ROWBUF) return -1;
    if (sd_read(t->off, ROW, nb)) return -1;
    if (gguf_dequant(t->type, ROW, n, G_A)) { snprintf(g_err, sizeof g_err, "dequant of small tensor"); return -1; }
    if (ps_alloc(n * 4u, addr)) return -1;
    return ps_write(*addr, G_A, n * 4u);
}

static int check_mat(const tten_t *t, uint32_t rows, uint32_t cols, const char *what, int l)
{
    if (!t->present) { snprintf(g_err, sizeof g_err, "layer %d has no %s", l, what); return -1; }
    if (t->rows != rows || t->cols != cols) {
        snprintf(g_err, sizeof g_err, "layer %d %s is %lux%lu, expected %lux%lu", l, what, (unsigned long)t->rows, (unsigned long)t->cols, (unsigned long)rows, (unsigned long)cols);
        return -1;
    }
    if (t->type != GGML_Q4_K && t->type != GGML_Q6_K && t->type != GGML_Q8_0) {
        snprintf(g_err, sizeof g_err, "layer %d %s type %lu has no fused kernel", l, what, (unsigned long)t->type);
        return -1;
    }
    if (t->row_bytes > TL_ROWBUF) { snprintf(g_err, sizeof g_err, "row of %lu bytes over TL_ROWBUF", (unsigned long)t->row_bytes); return -1; }
    return 0;
}

int tl_open(tl_info_t *info, char *err, int errlen)
{
    memset(&M, 0, sizeof M);
    memset(&R, 0, sizeof R);
    nsc = 0; g_arch[0] = 0; g_err[0] = 0;
    tl_stats_reset();
    g_fsize = plat_sd_size();
    g_ps_size = plat_ps_size();
    g_ps_cur = 0;
    build_byte_map();

#define FAIL() do { if (err && errlen > 0) snprintf(err, (size_t)errlen, "%s", g_err[0] ? g_err : "failed"); return -1; } while (0)

    char magic[4];
    uint32_t version;
    uint64_t n_tensors, n_kv;
    if (r_bytes(magic, 4) || memcmp(magic, "GGUF", 4)) { snprintf(g_err, sizeof g_err, "not a GGUF file"); FAIL(); }
    if (r_u32(&version) || r_u64(&n_tensors) || r_u64(&n_kv)) { snprintf(g_err, sizeof g_err, "truncated header"); FAIL(); }
    if (n_tensors > 100000u || n_kv > 100000u) { snprintf(g_err, sizeof g_err, "implausible counts"); FAIL(); }

    uint32_t tok_n = 0;
    for (uint64_t i = 0; i < n_kv; i++) {
        uint64_t kl;
        char key[128];
        uint32_t type;
        if (r_u64(&kl) || kl >= sizeof key) { snprintf(g_err, sizeof g_err, "bad key at entry %llu", (unsigned long long)i); FAIL(); }
        if (r_bytes(key, (uint32_t)kl)) FAIL();
        key[kl] = 0;
        if (r_u32(&type)) FAIL();
        if (type != GGUF_ARR) {
            int64_t iv = 0; double fv = 0; char sv[40] = { 0 };
            if (r_scalar(type, &iv, &fv, sv, sizeof sv)) { snprintf(g_err, sizeof g_err, "bad value for %s", key); FAIL(); }
            if (kl < 72 && nsc < NSCAL) {
                memcpy(SC[nsc].key, key, (size_t)kl + 1);          /* kl < 72 checked above */
                SC[nsc].i = iv; SC[nsc].f = fv;
                snprintf(SC[nsc].s, sizeof SC[nsc].s, "%s", sv);
                nsc++;
            }
            continue;
        }
        uint32_t at;
        uint64_t n;
        if (r_u32(&at) || r_u64(&n)) { snprintf(g_err, sizeof g_err, "bad array header for %s", key); FAIL(); }
        if (at == GGUF_STR && !strcmp(key, "tokenizer.ggml.tokens")) {
            if (stream_strings(n, &M.p_off, &M.p_blob, &tok_n)) FAIL();
            continue;
        }
        if (at == GGUF_STR && !strcmp(key, "tokenizer.ggml.merges")) {
            if (stream_strings(n, &M.m_off, &M.m_blob, &M.n_merges)) FAIL();
            continue;
        }
        if (at == GGUF_I32 && !strcmp(key, "tokenizer.ggml.token_type")) {
            /* only the special ones are kept: 22 of 151,936 in Qwen2.5 */
            M.n_spec = 0;
            for (uint64_t j = 0; j < n; j++) {
                int32_t t;
                if (r_bytes(&t, 4)) FAIL();
                if ((t == 3 || t == 4) && M.n_spec < TL_MAX_SPECIAL) M.spec[M.n_spec++] = (int32_t)j;
            }
            continue;
        }
        if (at == GGUF_STR) {
            for (uint64_t j = 0; j < n; j++) {
                uint64_t sl;
                if (r_u64(&sl) || r_skip(sl)) FAIL();
            }
            continue;
        }
        const size_t w = scalar_width(at);
        if (!w) { snprintf(g_err, sizeof g_err, "array of type %lu in %s", (unsigned long)at, key); FAIL(); }
        if (r_skip(n * w)) FAIL();
    }
    M.n_vocab = tok_n;
    if (!M.n_vocab) { snprintf(g_err, sizeof g_err, "no tokenizer.ggml.tokens"); FAIL(); }

    uint64_t nparams = 0;
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nl;
        char name[128];
        uint32_t nd, type;
        uint64_t dims[4] = { 1, 1, 1, 1 }, off;
        if (r_u64(&nl) || nl >= sizeof name) { snprintf(g_err, sizeof g_err, "bad tensor %llu", (unsigned long long)i); FAIL(); }
        if (r_bytes(name, (uint32_t)nl)) FAIL();
        name[nl] = 0;
        if (r_u32(&nd) || nd > 4) { snprintf(g_err, sizeof g_err, "bad tensor %s", name); FAIL(); }
        for (uint32_t d = 0; d < nd; d++) if (r_u64(&dims[d])) FAIL();
        if (r_u32(&type) || r_u64(&off)) { snprintf(g_err, sizeof g_err, "bad descriptor for %s", name); FAIL(); }
        nparams += dims[0] * dims[1] * dims[2] * dims[3];
        if (assign_tensor(name, type, dims, nd, off)) FAIL();
    }
    const int64_t align = sc_int("general.alignment", 32);
    const uint64_t here = r_tell();
    M.data_start = here;
    if (align > 0 && (here % (uint64_t)align)) M.data_start = here + (uint64_t)align - (here % (uint64_t)align);

    /* ---- hyperparameters, the way model_load_slice reads them ---- */
    {
        const char *a = sc_str("general.architecture");
        if (!a) { snprintf(g_err, sizeof g_err, "no general.architecture"); FAIL(); }
        snprintf(g_arch, sizeof g_arch, "%s", a);
        if (strcmp(g_arch, "qwen2")) { snprintf(g_err, sizeof g_err, "architecture %s: this runtime implements qwen2", g_arch); FAIL(); }
    }
    M.n_layer   = (int)arch_int("block_count", 0);
    M.dim       = (int)arch_int("embedding_length", 0);
    M.hidden    = (int)arch_int("feed_forward_length", 0);
    M.n_heads   = (int)arch_int("attention.head_count", 0);
    M.n_kv      = (int)arch_int("attention.head_count_kv", M.n_heads);
    M.eps       = (float)arch_flt("attention.layer_norm_rms_epsilon", 1e-6);
    M.rope_base = (float)arch_flt("rope.freq_base", 10000.0);
    if (!M.n_layer || !M.dim || !M.n_heads || !M.n_kv) { snprintf(g_err, sizeof g_err, "incomplete architecture metadata"); FAIL(); }
    {
        const int klen = (int)arch_int("attention.key_length", 0);
        M.head_dim = klen ? klen : M.dim / M.n_heads;
    }
    M.q_dim  = M.n_heads * M.head_dim;
    M.kv_dim = M.n_kv * M.head_dim;
    if (arch_int("expert_count", 0) > 0) { snprintf(g_err, sizeof g_err, "mixture-of-experts is not implemented here"); FAIL(); }
    if (M.n_layer > TL_MAX_LAYERS || M.dim > TL_MAX_DIM || M.q_dim > TL_MAX_DIM || M.hidden > TL_MAX_HIDDEN ||
        M.kv_dim > TL_MAX_KVDIM || M.n_kv > TL_MAX_KVH || M.n_heads > TL_MAX_HEADS || M.n_heads % M.n_kv) {
        snprintf(g_err, sizeof g_err, "model shape over this build's limits (layers %d dim %d hidden %d heads %d kv %d)",
                 M.n_layer, M.dim, M.hidden, M.n_heads, M.n_kv);
        FAIL();
    }
    if (!M.embd.present) { snprintf(g_err, sizeof g_err, "no token_embd.weight"); FAIL(); }
    M.vocab = (int)M.embd.rows;
    M.tied  = M.out.present ? 0 : 1;
    M.bos = (int32_t)sc_int("tokenizer.ggml.bos_token_id", -1);
    M.eos = (int32_t)sc_int("tokenizer.ggml.eos_token_id", -1);
    {
        const char *pre = sc_str("tokenizer.ggml.pre");
        M.pre_qwen2 = pre && !strcmp(pre, "qwen2");
    }

    /* absolute offsets */
    M.embd.off += M.data_start;
    if (M.out.present) M.out.off += M.data_start;
    if (!M.onorm.present) { snprintf(g_err, sizeof g_err, "no output_norm.weight"); FAIL(); }
    M.onorm.off += M.data_start;
    uint64_t per_tok = 0;
    for (int l = 0; l < M.n_layer; l++) {
        tlayer_t *L = &M.L[l];
        tten_t *all[] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->wg, &L->wu, &L->wd, &L->n_attn, &L->n_ffn, &L->bq, &L->bk, &L->bv };
        for (size_t k = 0; k < sizeof all / sizeof all[0]; k++) if (all[k]->present) all[k]->off += M.data_start;
        if (check_mat(&L->wq, (uint32_t)M.q_dim,  (uint32_t)M.dim,    "attn_q", l) ||
            check_mat(&L->wk, (uint32_t)M.kv_dim, (uint32_t)M.dim,    "attn_k", l) ||
            check_mat(&L->wv, (uint32_t)M.kv_dim, (uint32_t)M.dim,    "attn_v", l) ||
            check_mat(&L->wo, (uint32_t)M.dim,    (uint32_t)M.q_dim,  "attn_output", l) ||
            check_mat(&L->wg, (uint32_t)M.hidden, (uint32_t)M.dim,    "ffn_gate", l) ||
            check_mat(&L->wu, (uint32_t)M.hidden, (uint32_t)M.dim,    "ffn_up", l) ||
            check_mat(&L->wd, (uint32_t)M.dim,    (uint32_t)M.hidden, "ffn_down", l)) FAIL();
        if (!L->n_attn.present || !L->n_ffn.present) { snprintf(g_err, sizeof g_err, "layer %d has no norms", l); FAIL(); }
        const tten_t *mats[] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->wg, &L->wu, &L->wd };
        for (size_t k = 0; k < 7; k++) per_tok += (uint64_t)mats[k]->rows * mats[k]->row_bytes;
    }
    {
        const tten_t *head = M.tied ? &M.embd : &M.out;
        if (head->cols != (uint32_t)M.dim) { snprintf(g_err, sizeof g_err, "output head is %lu wide", (unsigned long)head->cols); FAIL(); }
        if (head->type != GGML_Q4_K && head->type != GGML_Q6_K && head->type != GGML_Q8_0) { snprintf(g_err, sizeof g_err, "head type %lu", (unsigned long)head->type); FAIL(); }
        per_tok += (uint64_t)head->rows * head->row_bytes + M.embd.row_bytes;
    }

    /* ---- PSRAM: tokenizer maps, then gains and biases, then the cache in whatever is left ---- */
    if (build_map(M.n_vocab, M.p_off, M.p_blob, &M.vmap, &M.vcap)) FAIL();
    if (M.n_merges && build_map(M.n_merges, M.m_off, M.m_blob, &M.mmap_, &M.mcap)) FAIL();
    const uint32_t ps_tok = g_ps_cur;

    for (int l = 0; l < M.n_layer; l++) {
        tlayer_t *L = &M.L[l];
        if (small_to_ps(&L->n_attn, (uint32_t)M.dim, &L->ps_attn) ||
            small_to_ps(&L->n_ffn,  (uint32_t)M.dim, &L->ps_ffn)  ||
            small_to_ps(&L->bq, (uint32_t)M.q_dim,  &L->ps_bq)    ||
            small_to_ps(&L->bk, (uint32_t)M.kv_dim, &L->ps_bk)    ||
            small_to_ps(&L->bv, (uint32_t)M.kv_dim, &L->ps_bv)) FAIL();
    }
    if (small_to_ps(&M.onorm, (uint32_t)M.dim, &M.ps_onorm)) FAIL();
    const uint32_t ps_small = g_ps_cur - ps_tok;

    {
        const uint64_t per_pos = (uint64_t)M.n_layer * (2u * (uint64_t)M.kv_dim + 2u * 4u * (uint64_t)M.n_kv);
        const uint64_t left = g_ps_size > g_ps_cur + 16u ? (uint64_t)(g_ps_size - g_ps_cur - 16u) : 0;
        uint64_t ms = left / per_pos;
        if (ms > TL_MAX_SEQ) ms = TL_MAX_SEQ;
        if (ms < 16) { snprintf(g_err, sizeof g_err, "PSRAM left for the cache holds %llu positions", (unsigned long long)ms); FAIL(); }
        M.max_seq = (int)ms;
        const uint32_t kb = (uint32_t)((uint64_t)M.n_layer * M.max_seq * M.kv_dim);
        const uint32_t sb = (uint32_t)((uint64_t)M.n_layer * M.max_seq * M.n_kv * 4u);
        if (ps_alloc(kb, &M.k_base) || ps_alloc(kb, &M.v_base) || ps_alloc(sb, &M.ks_base) || ps_alloc(sb, &M.vs_base)) FAIL();
    }
    M.ps_used = g_ps_cur;
    M.open = 1;

    if (info) {
        memset(info, 0, sizeof *info);
        info->n_layer = M.n_layer; info->dim = M.dim; info->hidden = M.hidden; info->n_heads = M.n_heads;
        info->n_kv = M.n_kv; info->head_dim = M.head_dim; info->q_dim = M.q_dim; info->vocab = M.vocab;
        info->max_seq = M.max_seq; info->bos = M.bos; info->eos = M.eos;
        info->file_bytes = g_fsize; info->sd_bytes_per_token = per_tok; info->ps_used = M.ps_used;
        info->n_merges = M.n_merges; info->ps_tokenizer = ps_tok; info->ps_small = ps_small;
        info->ps_kv = M.ps_used - ps_tok - ps_small; info->params = (double)nparams;
    }
    tl_stats_reset();
    return 0;
#undef FAIL
}

/* ================================================================================================
 *  the forward pass -- model_q.c's fast path
 * ============================================================================================== */
static void rmsnorm(float *out, const float *x, const float *g, int n, float eps)
{
    float ss = 0.0f;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    const float inv = 1.0f / sqrtf(ss / (float)n + eps);
    for (int i = 0; i < n; i++) out[i] = x[i] * inv * g[i];
}

static void softmax(float *v, int n)
{
    float mx = v[0];
    for (int i = 1; i < n; i++) if (v[i] > mx) mx = v[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) { v[i] = tlm_expf(v[i] - mx); sum += v[i]; }
    const float inv = 1.0f / sum;
    for (int i = 0; i < n; i++) v[i] *= inv;
}

static void rope(float *v, int n_heads, int head_dim, int pos, float base)
{
    const int half = head_dim / 2;
    for (int h = 0; h < n_heads; h++) {
        float *p = v + (size_t)h * head_dim;
        for (int i = 0; i < half; i++) {
            const float freq = tlm_rope_freq(base, i, head_dim);
            const float th = (float)pos * freq;
            float c, s;
            tlm_sincosf(th, &s, &c);
            const float x0 = p[i], x1 = p[i + half];
            p[i]        = x0 * c - x1 * s;
            p[i + half] = x0 * s + x1 * c;
        }
    }
}

static void kv_store(const float *src, int8_t *dst, float *scale, int n)
{
    const float lv = 127.0f;
    float amax = 0.0f;
    for (int i = 0; i < n; i++) {
        const float a = fabsf(src[i]);
        if (a > amax) amax = a;
    }
    const float s = amax / lv;
    *scale = s;
    const float inv = (s > 0.0f) ? 1.0f / s : 0.0f;
    for (int i = 0; i < n; i++) {
        float v = src[i] * inv;
        v = (v < 0.0f) ? v - 0.5f : v + 0.5f;
        int q = (int)v;
        if (q > 127) q = 127;
        if (q < -127) q = -127;
        dst[i] = (int8_t)q;
    }
}

/* Stream `nrows` rows of w, one ROW-sized block per storage read, dotting each against the quantized
 * activation already in XQ/XS(/XSUM). Writes out[], or -- when out is NULL -- keeps the two largest
 * results in (b1,i1),(b2,i2) exactly as tl_ref does over model_q's logits. */
static float g_b1, g_b2;
static int32_t g_i1, g_i2;

typedef struct { const tten_t *w; int nrows, r, per, presum; float *out; } it_rows1;

static void row_1(void *c, const uint8_t *p, int r)
{
    it_rows1 *it = (it_rows1 *)c;
    const float v = it->presum ? gguf_dot_q4k_presum(p, XQ, XS, XSUM, it->w->cols)
                               : gguf_dot_q(it->w->type, p, XQ, XS, it->w->cols);
    if (it->out) {
        it->out[r] = v;
    } else {
        if (v > g_b1) { g_b2 = g_b1; g_i2 = g_i1; g_b1 = v; g_i1 = r; }
        else if (v > g_b2) { g_b2 = v; g_i2 = r; }
    }
}

static int next_rows1(void *v, tl_job *j)
{
    it_rows1 *it = (it_rows1 *)v;
    if (it->r >= it->nrows) return 0;
    const int k = (it->nrows - it->r) < it->per ? (it->nrows - it->r) : it->per;
    j->off = it->w->off + (uint64_t)it->r * it->w->row_bytes; j->rb = it->w->row_bytes;
    j->r0 = it->r; j->k = k; j->row = row_1; j->done = 0; j->c = it;
    it->r += k;
    return 1;
}

static int stream_rows(const tten_t *w, int nrows, int presum, float *out)
{
    it_rows1 it = { w, nrows, 0, pipe_rows(w->row_bytes), presum, out };
    if (!it.per) { snprintf(g_err, sizeof g_err, "row wider than the row buffer"); return -1; }
    return pipeline(next_rows1, &it);
}

/* model_matvec_rows: quantize once for this matrix; Q4_K takes the presum kernel. */
static int matvec(const tten_t *w, const float *x, float *out, int nrows)
{
    gguf_quantize_act(x, w->cols, XQ, XS);
    if (w->type == GGML_Q4_K) {
        gguf_act_sums(XQ, w->cols, XSUM);
        return stream_rows(w, nrows, 1, out);
    }
    return stream_rows(w, nrows, 0, out);
}

/* matvec_group: one quantization of the shared input, gguf_dot_q on every row of all three. */
static int matvec_qkv(const tlayer_t *L, const float *x)
{
    gguf_quantize_act(x, L->wq.cols, XQ, XS);
    if (stream_rows(&L->wq, M.q_dim, 0, Q)) return -1;
    if (stream_rows(&L->wk, M.kv_dim, 0, KB)) return -1;
    return stream_rows(&L->wv, M.kv_dim, 0, VB);
}

static uint32_t kaddr(int l, int t)  { return M.k_base  + (uint32_t)(((uint64_t)l * M.max_seq + (uint64_t)t) * M.kv_dim); }
static uint32_t vaddr(int l, int t)  { return M.v_base  + (uint32_t)(((uint64_t)l * M.max_seq + (uint64_t)t) * M.kv_dim); }
static uint32_t ksaddr(int l, int t) { return M.ks_base + (uint32_t)(((uint64_t)l * M.max_seq + (uint64_t)t) * M.n_kv * 4u); }
static uint32_t vsaddr(int l, int t) { return M.vs_base + (uint32_t)(((uint64_t)l * M.max_seq + (uint64_t)t) * M.n_kv * 4u); }

/* This position's key and value into the cache: int8 with a scale per head (kv_store), then to PSRAM.
 * Shared by tl_forward and tl_prefill, so both write the cache the same way. */
static int kv_put(int l, int pos, const float *k, const float *v)
{
    const int hd = M.head_dim, nkv = M.n_kv;
    for (int h = 0; h < nkv; h++) {
        kv_store(k + (size_t)h * hd, KQ8 + (size_t)h * hd, &KSC[h], hd);
        kv_store(v + (size_t)h * hd, VQ8 + (size_t)h * hd, &VSC[h], hd);
    }
    if (ps_write(kaddr(l, pos), KQ8, (uint32_t)M.kv_dim) || ps_write(vaddr(l, pos), VQ8, (uint32_t)M.kv_dim) ||
        ps_write(ksaddr(l, pos), KSC, (uint32_t)nkv * 4u) || ps_write(vsaddr(l, pos), VSC, (uint32_t)nkv * 4u)) return -1;
    return 0;
}

/* Attention for one position over the cached positions 0..pos: scores for every head, a block of cached
 * positions at a time, then the value sum per head in ascending position order. out is q_dim floats.
 * The sequence's cache starts at row base (0 for a single sequence; tl_step gives each sequence its own).
 * Shared by tl_forward and tl_step. */
static int attention(int l, int base, int pos, const float *q, float *out)
{
    const int hd = M.head_dim, nkv = M.n_kv, nh = M.n_heads, kv_dim = M.kv_dim;
    const int group = nh / nkv;
    const float scale = 1.0f / sqrtf((float)hd);
    const int npos = pos + 1;
    for (int t0 = 0; t0 < npos; t0 += TL_KCHUNK) {
        const int n = (npos - t0) < TL_KCHUNK ? (npos - t0) : TL_KCHUNK;
        if (ps_read(kaddr(l, base + t0), KC, (uint32_t)(n * kv_dim)) || ps_read(ksaddr(l, base + t0), KCS, (uint32_t)(n * nkv) * 4u)) return -1;
        for (int h = 0; h < nh; h++) {
            const float *qh = q + (size_t)h * hd;
            const int kvh = h / group;
            float *att = ATT + (size_t)h * M.max_seq;
            for (int j = 0; j < n; j++) {
                const int8_t *kt = KC + (size_t)j * kv_dim + (size_t)kvh * hd;
                float s = 0.0f;
                for (int i = 0; i < hd; i++) s += qh[i] * (float)kt[i];
                att[t0 + j] = s * KCS[(size_t)j * nkv + kvh] * scale;
            }
        }
    }
    for (int h = 0; h < nh; h++) softmax(ATT + (size_t)h * M.max_seq, npos);
    for (int i = 0; i < M.q_dim; i++) out[i] = 0.0f;
    for (int t0 = 0; t0 < npos; t0 += TL_KCHUNK) {
        const int n = (npos - t0) < TL_KCHUNK ? (npos - t0) : TL_KCHUNK;
        if (ps_read(vaddr(l, base + t0), KC, (uint32_t)(n * kv_dim)) || ps_read(vsaddr(l, base + t0), KCS, (uint32_t)(n * nkv) * 4u)) return -1;
        for (int h = 0; h < nh; h++) {
            const int kvh = h / group;
            const float *att = ATT + (size_t)h * M.max_seq;
            float *oh = out + (size_t)h * hd;
            for (int j = 0; j < n; j++) {
                const int8_t *vt = KC + (size_t)j * kv_dim + (size_t)kvh * hd;
                const float a = att[t0 + j] * KCS[(size_t)j * nkv + kvh];
                for (int i = 0; i < hd; i++) oh[i] += a * (float)vt[i];
            }
        }
    }
    return 0;
}

int tl_matrix(int layer, int which, uint64_t *off, uint32_t *type, uint32_t *rows, uint32_t *cols,
              uint32_t *row_bytes)
{
    if (!M.open || which < 0 || which > 7) return -1;
    const tten_t *t;
    if (which == 7) t = &M.embd;
    else {
        if (layer < 0 || layer >= M.n_layer) return -1;
        const tlayer_t *L = &M.L[layer];
        const tten_t *all[7] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->wg, &L->wu, &L->wd };
        t = all[which];
    }
    if (!t->present) return -1;
    *off = t->off; *type = t->type; *rows = t->rows; *cols = t->cols; *row_bytes = t->row_bytes;
    return 0;
}

int tl_forward(int32_t token, int pos, int32_t *top1, float *l1, int32_t *top2, float *l2)
{
    if (!M.open) { snprintf(g_err, sizeof g_err, "model not open"); return -1; }
    if (pos < 0 || pos >= M.max_seq) { snprintf(g_err, sizeof g_err, "position %d outside 0..%d", pos, M.max_seq - 1); return -1; }
    if (token < 0 || token >= M.vocab) { snprintf(g_err, sizeof g_err, "token %d outside the vocabulary", (int)token); return -1; }
    const double t_start = plat_now();
    const int dim = M.dim, hd = M.head_dim, nkv = M.n_kv, nh = M.n_heads, kv_dim = M.kv_dim;

    double ts = t_start;                  /* stage stamps: timing only, the arithmetic is untouched */
#define STAGE(field) do { const double tn = plat_now(); S.field += tn - ts; ts = tn; } while (0)

    /* model_embed: one row of token_embd, dequantized */
    if (sd_read(M.embd.off + (uint64_t)token * M.embd.row_bytes, ROW, M.embd.row_bytes)) goto fail;
    if (gguf_dequant(M.embd.type, ROW, M.embd.cols, X)) { snprintf(g_err, sizeof g_err, "embedding type %lu", (unsigned long)M.embd.type); goto fail; }

    STAGE(t_embed);
    for (int l = 0; l < M.n_layer; l++) {
        const tlayer_t *L = &M.L[l];
        if (ps_read(L->ps_attn, G_A, (uint32_t)dim * 4u) || ps_read(L->ps_ffn, G_F, (uint32_t)dim * 4u)) goto fail;
        if (L->bq.present && ps_read(L->ps_bq, BQ, (uint32_t)M.q_dim * 4u)) goto fail;
        if (L->bk.present && ps_read(L->ps_bk, BK, (uint32_t)kv_dim * 4u)) goto fail;
        if (L->bv.present && ps_read(L->ps_bv, BV, (uint32_t)kv_dim * 4u)) goto fail;

        rmsnorm(XB, X, G_A, dim, M.eps);
        if (matvec_qkv(L, XB)) goto fail;
        STAGE(t_qkv);
        if (L->bq.present) for (int i = 0; i < M.q_dim; i++) Q[i]  += BQ[i];
        if (L->bk.present) for (int i = 0; i < kv_dim; i++)   KB[i] += BK[i];
        if (L->bv.present) for (int i = 0; i < kv_dim; i++)   VB[i] += BV[i];

        rope(Q,  nh,  hd, pos, M.rope_base);
        rope(KB, nkv, hd, pos, M.rope_base);

        if (kv_put(l, pos, KB, VB)) goto fail;

        if (attention(l, 0, pos, Q, XB)) goto fail;
        STAGE(t_att);
        if (matvec(&L->wo, XB, XB2, dim)) goto fail;
        for (int i = 0; i < dim; i++) X[i] += XB2[i];
        STAGE(t_wo);

        rmsnorm(XB, X, G_F, dim, M.eps);
        if (matvec(&L->wg, XB, HB,  M.hidden)) goto fail;
        if (matvec(&L->wu, XB, HB2, M.hidden)) goto fail;
        for (int i = 0; i < M.hidden; i++) {
            const float g = HB[i];
            HB[i] = (g / (1.0f + tlm_expf(-g))) * HB2[i];
        }
        if (matvec(&L->wd, HB, XB2, dim)) goto fail;
        for (int i = 0; i < dim; i++) X[i] += XB2[i];
        STAGE(t_ffn);
    }

    /* model_head: final norm, then every vocabulary row, keeping only the two best */
    if (ps_read(M.ps_onorm, G_A, (uint32_t)dim * 4u)) goto fail;
    rmsnorm(XB, X, G_A, dim, M.eps);
    g_b1 = -INFINITY; g_b2 = -INFINITY; g_i1 = 0; g_i2 = -1;
    if (matvec(M.tied ? &M.embd : &M.out, XB, NULL, M.vocab)) goto fail;
    if (top1) *top1 = g_i1;
    if (l1)   *l1   = g_b1;
    if (top2) *top2 = g_i2;
    if (l2)   *l2   = g_b2;
    STAGE(t_head);
#undef STAGE
    S.t_total += plat_now() - t_start;
    return 0;

fail:
    S.t_total += plat_now() - t_start;
    return -1;
}

/* ================================================================================================
 *  tl_prefill -- up to TL_BATCH prompt positions per pass over the weights
 *
 *  Per-token decoding reads all 1.83 GB of weights for every prompt token. Here each block of rows comes off
 *  the card once and is dotted with every position's input before the next block is read, so n prompt tokens
 *  cost one read of the weights plus n times the arithmetic.
 *
 *  IDENTICAL, NOT CLOSE. Every dot is the same kernel on the same bytes with the same per-position quantized
 *  input the per-token path builds; every float operation per position is the one tl_forward does, in the
 *  same order. The feed-forward output is quantized 32 at a time as each block of gate and up rows is
 *  finished -- gguf_quantize_act works in independent blocks of 32, so that is the same int8 and the same
 *  scale the down projection would have produced from the whole vector. tests/tl_host.c TL_PREFILL=1 checks
 *  the claim against model_q.c position by position.
 * ============================================================================================== */
#define NBD (TL_MAX_DIM / 32 + 1)
#if TL_BATCH > GGUF_NPOS_MAX
#error "TL_BATCH positions per pass exceeds what the batched kernels take (GGUF_NPOS_MAX)"
#endif
#define NBH (TL_MAX_HIDDEN / 32 + 1)

/* nrows rows of w, each read once, dotted with n positions' quantized inputs: out[p*os + r] = (or +=) dot */
typedef struct {
    const tten_t *w;
    int nrows, r, per, n, presum, acc;
    const int8_t *xqp[TL_BATCH];
    const float *xsp[TL_BATCH];
    const int32_t *xmp[TL_BATCH];
    float *out;
    size_t os;
    float *b1, *b2;                                       /* head: the two best per position */
    int32_t *i1, *i2;
} it_rowsn;

static void dots_n(it_rowsn *it, const uint8_t *row, float *v)
{
    /* every position against this row in one call: the M7 unpacks its weights once for all of them */
    if (it->presum) gguf_dot_q4k_presum_n(row, it->n, it->xqp, it->xsp, it->xmp, it->w->cols, v);
    else            gguf_dot_q_n(it->w->type, row, it->n, it->xqp, it->xsp, it->w->cols, v);
}

static void row_n(void *c, const uint8_t *p, int r)
{
    it_rowsn *it = (it_rowsn *)c;
    float v[TL_BATCH];
    dots_n(it, p, v);
    for (int q = 0; q < it->n; q++) {
        float *o = it->out + (size_t)q * it->os + (size_t)r;
        if (it->acc) *o += v[q]; else *o = v[q];
    }
}

static void row_head(void *c, const uint8_t *p, int r)
{
    it_rowsn *it = (it_rowsn *)c;
    float vv[TL_BATCH];
    dots_n(it, p, vv);
    for (int q = 0; q < it->n; q++) {
        const float v = vv[q];
        if (v > it->b1[q]) { it->b2[q] = it->b1[q]; it->i2[q] = it->i1[q]; it->b1[q] = v; it->i1[q] = r; }
        else if (v > it->b2[q]) { it->b2[q] = v; it->i2[q] = r; }
    }
}

static int next_rowsn(void *v, tl_job *j)
{
    it_rowsn *it = (it_rowsn *)v;
    if (it->r >= it->nrows) return 0;
    const int k = (it->nrows - it->r) < it->per ? (it->nrows - it->r) : it->per;
    j->off = it->w->off + (uint64_t)it->r * it->w->row_bytes; j->rb = it->w->row_bytes;
    j->r0 = it->r; j->k = k; j->row = it->b1 ? row_head : row_n; j->done = 0; j->c = it;
    it->r += k;
    return 1;
}

/* nrows rows of w, each read once, dotted with n positions' quantized inputs: out[p*os + r] = (or +=) dot */
static int stream_rows_n(const tten_t *w, int nrows, int n, const int8_t *xq, size_t xqs, const float *xs, size_t xss,
                         const int32_t *xsum, size_t sums, int presum, float *out, size_t os, int acc)
{
    it_rowsn it;
    memset(&it, 0, sizeof it);
    it.w = w; it.nrows = nrows; it.per = pipe_rows(w->row_bytes); it.n = n; it.presum = presum; it.acc = acc;
    it.out = out; it.os = os;
    if (!it.per) { snprintf(g_err, sizeof g_err, "row wider than the row buffer"); return -1; }
    for (int p = 0; p < n; p++) {
        it.xqp[p] = xq + (size_t)p * xqs;
        it.xsp[p] = xs + (size_t)p * xss;
        it.xmp[p] = xsum ? xsum + (size_t)p * sums : NULL;
    }
    return pipeline(next_rowsn, &it);
}

/* The output head for n positions: every vocabulary row once, the two best kept per position exactly as
 * stream_rows keeps them for one. */
static int head_n(const tten_t *w, int n, int presum, float *b1, int32_t *i1, float *b2, int32_t *i2)
{
    it_rowsn it;
    memset(&it, 0, sizeof it);
    it.w = w; it.nrows = (int)w->rows; it.per = pipe_rows(w->row_bytes); it.n = n; it.presum = presum;
    it.b1 = b1; it.b2 = b2; it.i1 = i1; it.i2 = i2;
    for (int p = 0; p < n; p++) {
        b1[p] = -INFINITY; b2[p] = -INFINITY; i1[p] = 0; i2[p] = -1;
        it.xqp[p] = PB.xq[p]; it.xsp[p] = PB.xs[p]; it.xmp[p] = PB.xsum[p];
    }
    return pipeline(next_rowsn, &it);
}

/* gate and up, 32 rows of each at a time for every position, then SwiGLU, and each 32 quantized straight into
 * PB.c.h. The 32 rows come in halves of 16 (a 32-row gate block of 18 KB x 2 does not fit a PIPE half), in the
 * same order as before: gate 0..15, gate 16..31, up 0..15, up 16..31, SwiGLU. */
typedef struct {
    const tlayer_t *L;
    int n, down_presum, gp, up, half, b0, part;           /* part 0..3: gate lo, gate hi, up lo, up hi */
    const int8_t *xqp[TL_BATCH];
    const float *xsp[TL_BATCH];
    const int32_t *xmp[TL_BATCH];
} it_ffn;

static void row_gate(void *c, const uint8_t *p, int r)
{
    it_ffn *it = (it_ffn *)c;
    float v[TL_BATCH];
    if (it->gp) gguf_dot_q4k_presum_n(p, it->n, it->xqp, it->xsp, it->xmp, it->L->wg.cols, v);
    else        gguf_dot_q_n(it->L->wg.type, p, it->n, it->xqp, it->xsp, it->L->wg.cols, v);
    for (int q = 0; q < it->n; q++) PB.g[q][r & 31] = v[q];
}

static void row_up(void *c, const uint8_t *p, int r)
{
    it_ffn *it = (it_ffn *)c;
    float v[TL_BATCH];
    if (it->up) gguf_dot_q4k_presum_n(p, it->n, it->xqp, it->xsp, it->xmp, it->L->wu.cols, v);
    else        gguf_dot_q_n(it->L->wu.type, p, it->n, it->xqp, it->xsp, it->L->wu.cols, v);
    for (int q = 0; q < it->n; q++) PB.u[q][r & 31] = v[q];
}

static void swiglu_block(void *c, int r0, int k)
{
    it_ffn *it = (it_ffn *)c;
    const int b0 = (r0 + k - 1) & ~31;
    for (int q = 0; q < it->n; q++) {
        float h[32];
        for (int j = 0; j < 32; j++) {
            const float g = PB.g[q][j];
            h[j] = (g / (1.0f + tlm_expf(-g))) * PB.u[q][j];
        }
        gguf_quantize_act(h, 32, PB.c.h.hq[q] + b0, &PB.c.h.hs[q][b0 / 32]);
        if (it->down_presum) gguf_act_sums(PB.c.h.hq[q] + b0, 32, &PB.c.h.hsum[q][b0 / 32]);
    }
}

static int next_ffn(void *v, tl_job *j)
{
    it_ffn *it = (it_ffn *)v;
    if (it->b0 >= M.hidden) return 0;
    const int isup = it->part >= 2;
    const tten_t *w = isup ? &it->L->wu : &it->L->wg;
    const int r0 = it->b0 + (it->part & 1) * it->half;
    j->off = w->off + (uint64_t)r0 * w->row_bytes; j->rb = w->row_bytes;
    j->r0 = r0; j->k = it->half; j->row = isup ? row_up : row_gate;
    j->done = (it->part == 3) ? swiglu_block : 0; j->c = it;
    if (++it->part == 4) { it->part = 0; it->b0 += 32; }
    return 1;
}

static int ffn_gate_up_n(const tlayer_t *L, int n, int down_presum)
{
    it_ffn it;
    memset(&it, 0, sizeof it);
    it.L = L; it.n = n; it.down_presum = down_presum;
    it.gp = L->wg.type == GGML_Q4_K; it.up = L->wu.type == GGML_Q4_K;
    it.half = 16;
    if ((uint32_t)it.half * L->wg.row_bytes > TL_PIPEBUF || (uint32_t)it.half * L->wu.row_bytes > TL_PIPEBUF ||
        (L->wg.row_bytes % 4) || (L->wu.row_bytes % 4)) {
        snprintf(g_err, sizeof g_err, "16 gate/up rows do not fit a row buffer half aligned");
        return -1;
    }
    for (int p = 0; p < n; p++) { it.xqp[p] = PB.xq[p]; it.xsp[p] = PB.xs[p]; it.xmp[p] = PB.xsum[p]; }
    return pipeline(next_ffn, &it);
}

int tl_step(const int32_t *tok, const int *pos, const int *base, int n,
            int32_t *top1, float *l1, int32_t *top2, float *l2)
{
    if (!M.open) { snprintf(g_err, sizeof g_err, "model not open"); return -1; }
    if (n < 1 || n > TL_BATCH) { snprintf(g_err, sizeof g_err, "pass of %d positions (1..%d)", n, TL_BATCH); return -1; }
    for (int p = 0; p < n; p++)
        if (pos[p] < 0 || base[p] < 0 || base[p] + pos[p] >= M.max_seq) {
            snprintf(g_err, sizeof g_err, "slot %d: position %d at cache row %d outside 0..%d", p, pos[p], base[p], M.max_seq - 1);
            return -1;
        }
    if (M.hidden % 32 || M.dim > TL_MAX_DIM || M.q_dim > TL_MAX_DIM) { snprintf(g_err, sizeof g_err, "shape not batchable"); return -1; }
    for (int p = 0; p < n; p++)
        if (tok[p] < 0 || tok[p] >= M.vocab) { snprintf(g_err, sizeof g_err, "token %d outside the vocabulary", (int)tok[p]); return -1; }
    const double t_start = plat_now();
    double ts = t_start;
#define STAGE(field) do { const double tn = plat_now(); S.field += tn - ts; ts = tn; } while (0)
    const int dim = M.dim, kv_dim = M.kv_dim, nh = M.n_heads, nkv = M.n_kv, hd = M.head_dim;

    for (int p = 0; p < n; p++) {
        if (sd_read(M.embd.off + (uint64_t)tok[p] * M.embd.row_bytes, ROW, M.embd.row_bytes)) goto fail;
        if (gguf_dequant(M.embd.type, ROW, M.embd.cols, PB.x[p])) { snprintf(g_err, sizeof g_err, "embedding type %lu", (unsigned long)M.embd.type); goto fail; }
    }
    STAGE(t_embed);

    for (int l = 0; l < M.n_layer; l++) {
        const tlayer_t *L = &M.L[l];
        if (ps_read(L->ps_attn, G_A, (uint32_t)dim * 4u) || ps_read(L->ps_ffn, G_F, (uint32_t)dim * 4u)) goto fail;
        if (L->bq.present && ps_read(L->ps_bq, BQ, (uint32_t)M.q_dim * 4u)) goto fail;
        if (L->bk.present && ps_read(L->ps_bk, BK, (uint32_t)kv_dim * 4u)) goto fail;
        if (L->bv.present && ps_read(L->ps_bv, BV, (uint32_t)kv_dim * 4u)) goto fail;

        /* q, k, v: one quantization of each position's normed input, gguf_dot_q on every row (matvec_group) */
        for (int p = 0; p < n; p++) {
            rmsnorm(XB, PB.x[p], G_A, dim, M.eps);
            gguf_quantize_act(XB, L->wq.cols, PB.xq[p], PB.xs[p]);
            gguf_act_sums(PB.xq[p], L->wq.cols, PB.xsum[p]);   /* presum == gguf_dot_q to the bit (dot_verify) */
        }
        if (stream_rows_n(&L->wq, M.q_dim, n, &PB.xq[0][0], TL_MAX_DIM, &PB.xs[0][0], NBD, &PB.xsum[0][0], NBD,
                          L->wq.type == GGML_Q4_K, &PB.c.q[0][0], TL_MAX_DIM, 0) ||
            stream_rows_n(&L->wk, kv_dim, n, &PB.xq[0][0], TL_MAX_DIM, &PB.xs[0][0], NBD, &PB.xsum[0][0], NBD,
                          L->wk.type == GGML_Q4_K, &PB.k[0][0], TL_MAX_KVDIM, 0) ||
            stream_rows_n(&L->wv, kv_dim, n, &PB.xq[0][0], TL_MAX_DIM, &PB.xs[0][0], NBD, &PB.xsum[0][0], NBD,
                          L->wv.type == GGML_Q4_K, &PB.v[0][0], TL_MAX_KVDIM, 0)) goto fail;
        STAGE(t_qkv);

        for (int p = 0; p < n; p++) {
            float *q = PB.c.q[p], *k = PB.k[p], *v = PB.v[p];
            if (L->bq.present) for (int i = 0; i < M.q_dim; i++) q[i] += BQ[i];
            if (L->bk.present) for (int i = 0; i < kv_dim; i++)   k[i] += BK[i];
            if (L->bv.present) for (int i = 0; i < kv_dim; i++)   v[i] += BV[i];
            rope(q, nh,  hd, pos[p], M.rope_base);
            rope(k, nkv, hd, pos[p], M.rope_base);
            if (kv_put(l, base[p] + pos[p], k, v)) goto fail;
        }
        /* every slot's key and value are in the cache before any slot attends, so a later position of the
         * same sequence in this pass sees the earlier ones, exactly as it would one pass later */
        const int wo_presum = L->wo.type == GGML_Q4_K;
        for (int p = 0; p < n; p++) {
            if (attention(l, base[p], pos[p], PB.c.q[p], XB)) goto fail;
            gguf_quantize_act(XB, L->wo.cols, PB.xq[p], PB.xs[p]);
            if (wo_presum) gguf_act_sums(PB.xq[p], L->wo.cols, PB.xsum[p]);
        }
        STAGE(t_att);
        if (stream_rows_n(&L->wo, dim, n, &PB.xq[0][0], TL_MAX_DIM, &PB.xs[0][0], NBD, &PB.xsum[0][0], NBD, wo_presum,
                          &PB.x[0][0], TL_MAX_DIM, 1)) goto fail;
        STAGE(t_wo);

        /* feed-forward: gate and up take the same quantized input, as matvec does for each of them */
        for (int p = 0; p < n; p++) {
            rmsnorm(XB, PB.x[p], G_F, dim, M.eps);
            gguf_quantize_act(XB, L->wg.cols, PB.xq[p], PB.xs[p]);
            gguf_act_sums(PB.xq[p], L->wg.cols, PB.xsum[p]);
        }
        const int down_presum = L->wd.type == GGML_Q4_K;
        if (ffn_gate_up_n(L, n, down_presum)) goto fail;
        if (stream_rows_n(&L->wd, dim, n, &PB.c.h.hq[0][0], TL_MAX_HIDDEN, &PB.c.h.hs[0][0], NBH, &PB.c.h.hsum[0][0], NBH,
                          down_presum, &PB.x[0][0], TL_MAX_DIM, 1)) goto fail;
        STAGE(t_ffn);
    }

    {
        const tten_t *head = M.tied ? &M.embd : &M.out;
        const int hp = head->type == GGML_Q4_K;
        if (ps_read(M.ps_onorm, G_A, (uint32_t)dim * 4u)) goto fail;
        for (int p = 0; p < n; p++) {
            rmsnorm(XB, PB.x[p], G_A, dim, M.eps);
            gguf_quantize_act(XB, head->cols, PB.xq[p], PB.xs[p]);
            if (hp) gguf_act_sums(PB.xq[p], head->cols, PB.xsum[p]);
        }
        float b1[TL_BATCH], b2[TL_BATCH];
        int32_t i1[TL_BATCH], i2[TL_BATCH];
        if (head_n(head, n, hp, b1, i1, b2, i2)) goto fail;
        for (int p = 0; p < n; p++) {
            if (top1) top1[p] = i1[p];
            if (l1)   l1[p]   = b1[p];
            if (top2) top2[p] = i2[p];
            if (l2)   l2[p]   = b2[p];
        }
    }
    STAGE(t_head);
#undef STAGE
    S.t_total += plat_now() - t_start;
    return 0;

fail:
    S.t_total += plat_now() - t_start;
    return -1;
}

int tl_prefill(const int32_t *tok, int n, int pos0, int32_t *top1, float *l1, int32_t *top2, float *l2)
{
    int pos[TL_BATCH], base[TL_BATCH];
    if (n < 1 || n > TL_BATCH) { snprintf(g_err, sizeof g_err, "prefill of %d positions (1..%d)", n, TL_BATCH); return -1; }
    for (int p = 0; p < n; p++) { pos[p] = pos0 + p; base[p] = 0; }
    return tl_step(tok, pos, base, n, top1, l1, top2, l2);
}

/* ================================================================================================
 *  tl_multi -- several prompts answered together, one pass over the weights feeding all of them
 *
 *  The card read is three quarters of a pass (docs/54) and does not grow with the number of positions a
 *  pass feeds; the arithmetic does. Answering k prompts one after another reads the weights once per token
 *  of each; answering them together reads them once per token of the longest. Every sequence keeps its own
 *  rows of the attention cache and its own positions, so each one's tokens and logits are the ones it gets
 *  alone (tests/tl_host.c TL_MULTI checks each against tl_ref.exe).
 *
 *  A pass gives every unfinished sequence one slot, then hands the slots left over to sequences still
 *  reading their prompt, one at a time in sequence order, until the pass is full.
 * ============================================================================================== */
#ifndef TL_MAX_SHARE
#define TL_MAX_SHARE   64         /* longest shared prompt opening copied instead of computed */
#endif
#define TL_MIN_SHARE   4          /* shorter openings are not worth a copy                    */
static int g_share = 1;
/* the top-2 each sequence printed at its first TL_MAX_SHARE positions, replayed for a sequence that copies
 * those positions from it -- they are exactly what that sequence would have computed there */
static struct { int32_t t1, t2; float l1, l2; } PREF[TL_BATCH][TL_MAX_SHARE] TL_BIG;

int tl_multi_share(int on) { const int was = g_share; g_share = on ? 1 : 0; return was; }

int tl_multi_begin(tl_seq_t *s, int ns)
{
    if (!M.open) { snprintf(g_err, sizeof g_err, "model not open"); return -1; }
    if (ns < 1 || ns > TL_BATCH) { snprintf(g_err, sizeof g_err, "%d sequences (1..%d)", ns, TL_BATCH); return -1; }
    int row = 0;
    for (int i = 0; i < ns; i++) {
        if (s[i].n_prompt < 1 || s[i].n_gen < 0) { snprintf(g_err, sizeof g_err, "sequence %d: empty", i); return -1; }
        s[i].base = row;
        s[i].fed = 0;
        s[i].done = 0;
        s[i].donor = -1;
        s[i].share = 0;
        row += s[i].n_prompt + s[i].n_gen;
        /* the earlier sequence with the longest common opening; the last prompt token is always computed
         * here, because its logits are this sequence's first answer token */
        for (int j = 0; g_share && j < i; j++) {
            int k = 0;
            const int lim = s[i].n_prompt - 1 < s[j].n_prompt ? s[i].n_prompt - 1 : s[j].n_prompt;
            while (k < lim && k < TL_MAX_SHARE && s[i].ids[k] == s[j].ids[k]) k++;
            if (k >= TL_MIN_SHARE && k > s[i].share) { s[i].share = k; s[i].donor = j; }
        }
    }
    if (row > M.max_seq) { snprintf(g_err, sizeof g_err, "the sequences need %d cache rows, there are %d", row, M.max_seq); return -1; }
    return 0;
}

/* rows [from, from+n) of every layer's cache to [to, to+n): keys, values and both scale arrays */
static int kv_copy(int from, int to, int n)
{
    const uint32_t kd = (uint32_t)M.kv_dim, sd = (uint32_t)M.n_kv * 4u;
    for (int l = 0; l < M.n_layer; l++)
        for (int t0 = 0; t0 < n; t0 += TL_KCHUNK) {
            const uint32_t c = (uint32_t)((n - t0) < TL_KCHUNK ? (n - t0) : TL_KCHUNK);
            if (ps_read(kaddr(l, from + t0), KC, c * kd) || ps_write(kaddr(l, to + t0), KC, c * kd) ||
                ps_read(vaddr(l, from + t0), KC, c * kd) || ps_write(vaddr(l, to + t0), KC, c * kd) ||
                ps_read(ksaddr(l, from + t0), KCS, c * sd) || ps_write(ksaddr(l, to + t0), KCS, c * sd) ||
                ps_read(vsaddr(l, from + t0), KCS, c * sd) || ps_write(vsaddr(l, to + t0), KCS, c * sd)) return -1;
        }
    return 0;
}

int tl_multi_pass(tl_seq_t *s, int ns, tl_multi_emit_fn emit, void *ctx)
{
    int32_t tok[TL_BATCH], o1[TL_BATCH], o2[TL_BATCH];
    float f1[TL_BATCH], f2[TL_BATCH];
    int pos[TL_BATCH], base[TL_BATCH], who[TL_BATCH], take[TL_BATCH];

    /* sequences whose shared opening has been computed by their donor copy it now */
    for (int i = 0; i < ns; i++) {
        tl_seq_t *q = &s[i];
        if (q->done || q->donor < 0 || q->fed || s[q->donor].fed < q->share) continue;
        if (kv_copy(s[q->donor].base, q->base, q->share)) return -1;
        for (int t = 0; t < q->share; t++) {
            PREF[i][t] = PREF[q->donor][t];
            if (emit) emit(ctx, i, t, q->ids[t], PREF[i][t].t1, PREF[i][t].l1, PREF[i][t].t2, PREF[i][t].l2);
        }
        q->fed = q->share;
    }

    int active = 0, waiting = 0;
    for (int i = 0; i < ns; i++) {
        take[i] = 0;
        if (s[i].done) continue;
        if (s[i].donor >= 0 && s[i].fed < s[i].share) { waiting++; continue; }   /* its donor is not there yet */
        take[i] = 1;
        active++;
    }
    if (!active) {
        if (waiting) { snprintf(g_err, sizeof g_err, "every unfinished sequence waits for a donor"); return -1; }
        return 0;
    }
    for (int left = TL_BATCH - active, more = 1; left > 0 && more; ) {
        more = 0;
        for (int i = 0; i < ns && left > 0; i++)
            if (take[i] && s[i].fed + take[i] < s[i].n_prompt) { take[i]++; left--; more = 1; }
    }
    int n = 0;
    for (int i = 0; i < ns; i++)
        for (int j = 0; j < take[i]; j++) {
            tok[n] = s[i].ids[s[i].fed + j]; pos[n] = s[i].fed + j; base[n] = s[i].base; who[n] = i; n++;
        }
    if (tl_step(tok, pos, base, n, o1, f1, o2, f2)) return -1;
    for (int p = 0; p < n; p++) {
        tl_seq_t *q = &s[who[p]];
        if (pos[p] < TL_MAX_SHARE) {
            PREF[who[p]][pos[p]].t1 = o1[p]; PREF[who[p]][pos[p]].l1 = f1[p];
            PREF[who[p]][pos[p]].t2 = o2[p]; PREF[who[p]][pos[p]].l2 = f2[p];
        }
        if (emit) emit(ctx, who[p], pos[p], tok[p], o1[p], f1[p], o2[p], f2[p]);
        q->fed = pos[p] + 1;
        if (pos[p] >= q->n_prompt - 1) {
            q->ids[q->fed] = o1[p];
            if (o1[p] == M.eos || q->fed >= q->n_prompt + q->n_gen) q->done = 1;
        }
    }
    return n;
}
