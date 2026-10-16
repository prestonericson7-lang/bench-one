/* ===========================================================================================
 *  tl_core.c -- see tl_core.h. A GGUF transformer with streamed weights and external memory.
 * ===========================================================================================
 *
 *  WHERE EVERYTHING LIVES (Qwen2.5-Coder-3B: dim 2048, hidden 11008, 16 heads, 2 kv heads, 36 layers)
 *
 *    storage (SD)   THE WHOLE MODEL. Every weight matrix and the embedding table, 1.93 GB, read once per
 *                   token in blocks of whole rows; the tied embedding is also the output head, read in
 *                   full; every norm gain and bias (~0.9 MB), read where each layer uses it. The tokenizer's
 *                   tables (151,936 pieces + 151,387 merges + two open-addressing maps, 8.3 MB) in a second
 *                   file beside the model, built from it once (see "the tokenizer's tables live on the card")
 *    PSRAM          the model's working memory and nothing else: the attention cache, int8 + one float
 *                   scale per head per position (19 KB/position, 3,072 positions in 58.4 MB)
 *    on-chip RAM    the buffers listed below
 *
 *  STATIC RAM, and why each is that size (defaults, this model):
 *    X XB XB2 Q         4 x 2048 floats        32 KB   activations, exactly model_q's x xb xb2 q
 *    KB VB              2 x 512 floats          4 KB   this position's key and value
 *    HB HB2             2 x 11008 floats       86 KB   feed-forward hidden (big, OCRAM on the Teensy)
 *    XQ XS XSUM         11008 + 2 x 345        14 KB   the int8 activation, its per-32 scales and sums
 *    ATT                16 heads x 3072 pos   192 KB   attention scores for every head (OCRAM)
 *    ROW                64 KB                  64 KB   one block of weight rows (OCRAM)
 *    KC/VC + scales     64 pos x 512 + 64 x 8  34 KB   one block of the cache pulled from PSRAM
 *    G_A G_F BQ BK BV   2048 x 3 + 512 x 2     28 KB   this layer's gains and biases, from the card
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
#define TL_MAX_HEADS   32         /* Qwen3-30B-A3B: 32 query heads (the 3B has 16) */
#endif
#ifndef TL_MAX_LAYERS
#define TL_MAX_LAYERS  48         /* Qwen3-30B-A3B: 48 (the 3B has 36) */
#endif
#ifndef TL_MAX_SEQ
#define TL_MAX_SEQ     3072       /* the PSRAM holds only the attention cache: 3,072 positions of this model
                                     are 58.4 MB, which 7 banks hold as well as 8 (ATT below grows with it) */
#endif
/* The attention scores of every head at one position: heads x positions floats. Sized for the 3B's 16 heads at
 * TL_MAX_SEQ; a model with more heads gets fewer positions (max_seq is clamped to TL_ATT_FLOATS / heads at open).
 * The 30B's cache fills the PSRAM at ~1,300 positions anyway, under its 1,536 here. */
#ifndef TL_ATT_FLOATS
#define TL_ATT_FLOATS  (16 * TL_MAX_SEQ)
#endif
#define TL_MAX_HD      256        /* head_dim: the per-head q/k norm gains */
/* MIXTURE OF EXPERTS (qwen3moe, 2026-10-16). The router's 128 logits per position, the 8 picks, an expert's
 * hidden width (768 for Qwen3-30B-A3B), and the positions a batched pass feeds a MoE model (its buffers share
 * the dense feed-forward's space in PB, which holds four positions' worth). */
#define TL_MAX_EXPERT  128
#define TL_MAX_EUSED   8
#define TL_MAX_EFF     1024
#define TL_MOE_BATCH   4
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
/* Code that runs once (opening the model) goes to flash on the Teensy, not ITCM: ITCM grows in 32 KB banks
 * carved out of the stack's RAM, and the 30B's code crossed an edge by 3 KB (stack -2 KB). noinline, or GCC
 * folds it back into an ITCM caller. */
#if defined(__IMXRT1062__)
  #define TL_COLD __attribute__((section(".flashmem"), noinline))
#else
  #define TL_COLD
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

/* ---- the PSRAM: independent chips, each holding whole layers (tl_plat.h) ------------------------------
 * ONE CHIP, ONE JOB. The chips are not one memory: each has its own timing, and moving the bus from one to
 * another is a chip reset with a 2 ms wait (the board's apply()). Until v8 the core saw one flat address
 * space and laid the cache across it as four long arrays, so attention on one layer touched K on one chip,
 * its scales on another, V on a third -- about eight re-selects a layer, ~290 a token, thousands at long
 * context, and a chip that failed took an arbitrary slice out of the middle of every array. Now a layer's
 * whole cache -- keys, values, scales and a checksum per row -- lives inside ONE chip, a layer's attention
 * never leaves it, and a chip that fails loses only whole layers, which move to spare room on the others
 * (tl_bank_fault) and are recomputed by re-running the prompt. A read is one raw transfer: the row checksum
 * (FNV-1a 32, written with the row) says whether it was right, and a row that was not is read again. */
#ifndef TL_MAX_BANKS
#define TL_MAX_BANKS   16
#endif
#ifndef TL_SPARE_MIN
#define TL_SPARE_MIN   1024       /* keep a spare bank's worth of layer slots when the cache still holds this many positions */
#endif
static int      g_nbank;
static uint32_t g_bank_bytes[TL_MAX_BANKS];
static int      g_bank_dead[TL_MAX_BANKS];
static int      g_fault_bank = -1;             /* the bank of the last PSRAM failure (tl_fault_bank) */
#define SCR 0                                  /* the tokenizer's build scratch: bank 0, erased after */
static uint32_t g_ps_size;                     /* bytes of that scratch bank */

static int ps_read(int b, uint32_t off, void *dst, uint32_t n)
{
    if (!n) return 0;
    if (b < 0 || b >= g_nbank || (uint64_t)off + n > g_bank_bytes[b]) {
        snprintf(g_err, sizeof g_err, "PSRAM read outside bank %d: %lu+%lu", b, (unsigned long)off, (unsigned long)n);
        return -1;
    }
    const double t = plat_now();
    const int r = plat_ps_read(b, off, dst, n);
    S.t_ps += plat_now() - t;
    S.ps_read += n;
    if (r) { snprintf(g_err, sizeof g_err, "PSRAM bank %d read failed at %lu (+%lu)", b, (unsigned long)off, (unsigned long)n); g_fault_bank = b; }
    return r;
}
static int ps_write(int b, uint32_t off, const void *src, uint32_t n)
{
    if (!n) return 0;
    if (b < 0 || b >= g_nbank || (uint64_t)off + n > g_bank_bytes[b]) {
        snprintf(g_err, sizeof g_err, "PSRAM write outside bank %d: %lu+%lu", b, (unsigned long)off, (unsigned long)n);
        return -1;
    }
    const double t = plat_now();
    const int r = plat_ps_write(b, off, src, n);
    S.t_ps += plat_now() - t;
    S.ps_written += n;
    if (r) { snprintf(g_err, sizeof g_err, "PSRAM bank %d write failed at %lu (+%lu)", b, (unsigned long)off, (unsigned long)n); g_fault_bank = b; }
    return r;
}
int tl_fault_bank(void) { return g_fault_bank; }

/* ---- the model --------------------------------------------------------------------------- */
typedef struct {
    uint64_t off;             /* ABSOLUTE file offset of row 0 */
    uint32_t rows, cols, row_bytes;
    uint16_t type;
    uint8_t  present;         /* 24 bytes: 48 layers x 18 of these is the model table */
} tten_t;

typedef struct {
    tten_t   wq, wk, wv, wo, wg, wu, wd;
    tten_t   n_attn, n_ffn, bq, bk, bv;          /* small float tensors, read from the card where used */
    tten_t   qn, kn;                             /* Qwen3: per-head q and k RMSNorm gains (head_dim) */
    tten_t   rt, eg, eu, ed;                     /* mixture of experts: the router (F32, experts x dim) and the
                                                    three expert stacks, expert e's rows at e x eff (gate, up)
                                                    and e x dim (down) */
} tlayer_t;

static struct {
    int      n_layer, dim, hidden, n_heads, n_kv, head_dim, q_dim, kv_dim, vocab, max_seq;
    int      n_expert, n_used, eff;              /* 0 0 0 for a dense model */
    int      qm;                                 /* PB rows a q_dim vector takes: 1, or 2 when q_dim > dim limit */
    int      batch;                              /* positions a pass may feed (tl_batch) */
    float    eps, rope_base;
    tlayer_t L[TL_MAX_LAYERS];
    tten_t   embd, onorm, out;
    int      tied;
    uint64_t data_start;
    /* tokenizer: its tables in the card store (tl_plat.h), addresses relative to the store's table area */
    uint32_t n_vocab, n_merges;
    uint32_t p_off, p_blob, m_off, m_blob;       /* offset tables have n+1 entries */
    uint32_t vmap, vcap, mmap_, mcap;            /* slot = id+1 / rank+1, 0 = empty */
    int32_t  bos, eos;
    int      pre_qwen2;                          /* tokenizer.ggml.pre is "qwen2"     */
    int32_t  spec[TL_MAX_SPECIAL];               /* token_type 3 (control) or 4 (user-defined) */
    int      n_spec;
    /* attention cache in PSRAM: one layer per region, one region inside one bank (PL below); a region is
     * K rows, then key scales, V rows, value scales, then a checksum per K row and per V row */
    uint32_t r_ks, r_v, r_vs, r_sk, r_sv, layer_bytes;
    int      layer_slots, spare_slots, one_bank_spare, banks_used;
    uint32_t ps_used;
    int      open;
} M TL_BIG;                    /* OCRAM on the Teensy: 21 KB at 48 layers, and not touched in an inner loop */
/* whether M holds an open model. NOT M.open: the Teensy's startup zeroes only DTCM (.bss), never OCRAM
 * (.bss.dma, where TL_BIG puts M), so before the first tl_open M is whatever the RAM powered up holding */
static int g_mopen;

