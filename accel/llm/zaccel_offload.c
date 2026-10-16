/* ===========================================================================================
 *  zaccel_offload.c -- the Zynq matrix engines taking a share of the model's matrix-vector work
 * ===========================================================================================
 *
 *  Every dense matrix of every resident layer, and the output head, is split by rows: rows [0, nz)
 *  live on the engines, requantized to int8 with one scale per row, and rows [nz, n) stay on this CPU
 *  in their GGUF form. A matvec sends the activation (int8, one scale for the vector) to every engine,
 *  computes its own rows meanwhile, then joins: out[r] = acc[r] * wscale[r] * xscale for the engines'
 *  rows.
 *
 *  SEVERAL ENGINES. --zaccel HOST[:PORT][,HOST2[:PORT2]...] names up to ZMAX_ENG engines (two Zynq
 *  boards in the machine, machine/README.md). The engines' rows [0, nz) are divided among them in
 *  proportion to each one's measured rate, each engine holds its slice as its own tensors, and a
 *  matvec drives all of them at once from one thread each. Every row is still computed whole by
 *  exactly one engine, so the answer with two engines is BYTE-IDENTICAL to the answer with one
 *  (accel/llm/test_two_engines.sh holds it to that).
 *
 *  ROW BANDS. The engine's row count is 16 bits (accel/SPEC.md §1) and the output head has 151,936
 *  rows, so a slice is uploaded as bands of at most ZBAND_ROWS rows, each its own tensor per column
 *  chunk. The bands are also the unit of quantization, which keeps the upload's working memory to a
 *  few tens of MB on a board whose Linux has 256 MB.
 *
 *  THE SPLIT IS MEASURED, NOT GUESSED. At attach time every side is timed on a real matrix of the
 *  model: this CPU's rows per second, and each engine's fixed cost per call (network round trip) plus
 *  its rows per second. Each matrix then gets the nz that finishes the CPU and the engines together,
 *  and 0 when the round trip alone costs more than the CPU needs for the whole matrix -- so an engine
 *  can only ever take work it finishes sooner than the CPU would. Each engine's free memory caps its
 *  own slice.
 *
 *  OUTLIERS. A transformer's activations carry a few channels tens of times larger than the rest, so
 *  one int8 scale for a whole vector crushes every other value (measured on Qwen2.5-Coder 3B: 6-9%
 *  error per product, garbage after 36 layers). Both sides are therefore ROTATED by a block Hadamard
 *  matrix first -- the engines' weight rows once at upload, the activation on every call. H/sqrt(b) is
 *  orthonormal, so (H w).(H x)/b = w.x exactly, and an outlier is spread across its whole block
 *  before quantizing. (The QuaRot result; b is the largest power of two dividing the row, <= 4096.)
 *  Each engine column chunk also gets its own activation scale.
 *
 *  Rows wider than the engine's 4096 columns are split into column chunks; each chunk's integer
 *  partial sum is scaled by its own activation scale and the chunks add in float.
 *
 *  If an engine stops answering mid-run, its rows of that matvec are recomputed here and that engine
 *  is retired for the rest of the run; the others carry on. With every engine gone the offload is
 *  off. The answer is always complete.
 *
 *  $ZACCEL_HEAD=0 keeps the output head on the CPU (its quantization cost is a ppl.c question per
 *  model; the default offloads it). $ZACCEL_WBITS=4 uses int4 weights on the engines.
 *
 *  Numbers only count on the boards against the real engines. On a PC this checks correctness and
 *  the quality cost of the int8 requantization (tests/ppl.c), nothing else.
 * ===========================================================================================
 */
#include "model_q.h"
#include "gguf.h"
#include "libzaccel.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ZCOLS      4096      /* the engine's widest row (accel/SPEC.md §1) */
#define ZPARTS     8
#define ZBATCH     8         /* the engine's batch: one weight read serves 8 positions */
#define ZMAX_ENG   4         /* engines a host list may name */
#define ZBAND_ROWS 16384u    /* rows per uploaded tensor: under the 16-bit limit, and the quantization
                              * buffers for one band of the widest matrix stay under ~50 MB */

static int env_flag(const char *name, int dflt)
{
    const char *e = getenv(name);
    return (e && *e) ? atoi(e) : dflt;
}
/* Weight width on the engines: 8 (default) or 4 ($ZACCEL_WBITS=4). int4 halves the bytes, so an
 * engine reads twice the weights per second and twice as many rows fit in its memory; whether the
 * model can afford it is a ppl.c question, answered per model, never assumed. */
static int wbits(void)
{
    static int b = 0;
    if (!b) b = env_flag("ZACCEL_WBITS", 8) == 4 ? 4 : 8;
    return b;
}

typedef struct { zaccel_tensor_t t; uint32_t c0, nc; } zpart_t;
typedef struct {                 /* one band of one engine's slice: rows [r0, r0 + nr), as column chunks */
    uint32_t r0, nr;
    int      nparts;
    zpart_t  part[ZPARTS];
} zband_t;
typedef struct {                 /* one engine's slice of one matrix: rows [r0, r0 + nr) in bands */
    uint32_t r0, nr;
    int      nband;
    zband_t *band;
} zslice_t;
typedef struct {
    const qten_t *w;
    uint32_t nz;                 /* rows [0, nz) are on the engines, in slices */
    zslice_t sl[ZMAX_ENG];
    float   *ws;                 /* per-row weight scale, rows [0, nz) */
    int      hb;                 /* Hadamard block: the largest power of two dividing cols, <= 4096 */
} zmat_t;

typedef struct {
    zaccel_t      *z;
    zaccel_info_t  in;
    char           name[64];
    double         rz, t0;       /* measured: rows per second, fixed seconds per call */
    int            dead;         /* retired after a failure: its rows are the CPU's now */
    uint64_t       calls, fails;
    int32_t       *tmp;          /* [ZBATCH][ZBAND_ROWS] integer results of one band's part */
    int8_t        *blk;          /* [ZBATCH][ZCOLS] one column chunk of the activations */
} zeng_t;

typedef struct {
    zeng_t    eng[ZMAX_ENG];
    int       neng;
    zmat_t   *mats;
    int       n, cap;
    int8_t   *xq;                /* the rotated activations, int8: [ZBATCH][max_cols] */
    float    *xr;                /* the rotated activations, float: [ZBATCH][max_cols] */
    float    *sxp;               /* activation scale per vector per column chunk: [ZBATCH][ZPARTS] */
    float    *facc;              /* partial sums, scaled: [max_rows][ZBATCH] */
    int       dead;              /* every engine failed: everything back on the CPU */
    uint64_t  calls, fails;
} zoff_t;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static zmat_t *zfind(zoff_t *zo, const qten_t *w)
{
    for (int i = 0; i < zo->n; i++)
        if (zo->mats[i].w == w) return &zo->mats[i];
    return NULL;
}

static int hblock(uint32_t cols)
{
    int b = 1;
    while (b < ZCOLS && cols % (uint32_t)(b * 2) == 0) b *= 2;
    return b;
}
/* v <- (H v) / sqrt(b) on every b-block of v: the fast Walsh-Hadamard transform, orthonormal */
static void hrotate(float *v, uint32_t cols, int b)
{
    if (b < 2) return;
    const float k = 1.0f / sqrtf((float)b);
    for (uint32_t c0 = 0; c0 < cols; c0 += (uint32_t)b) {
        float *x = v + c0;
        for (int h = 1; h < b; h <<= 1)
            for (int i = 0; i < b; i += h << 1)
                for (int j = i; j < i + h; j++) { const float a = x[j], d = x[j + h]; x[j] = a + d; x[j + h] = a - d; }
        for (int j = 0; j < b; j++) x[j] *= k;
    }
}

static void zband_free(zoff_t *zo, int e, zband_t *b)
{
    if (!zo->eng[e].dead)
        for (int i = 0; i < b->nparts; i++) zaccel_free(zo->eng[e].z, &b->part[i].t);
    b->nparts = 0;
}
static void zslice_free(zoff_t *zo, int e, zslice_t *s)
{
    for (int k = 0; k < s->nband; k++) zband_free(zo, e, &s->band[k]);
    free(s->band);
    memset(s, 0, sizeof *s);
}
static void zdrop(zoff_t *zo, zmat_t *zm)
{
    for (int e = 0; e < zo->neng; e++) zslice_free(zo, e, &zm->sl[e]);
    free(zm->ws);
    memset(zm, 0, sizeof *zm);
}

/* rows [r0, r0 + nr) of w: dequantized, rotated, int8 with a scale per row (into ws[r0..]), returned
 * as one buffer [nr][cols] the caller frees. NULL = out of memory. */
static int8_t *zquant_rows(const qten_t *w, int hb, uint32_t r0, uint32_t nr, float *ws)
{
    int8_t *q = (int8_t *)malloc((size_t)nr * w->cols);
    float *row = (float *)malloc(w->cols * sizeof(float));
    if (!q || !row) { free(q); free(row); return NULL; }
    const int qmax = wbits() == 4 ? 7 : 127;
    for (uint32_t r = 0; r < nr; r++) {
        gguf_dequant(w->type, w->raw + (size_t)(r0 + r) * w->row_bytes, w->cols, row);
        hrotate(row, w->cols, hb);
        float amax = 0.0f;
        for (uint32_t c = 0; c < w->cols; c++) amax = fmaxf(amax, fabsf(row[c]));
        const float s = amax > 0.0f ? amax / (float)qmax : 1.0f;
        ws[r0 + r] = s;
        for (uint32_t c = 0; c < w->cols; c++) {
            long v = lrintf(row[c] / s);
            q[(size_t)r * w->cols + c] = (int8_t)(v > qmax ? qmax : v < -qmax ? -qmax : v);
        }
    }
    free(row);
    return q;
}