/* ---- on-chip buffers ---------------------------------------------------------------------- */
static float   X[TL_MAX_DIM], XB[TL_MAX_DIM], XB2[TL_MAX_DIM], Q[TL_MAX_DIM];
static float   KB[TL_MAX_KVDIM], VB[TL_MAX_KVDIM];
#ifndef TL_BATCH
#define TL_BATCH       8          /* prompt positions per pass over the weights in tl_prefill */
#endif
#ifndef TL_FFN_GMAX
#define TL_FFN_GMAX    256        /* largest group of gate/up rows a batched pass computes before SwiGLU */
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
    float   g[TL_BATCH][TL_FFN_GMAX];   /* one group of gate rows, then SwiGLU(gate, up) in place (tl_set_ffn_group) */
    union {
        float q[TL_BATCH][TL_MAX_DIM];
        struct {
            int8_t  hq[TL_BATCH][TL_MAX_HIDDEN];
            float   hs[TL_BATCH][TL_MAX_HIDDEN / 32 + 1];
            int32_t hsum[TL_BATCH][TL_MAX_HIDDEN / 32 + 1];
        } h;
        /* a mixture-of-experts layer's feed-forward, for up to TL_MOE_BATCH positions (moe_ffn_n): the normed
         * input in float (the router is a float matrix), the weighted sum of the experts' outputs, one expert's
         * hidden (gate, then SwiGLU in place) and that hidden quantized for its down projection, and the router's
         * scores. 87 KB inside the dense layout's 110: q is dead after attention, h unused by a MoE model. */
        struct {
            float   xin[TL_MOE_BATCH][TL_MAX_DIM];
            float   acc[TL_MOE_BATCH][TL_MAX_DIM];
            float   h[TL_MOE_BATCH][TL_MAX_EFF];
            int8_t  hq[TL_MOE_BATCH][TL_MAX_EFF];
            float   hs[TL_MOE_BATCH][TL_MAX_EFF / 32 + 1];
            int32_t hsum[TL_MOE_BATCH][TL_MAX_EFF / 32 + 1];
            float   rl[TL_MOE_BATCH][TL_MAX_EXPERT];
        } m;
    } c;
} tl_batch_t;
typedef struct { float hb[TL_MAX_HIDDEN], hb2[TL_MAX_HIDDEN]; } tl_tok_t;
static union { tl_batch_t b; tl_tok_t t; } AR TL_BIG;
#define HB  (AR.t.hb)
#define HB2 (AR.t.hb2)
#define PB  (AR.b)

void *tl_scratch(uint32_t *bytes) { if (bytes) *bytes = (uint32_t)sizeof AR; return &AR; }
/* one of the two row buffers a pass works rows from (DTCM on the Teensy, not cached); a board's kernel bench
 * borrows it between passes, so it times rows where the model keeps them */
void *tl_rowbuf(uint32_t *bytes) { if (bytes) *bytes = TL_PIPEBUF; return PIPE[0]; }
int   tl_batch(void) { return g_mopen ? M.batch : TL_BATCH; }
static int8_t  XQ[TL_MAX_HIDDEN];
static float   XS[TL_MAX_HIDDEN / 32 + 1];
static int32_t XSUM[TL_MAX_HIDDEN / 32 + 1];
static float   ATT[TL_ATT_FLOATS] TL_BIG;

static int8_t  KC[TL_KCHUNK * TL_MAX_KVDIM];
static float   KCS[TL_KCHUNK * TL_MAX_KVH];
static uint32_t SUMC[TL_KCHUNK];              /* the chunk's row checksums, read beside the rows */
typedef struct { int bank; uint32_t base; } tl_place_t;
static tl_place_t PL[TL_MAX_LAYERS];          /* where each layer's cache region is */
static int      g_bank_slots[TL_MAX_BANKS], g_bank_used[TL_MAX_BANKS];
static int8_t  KQ8[TL_MAX_KVDIM], VQ8[TL_MAX_KVDIM];
static float   KSC[TL_MAX_KVH], VSC[TL_MAX_KVH];
static float   G_A[TL_MAX_DIM], G_F[TL_MAX_DIM], BQ[TL_MAX_DIM], BK[TL_MAX_KVDIM], BV[TL_MAX_KVDIM];
static float   G_QN[TL_MAX_HD], G_KN[TL_MAX_HD];  /* Qwen3's per-head q/k norm gains, this layer's */

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

/* ---- scratch allocation in bank SCR, for building the tokenizer's tables once ------------------------ */
static uint32_t g_ps_cur;
static int ps_alloc(uint32_t n, uint32_t *addr)
{
    g_ps_cur = (g_ps_cur + 3u) & ~3u;
    if ((uint64_t)g_ps_cur + n > g_ps_size) {
        snprintf(g_err, sizeof g_err, "the tokenizer's tables need %lu more bytes at %lu; bank %d has %lu (one bank must hold them while they are built)",
                 (unsigned long)n, (unsigned long)g_ps_cur, SCR, (unsigned long)g_ps_size);
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
        if (ps_write(SCR, a, ROW, k)) return -1;
        a += k; n -= k;
    }
    return 0;
}

/* ---- the tokenizer's tables live on the card ----------------------------------------------------------
 * THE MODEL STAYS ON THE CARD; THE PSRAM IS THE MODEL'S WORKING MEMORY. The tokenizer's tables (offset
 * tables, string blobs and two hash maps: 8.3 MB for Qwen2.5) are built once from the model file -- in PSRAM
 * as scratch, because a hash map is built by random writes -- then copied to the card store (tl_plat.h;
 * "qwen3b.tok" on the Teensy), read back and checked there, and the PSRAM copy erased. A later open that
 * finds the store matching the model skips the build and never puts any of it in PSRAM. Every lookup then
 * reads the card: a prompt's encoding is some hundreds of small reads, a generated token's text two. */
#define TOK_HDR   512u                   /* the store: a header sector, then the tables */
#define TOK_MAGIC "TLTOK01"
typedef struct {
    char     magic[8];
    uint64_t file_bytes;                 /* the model file the tables were built from */
    uint64_t tok_at, merge_at;           /* where its two string arrays start in that file */
    uint32_t n_vocab, n_merges;
    uint32_t p_off, p_blob, m_off, m_blob, vmap, vcap, mmap_, mcap;
    uint32_t bytes;                      /* table bytes after the header */
    uint32_t pad;
    uint64_t sum;                        /* FNV-1a 64 of those bytes */
} tok_hdr_t;
static int g_tok_card;                   /* 0 while the tables are being built in PSRAM, 1 once on the card */
static int tok_rd(uint32_t a, void *dst, uint32_t n)
{
    if (!g_tok_card) return ps_read(SCR, a, dst, n);
    if (!n) return 0;
    const int r = plat_tok_read(TOK_HDR + a, dst, n);
    if (r) snprintf(g_err, sizeof g_err, "tokenizer store read failed at %lu (+%lu)", (unsigned long)a, (unsigned long)n);
    return r;
}
static uint64_t fnv_add(uint64_t h, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
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
        if (no == 256) { if (ps_write(SCR, oaddr, ob, 1024)) return -1; oaddr += 1024; no = 0; }
        for (uint32_t k = 0; k < cl; ) {
            uint32_t c = cl - k;
            if (c > sizeof bb - nbb) c = (uint32_t)sizeof bb - nbb;
            memcpy(bb + nbb, s + k, c); nbb += c; k += c;
            if (nbb == sizeof bb) {
                if ((uint64_t)baddr + nbb > g_ps_size) { snprintf(g_err, sizeof g_err, "PSRAM full while storing strings"); return -1; }
                if (ps_write(SCR, baddr, bb, nbb)) return -1;
                baddr += nbb; nbb = 0;
            }
        }
        rel += cl;
    }
    ob[no++] = rel;                                   /* the n+1'th entry: end of the last string */
    if (ps_write(SCR, oaddr, ob, no * 4u)) return -1;
    if (nbb) {
        if ((uint64_t)baddr + nbb > g_ps_size) { snprintf(g_err, sizeof g_err, "PSRAM full while storing strings"); return -1; }
        if (ps_write(SCR, baddr, bb, nbb)) return -1;
    }
    g_ps_cur = *blob + rel;
    *count = (uint32_t)n;
    return 0;
}

/* ================================================================================================
 *  the tokenizer: shared/tokenizer.c with its maps and strings in the card store
 * ============================================================================================== */
static uint16_t byte_cp[256], cp_byte[512];

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ull; }
    return h;
}

/* String k of a table -> buf (NUL terminated). Returns its length or -1. From PSRAM while building, the card after. */
static int ps_string(uint32_t off_tab, uint32_t blob, uint32_t k, char *buf, int max)
{
    uint32_t o[2];
    if (tok_rd(off_tab + 4u * k, o, 8)) return -1;
    const uint32_t len = o[1] - o[0];
    if ((int)len >= max) { snprintf(g_err, sizeof g_err, "stored string %lu too long", (unsigned long)k); return -1; }
    if (tok_rd(blob + o[0], buf, len)) return -1;
    buf[len] = 0;
    return (int)len;
}

/* map_get/map_put from tokenizer.c: linear probing from fnv1a & (cap-1), first writer wins. A slot holds
 * value+1 and the key is the string with that index, so a probe reads the slot and then the string.
 * map_put builds (PSRAM); map_get looks up (the card, once the tables are there). */