/* one band (rows [r0, r0 + nr), already quantized in q with stride cols) -> engine e, as column chunks.
 * 0 ok, 1 the engine is full, -1 error. */
static int zband_upload(zoff_t *zo, int e, const int8_t *q, uint32_t cols, uint32_t r0, uint32_t nr,
                        zband_t *b, char *msg, int msglen)
{
    memset(b, 0, sizeof *b);
    b->r0 = r0; b->nr = nr;
    const uint32_t mode = wbits() == 4 ? ZACCEL_MODE_INT4 : ZACCEL_MODE_INT8;
    for (uint32_t c0 = 0; c0 < cols; c0 += ZCOLS) {
        const uint32_t nc = (cols - c0) < ZCOLS ? (cols - c0) : ZCOLS;
        if (b->nparts == ZPARTS) { snprintf(msg, msglen, "row too wide (%u columns)", cols); zband_free(zo, e, b); return -1; }
        const size_t rb = zaccel_row_bytes(mode, nc);
        uint8_t *packed = (uint8_t *)calloc((size_t)nr, rb);
        int8_t *sub = (int8_t *)malloc((size_t)nr * nc);
        if (!packed || !sub) { free(packed); free(sub); snprintf(msg, msglen, "out of memory"); zband_free(zo, e, b); return -1; }
        for (uint32_t r = 0; r < nr; r++) memcpy(sub + (size_t)r * nc, q + (size_t)r * cols + c0, nc);
        if (mode == ZACCEL_MODE_INT4) zaccel_pack_int4(sub, nr, nc, packed);
        else zaccel_pack_int8(sub, nr, nc, packed);
        zpart_t *p = &b->part[b->nparts];
        int rc = zaccel_load(zo->eng[e].z, mode, nr, nc, packed, &p->t);
        free(packed); free(sub);
        if (rc) {
            snprintf(msg, msglen, "%s: LOAD failed: %s", zo->eng[e].name, zaccel_strerror(rc));
            zband_free(zo, e, b);
            return rc == ZACCEL_ST_NOMEM ? 1 : -1;
        }
        p->c0 = c0; p->nc = nc;
        b->nparts++;
    }
    return 0;
}

/* engine e's slice, rows [r0, r0 + nr): quantized and uploaded band by band */
static int zslice_upload(zoff_t *zo, int e, const qten_t *w, int hb, float *ws, uint32_t r0, uint32_t nr,
                         zslice_t *s, char *msg, int msglen)
{
    memset(s, 0, sizeof *s);
    s->r0 = r0; s->nr = nr;
    if (nr == 0) return 0;
    s->nband = (int)((nr + ZBAND_ROWS - 1) / ZBAND_ROWS);
    s->band = (zband_t *)calloc((size_t)s->nband, sizeof(zband_t));
    if (!s->band) { snprintf(msg, msglen, "out of memory"); s->nband = 0; return -1; }
    for (int k = 0; k < s->nband; k++) {
        const uint32_t br0 = r0 + (uint32_t)k * ZBAND_ROWS;
        const uint32_t bnr = (r0 + nr - br0) < ZBAND_ROWS ? (r0 + nr - br0) : ZBAND_ROWS;
        int8_t *q = zquant_rows(w, hb, br0, bnr, ws);
        if (!q) { snprintf(msg, msglen, "out of memory"); zslice_free(zo, e, s); return -1; }
        int rc = zband_upload(zo, e, q, w->cols, br0, bnr, &s->band[k], msg, msglen);
        free(q);
        if (rc) { s->nband = k; zslice_free(zo, e, s); return rc; }
    }
    return 0;
}

/* rows [0, nz) of w onto the engines: nzk[e] rows for engine e, in order; sum = nz. Returns 0,
 * 1 (an engine is full: nothing of this matrix uploaded), or -1 (error). */
static int zupload(zoff_t *zo, const qten_t *w, const uint32_t *nzk, zmat_t *zm, char *msg, int msglen)
{
    memset(zm, 0, sizeof *zm);
    zm->w = w;
    zm->hb = hblock(w->cols);
    uint32_t nz = 0;
    for (int e = 0; e < zo->neng; e++) nz += nzk[e];
    if (nz == 0) return 0;
    zm->ws = (float *)malloc(nz * sizeof(float));
    if (!zm->ws) { snprintf(msg, msglen, "out of memory"); return -1; }
    uint32_t r0 = 0;
    for (int e = 0; e < zo->neng; e++) {
        int rc = zslice_upload(zo, e, w, zm->hb, zm->ws, r0, nzk[e], &zm->sl[e], msg, msglen);
        if (rc) {                                   /* undo the slices already on the other engines */
            for (int k = 0; k < e; k++) zslice_free(zo, k, &zm->sl[k]);
            free(zm->ws); memset(zm, 0, sizeof *zm);
            return rc;
        }
        r0 += nzk[e];
    }
    zm->nz = nz;
    int hb = zm->hb;                                /* a whole number of blocks per chunk */
    for (int e = 0; e < zo->neng; e++)
        for (int k = 0; k < zm->sl[e].nband; k++)
            for (int i = 0; i < zm->sl[e].band[k].nparts; i++)
                if (zm->sl[e].band[k].part[i].nc % (uint32_t)hb) zm->hb = 1;
    if (zm->hb != hb) { snprintf(msg, msglen, "internal: Hadamard block %d straddles a column chunk", hb); return -1; }
    return 0;
}

/* the column chunks of this matrix (every band of every slice chunks columns identically) */
static const zband_t *zchunks(const zmat_t *zm, int neng)
{
    for (int e = 0; e < neng; e++)
        if (zm->sl[e].nband) return &zm->sl[e].band[0];
    return NULL;
}

/* activation vector v of this matrix: rotate a copy, then int8 per column chunk, each its own scale */
static void zprep(zoff_t *zo, const zmat_t *zm, const float *x, int v)
{
    const uint32_t cols = zm->w->cols;
    const zband_t *cs = zchunks(zm, zo->neng);
    float *xr = zo->xr + (size_t)v * cols;
    int8_t *q = zo->xq + (size_t)v * cols;
    memcpy(xr, x, cols * sizeof(float));
    hrotate(xr, cols, zm->hb);
    if (!cs) return;
    for (int i = 0; i < cs->nparts; i++) {
        const uint32_t c0 = cs->part[i].c0, nc = cs->part[i].nc;
        float amax = 0.0f;
        for (uint32_t c = 0; c < nc; c++) amax = fmaxf(amax, fabsf(xr[c0 + c]));
        const float sc = amax > 0.0f ? amax / 127.0f : 1.0f;
        zo->sxp[v * ZPARTS + i] = sc;
        for (uint32_t c = 0; c < nc; c++) {
            long t = lrintf(xr[c0 + c] / sc);
            q[c0 + c] = (int8_t)(t > 127 ? 127 : t < -127 ? -127 : t);
        }
    }
}

/* engine e's slice of one matvec (nb = 1) or of nb prefill positions: facc rows [r0, r0 + nr), band by
 * band. Reads xq / sxp (written by zprep before the workers start), writes only its own tmp, blk and
 * its own rows of facc. */
typedef struct { zoff_t *zo; zmat_t *zm; int e, nb, rc; } zjob_t;
static void *zrun(void *arg)
{
    zjob_t *j = (zjob_t *)arg;
    zoff_t *zo = j->zo; zmat_t *zm = j->zm; zeng_t *E = &zo->eng[j->e];
    const zslice_t *s = &zm->sl[j->e];
    const uint32_t cols = zm->w->cols;
    const int nb = j->nb;
    j->rc = 0;
    if (s->nr == 0 || E->dead) return NULL;
    for (uint32_t r = 0; r < s->nr; r++) for (int v = 0; v < nb; v++) zo->facc[(size_t)(s->r0 + r) * nb + v] = 0.0f;
    for (int k = 0; k < s->nband; k++) {
        const zband_t *b = &s->band[k];
        for (int i = 0; i < b->nparts; i++) {
            const uint32_t c0 = b->part[i].c0, nc = b->part[i].nc;
            const int8_t *A = zo->xq + c0;
            if (nb > 1) {
                for (int v = 0; v < nb; v++) memcpy(E->blk + (size_t)v * nc, zo->xq + (size_t)v * cols + c0, nc);
                A = E->blk;
            }
            int rc = zaccel_gemv(E->z, &b->part[i].t, (uint32_t)nb, A, E->tmp, NULL, NULL);
            if (rc) { j->rc = rc; return NULL; }
            for (uint32_t r = 0; r < b->nr; r++)                                /* Y[r][v] */
                for (int v = 0; v < nb; v++)
                    zo->facc[(size_t)(b->r0 + r) * nb + v] += (float)E->tmp[(size_t)r * nb + v] * zo->sxp[v * ZPARTS + i];
        }
    }
    E->calls++;
    return NULL;
}

/* start one worker per live engine that holds rows of zm; returns the count started */
static int zstart(zoff_t *zo, zmat_t *zm, int nb, zjob_t *jobs, pthread_t *th, int *threaded)
{
    int k = 0;
    for (int e = 0; e < zo->neng; e++) {
        if (zo->eng[e].dead || zm->sl[e].nr == 0) continue;
        jobs[k] = (zjob_t){ zo, zm, e, nb, 0 };
        threaded[k] = pthread_create(&th[k], NULL, zrun, &jobs[k]) == 0;
        if (!threaded[k]) zrun(&jobs[k]);
        k++;
    }
    return k;
}