static int32_t map_get(uint32_t map, uint32_t cap, uint32_t off_tab, uint32_t blob, const char *k, int32_t dflt)
{
    static char cand[TL_KEYMAX];
    uint32_t i = (uint32_t)(fnv1a(k) & (cap - 1));
    for (;;) {
        uint32_t slot;
        if (tok_rd(map + 4u * i, &slot, 4)) return dflt;
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
        if (ps_read(SCR, map + 4u * i, &slot, 4)) return -1;
        if (!slot) { slot = v + 1; return ps_write(SCR, map + 4u * i, &slot, 4); }
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
        if (ps_read(SCR, off_tab + 4u * j0, ob, (k + 1) * 4u)) return -1;
        for (uint32_t j = 0; j < k; j++) {
            const uint32_t len = ob[j + 1] - ob[j];
            if (len >= TL_KEYMAX) { snprintf(g_err, sizeof g_err, "string %lu too long", (unsigned long)(j0 + j)); return -1; }
            if (ps_read(SCR, blob + ob[j], s, len)) return -1;
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

/* BPE over one chunk: shared/tokenizer.c's bpe_chunk with the maps in the card store -- symbols are spans of one
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
    if (!g_mopen) return -1;
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
        { "attn_q_norm.weight", offsetof(tlayer_t, qn) },     { "attn_k_norm.weight", offsetof(tlayer_t, kn) },
        { "ffn_gate_inp.weight", offsetof(tlayer_t, rt) },    { "ffn_gate_exps.weight", offsetof(tlayer_t, eg) },
        { "ffn_up_exps.weight", offsetof(tlayer_t, eu) },     { "ffn_down_exps.weight", offsetof(tlayer_t, ed) },
    };
    for (size_t k = 0; k < sizeof K / sizeof K[0]; k++)
        if (!strcmp(p, K[k].s)) return set_tensor((tten_t *)((char *)L + K[k].o), type, dims, nd, off);
    /* Anything else in a block means a model this runtime does not implement -- refuse rather than run
     * a different network (a shared expert, a sliding-window mask, and so on). */
    snprintf(g_err, sizeof g_err, "unsupported tensor %s", name);
    return -1;
}

/* A small float tensor (a gain or bias): checked once at open, then read from the model file on the card
 * each time a layer uses it and converted to float the way gguf_read_f32 does -- none of the model is kept
 * in PSRAM. 36 layers x 26 KB is 0.94 MB a pass, 0.05% of the 1.83 GB the weights already cost. ROW is free
 * wherever this is called: every pipeline has finished its last block by then. */
static int small_check(const tten_t *t, uint32_t n)
{
    if (!t->present) return 0;
    if (t->rows * t->cols != n || n > TL_MAX_DIM) { snprintf(g_err, sizeof g_err, "small tensor of %lu elements, expected %lu", (unsigned long)(t->rows * t->cols), (unsigned long)n); return -1; }
    if (t->type != GGML_F32 && t->type != GGML_F16) { snprintf(g_err, sizeof g_err, "small tensor type %lu", (unsigned long)t->type); return -1; }
    if (t->row_bytes * t->rows > TL_ROWBUF) { snprintf(g_err, sizeof g_err, "small tensor over TL_ROWBUF"); return -1; }
    return 0;
}
static int small_read(const tten_t *t, uint32_t n, float *dst)
{
    if (sd_read(t->off, ROW, t->row_bytes * t->rows)) return -1;
    if (gguf_dequant(t->type, ROW, n, dst)) { snprintf(g_err, sizeof g_err, "dequant of small tensor"); return -1; }
    return 0;
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

/* The card store's tables, summed the way they were when written (FNV-1a 64 over the table bytes). */
static int tok_store_sum(uint32_t bytes, uint64_t *sum)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t a = 0; a < bytes; a += TL_ROWBUF) {
        const uint32_t k = bytes - a < TL_ROWBUF ? bytes - a : TL_ROWBUF;
        if (plat_tok_read(TOK_HDR + a, ROW, k)) { snprintf(g_err, sizeof g_err, "tokenizer store read failed at %lu", (unsigned long)a); return -1; }
        h = fnv_add(h, ROW, k);
    }
    *sum = h;
    return 0;
}

#define TL_REBUILD (-2)
TL_COLD static int tl_open_try(tl_info_t *info, char *err, int errlen, int force_build)
{
    g_mopen = 0;
    memset(&M, 0, sizeof M);
    memset(&R, 0, sizeof R);
    nsc = 0; g_arch[0] = 0; g_err[0] = 0;
    tl_stats_reset();
    g_fsize = plat_sd_size();
    g_tok_card = 0;
    build_byte_map();

#define FAIL() do { if (err && errlen > 0) snprintf(err, (size_t)errlen, "%s", g_err[0] ? g_err : "failed"); return -1; } while (0)

    g_nbank = plat_ps_banks();
    if (g_nbank > TL_MAX_BANKS) g_nbank = TL_MAX_BANKS;
    if (g_nbank < 1) { snprintf(g_err, sizeof g_err, "no PSRAM bank qualified"); FAIL(); }
    for (int b = 0; b < g_nbank; b++) { g_bank_bytes[b] = plat_ps_bank_bytes(b); g_bank_dead[b] = 0; }
    g_ps_size = g_bank_bytes[SCR];
    g_ps_cur = 0;
    g_fault_bank = -1;

    /* The tokenizer store on the card: used as it is only if it was built from this very file and every
     * table byte still sums to what was written. Anything else is rebuilt from the model. */
    static tok_hdr_t H;
    memset(&H, 0, sizeof H);
    int reuse = 0;
    if (!force_build && !plat_tok_open(0) && !plat_tok_read(0, &H, sizeof H) && !memcmp(H.magic, TOK_MAGIC, 8) &&
        H.file_bytes == g_fsize && H.bytes > 0) {
        uint64_t sum;
        if (!tok_store_sum(H.bytes, &sum) && sum == H.sum) reuse = 1;
    }
    g_err[0] = 0;
    uint64_t tok_at = 0, merge_at = 0;

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
        if (at == GGUF_STR && (!strcmp(key, "tokenizer.ggml.tokens") || !strcmp(key, "tokenizer.ggml.merges"))) {
            const int is_tok = !strcmp(key, "tokenizer.ggml.tokens");
            const uint64_t here = r_tell();
            if (reuse) {                                        /* the store has them: pass over the strings */
                if (here != (is_tok ? H.tok_at : H.merge_at) || n != (is_tok ? H.n_vocab : H.n_merges)) return TL_REBUILD;
                for (uint64_t j = 0; j < n; j++) {
                    uint64_t sl;
                    if (r_u64(&sl) || r_skip(sl)) FAIL();
                }
                if (is_tok) tok_n = (uint32_t)n; else M.n_merges = (uint32_t)n;
            } else if (is_tok) {
                tok_at = here;
                if (stream_strings(n, &M.p_off, &M.p_blob, &tok_n)) FAIL();
            } else {
                merge_at = here;
                if (stream_strings(n, &M.m_off, &M.m_blob, &M.n_merges)) FAIL();
            }
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
        if (strcmp(g_arch, "qwen2") && strcmp(g_arch, "qwen3moe")) {
            snprintf(g_err, sizeof g_err, "architecture %s: this runtime implements qwen2 and qwen3moe", g_arch);
            FAIL();
        }
    }
    const int moe = !strcmp(g_arch, "qwen3moe");
    M.n_layer   = (int)arch_int("block_count", 0);
    M.dim       = (int)arch_int("embedding_length", 0);
    M.hidden    = moe ? 0 : (int)arch_int("feed_forward_length", 0);
    M.n_expert  = moe ? (int)arch_int("expert_count", 0) : 0;
    M.n_used    = moe ? (int)arch_int("expert_used_count", 0) : 0;
    M.eff       = moe ? (int)arch_int("expert_feed_forward_length", 0) : 0;
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
    if (!moe && arch_int("expert_count", 0) > 0) { snprintf(g_err, sizeof g_err, "%s with experts", g_arch); FAIL(); }
    if (moe && (M.n_expert < 1 || M.n_expert > TL_MAX_EXPERT || M.n_used < 1 || M.n_used > TL_MAX_EUSED ||
                M.n_used > M.n_expert || M.eff < 32 || M.eff > TL_MAX_EFF || M.eff % 32)) {
        snprintf(g_err, sizeof g_err, "experts over this build's limits (%d experts, %d used, width %d)", M.n_expert, M.n_used, M.eff);
        FAIL();
    }
    /* A query wider than TL_MAX_DIM (the 30B: 32 heads x 128 = 4096 over a 2048 residual) takes two PB rows a
     * position, so a batched pass feeds half the positions; the PB arrays keep their size. */
    M.qm = (M.q_dim + TL_MAX_DIM - 1) / TL_MAX_DIM;
    M.batch = TL_BATCH / (M.qm > 0 ? M.qm : 1);
    if (M.n_expert && M.batch > TL_MOE_BATCH) M.batch = TL_MOE_BATCH;
    if (M.n_layer > TL_MAX_LAYERS || M.dim > TL_MAX_DIM || M.qm < 1 || M.qm > 2 || M.hidden > TL_MAX_HIDDEN ||
        M.kv_dim > TL_MAX_KVDIM || M.n_kv > TL_MAX_KVH || M.n_heads > TL_MAX_HEADS || M.n_heads % M.n_kv ||
        M.head_dim > TL_MAX_HD || (M.qm > 1 && M.q_dim > TL_BATCH * TL_MAX_KVDIM) || M.batch < 1) {
        /* (the last-but-one: tl_step puts a wide query's attention output in PB.k, which is free by then) */
        snprintf(g_err, sizeof g_err, "model shape over this build's limits (layers %d dim %d q %d hidden %d heads %d kv %d)",
                 M.n_layer, M.dim, M.q_dim, M.hidden, M.n_heads, M.n_kv);
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
        tten_t *all[] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->wg, &L->wu, &L->wd, &L->n_attn, &L->n_ffn, &L->bq, &L->bk, &L->bv,
                          &L->qn, &L->kn, &L->rt, &L->eg, &L->eu, &L->ed };
        for (size_t k = 0; k < sizeof all / sizeof all[0]; k++) if (all[k]->present) all[k]->off += M.data_start;
        if (check_mat(&L->wq, (uint32_t)M.q_dim,  (uint32_t)M.dim,    "attn_q", l) ||
            check_mat(&L->wk, (uint32_t)M.kv_dim, (uint32_t)M.dim,    "attn_k", l) ||
            check_mat(&L->wv, (uint32_t)M.kv_dim, (uint32_t)M.dim,    "attn_v", l) ||
            check_mat(&L->wo, (uint32_t)M.dim,    (uint32_t)M.q_dim,  "attn_output", l)) FAIL();
        if (!L->n_attn.present || !L->n_ffn.present) { snprintf(g_err, sizeof g_err, "layer %d has no norms", l); FAIL(); }
        const tten_t *mats[] = { &L->wq, &L->wk, &L->wv, &L->wo };
        for (size_t k = 0; k < 4; k++) per_tok += (uint64_t)mats[k]->rows * mats[k]->row_bytes;
        if (moe) {
            if (L->wg.present || L->wu.present || L->wd.present) { snprintf(g_err, sizeof g_err, "layer %d: dense and expert feed-forward both", l); FAIL(); }
            if (!L->qn.present || !L->kn.present) { snprintf(g_err, sizeof g_err, "layer %d has no q/k norms", l); FAIL(); }
            const uint32_t ne = (uint32_t)M.n_expert, ef = (uint32_t)M.eff, d = (uint32_t)M.dim;
            if (!L->rt.present || L->rt.type != GGML_F32 || L->rt.rows != ne || L->rt.cols != d || L->rt.row_bytes > TL_ROWBUF) {
                snprintf(g_err, sizeof g_err, "layer %d: the router is not an F32 %lux%lu matrix", l, (unsigned long)ne, (unsigned long)d);
                FAIL();
            }
            if (check_mat(&L->eg, ne * ef, d,  "ffn_gate_exps", l) || check_mat(&L->eu, ne * ef, d, "ffn_up_exps", l) ||
                check_mat(&L->ed, ne * d,  ef, "ffn_down_exps", l)) FAIL();
            if ((L->eg.row_bytes % 4) || (L->eu.row_bytes % 4)) { snprintf(g_err, sizeof g_err, "layer %d: expert rows not 4-aligned", l); FAIL(); }
            /* only the experts that fire come off the card -- the whole reason a 30B is worth running -- and the
             * router in full */
            per_tok += (uint64_t)M.n_used * ef * (L->eg.row_bytes + L->eu.row_bytes) + (uint64_t)M.n_used * d * L->ed.row_bytes;
            per_tok += (uint64_t)L->rt.rows * L->rt.row_bytes;
        } else {
            if (L->rt.present || L->eg.present) { snprintf(g_err, sizeof g_err, "layer %d: experts in a dense model", l); FAIL(); }
            if (check_mat(&L->wg, (uint32_t)M.hidden, (uint32_t)M.dim,    "ffn_gate", l) ||
                check_mat(&L->wu, (uint32_t)M.hidden, (uint32_t)M.dim,    "ffn_up", l) ||
                check_mat(&L->wd, (uint32_t)M.dim,    (uint32_t)M.hidden, "ffn_down", l)) FAIL();
            per_tok += (uint64_t)L->wg.rows * L->wg.row_bytes + (uint64_t)L->wu.rows * L->wu.row_bytes +
                       (uint64_t)L->wd.rows * L->wd.row_bytes;
        }
    }
    {
        const tten_t *head = M.tied ? &M.embd : &M.out;
        if (head->cols != (uint32_t)M.dim) { snprintf(g_err, sizeof g_err, "output head is %lu wide", (unsigned long)head->cols); FAIL(); }
        if (head->type != GGML_Q4_K && head->type != GGML_Q6_K && head->type != GGML_Q8_0) { snprintf(g_err, sizeof g_err, "head type %lu", (unsigned long)head->type); FAIL(); }
        per_tok += (uint64_t)head->rows * head->row_bytes + M.embd.row_bytes;
    }

    /* ---- the tokenizer's tables: from the card store, or built now and put there ---- */
    uint32_t tok_bytes;
    if (reuse) {
        M.p_off = H.p_off; M.p_blob = H.p_blob; M.m_off = H.m_off; M.m_blob = H.m_blob;
        M.vmap = H.vmap; M.vcap = H.vcap; M.mmap_ = H.mmap_; M.mcap = H.mcap;
        tok_bytes = H.bytes;
    } else {
        /* built in PSRAM as scratch -- a hash map is random writes -- then copied to the card, read back
         * from the card and checked, and erased from PSRAM */
        if (build_map(M.n_vocab, M.p_off, M.p_blob, &M.vmap, &M.vcap)) FAIL();
        if (M.n_merges && build_map(M.n_merges, M.m_off, M.m_blob, &M.mmap_, &M.mcap)) FAIL();
        tok_bytes = g_ps_cur;
        if (plat_tok_open(TOK_HDR + tok_bytes)) { snprintf(g_err, sizeof g_err, "could not create the tokenizer store on the card (%lu bytes)", (unsigned long)(TOK_HDR + tok_bytes)); FAIL(); }
        uint64_t h = 1469598103934665603ull;
        for (uint32_t a = 0; a < tok_bytes; a += TL_ROWBUF) {
            const uint32_t k = tok_bytes - a < TL_ROWBUF ? tok_bytes - a : TL_ROWBUF;
            if (ps_read(SCR, a, ROW, k)) FAIL();
            h = fnv_add(h, ROW, k);
            if (plat_tok_write(TOK_HDR + a, ROW, k)) { snprintf(g_err, sizeof g_err, "tokenizer store write failed at %lu", (unsigned long)a); FAIL(); }
        }
        static uint8_t hs[TOK_HDR];
        memset(&H, 0, sizeof H);
        memcpy(H.magic, TOK_MAGIC, 8);
        H.file_bytes = g_fsize; H.tok_at = tok_at; H.merge_at = merge_at;
        H.n_vocab = tok_n; H.n_merges = M.n_merges;
        H.p_off = M.p_off; H.p_blob = M.p_blob; H.m_off = M.m_off; H.m_blob = M.m_blob;
        H.vmap = M.vmap; H.vcap = M.vcap; H.mmap_ = M.mmap_; H.mcap = M.mcap;
        H.bytes = tok_bytes; H.sum = h;
        memset(hs, 0, sizeof hs);
        memcpy(hs, &H, sizeof H);
        if (plat_tok_write(0, hs, TOK_HDR) || plat_tok_flush()) { snprintf(g_err, sizeof g_err, "tokenizer store header write failed"); FAIL(); }
        uint64_t back;
        if (tok_store_sum(tok_bytes, &back)) FAIL();
        if (back != h) { snprintf(g_err, sizeof g_err, "the tokenizer store did not read back from the card as written"); FAIL(); }
        if (ps_zero(0, tok_bytes)) FAIL();
        g_ps_cur = 0;
    }
    g_tok_card = 1;

    /* ---- norms and biases: they stay in the model file; checked here, read where each layer uses them ---- */
    for (int l = 0; l < M.n_layer; l++) {
        tlayer_t *L = &M.L[l];
        if (small_check(&L->n_attn, (uint32_t)M.dim) || small_check(&L->n_ffn, (uint32_t)M.dim) ||
            small_check(&L->bq, (uint32_t)M.q_dim) || small_check(&L->bk, (uint32_t)M.kv_dim) ||
            small_check(&L->bv, (uint32_t)M.kv_dim) || small_check(&L->qn, (uint32_t)M.head_dim) ||
            small_check(&L->kn, (uint32_t)M.head_dim)) FAIL();
        const tten_t *sm[] = { &L->n_attn, &L->n_ffn, &L->bq, &L->bk, &L->bv, &L->qn, &L->kn };
        for (size_t k = 0; k < 7; k++) if (sm[k]->present) per_tok += (uint64_t)sm[k]->rows * sm[k]->row_bytes;
    }
    if (small_check(&M.onorm, (uint32_t)M.dim)) FAIL();
    per_tok += (uint64_t)M.onorm.rows * M.onorm.row_bytes;

    /* ---- PSRAM: the attention cache, one layer per region, one region per bank, and nothing else ----
     * A layer's region at S positions is S x (2 kv_dim + 2 n_kv x 4 + 8) bytes; a bank holds floor(bytes / region)
     * whole layers. S is the largest count for which the banks hold every layer AND a spare bank's worth of
     * layer slots (so a bank that fails mid-run can be retired), as long as that leaves TL_SPARE_MIN positions;
     * otherwise the largest S for which they hold every layer. Nothing ever straddles two banks. */
    {
        const uint64_t per_pos = 2u * (uint64_t)M.kv_dim + 2u * 4u * (uint64_t)M.n_kv + 8u;
        int s_red = 0, s_max = 0;
        int s_lim = TL_ATT_FLOATS / M.n_heads;                        /* every head's scores at one position */
        if (s_lim > TL_MAX_SEQ) s_lim = TL_MAX_SEQ;
        for (int lo = 16, hi = s_lim; lo <= hi; ) {                   /* slots fall as S grows: binary search */
            const int mid = (lo + hi) / 2;
            int total = 0, biggest = 0;
            for (int b = 0; b < g_nbank; b++) {
                const int k = (int)(g_bank_bytes[b] / (per_pos * (uint64_t)mid));
                total += k;
                if (k > biggest) biggest = k;
            }
            if (total >= M.n_layer) { s_max = mid; if (total >= M.n_layer + biggest) s_red = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (s_max < 16) { snprintf(g_err, sizeof g_err, "%d PSRAM banks do not hold %d layers of cache at 16 positions", g_nbank, M.n_layer); FAIL(); }
        /* s_red found by the search above is the largest S with the spare condition ONLY IF the condition is monotone
         * in S, which it is not quite (biggest falls in steps); re-scan downward from s_max for the true largest */
        s_red = 0;
        for (int S2 = s_max; S2 >= TL_SPARE_MIN && !s_red; S2--) {
            int total = 0, biggest = 0;
            for (int b = 0; b < g_nbank; b++) {
                const int k = (int)(g_bank_bytes[b] / (per_pos * (uint64_t)S2));
                total += k;
                if (k > biggest) biggest = k;
            }
            if (total >= M.n_layer + biggest) s_red = S2;
        }
        M.max_seq = s_red ? s_red : s_max;
        M.layer_bytes = (uint32_t)(per_pos * (uint64_t)M.max_seq);
        const uint32_t S = (uint32_t)M.max_seq, kd = (uint32_t)M.kv_dim, sd = (uint32_t)M.n_kv * 4u;
        M.r_ks = S * kd; M.r_v = M.r_ks + S * sd; M.r_vs = M.r_v + S * kd; M.r_sk = M.r_vs + S * sd; M.r_sv = M.r_sk + S * 4u;
        int l = 0;
        M.layer_slots = 0; M.banks_used = 0;
        for (int b = 0; b < g_nbank; b++) {
            g_bank_slots[b] = (int)(g_bank_bytes[b] / M.layer_bytes);
            g_bank_used[b] = 0;
            M.layer_slots += g_bank_slots[b];
            while (l < M.n_layer && g_bank_used[b] < g_bank_slots[b]) {
                PL[l].bank = b; PL[l].base = (uint32_t)g_bank_used[b] * M.layer_bytes;
                g_bank_used[b]++; l++;
            }
            if (g_bank_used[b]) M.banks_used++;
        }
        M.spare_slots = M.layer_slots - M.n_layer;
        M.one_bank_spare = 1;
        for (int b = 0; b < g_nbank; b++)                             /* every bank's layers fit in the others' free slots */
            if (g_bank_used[b] && M.spare_slots - (g_bank_slots[b] - g_bank_used[b]) < g_bank_used[b]) M.one_bank_spare = 0;
        M.ps_used = (uint32_t)M.n_layer * M.layer_bytes;
    }
    M.open = 1;
    g_mopen = 1;

    if (info) {
        memset(info, 0, sizeof *info);
        info->n_layer = M.n_layer; info->dim = M.dim; info->hidden = M.hidden; info->n_heads = M.n_heads;
        info->n_kv = M.n_kv; info->head_dim = M.head_dim; info->q_dim = M.q_dim; info->vocab = M.vocab;
        info->max_seq = M.max_seq; info->bos = M.bos; info->eos = M.eos;
        info->file_bytes = g_fsize; info->sd_bytes_per_token = per_tok; info->ps_used = M.ps_used;
        info->n_merges = M.n_merges; info->params = (double)nparams;
        info->tok_store_bytes = TOK_HDR + tok_bytes; info->tok_built = !reuse;
        info->banks = g_nbank; info->banks_used = M.banks_used; info->layer_slots = M.layer_slots;
        info->spare_slots = M.spare_slots; info->one_bank_spare = M.one_bank_spare; info->layer_bytes = M.layer_bytes;
        info->n_expert = M.n_expert; info->n_used = M.n_used; info->eff = M.eff; info->batch = M.batch;
    }
    tl_stats_reset();
    return 0;
#undef FAIL
}

int tl_open(tl_info_t *info, char *err, int errlen)
{
    const int r = tl_open_try(info, err, errlen, 0);
    return r == TL_REBUILD ? tl_open_try(info, err, errlen, 1) : r;   /* the store no longer matches */
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

/* model_q.c's head_norm, verbatim: Qwen3's RMSNorm WITHIN each head of q or k, before the rotation */
static void head_norm(float *v, int heads, int hd, const float *gain, float eps)
{
    for (int h = 0; h < heads; h++) {
        float *p = v + (size_t)h * hd;
        float ss = 0.0f;
        for (int i = 0; i < hd; i++) ss += p[i] * p[i];
        const float inv = 1.0f / sqrtf(ss / (float)hd + eps);
        for (int i = 0; i < hd; i++) p[i] = p[i] * inv * gain[i];
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

/* Offsets inside a layer's region (tl_plat.h: the whole region is inside one bank). which: 0 keys, 1 values. */
static uint32_t o_row(int which, int t) { return (which ? M.r_v : 0u) + (uint32_t)t * (uint32_t)M.kv_dim; }
static uint32_t o_sc (int which, int t) { return (which ? M.r_vs : M.r_ks) + (uint32_t)t * (uint32_t)M.n_kv * 4u; }
static uint32_t o_sum(int which, int t) { return (which ? M.r_sv : M.r_sk) + (uint32_t)t * 4u; }

/* FNV-1a 32 over a row and its scales: written with the row, checked on every read of it */
static uint32_t row_sum(const void *row, uint32_t nrow, const void *sc, uint32_t nsc)
{
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)row;
    for (uint32_t i = 0; i < nrow; i++) { h ^= p[i]; h *= 16777619u; }
    p = (const uint8_t *)sc;
    for (uint32_t i = 0; i < nsc; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* This position's key and value into the cache: int8 with a scale per head (kv_store), then to the layer's
 * bank with a checksum per row. Shared by tl_forward and tl_prefill, so both write the cache the same way. */
static int kv_put(int l, int pos, const float *k, const float *v)
{
    const int hd = M.head_dim, nkv = M.n_kv;
    const uint32_t kd = (uint32_t)M.kv_dim, sd = (uint32_t)nkv * 4u;
    for (int h = 0; h < nkv; h++) {
        kv_store(k + (size_t)h * hd, KQ8 + (size_t)h * hd, &KSC[h], hd);
        kv_store(v + (size_t)h * hd, VQ8 + (size_t)h * hd, &VSC[h], hd);
    }
    const uint32_t sk = row_sum(KQ8, kd, KSC, sd), sv = row_sum(VQ8, kd, VSC, sd);
    const int b = PL[l].bank;
    const uint32_t a = PL[l].base;
    if (ps_write(b, a + o_row(0, pos), KQ8, kd) || ps_write(b, a + o_sc(0, pos), KSC, sd) || ps_write(b, a + o_sum(0, pos), &sk, 4) ||
        ps_write(b, a + o_row(1, pos), VQ8, kd) || ps_write(b, a + o_sc(1, pos), VSC, sd) || ps_write(b, a + o_sum(1, pos), &sv, 4)) return -1;
    return 0;
}

/* n cached rows of layer l from position t0 (keys or values, with their scales) into KC and KCS, every row
 * checked against its checksum. A row that fails is read again, up to three times; one that never reads
 * right is the bank's failure (tl_fault_bank). All in the layer's one bank. */
static int cache_read(int l, int which, int t0, int n)
{
    const int b = PL[l].bank;
    const uint32_t a = PL[l].base, kd = (uint32_t)M.kv_dim, sd = (uint32_t)M.n_kv * 4u;
    if (ps_read(b, a + o_row(which, t0), KC, (uint32_t)n * kd) || ps_read(b, a + o_sc(which, t0), KCS, (uint32_t)n * sd) ||
        ps_read(b, a + o_sum(which, t0), SUMC, (uint32_t)n * 4u)) return -1;
    for (int j = 0; j < n; j++) {
        int tries = 0;
        while (row_sum(KC + (size_t)j * kd, kd, (const uint8_t *)KCS + (size_t)j * sd, sd) != SUMC[j]) {
            if (++tries > 3) {
                S.ps_rows_bad++;
                g_fault_bank = b;
                snprintf(g_err, sizeof g_err, "PSRAM bank %d: row %d of layer %d never read back right", b, t0 + j, l);
                return -1;
            }
            if (ps_read(b, a + o_row(which, t0 + j), KC + (size_t)j * kd, kd) ||
                ps_read(b, a + o_sc(which, t0 + j), (uint8_t *)KCS + (size_t)j * sd, sd) ||
                ps_read(b, a + o_sum(which, t0 + j), &SUMC[j], 4)) return -1;
        }
        if (tries) {
            S.ps_rows_reread++;
            if (S.ps_rows_reread <= 8)                             /* the first few, so the log names the chip */
                plat_log("    cache row re-read: bank %d, layer %d, position %d (%s), right after %d more read(s)",
                         b, l, t0 + j, which ? "values" : "keys", tries);
        }
    }
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
        if (cache_read(l, 0, base + t0, n)) return -1;
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
        if (cache_read(l, 1, base + t0, n)) return -1;
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

int tl_layer_bank(int l) { return (g_mopen && l >= 0 && l < M.n_layer) ? PL[l].bank : -1; }

/* A bank has failed: retire it and move each of its layers to a free slot on a live bank. What those layers
 * held is gone, so the caller re-runs the prompt; every position is then recomputed, exactly. */
int tl_bank_fault(int bank)
{
    if (!g_mopen || bank < 0 || bank >= g_nbank) { snprintf(g_err, sizeof g_err, "no such bank %d", bank); return -1; }
    if (g_bank_dead[bank]) { snprintf(g_err, sizeof g_err, "bank %d already retired", bank); return -1; }
    int need = 0, have = 0;
    for (int l = 0; l < M.n_layer; l++) need += PL[l].bank == bank;
    for (int b = 0; b < g_nbank; b++) if (b != bank && !g_bank_dead[b]) have += g_bank_slots[b] - g_bank_used[b];
    if (have < need) { snprintf(g_err, sizeof g_err, "bank %d held %d layers; only %d spare slots on the live banks", bank, need, have); return -1; }
    g_bank_dead[bank] = 1;
    for (int l = 0; l < M.n_layer; l++) {
        if (PL[l].bank != bank) continue;
        int c = -1;
        for (int b = 0; b < g_nbank; b++) if (b != bank && !g_bank_dead[b] && g_bank_used[b] < g_bank_slots[b]) { c = b; break; }
        PL[l].bank = c; PL[l].base = (uint32_t)g_bank_used[c] * M.layer_bytes;
        g_bank_used[c]++;
    }
    g_bank_used[bank] = 0; g_bank_slots[bank] = 0;
    M.layer_slots = 0; M.banks_used = 0;
    for (int b = 0; b < g_nbank; b++) { M.layer_slots += g_bank_slots[b]; M.banks_used += g_bank_used[b] > 0; }
    M.spare_slots = M.layer_slots - M.n_layer;
    M.one_bank_spare = 1;
    for (int b = 0; b < g_nbank; b++)
        if (g_bank_used[b] && M.spare_slots - (g_bank_slots[b] - g_bank_used[b]) < g_bank_used[b]) M.one_bank_spare = 0;
    g_fault_bank = -1;
    return 0;
}

/* The cache the way attention uses it, timed (tl_core.h). Layer 0's region; between prompts only. */
int tl_cache_bench(int n, double *t_write, double *t_read, uint32_t *reread)
{
    if (!g_mopen) return -1;
    if (n > M.max_seq) n = M.max_seq;
    const uint32_t r0 = S.ps_rows_reread;
    double t = plat_now();
    for (int p = 0; p < n; p++) {
        for (int i = 0; i < M.kv_dim; i++) {                       /* rows that look like keys and values */
            KB[i] = (float)((i * 7 + p * 13) % 61 - 30) * 0.05f;
            VB[i] = (float)((i * 11 + p * 3) % 53 - 26) * 0.04f;
        }
        if (kv_put(0, p, KB, VB)) return -1;
    }
    *t_write = plat_now() - t;
    t = plat_now();
    for (int w = 0; w < 2; w++)
        for (int t0 = 0; t0 < n; t0 += TL_KCHUNK) {
            const int c = (n - t0) < TL_KCHUNK ? (n - t0) : TL_KCHUNK;
            if (cache_read(0, w, t0, c)) return -1;
        }
    *t_read = plat_now() - t;
    *reread = S.ps_rows_reread - r0;
    return 0;
}

int tl_matrix(int layer, int which, uint64_t *off, uint32_t *type, uint32_t *rows, uint32_t *cols,
              uint32_t *row_bytes)
{
    if (!g_mopen || which < 0 || which > 7) return -1;
    const tten_t *t;
    if (which == 7) t = &M.embd;
    else {
        if (layer < 0 || layer >= M.n_layer) return -1;
        const tlayer_t *L = &M.L[layer];
        const tten_t *all[7] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->wg, &L->wu, &L->wd };
        t = all[which];
        if (M.n_expert && which >= 4) {            /* a MoE layer: expert 0's gate, up or down rows */
            const tten_t *ex[3] = { &L->eg, &L->eu, &L->ed };
            t = ex[which - 4];
            if (!t->present) return -1;
            *off = t->off; *type = t->type; *rows = (uint32_t)(which == 6 ? M.dim : M.eff); *cols = t->cols;
            *row_bytes = t->row_bytes;
            return 0;
        }
    }
    if (!t->present) return -1;
    *off = t->off; *type = t->type; *rows = t->rows; *cols = t->cols; *row_bytes = t->row_bytes;
    return 0;
}

static int g_logits_from;                 /* tl_set_logits_from, below */

int tl_forward(int32_t token, int pos, int32_t *top1, float *l1, int32_t *top2, float *l2)
{
    if (!g_mopen) { snprintf(g_err, sizeof g_err, "model not open"); return -1; }
    if (pos < 0 || pos >= M.max_seq) { snprintf(g_err, sizeof g_err, "position %d outside 0..%d", pos, M.max_seq - 1); return -1; }
    if (token < 0 || token >= M.vocab) { snprintf(g_err, sizeof g_err, "token %d outside the vocabulary", (int)token); return -1; }
    if (M.n_expert || M.qm > 1 || M.L[0].qn.present) {
        /* A mixture-of-experts model (or a query wider than the dim buffers, or Qwen3's head norms) runs through
         * the batched pass with one slot: tl_step at n = 1 is the per-token arithmetic -- the PC proves the two
         * paths identical for the 3B (verify_pc.py: per token and prompt-batched both equal tl_ref) -- and the
         * expert machinery then exists once. Every slot's scores are wanted here. */
        int p = pos, b = 0;
        const int was = g_logits_from;
        g_logits_from = 0;
        const int r = tl_step(&token, &p, &b, 1, top1, l1, top2, l2);
        g_logits_from = was;
        return r;
    }
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
        if (small_read(&L->n_attn, (uint32_t)dim, G_A) || small_read(&L->n_ffn, (uint32_t)dim, G_F)) goto fail;
        if (L->bq.present && small_read(&L->bq, (uint32_t)M.q_dim, BQ)) goto fail;
        if (L->bk.present && small_read(&L->bk, (uint32_t)kv_dim, BK)) goto fail;
        if (L->bv.present && small_read(&L->bv, (uint32_t)kv_dim, BV)) goto fail;

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
    if (small_read(&M.onorm, (uint32_t)dim, G_A)) goto fail;
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
        it.xqp[p] = PB.xq[p * M.qm]; it.xsp[p] = PB.xs[p * M.qm]; it.xmp[p] = PB.xsum[p * M.qm];
    }
    return pipeline(next_rowsn, &it);
}

/* gate and up for every position, a GROUP of G rows of each at a time, then SwiGLU over the group, each 32 of it
 * quantized straight into PB.c.h.
 *
 * WHY THE GROUP (2026-10-16, measured): until now G was 32, read as four 16-row blocks -- gate 0..15, gate
 * 16..31, up 0..15, up 16..31 -- because 32 output rows per position was all the buffer held. That is 1,376
 * card reads a layer, the reads hopping between the gate and the up matrix 688 times, where the one-position
 * path (stream_rows) reads gate whole and then up whole: 1,048 reads, one hop. Measured on the board, a
 * two-position prompt pass waited 83.3 s for the card against 76.1 s for the decode pass after it -- same bytes,
 * same card, same clock. With G = 256 and full PIPE blocks it is 1,118 reads and 86 hops.
 *
 * THE ANSWER CANNOT CHANGE. Each row's dot is the same call on the same bytes; each SwiGLU element the same
 * expression of the same two floats; each 32 of the hidden vector quantized by the same call as before. Only
 * the order in which rows come off the card moves. G = 32 with 16-row blocks is exactly the old order. Rows
 * are placed by their absolute index (r % G): the pipeline works block i while block i+1 is read, after the
 * iterator has already moved on. */
static int g_ffn_group = TL_FFN_GMAX;

/* the output head only for slots from `slot` on (0: all, the default; tl_step's n or more: none) */
static int g_logits_from = 0;
int tl_set_logits_from(int slot)
{
    const int was = g_logits_from;
    if (slot >= 0) g_logits_from = slot;
    return was;
}

int tl_set_ffn_group(int g)
{
    if (g == 0) return g_ffn_group;
    if (g < 32 || g > TL_FFN_GMAX || g % 32) return -1;
    const int was = g_ffn_group;
    g_ffn_group = g;
    return was;
}

typedef struct {
    const tlayer_t *L;
    int n, down_presum, gp, up, G, blk[2], b0, part, r;   /* part 0 gate, 1 up; r = rows of it done in this group */
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
    for (int q = 0; q < it->n; q++) PB.g[q][r % it->G] = v[q];
}

static void row_up(void *c, const uint8_t *p, int r)
{
    it_ffn *it = (it_ffn *)c;
    float v[TL_BATCH];
    if (it->up) gguf_dot_q4k_presum_n(p, it->n, it->xqp, it->xsp, it->xmp, it->L->wu.cols, v);
    else        gguf_dot_q_n(it->L->wu.type, p, it->n, it->xqp, it->xsp, it->L->wu.cols, v);
    /* SwiGLU the moment the up value exists, over the gate value in place: the same expression on the same two
     * floats as computing it later from a separate up buffer, so the same bits, and 8 KB less OCRAM */
    for (int q = 0; q < it->n; q++) {
        float *h = &PB.g[q][r % it->G];
        const float g = *h;
        *h = (g / (1.0f + tlm_expf(-g))) * v[q];
    }
}

/* after the last up block of a group: its SwiGLU values (row_up) quantized 32 at a time, exactly as the old
 * 32-row step did */
static void swiglu_group(void *c, int r0, int k)
{
    it_ffn *it = (it_ffn *)c;
    const int g0 = ((r0 + k - 1) / it->G) * it->G;
    const int gn = (M.hidden - g0) < it->G ? (M.hidden - g0) : it->G;
    for (int b = 0; b < gn; b += 32) {
        const int b0 = g0 + b;
        for (int q = 0; q < it->n; q++) {
            gguf_quantize_act(&PB.g[q][b], 32, PB.c.h.hq[q] + b0, &PB.c.h.hs[q][b0 / 32]);
            if (it->down_presum) gguf_act_sums(PB.c.h.hq[q] + b0, 32, &PB.c.h.hsum[q][b0 / 32]);
        }
    }
}

static int next_ffn(void *v, tl_job *j)
{
    it_ffn *it = (it_ffn *)v;
    if (it->b0 >= M.hidden) return 0;
    const int gn = (M.hidden - it->b0) < it->G ? (M.hidden - it->b0) : it->G;
    const tten_t *w = it->part ? &it->L->wu : &it->L->wg;
    /* the group's rows in nb near-equal reads, never a full set and a small tail: under ADMA2 a read hides behind
     * the previous block's rows, and the read after a 4-row tail hid behind 4 rows (board, 2026-10-16: an
     * 8-position pass +2.5 s, card wait 1.9 -> 3.9 s). Same reads, same row order; G=32 is still 16 + 16. */
    const int nb = (gn + it->blk[it->part] - 1) / it->blk[it->part];
    const int even = (gn + nb - 1) / nb;
    const int k = (gn - it->r) < even ? (gn - it->r) : even;
    const int r0 = it->b0 + it->r;
    j->off = w->off + (uint64_t)r0 * w->row_bytes; j->rb = w->row_bytes;
    j->r0 = r0; j->k = k; j->row = it->part ? row_up : row_gate;
    j->done = (it->part == 1 && it->r + k == gn) ? swiglu_group : 0; j->c = it;
    it->r += k;
    if (it->r == gn) {
        it->r = 0;
        if (it->part == 0) it->part = 1;
        else { it->part = 0; it->b0 += gn; }
    }
    return 1;
}

static int ffn_gate_up_n(const tlayer_t *L, int n, int down_presum)
{
    it_ffn it;
    memset(&it, 0, sizeof it);
    it.L = L; it.n = n; it.down_presum = down_presum;
    it.gp = L->wg.type == GGML_Q4_K; it.up = L->wu.type == GGML_Q4_K;
    it.G = g_ffn_group;
    if (M.hidden % 32 || (L->wg.row_bytes % 4) || (L->wu.row_bytes % 4)) {
        snprintf(g_err, sizeof g_err, "gate/up rows not 4-aligned, or a hidden size not a multiple of 32");
        return -1;
    }
    for (int m = 0; m < 2; m++) {
        const uint32_t rb = m ? L->wu.row_bytes : L->wg.row_bytes;
        int b = it.G == 32 ? 16 : pipe_rows(rb);           /* 32: the old 16-row halves, exactly */
        if (b > it.G) b = it.G;
        if (b < 1 || (uint32_t)b * rb > TL_PIPEBUF) {
            snprintf(g_err, sizeof g_err, "gate/up rows do not fit a row buffer half");
            return -1;
        }
        it.blk[m] = b;
    }
    for (int p = 0; p < n; p++) { it.xqp[p] = PB.xq[p]; it.xsp[p] = PB.xs[p]; it.xmp[p] = PB.xsum[p]; }
    return pipeline(next_ffn, &it);
}

/* ================================================================================================
 *  THE MIXTURE-OF-EXPERTS FEED-FORWARD, for the n positions of one pass (qwen3moe, 2026-10-16)
 *
 *  model_q.c's arithmetic, operation for operation: the router a float matrix dotted in plain order, softmax
 *  over every expert, the top n_used picked and renormalised, then -- the one order this file chose and model_q
 *  adopted with it -- the picks summed in ascending expert number. Each picked expert's gate and up rows
 *  against the quantized normed input, SwiGLU, that hidden quantized, its down rows, and the result times the
 *  expert's weight added into a sum the residual takes at the end.
 *
 *  WHAT COMES OFF THE CARD: the router (1 MB a layer, F32) and, for each expert any position picked, that
 *  expert's gate, up and down rows -- read ONCE for every position that picked it. A pass walks the experts in
 *  ascending number, so the card reads each expert stack front to back, and each position's sum receives its
 *  experts in ascending order, which is what makes a batched pass equal the per-token one to the bit.
 *
 *  One pipeline for the whole layer: expert after expert, gate rows, up rows, down rows. The pipeline works
 *  block i while block i+1 comes off the card, and block i+1 may already belong to the next expert -- so every
 *  job carries its own expert's context (tl_ex_t), never the iterator's. One hidden buffer serves every expert:
 *  an expert's down blocks are all worked before the next expert's first gate block. */
typedef struct it_moe it_moe;
typedef struct {
    it_moe        *it;
    int            e, np;                       /* the expert, and how many of the pass's positions picked it */
    int            pos[TL_MOE_BATCH];           /* those positions (pass slots), ascending */
    float          w[TL_MOE_BATCH];             /* each one's renormalised weight for this expert */
    const int8_t  *xqp[TL_MOE_BATCH];           /* their quantized normed inputs (gate and up) */
    const float   *xsp[TL_MOE_BATCH];
    const int32_t *xmp[TL_MOE_BATCH];
} tl_ex_t;
struct it_moe {
    const tlayer_t *L;
    int nx, x, part, r;                         /* experts this layer; the one being read; 0 gate 1 up 2 down; rows done */
    int blk[3];                                 /* rows a block, per part */
    int gp, up, dp;                             /* Q4_K presum kernels for gate, up, down */
    const int8_t  *hqp[TL_MOE_BATCH];           /* the hidden, quantized, by expert-local index */
    const float   *hsp[TL_MOE_BATCH];
    const int32_t *hmp[TL_MOE_BATCH];
    tl_ex_t ex[TL_MOE_BATCH * TL_MAX_EUSED];
};
static it_moe MI;

/* the router: one F32 row, every position, model_q's scalar loop */
typedef struct { int n; } it_rt;
static void row_router(void *c, const uint8_t *p, int r)
{
    const it_rt *it = (const it_rt *)c;
    const float *row = (const float *)p;        /* 4-aligned: tensor data is 32-aligned, rows are 4*dim bytes */
    for (int q = 0; q < it->n; q++) {
        const float *xin = PB.c.m.xin[q];
        float acc = 0.0f;
        for (int i = 0; i < M.dim; i++) acc += row[i] * xin[i];
        PB.c.m.rl[q][r] = acc;
    }
}
typedef struct { const tten_t *w; int r, per; it_rt c; } it_rtn;
static int next_router(void *v, tl_job *j)
{
    it_rtn *it = (it_rtn *)v;
    if (it->r >= (int)it->w->rows) return 0;
    const int k = ((int)it->w->rows - it->r) < it->per ? ((int)it->w->rows - it->r) : it->per;
    j->off = it->w->off + (uint64_t)it->r * it->w->row_bytes; j->rb = it->w->row_bytes;
    j->r0 = it->r; j->k = k; j->row = row_router; j->done = 0; j->c = &it->c;
    it->r += k;
    return 1;
}

static void row_egate(void *c, const uint8_t *p, int r)
{
    const tl_ex_t *x = (const tl_ex_t *)c;
    float v[TL_MOE_BATCH];
    if (x->it->gp) gguf_dot_q4k_presum_n(p, x->np, x->xqp, x->xsp, x->xmp, x->it->L->eg.cols, v);
    else           gguf_dot_q_n(x->it->L->eg.type, p, x->np, x->xqp, x->xsp, x->it->L->eg.cols, v);
    for (int q = 0; q < x->np; q++) PB.c.m.h[q][r] = v[q];
}
static void row_eup(void *c, const uint8_t *p, int r)
{
    const tl_ex_t *x = (const tl_ex_t *)c;
    float v[TL_MOE_BATCH];
    if (x->it->up) gguf_dot_q4k_presum_n(p, x->np, x->xqp, x->xsp, x->xmp, x->it->L->eu.cols, v);
    else           gguf_dot_q_n(x->it->L->eu.type, p, x->np, x->xqp, x->xsp, x->it->L->eu.cols, v);
    for (int q = 0; q < x->np; q++) {           /* model_q's SwiGLU, on the gate value in place */
        float *h = &PB.c.m.h[q][r];
        const float g = *h;
        *h = (g / (1.0f + tlm_expf(-g))) * v[q];
    }
}
/* after the expert's last up row: its hidden quantized for the down projection, as model_q quantizes each expert's */
static void moe_swiglu(void *c, int r0, int k)
{
    (void)r0; (void)k;
    const tl_ex_t *x = (const tl_ex_t *)c;
    for (int q = 0; q < x->np; q++) {
        gguf_quantize_act(PB.c.m.h[q], (uint64_t)M.eff, PB.c.m.hq[q], PB.c.m.hs[q]);
        if (x->it->dp) gguf_act_sums(PB.c.m.hq[q], (uint64_t)M.eff, PB.c.m.hsum[q]);
    }
}
static void row_edown(void *c, const uint8_t *p, int r)
{
    const tl_ex_t *x = (const tl_ex_t *)c;
    const it_moe *it = x->it;
    float v[TL_MOE_BATCH];
    if (it->dp) gguf_dot_q4k_presum_n(p, x->np, it->hqp, it->hsp, it->hmp, it->L->ed.cols, v);
    else        gguf_dot_q_n(it->L->ed.type, p, x->np, it->hqp, it->hsp, it->L->ed.cols, v);
    for (int q = 0; q < x->np; q++) PB.c.m.acc[x->pos[q]][r] += x->w[q] * v[q];   /* acc[i] += w * o[i] */
}
static int next_moe(void *v, tl_job *j)
{
    it_moe *it = (it_moe *)v;
    if (it->x >= it->nx) return 0;
    tl_ex_t *x = &it->ex[it->x];
    const tten_t *w = it->part == 0 ? &it->L->eg : it->part == 1 ? &it->L->eu : &it->L->ed;
    const int rows = it->part == 2 ? M.dim : M.eff;
    /* near-equal blocks (as next_ffn), an even count when a row is not a multiple of 4 bytes (Q6_K down rows
     * of 768 are 630) so every block starts 4-aligned */
    const int nb = (rows + it->blk[it->part] - 1) / it->blk[it->part];
    int even = (rows + nb - 1) / nb;
    if ((w->row_bytes % 4) && (even & 1)) even++;
    if (even > it->blk[it->part]) even = it->blk[it->part];
    const int k = (rows - it->r) < even ? (rows - it->r) : even;
    j->off = w->off + ((uint64_t)x->e * (uint64_t)rows + (uint64_t)it->r) * w->row_bytes; j->rb = w->row_bytes;
    j->r0 = it->r; j->k = k;
    j->row = it->part == 0 ? row_egate : it->part == 1 ? row_eup : row_edown;
    j->done = (it->part == 1 && it->r + k == rows) ? moe_swiglu : 0;
    j->c = x;
    it->r += k;
    if (it->r == rows) { it->r = 0; if (++it->part == 3) { it->part = 0; it->x++; } }
    return 1;
}

static int moe_ffn_n(const tlayer_t *L, int n)
{
    const int dim = M.dim, ne = M.n_expert, ku = M.n_used, qm = M.qm;
    it_moe *it = &MI;
    memset(it, 0, sizeof *it);
    it->L = L;
    it->gp = L->eg.type == GGML_Q4_K; it->up = L->eu.type == GGML_Q4_K; it->dp = L->ed.type == GGML_Q4_K;
    /* the normed input: in float for the router, quantized once for every expert's gate and up (experts_fused) */
    for (int p = 0; p < n; p++) {
        rmsnorm(PB.c.m.xin[p], PB.x[p], G_F, dim, M.eps);
        gguf_quantize_act(PB.c.m.xin[p], (uint64_t)dim, PB.xq[p * qm], PB.xs[p * qm]);
        gguf_act_sums(PB.xq[p * qm], (uint64_t)dim, PB.xsum[p * qm]);   /* presum == gguf_dot_q to the bit */
    }
    /* the router, every row once for all positions */
    {
        it_rtn r;
        memset(&r, 0, sizeof r);
        r.w = &L->rt; r.per = pipe_rows(L->rt.row_bytes); r.c.n = n;
        if (!r.per) { snprintf(g_err, sizeof g_err, "router row wider than the row buffer"); return -1; }
        if (pipeline(next_router, &r)) return -1;
    }
    /* route() of model_q, per position: softmax, the top ku (first index on a tie), renormalised, then sorted by
     * expert number for the sum */
    int pick[TL_MOE_BATCH][TL_MAX_EUSED];
    float wgt[TL_MOE_BATCH][TL_MAX_EUSED];
    for (int p = 0; p < n; p++) {
        float *pr = PB.c.m.rl[p];
        softmax(pr, ne);
        for (int j = 0; j < ku; j++) {
            int best = -1;
            float bv = -1e30f;
            for (int e = 0; e < ne; e++) {
                int taken = 0;
                for (int q = 0; q < j; q++) if (pick[p][q] == e) { taken = 1; break; }
                if (!taken && pr[e] > bv) { bv = pr[e]; best = e; }
            }
            pick[p][j] = best;
            wgt[p][j] = bv;
        }
        float sum = 0.0f;
        for (int j = 0; j < ku; j++) sum += wgt[p][j];
        if (sum > 0.0f) for (int j = 0; j < ku; j++) wgt[p][j] /= sum;
        for (int j = 1; j < ku; j++) {
            const int pe = pick[p][j];
            const float pw = wgt[p][j];
            int q = j - 1;
            while (q >= 0 && pick[p][q] > pe) { pick[p][q + 1] = pick[p][q]; wgt[p][q + 1] = wgt[p][q]; q--; }
            pick[p][q + 1] = pe; wgt[p][q + 1] = pw;
        }
    }
    /* the experts any position picked, ascending, each with the positions that picked it */
    for (int e = 0; e < ne; e++) {
        tl_ex_t *x = &it->ex[it->nx];
        x->np = 0;
        for (int p = 0; p < n; p++)
            for (int j = 0; j < ku; j++)
                if (pick[p][j] == e) {
                    x->pos[x->np] = p; x->w[x->np] = wgt[p][j];
                    x->xqp[x->np] = PB.xq[p * qm]; x->xsp[x->np] = PB.xs[p * qm]; x->xmp[x->np] = PB.xsum[p * qm];
                    x->np++;
                    break;
                }
        if (x->np) { x->it = it; x->e = e; it->nx++; }
    }
    for (int q = 0; q < TL_MOE_BATCH; q++) { it->hqp[q] = PB.c.m.hq[q]; it->hsp[q] = PB.c.m.hs[q]; it->hmp[q] = PB.c.m.hsum[q]; }
    const tten_t *parts[3] = { &L->eg, &L->eu, &L->ed };
    for (int m = 0; m < 3; m++) {
        it->blk[m] = pipe_rows(parts[m]->row_bytes);
        if (it->blk[m] < 1) { snprintf(g_err, sizeof g_err, "expert rows do not fit a row buffer half"); return -1; }
    }
    for (int p = 0; p < n; p++) for (int i = 0; i < dim; i++) PB.c.m.acc[p][i] = 0.0f;
    if (pipeline(next_moe, it)) return -1;
    for (int p = 0; p < n; p++) for (int i = 0; i < dim; i++) PB.x[p][i] += PB.c.m.acc[p][i];
    return 0;
}

int tl_step(const int32_t *tok, const int *pos, const int *base, int n,
            int32_t *top1, float *l1, int32_t *top2, float *l2)
{
    if (!g_mopen) { snprintf(g_err, sizeof g_err, "model not open"); return -1; }
    if (n < 1 || n > M.batch) { snprintf(g_err, sizeof g_err, "pass of %d positions (1..%d)", n, M.batch); return -1; }
    for (int p = 0; p < n; p++)
        if (pos[p] < 0 || base[p] < 0 || base[p] + pos[p] >= M.max_seq) {
            snprintf(g_err, sizeof g_err, "slot %d: position %d at cache row %d outside 0..%d", p, pos[p], base[p], M.max_seq - 1);
            return -1;
        }
    if ((!M.n_expert && (M.hidden % 32 || M.qm != 1)) || M.dim > TL_MAX_DIM || M.q_dim > M.qm * TL_MAX_DIM) {
        snprintf(g_err, sizeof g_err, "shape not batchable");
        return -1;
    }
    for (int p = 0; p < n; p++)
        if (tok[p] < 0 || tok[p] >= M.vocab) { snprintf(g_err, sizeof g_err, "token %d outside the vocabulary", (int)tok[p]); return -1; }
    const double t_start = plat_now();
    double ts = t_start;
#define STAGE(field) do { const double tn = plat_now(); S.field += tn - ts; ts = tn; } while (0)
    const int dim = M.dim, kv_dim = M.kv_dim, nh = M.n_heads, nkv = M.n_kv, hd = M.head_dim;
    /* a position's q-wide vectors (its query; the attention output that feeds wo) take qm PB rows: position p's
     * quantized input is PB.xq[p * qm], its query PB.c.q[p * qm]. qm is 1 for the 3B, so its layout is unchanged. */
    const int qm = M.qm;
    const size_t XQS = (size_t)qm * TL_MAX_DIM, XSS = (size_t)qm * NBD;
    float *const AO = qm > 1 ? &PB.k[0][0] : XB;          /* one position's attention output, q_dim floats */

    for (int p = 0; p < n; p++) {
        if (sd_read(M.embd.off + (uint64_t)tok[p] * M.embd.row_bytes, ROW, M.embd.row_bytes)) goto fail;
        if (gguf_dequant(M.embd.type, ROW, M.embd.cols, PB.x[p])) { snprintf(g_err, sizeof g_err, "embedding type %lu", (unsigned long)M.embd.type); goto fail; }
    }
    STAGE(t_embed);

    for (int l = 0; l < M.n_layer; l++) {
        const tlayer_t *L = &M.L[l];
        if (small_read(&L->n_attn, (uint32_t)dim, G_A) || small_read(&L->n_ffn, (uint32_t)dim, G_F)) goto fail;
        if (L->bq.present && small_read(&L->bq, (uint32_t)M.q_dim, BQ)) goto fail;
        if (L->bk.present && small_read(&L->bk, (uint32_t)kv_dim, BK)) goto fail;
        if (L->bv.present && small_read(&L->bv, (uint32_t)kv_dim, BV)) goto fail;
        if (L->qn.present && small_read(&L->qn, (uint32_t)hd, G_QN)) goto fail;
        if (L->kn.present && small_read(&L->kn, (uint32_t)hd, G_KN)) goto fail;

        /* q, k, v: one quantization of each position's normed input, gguf_dot_q on every row (matvec_group) */
        for (int p = 0; p < n; p++) {
            rmsnorm(XB, PB.x[p], G_A, dim, M.eps);
            gguf_quantize_act(XB, L->wq.cols, PB.xq[p * qm], PB.xs[p * qm]);
            gguf_act_sums(PB.xq[p * qm], L->wq.cols, PB.xsum[p * qm]);   /* presum == gguf_dot_q to the bit (dot_verify) */
        }
        if (stream_rows_n(&L->wq, M.q_dim, n, &PB.xq[0][0], XQS, &PB.xs[0][0], XSS, &PB.xsum[0][0], XSS,
                          L->wq.type == GGML_Q4_K, &PB.c.q[0][0], XQS, 0) ||
            stream_rows_n(&L->wk, kv_dim, n, &PB.xq[0][0], XQS, &PB.xs[0][0], XSS, &PB.xsum[0][0], XSS,
                          L->wk.type == GGML_Q4_K, &PB.k[0][0], TL_MAX_KVDIM, 0) ||
            stream_rows_n(&L->wv, kv_dim, n, &PB.xq[0][0], XQS, &PB.xs[0][0], XSS, &PB.xsum[0][0], XSS,
                          L->wv.type == GGML_Q4_K, &PB.v[0][0], TL_MAX_KVDIM, 0)) goto fail;
        STAGE(t_qkv);

        for (int p = 0; p < n; p++) {
            float *q = PB.c.q[p * qm], *k = PB.k[p], *v = PB.v[p];
            if (L->bq.present) for (int i = 0; i < M.q_dim; i++) q[i] += BQ[i];
            if (L->bk.present) for (int i = 0; i < kv_dim; i++)   k[i] += BK[i];
            if (L->bv.present) for (int i = 0; i < kv_dim; i++)   v[i] += BV[i];
            if (L->qn.present) head_norm(q, nh,  hd, G_QN, M.eps);   /* Qwen3: within each head, before the rotation */
            if (L->kn.present) head_norm(k, nkv, hd, G_KN, M.eps);
            rope(q, nh,  hd, pos[p], M.rope_base);
            rope(k, nkv, hd, pos[p], M.rope_base);
            if (kv_put(l, base[p] + pos[p], k, v)) goto fail;
        }
        /* every slot's key and value are in the cache before any slot attends, so a later position of the
         * same sequence in this pass sees the earlier ones, exactly as it would one pass later (and PB.k, written
         * to the cache, is free to take a wide query's attention output: AO) */
        const int wo_presum = L->wo.type == GGML_Q4_K;
        for (int p = 0; p < n; p++) {
            if (attention(l, base[p], pos[p], PB.c.q[p * qm], AO)) goto fail;
            gguf_quantize_act(AO, L->wo.cols, PB.xq[p * qm], PB.xs[p * qm]);
            if (wo_presum) gguf_act_sums(PB.xq[p * qm], L->wo.cols, PB.xsum[p * qm]);
        }
        STAGE(t_att);
        if (stream_rows_n(&L->wo, dim, n, &PB.xq[0][0], XQS, &PB.xs[0][0], XSS, &PB.xsum[0][0], XSS, wo_presum,
                          &PB.x[0][0], TL_MAX_DIM, 1)) goto fail;
        STAGE(t_wo);

        if (M.n_expert) {
            if (moe_ffn_n(L, n)) goto fail;
            STAGE(t_ffn);
            continue;
        }
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
        /* the output head only for the slots from g_logits_from on (tl_set_logits_from): a prompt position's scores
         * are used only at the prompt's last position, and the head is a tenth of the weights. Those slots' inputs
         * are moved to the front, the head runs over them alone -- or not at all, its rows not even read -- and
         * every other slot reports top1 = top2 = -1. The slots computed get the same bits as before. */
        const int from = g_logits_from < n ? g_logits_from : n, m = n - from;
        for (int p = 0; p < from; p++) {
            if (top1) top1[p] = -1;
            if (l1)   l1[p]   = 0.0f;
            if (top2) top2[p] = -1;
            if (l2)   l2[p]   = 0.0f;
        }
        if (m > 0) {
            const tten_t *head = M.tied ? &M.embd : &M.out;
            const int hp = head->type == GGML_Q4_K;
            if (small_read(&M.onorm, (uint32_t)dim, G_A)) goto fail;
            for (int q = 0; q < m; q++) {
                rmsnorm(XB, PB.x[from + q], G_A, dim, M.eps);
                gguf_quantize_act(XB, head->cols, PB.xq[q * qm], PB.xs[q * qm]);
                if (hp) gguf_act_sums(PB.xq[q * qm], head->cols, PB.xsum[q * qm]);
            }
            float b1[TL_BATCH], b2[TL_BATCH];
            int32_t i1[TL_BATCH], i2[TL_BATCH];
            if (head_n(head, m, hp, b1, i1, b2, i2)) goto fail;
            for (int q = 0; q < m; q++) {
                const int p = from + q;
                if (top1) top1[p] = i1[q];
                if (l1)   l1[p]   = b1[q];
                if (top2) top2[p] = i2[q];
                if (l2)   l2[p]   = b2[q];
            }
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
    if (n < 1 || n > M.batch) { snprintf(g_err, sizeof g_err, "prefill of %d positions (1..%d)", n, M.batch); return -1; }
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
    if (!g_mopen) { snprintf(g_err, sizeof g_err, "model not open"); return -1; }
    if (ns < 1 || ns > M.batch) { snprintf(g_err, sizeof g_err, "%d sequences (1..%d)", ns, M.batch); return -1; }
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

/* rows [from, from+n) of every layer's cache to [to, to+n): keys, values, both scale arrays and both
 * checksum arrays -- within each layer's one bank */
static int kv_copy(int from, int to, int n)
{
    const uint32_t kd = (uint32_t)M.kv_dim, sd = (uint32_t)M.n_kv * 4u;
    for (int l = 0; l < M.n_layer; l++) {
        const int b = PL[l].bank;
        const uint32_t a = PL[l].base;
        for (int t0 = 0; t0 < n; t0 += TL_KCHUNK) {
            const uint32_t c = (uint32_t)((n - t0) < TL_KCHUNK ? (n - t0) : TL_KCHUNK);
            for (int w = 0; w < 2; w++)
                if (ps_read(b, a + o_row(w, from + t0), KC, c * kd)   || ps_write(b, a + o_row(w, to + t0), KC, c * kd) ||
                    ps_read(b, a + o_sc(w, from + t0), KCS, c * sd)   || ps_write(b, a + o_sc(w, to + t0), KCS, c * sd) ||
                    ps_read(b, a + o_sum(w, from + t0), SUMC, c * 4u) || ps_write(b, a + o_sum(w, to + t0), SUMC, c * 4u)) return -1;
        }
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
    for (int left = M.batch - active, more = 1; left > 0 && more; ) {
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