/* an engine failed during this call: its rows are redone here now and it is retired */
static void zretire(zoff_t *zo, int e, int rc)
{
    zeng_t *E = &zo->eng[e];
    E->dead = 1; E->fails++; zo->fails++;
    int alive = 0;
    for (int k = 0; k < zo->neng; k++) alive += !zo->eng[k].dead;
    fprintf(stderr, "zaccel: %s: %s -- that engine is retired, its rows are the CPU's now%s\n",
            E->name, zaccel_strerror(rc), alive ? "" : "; no engine left, offload off");
    if (!alive) zo->dead = 1;
}

int model_zaccel_matvec(model_t *m, const qten_t *w, const float *x, float *out, int n)
{
    zoff_t *zo = (zoff_t *)m->zo;
    if (!zo || zo->dead) return 0;
    zmat_t *zm = zfind(zo, w);
    if (!zm || zm->nz == 0 || n != (int)w->rows) return 0;
    zprep(zo, zm, x, 0);
    zjob_t jobs[ZMAX_ENG]; pthread_t th[ZMAX_ENG]; int threaded[ZMAX_ENG];
    const int k = zstart(zo, zm, 1, jobs, th, threaded);
    model_matvec_rows(m, w, x, out, (int)zm->nz, n);                 /* this CPU's rows, concurrently */
    for (int e = 0; e < zo->neng; e++)                               /* rows of engines retired earlier */
        if (zo->eng[e].dead && zm->sl[e].nr)
            model_matvec_rows(m, w, x, out, (int)zm->sl[e].r0, (int)(zm->sl[e].r0 + zm->sl[e].nr));
    for (int i = 0; i < k; i++) if (threaded[i]) pthread_join(th[i], NULL);
    zo->calls++;
    for (int i = 0; i < k; i++) {
        const zslice_t *s = &zm->sl[jobs[i].e];
        if (jobs[i].rc) {
            zretire(zo, jobs[i].e, jobs[i].rc);
            model_matvec_rows(m, w, x, out, (int)s->r0, (int)(s->r0 + s->nr));
        } else {
            for (uint32_t r = s->r0; r < s->r0 + s->nr; r++) out[r] = zo->facc[r] * zm->ws[r];
        }
    }
    return 1;
}

/* PREFILL: n positions, sent to the engines ZBATCH at a time, so each weight read from their DDR3
 * serves ZBATCH positions -- the engine's batch is exactly the reuse prefill has. Each position keeps
 * its own activation scale. out[j][r] layout as model_matmul_rows. The coordinator thread owns
 * xq / sxp (zprep) and facc's write-out; the per-engine workers own their slices. */
typedef struct { model_t *m; zoff_t *zo; zmat_t *zm; const float *X; int n; float *out; int rows; } zmm_t;
static void *zrun_mm(void *arg)
{
    zmm_t *j = (zmm_t *)arg;
    zoff_t *zo = j->zo; zmat_t *zm = j->zm;
    const uint32_t cols = zm->w->cols;
    for (int j0 = 0; j0 < j->n; j0 += ZBATCH) {
        const int nb = (j->n - j0) < ZBATCH ? (j->n - j0) : ZBATCH;
        for (int v = 0; v < nb; v++) zprep(zo, zm, j->X + (size_t)(j0 + v) * cols, v);
        zjob_t jobs[ZMAX_ENG]; pthread_t th[ZMAX_ENG]; int threaded[ZMAX_ENG];
        const int k = zstart(zo, zm, nb, jobs, th, threaded);
        for (int i = 0; i < k; i++) if (threaded[i]) pthread_join(th[i], NULL);
        for (int i = 0; i < k; i++) {
            const zslice_t *s = &zm->sl[jobs[i].e];
            if (jobs[i].rc) {
                zretire(zo, jobs[i].e, jobs[i].rc);
                model_matmul_rows(zm->w, j->X + (size_t)j0 * cols, nb, j->out + (size_t)j0 * j->rows, (int)s->r0, (int)(s->r0 + s->nr));
            } else {
                for (uint32_t r = s->r0; r < s->r0 + s->nr; r++)
                    for (int v = 0; v < nb; v++)
                        j->out[(size_t)(j0 + v) * j->rows + r] = zo->facc[(size_t)r * nb + v] * zm->ws[r];
            }
        }
    }
    return NULL;
}

int model_zaccel_matmul(model_t *m, const qten_t *w, const float *X, int n, float *out)
{
    zoff_t *zo = (zoff_t *)m->zo;
    if (!zo || zo->dead || n < 1) return 0;
    zmat_t *zm = zfind(zo, w);
    if (!zm || zm->nz == 0) return 0;
    zmm_t job = { m, zo, zm, X, n, out, (int)w->rows };
    pthread_t th;
    const int threaded = pthread_create(&th, NULL, zrun_mm, &job) == 0;
    if (!threaded) zrun_mm(&job);
    model_matmul_rows(w, X, n, out, (int)zm->nz, (int)w->rows);      /* this CPU's rows, concurrently */
    for (int e = 0; e < zo->neng; e++)                               /* rows of engines retired earlier */
        if (zo->eng[e].dead && zm->sl[e].nr)
            model_matmul_rows(w, X, n, out, (int)zm->sl[e].r0, (int)(zm->sl[e].r0 + zm->sl[e].nr));
    if (threaded) pthread_join(th, NULL);
    zo->calls++;
    return 1;
}

/* ------------------------------------------------------------------------------------------ */
static void each_matrix(model_t *m, void (*fn)(void *, const qten_t *), void *ctx)
{
    for (int l = 0; l < m->n_layer; l++) {
        mlayer_t *L = &m->L[l];
        const qten_t *all[7] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->w_gate, &L->w_up, &L->w_down };
        for (int i = 0; i < 7; i++)
            if (all[i]->raw && all[i]->rows && all[i]->cols) fn(ctx, all[i]);
    }
    /* the output head: read in full every token (docs/59), 28% of a 0.5B's multiply-adds and 10% of the
     * 3B's; its logits feed the argmax directly, so $ZACCEL_HEAD=0 keeps it exact on the CPU if a
     * model's ppl says so */
    if (env_flag("ZACCEL_HEAD", 1) && m->out_head.raw && m->out_head.rows && m->out_head.cols) fn(ctx, &m->out_head);
}
typedef struct { const qten_t **v; int n; uint64_t weights; uint32_t max_rows, max_cols; } collect_t;
static void collect(void *ctx, const qten_t *w)
{
    collect_t *c = (collect_t *)ctx;
    c->v[c->n++] = w;
    c->weights += (uint64_t)w->rows * w->cols;
    if (w->rows > c->max_rows) c->max_rows = w->rows;
    if (w->cols > c->max_cols) c->max_cols = w->cols;
}
static void count(void *ctx, const qten_t *w) { (void)w; (*(int *)ctx)++; }

/* nz that finishes both halves together: (n - nz) / rc = t0 + nz / rz, clamped to [0, n] */
static uint32_t best_nz(uint32_t n, double rc, double t0, double rz)
{
    double nz = ((double)n / rc - t0) / (1.0 / rc + 1.0 / rz);
    if (nz <= 0.0) return 0;
    if (nz >= n) return n;
    return (uint32_t)nz;
}

static size_t slice_bytes(uint32_t nr, uint32_t cols)
{
    return (size_t)nr * (wbits() == 4 ? (cols + 15) / 16 * 8 : (cols + 7) / 8 * 8);
}

static void zfree_all(zoff_t *zo)
{
    for (int i = 0; i < zo->n; i++) zdrop(zo, &zo->mats[i]);
    for (int e = 0; e < zo->neng; e++) { zaccel_close(zo->eng[e].z); free(zo->eng[e].tmp); free(zo->eng[e].blk); }
    free(zo->mats); free(zo->xq); free(zo->xr); free(zo->sxp); free(zo->facc);
    free(zo);
}

/* "auto" | "host" | "host:port" | "h1,h2:port,..." -> connected engines. Returns the count, 0 = none. */
static int zconnect_all(zoff_t *zo, const char *host, char *msg, int msglen)
{
    char list[256];
    snprintf(list, sizeof list, "%s", (host && *host && strcmp(host, "auto")) ? host : "");
    char *save = NULL;
    char *tok = list[0] ? strtok_r(list, ",", &save) : NULL;
    do {
        if (zo->neng == ZMAX_ENG) break;
        zeng_t *E = &zo->eng[zo->neng];
        char hbuf[128] = "";
        int port = 0;
        if (tok) {
            snprintf(hbuf, sizeof hbuf, "%s", tok);
            char *colon = strchr(hbuf, ':');
            if (colon && !strchr(colon + 1, ':')) { *colon = 0; port = atoi(colon + 1); }
        }
        E->z = zaccel_connect(hbuf[0] ? hbuf : NULL, port);
        if (!E->z) {
            snprintf(msg, msglen, "no engine at %s", tok ? tok : "10.20.0.2 / 10.77.0.2");
            if (tok) return 0;                             /* a named engine that is missing is an error */
            break;
        }
        if (zaccel_info(E->z, &E->in)) { snprintf(msg, msglen, "%s: INFO failed", tok ? tok : "engine"); zaccel_close(E->z); E->z = NULL; return 0; }
        snprintf(E->name, sizeof E->name, "%s", tok ? tok : zaccel_default_host());
        zo->neng++;
        tok = tok ? strtok_r(NULL, ",", &save) : NULL;
    } while (tok);
    return zo->neng;
}

long long model_zaccel_attach(model_t *m, const char *host, double share, char *msg, int msglen)
{
    snprintf(msg, msglen, "not attached");
    if (m->zo) model_zaccel_detach(m);
    zoff_t *zo = (zoff_t *)calloc(1, sizeof *zo);
    if (!zo) { snprintf(msg, msglen, "out of memory"); return -1; }
    if (!zconnect_all(zo, host, msg, msglen)) { zfree_all(zo); return -1; }

    int nmat = 0;
    each_matrix(m, count, &nmat);
    collect_t c = { (const qten_t **)calloc(nmat ? nmat : 1, sizeof(qten_t *)), 0, 0, 0, 0 };
    each_matrix(m, collect, &c);
    zo->cap = nmat;
    zo->mats = (zmat_t *)calloc(nmat ? nmat : 1, sizeof(zmat_t));
    zo->xq = (int8_t *)calloc((size_t)ZBATCH * c.max_cols + 8, 1);
    zo->xr = (float *)calloc((size_t)ZBATCH * c.max_cols + 8, sizeof(float));
    zo->sxp = (float *)calloc((size_t)ZBATCH * ZPARTS, sizeof(float));
    zo->facc = (float *)calloc((size_t)ZBATCH * c.max_rows + 1, sizeof(float));
    int ok = c.v && zo->mats && zo->xq && zo->xr && zo->sxp && zo->facc;
    for (int e = 0; e < zo->neng && ok; e++) {
        zo->eng[e].tmp = (int32_t *)calloc((size_t)ZBATCH * ZBAND_ROWS + 1, sizeof(int32_t));
        zo->eng[e].blk = (int8_t *)calloc((size_t)ZBATCH * ZCOLS, 1);
        ok = zo->eng[e].tmp && zo->eng[e].blk;
    }
    uint32_t *nzs = (uint32_t *)calloc((size_t)(nmat ? nmat : 1) * ZMAX_ENG, sizeof(uint32_t));   /* [mat][engine] */
    if (!ok || !nzs) { snprintf(msg, msglen, "out of memory"); free(nzs); free(c.v); zfree_all(zo); m->zo = NULL; return -1; }
    m->zo = NULL;                                          /* measurements below run CPU-only */

    /* ---- the split -------------------------------------------------------------------------- */
    double rc = 0, rz_sum = 0, t0_max = 0;
    if (share >= 0.0) {                                    /* a fixed share, equal slices */
        for (int i = 0; i < nmat; i++) {
            uint32_t nz = (uint32_t)(share > 1.0 ? c.v[i]->rows : share * c.v[i]->rows), done = 0;
            for (int e = 0; e < zo->neng; e++) { uint32_t k = (e == zo->neng - 1) ? nz - done : nz / zo->neng; nzs[i * ZMAX_ENG + e] = k; done += k; }
        }
    } else {
        /* the widest-output matrix of the first layer is the typical case: time it every way */
        const qten_t *w = c.v[0];
        for (int i = 0; i < nmat && i < 7; i++) if (c.v[i]->rows > w->rows && c.v[i]->cols <= ZCOLS) w = c.v[i];
        float *x = (float *)malloc(w->cols * sizeof(float)), *o = (float *)malloc(w->rows * sizeof(float));
        if (!x || !o) { snprintf(msg, msglen, "out of memory"); free(x); free(o); free(nzs); free(c.v); zfree_all(zo); return -1; }
        for (uint32_t k = 0; k < w->cols; k++) x[k] = (float)sin(0.37 * k);
        model_matvec_rows(m, w, x, o, 0, (int)w->rows);    /* warm */
        double ta = now_s();
        for (int it = 0; it < 3; it++) model_matvec_rows(m, w, x, o, 0, (int)w->rows);
        rc = 3.0 * w->rows / (now_s() - ta);
        /* each engine at two sizes: the difference is its rate, the rest its fixed cost per call */
        const uint32_t na = w->rows < 64 ? w->rows : 64, nb = w->rows < 1024 ? w->rows : 1024;
        for (int e = 0; e < zo->neng; e++) {
            uint32_t ka[ZMAX_ENG] = {0}, kb[ZMAX_ENG] = {0};
            ka[e] = na; kb[e] = nb;
            zmat_t za, zb; char e2[128];
            if (zupload(zo, w, ka, &za, e2, sizeof e2) || zupload(zo, w, kb, &zb, e2, sizeof e2)) {
                snprintf(msg, msglen, "calibration upload failed: %s", e2); free(x); free(o); free(nzs); free(c.v); zfree_all(zo); return -1;
            }
            double tA = 1e9, tB = 1e9;
            for (int it = 0; it < 5; it++) {
                zjob_t ja = { zo, &za, e, 1, 0 }, jb = { zo, &zb, e, 1, 0 };
                zprep(zo, &za, x, 0);
                double s0 = now_s(); zrun(&ja); double s1 = now_s(); zrun(&jb); double s2 = now_s();
                if (ja.rc || jb.rc) { snprintf(msg, msglen, "%s: calibration GEMV failed", zo->eng[e].name); free(x); free(o); free(nzs); free(c.v); zfree_all(zo); return -1; }
                if (s1 - s0 < tA) tA = s1 - s0;
                if (s2 - s1 < tB) tB = s2 - s1;
            }
            zdrop(zo, &za); zdrop(zo, &zb);
            zeng_t *E = &zo->eng[e];
            E->rz = (nb > na && tB > tA) ? (double)(nb - na) / (tB - tA) : 1e12;
            E->t0 = tA - na / E->rz; if (E->t0 < 0) E->t0 = 0;
            rz_sum += E->rz;
            if (E->t0 > t0_max) t0_max = E->t0;
        }
        /* the engines run at once: together they are one engine of rate sum(rz) and the slowest
         * round trip; each matrix's nz is split among them by rate. Rates scale with row width;
         * each 4096-column chunk of each band is a call. */
        for (int i = 0; i < nmat; i++) {
            const double k = (double)w->cols / c.v[i]->cols;
            const double calls = (double)((c.v[i]->cols + ZCOLS - 1) / ZCOLS) * (double)((c.v[i]->rows + ZBAND_ROWS - 1) / ZBAND_ROWS);
            const uint32_t nz = best_nz(c.v[i]->rows, rc * k, t0_max * calls, rz_sum * k);
            uint32_t done = 0;
            for (int e = 0; e < zo->neng; e++) {
                uint32_t part = (e == zo->neng - 1) ? nz - done : (uint32_t)(nz * (zo->eng[e].rz / rz_sum));
                nzs[i * ZMAX_ENG + e] = part; done += part;
            }
        }
        free(x); free(o);
    }
    /* each engine's free memory caps its own slices (int8: one byte a weight, rows padded to 8) */
    for (int e = 0; e < zo->neng; e++) {
        uint64_t need = 0, have = (uint64_t)zo->eng[e].in.mem_free_mb << 20;
        for (int i = 0; i < nmat; i++) need += slice_bytes(nzs[i * ZMAX_ENG + e], c.v[i]->cols);
        if (need > have * 95 / 100) {
            const double k = (double)(have * 95 / 100) / (double)need;
            for (int i = 0; i < nmat; i++) nzs[i * ZMAX_ENG + e] = (uint32_t)(nzs[i * ZMAX_ENG + e] * k);
        }
    }
    /* ---- upload ------------------------------------------------------------------------------ */
    long long weights = 0;
    for (int i = 0; i < nmat; i++) {
        char e2[128];
        int rc2 = zupload(zo, c.v[i], &nzs[i * ZMAX_ENG], &zo->mats[zo->n], e2, sizeof e2);
        if (rc2 > 0) break;                                /* an engine is full: stop here */
        if (rc2 < 0) { snprintf(msg, msglen, "%s", e2); free(nzs); free(c.v); zfree_all(zo); return -1; }
        weights += (long long)zo->mats[zo->n].nz * c.v[i]->cols;
        zo->n++;
    }
    free(nzs);
    m->zo = zo;
    {
        char engs[160] = "";
        for (int e = 0; e < zo->neng; e++) {
            char one[64];
            if (share >= 0.0) snprintf(one, sizeof one, "%s%s(%s)", e ? "+" : "", zo->eng[e].name, zo->eng[e].in.engine ? "pl" : "cpu");
            else snprintf(one, sizeof one, "%s%s(%s %.0f rows/s +%.0f us)", e ? "+" : "", zo->eng[e].name, zo->eng[e].in.engine ? "pl" : "cpu", zo->eng[e].rz, zo->eng[e].t0 * 1e6);
            strncat(engs, one, sizeof engs - strlen(engs) - 1);
        }
        const char *head = env_flag("ZACCEL_HEAD", 1) ? "+head" : "layers only";
        if (share >= 0.0)
            snprintf(msg, msglen, "%.1f%% of %llu weights on %d engine%s (share %.3f fixed, int%d per-row, Hadamard-rotated, %s): %s",
                     100.0 * weights / (double)c.weights, (unsigned long long)c.weights, zo->neng, zo->neng > 1 ? "s" : "", share, wbits(), head, engs);
        else
            snprintf(msg, msglen, "%.1f%% of %llu weights on %d engine%s (measured: CPU %.0f rows/s; int%d, %s): %s",
                     100.0 * weights / (double)c.weights, (unsigned long long)c.weights, zo->neng, zo->neng > 1 ? "s" : "", rc, wbits(), head, engs);
    }
    free(c.v);
    return weights;
}

void model_zaccel_detach(model_t *m)
{
    zoff_t *zo = (zoff_t *)m->zo;
    if (!zo) return;
    m->zo = NULL;
    zfree_all(zo);
}
