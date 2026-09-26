/* ===========================================================================================
 *  zaccel_offload.c -- the Zynq's matrix engine taking a share of the model's matrix-vector work
 * ===========================================================================================
 *
 *  Every dense matrix of every resident layer is split by rows: rows [0, nz) live on the Zynq,
 *  requantized to int8 with one scale per row, and rows [nz, n) stay on this CPU in their GGUF form.
 *  A matvec sends the activation (int8, one scale for the vector) to the Zynq, computes its own rows
 *  meanwhile, then joins: out[r] = acc[r] * wscale[r] * xscale for the Zynq's rows.
 *
 *  THE SPLIT IS MEASURED, NOT GUESSED. At attach time both sides are timed on a real matrix of the
 *  model: this CPU's rows per second, and the Zynq's fixed cost per call (network round trip) plus its
 *  rows per second. Each matrix then gets the nz that finishes both halves together, and 0 when the
 *  round trip alone costs more than the CPU needs for the whole matrix -- so the Zynq can only ever
 *  take work it finishes sooner than the CPU would. The Zynq's free memory caps the total.
 *
 *  OUTLIERS. A transformer's activations carry a few channels tens of times larger than the rest, so
 *  one int8 scale for a whole vector crushes every other value (measured on Qwen2.5-Coder 3B: 6-9%
 *  error per product, garbage after 36 layers). Both sides are therefore ROTATED by a block Hadamard
 *  matrix first -- the Zynq's weight rows once at upload, the activation on every call. H/sqrt(b) is
 *  orthonormal, so (H w).(H x)/b = w.x exactly, and an outlier is spread across its whole block
 *  before quantizing. (The QuaRot result; b is the largest power of two dividing the row, <= 4096.)
 *  Each engine column chunk also gets its own activation scale.
 *
 *  Rows wider than the engine's 4096 columns are split into column chunks; each chunk's integer
 *  partial sum is scaled by its own activation scale and the chunks add in float.
 *
 *  If the Zynq stops answering mid-run, that matvec's rows are recomputed here and the offload is
 *  switched off: the answer is always complete.
 *
 *  Numbers only count on the Orange Pi against the real Zynq. On a PC this checks correctness and the
 *  quality cost of the int8 requantization (tests/ppl.c), nothing else.
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

#define ZCOLS  4096          /* the engine's widest row (accel/SPEC.md §1) */
#define ZPARTS 8
#define ZBATCH 8            /* the engine's batch: one weight read serves 8 positions */

/* Weight width on the Zynq: 8 (default) or 4 ($ZACCEL_WBITS=4). int4 halves the bytes, so the engine
 * reads twice the weights per second and twice as many rows fit in its memory; whether the model can
 * afford it is a ppl.c question, answered per model, never assumed. */
static int wbits(void)
{
    static int b = 0;
    if (!b) { const char *e = getenv("ZACCEL_WBITS"); b = (e && atoi(e) == 4) ? 4 : 8; }
    return b;
}

typedef struct { zaccel_tensor_t t; uint32_t c0, nc; } zpart_t;
typedef struct {
    const qten_t *w;
    uint32_t nz;             /* rows [0, nz) are on the Zynq */
    int      nparts;
    zpart_t  part[ZPARTS];
    float   *ws;             /* per-row weight scale */
    int      hb;             /* Hadamard block: the largest power of two dividing cols, <= 4096 */
} zmat_t;

typedef struct {
    zaccel_t *z;
    zmat_t   *mats;
    int       n, cap;
    int8_t   *xq;            /* the rotated activations, int8: [ZBATCH][max_cols] */
    float    *xr;            /* the rotated activations, float: [ZBATCH][max_cols] */
    float    *sxp;           /* activation scale per vector per column chunk: [ZBATCH][ZPARTS] */
    int8_t   *blk;           /* one column chunk of each of them: [ZBATCH][ZCOLS] */
    float    *facc;          /* partial sums, scaled: [max_rows][ZBATCH] */
    int32_t  *acc, *tmp;
    size_t    rows_cap;
    int       dead;          /* the Zynq failed: everything back on the CPU */
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

/* rows [0, nz) of w, rotated, -> int8 with a scale per row, uploaded as ceil(cols/4096) column chunks */
static int zupload(zoff_t *zo, const qten_t *w, uint32_t nz, zmat_t *zm, char *msg, int msglen)
{
    memset(zm, 0, sizeof *zm);
    zm->w = w;
    zm->hb = hblock(w->cols);
    if (nz == 0) return 0;
    zm->ws = (float *)malloc(nz * sizeof(float));
    int8_t *q = (int8_t *)malloc((size_t)nz * w->cols);
    float *row = (float *)malloc(w->cols * sizeof(float));
    if (!zm->ws || !q || !row) { free(q); free(row); free(zm->ws); zm->ws = NULL; snprintf(msg, msglen, "out of memory"); return -1; }
    for (uint32_t r = 0; r < nz; r++) {
        gguf_dequant(w->type, w->raw + (size_t)r * w->row_bytes, w->cols, row);
        hrotate(row, w->cols, zm->hb);
        float amax = 0.0f;
        for (uint32_t c = 0; c < w->cols; c++) amax = fmaxf(amax, fabsf(row[c]));
        const int qmax = wbits() == 4 ? 7 : 127;
        const float s = amax > 0.0f ? amax / (float)qmax : 1.0f;
        zm->ws[r] = s;
        for (uint32_t c = 0; c < w->cols; c++) {
            long v = lrintf(row[c] / s);
            q[(size_t)r * w->cols + c] = (int8_t)(v > qmax ? qmax : v < -qmax ? -qmax : v);
        }
    }
    free(row);
    for (uint32_t c0 = 0; c0 < w->cols; c0 += ZCOLS) {
        const uint32_t nc = (w->cols - c0) < ZCOLS ? (w->cols - c0) : ZCOLS;
        if (zm->nparts == ZPARTS) { free(q); snprintf(msg, msglen, "row too wide (%u columns)", w->cols); return -1; }
        const uint32_t mode = wbits() == 4 ? ZACCEL_MODE_INT4 : ZACCEL_MODE_INT8;
        const size_t rb = zaccel_row_bytes(mode, nc);
        uint8_t *packed = (uint8_t *)calloc((size_t)nz, rb);
        int8_t *sub = (int8_t *)malloc((size_t)nz * nc);
        if (!packed || !sub) {
            free(packed); free(sub); free(q);
            for (int k = 0; k < zm->nparts; k++) zaccel_free(zo->z, &zm->part[k].t);
            free(zm->ws); memset(zm, 0, sizeof *zm);
            snprintf(msg, msglen, "out of memory"); return -1;
        }
        for (uint32_t r = 0; r < nz; r++) memcpy(sub + (size_t)r * nc, q + (size_t)r * w->cols + c0, nc);
        if (mode == ZACCEL_MODE_INT4) zaccel_pack_int4(sub, nz, nc, packed);
        else zaccel_pack_int8(sub, nz, nc, packed);
        zpart_t *p = &zm->part[zm->nparts];
        int rc = zaccel_load(zo->z, mode, nz, nc, packed, &p->t);
        free(packed); free(sub);
        if (rc) {                          /* release the chunks this matrix already has on the Zynq */
            free(q);
            snprintf(msg, msglen, "LOAD failed: %s", zaccel_strerror(rc));
            for (int k = 0; k < zm->nparts; k++) zaccel_free(zo->z, &zm->part[k].t);
            free(zm->ws);
            memset(zm, 0, sizeof *zm);
            return rc == ZACCEL_ST_NOMEM ? 1 : -1;
        }
        p->c0 = c0; p->nc = nc;
        zm->nparts++;
    }
    free(q);
    zm->nz = nz;
    int hb = zm->hb;                               /* a whole number of blocks per chunk */
    for (int i = 0; i < zm->nparts; i++) if (zm->part[i].nc % (uint32_t)hb) zm->hb = 1;
    if (zm->hb != hb) { snprintf(msg, msglen, "internal: Hadamard block %d straddles a column chunk", hb); return -1; }
    return 0;
}

static void zdrop(zoff_t *zo, zmat_t *zm)
{
    for (int i = 0; i < zm->nparts; i++) zaccel_free(zo->z, &zm->part[i].t);
    free(zm->ws);
    memset(zm, 0, sizeof *zm);
}

/* activation vector v of this matrix: rotate a copy, then int8 per column chunk, each its own scale */
static void zprep(zoff_t *zo, const zmat_t *zm, const float *x, int v)
{
    const uint32_t cols = zm->w->cols;
    float *xr = zo->xr + (size_t)v * cols;
    int8_t *q = zo->xq + (size_t)v * cols;
    memcpy(xr, x, cols * sizeof(float));
    hrotate(xr, cols, zm->hb);
    for (int i = 0; i < zm->nparts; i++) {
        const uint32_t c0 = zm->part[i].c0, nc = zm->part[i].nc;
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

/* the Zynq's half of one matvec: acc[0..nz) = sum over column chunks */
typedef struct { zoff_t *zo; zmat_t *zm; int rc; } zjob_t;
static void *zrun(void *arg)
{
    zjob_t *j = (zjob_t *)arg;
    zoff_t *zo = j->zo; zmat_t *zm = j->zm;
    memset(zo->facc, 0, zm->nz * sizeof(float));
    for (int i = 0; i < zm->nparts; i++) {
        int rc = zaccel_gemv(zo->z, &zm->part[i].t, 1, zo->xq + zm->part[i].c0, zo->tmp, NULL, NULL);
        if (rc) { j->rc = rc; return NULL; }
        const float sc = zo->sxp[i];
        for (uint32_t r = 0; r < zm->nz; r++) zo->facc[r] += (float)zo->tmp[r] * sc;
    }
    j->rc = 0;
    return NULL;
}

int model_zaccel_matvec(model_t *m, const qten_t *w, const float *x, float *out, int n)
{
    zoff_t *zo = (zoff_t *)m->zo;
    if (!zo || zo->dead) return 0;
    zmat_t *zm = zfind(zo, w);
    if (!zm || zm->nz == 0 || n != (int)w->rows) return 0;
    zprep(zo, zm, x, 0);
    zjob_t job = { zo, zm, 0 };
    pthread_t th;
    const int threaded = pthread_create(&th, NULL, zrun, &job) == 0;
    if (!threaded) zrun(&job);
    model_matvec_rows(m, w, x, out, (int)zm->nz, n);        /* this CPU's rows, concurrently */
    if (threaded) pthread_join(th, NULL);
    zo->calls++;
    if (job.rc) {                                           /* the Zynq failed: redo its rows here */
        zo->fails++;
        zo->dead = 1;
        fprintf(stderr, "zaccel: %s -- offload off, the CPU takes every row from here\n", zaccel_strerror(job.rc));
        model_matvec_rows(m, w, x, out, 0, (int)zm->nz);
        return 1;
    }
    for (uint32_t r = 0; r < zm->nz; r++) out[r] = zo->facc[r] * zm->ws[r];
    return 1;
}

/* PREFILL: n positions, sent to the Zynq ZBATCH at a time, so each weight read from its DDR3 serves
 * ZBATCH positions -- the engine's batch is exactly the reuse prefill has. Each position keeps its own
 * activation scale. out[j][r] layout as model_matmul_rows. */
typedef struct { zoff_t *zo; zmat_t *zm; const float *X; int n; float *out; int rows; int rc; } zmm_t;
static void *zrun_mm(void *arg)
{
    zmm_t *j = (zmm_t *)arg;
    zoff_t *zo = j->zo; zmat_t *zm = j->zm;
    const uint32_t cols = zm->w->cols, nz = zm->nz;
    for (int j0 = 0; j0 < j->n; j0 += ZBATCH) {
        const int nb = (j->n - j0) < ZBATCH ? (j->n - j0) : ZBATCH;
        for (int v = 0; v < nb; v++) zprep(zo, zm, j->X + (size_t)(j0 + v) * cols, v);
        memset(zo->facc, 0, (size_t)nz * nb * sizeof(float));
        for (int i = 0; i < zm->nparts; i++) {
            const uint32_t c0 = zm->part[i].c0, nc = zm->part[i].nc;
            for (int v = 0; v < nb; v++) memcpy(zo->blk + (size_t)v * nc, zo->xq + (size_t)v * cols + c0, nc);
            int rc = zaccel_gemv(zo->z, &zm->part[i].t, (uint32_t)nb, zo->blk, zo->tmp, NULL, NULL);
            if (rc) { j->rc = rc; return NULL; }
            for (uint32_t r = 0; r < nz; r++)                                           /* Y[r][v] */
                for (int v = 0; v < nb; v++)
                    zo->facc[(size_t)r * nb + v] += (float)zo->tmp[(size_t)r * nb + v] * zo->sxp[v * ZPARTS + i];
        }
        for (uint32_t r = 0; r < nz; r++)
            for (int v = 0; v < nb; v++)
                j->out[(size_t)(j0 + v) * j->rows + r] = zo->facc[(size_t)r * nb + v] * zm->ws[r];
    }
    j->rc = 0;
    return NULL;
}

int model_zaccel_matmul(model_t *m, const qten_t *w, const float *X, int n, float *out)
{
    zoff_t *zo = (zoff_t *)m->zo;
    if (!zo || zo->dead || n < 1) return 0;
    zmat_t *zm = zfind(zo, w);
    if (!zm || zm->nz == 0) return 0;
    zmm_t job = { zo, zm, X, n, out, (int)w->rows, 0 };
    pthread_t th;
    const int threaded = pthread_create(&th, NULL, zrun_mm, &job) == 0;
    if (!threaded) zrun_mm(&job);
    model_matmul_rows(w, X, n, out, (int)zm->nz, (int)w->rows);    /* this CPU's rows, concurrently */
    if (threaded) pthread_join(th, NULL);
    zo->calls++;
    if (job.rc) {
        zo->fails++;
        zo->dead = 1;
        fprintf(stderr, "zaccel: %s -- offload off, the CPU takes every row from here\n", zaccel_strerror(job.rc));
        model_matmul_rows(w, X, n, out, 0, (int)zm->nz);
    }
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

long long model_zaccel_attach(model_t *m, const char *host, double share, char *msg, int msglen)
{
    snprintf(msg, msglen, "not attached");
    if (m->zo) model_zaccel_detach(m);
    zoff_t *zo = (zoff_t *)calloc(1, sizeof *zo);
    if (!zo) { snprintf(msg, msglen, "out of memory"); return -1; }
    char hbuf[128] = "";
    int port = 0;
    if (host && *host) {                                   /* "host" or "host:port" */
        snprintf(hbuf, sizeof hbuf, "%s", host);
        char *colon = strchr(hbuf, ':');
        if (colon && !strchr(colon + 1, ':')) { *colon = 0; port = atoi(colon + 1); }
    }
    zo->z = zaccel_connect(hbuf[0] ? hbuf : NULL, port);
    if (!zo->z) { snprintf(msg, msglen, "no Zynq at %s", host && *host ? host : "10.20.0.2 / 10.77.0.2"); free(zo); return -1; }
    zaccel_info_t in;
    if (zaccel_info(zo->z, &in)) { snprintf(msg, msglen, "INFO failed"); zaccel_close(zo->z); free(zo); return -1; }

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
    zo->blk = (int8_t *)calloc((size_t)ZBATCH * ZCOLS, 1);
    zo->acc = (int32_t *)calloc((size_t)ZBATCH * c.max_rows + 1, sizeof(int32_t));
    zo->tmp = (int32_t *)calloc((size_t)ZBATCH * c.max_rows + 1, sizeof(int32_t));
    if (!c.v || !zo->mats || !zo->xq || !zo->xr || !zo->sxp || !zo->facc || !zo->blk || !zo->acc || !zo->tmp) { snprintf(msg, msglen, "out of memory"); goto fail; }
    m->zo = NULL;                                          /* measurements below run CPU-only */

    /* ---- the split -------------------------------------------------------------------------- */
    double rc = 0, t0 = 0, rz = 0;
    uint32_t *nzs = (uint32_t *)calloc(nmat ? nmat : 1, sizeof(uint32_t));
    if (!nzs) { snprintf(msg, msglen, "out of memory"); goto fail; }
    if (share >= 0.0) {
        for (int i = 0; i < nmat; i++) nzs[i] = (uint32_t)(share > 1.0 ? c.v[i]->rows : share * c.v[i]->rows);
    } else {
        /* the widest-output matrix of the first layer is the typical case: time it both ways */
        const qten_t *w = c.v[0];
        for (int i = 0; i < nmat && i < 7; i++) if (c.v[i]->rows > w->rows && c.v[i]->cols <= ZCOLS) w = c.v[i];
        float *x = (float *)malloc(w->cols * sizeof(float)), *o = (float *)malloc(w->rows * sizeof(float));
        for (uint32_t k = 0; k < w->cols; k++) x[k] = (float)sin(0.37 * k);
        model_matvec_rows(m, w, x, o, 0, (int)w->rows);    /* warm */
        double ta = now_s();
        for (int it = 0; it < 3; it++) model_matvec_rows(m, w, x, o, 0, (int)w->rows);
        rc = 3.0 * w->rows / (now_s() - ta);
        /* the Zynq at two sizes: the difference is its rate, the rest is the fixed cost per call */
        uint32_t na = w->rows < 64 ? w->rows : 64, nb = w->rows < 1024 ? w->rows : 1024;
        zmat_t za, zb; char e2[128];
        if (zupload(zo, w, na, &za, e2, sizeof e2) || zupload(zo, w, nb, &zb, e2, sizeof e2)) {
            snprintf(msg, msglen, "calibration upload failed: %s", e2); free(x); free(o); free(nzs); goto fail;
        }
        double tA = 1e9, tB = 1e9;
        for (int it = 0; it < 5; it++) {
            zjob_t ja = { zo, &za, 0 }, jb = { zo, &zb, 0 };
            zprep(zo, &za, x, 0);
            double s0 = now_s(); zrun(&ja); double s1 = now_s(); zrun(&jb); double s2 = now_s();
            if (ja.rc || jb.rc) { snprintf(msg, msglen, "calibration GEMV failed"); free(x); free(o); free(nzs); goto fail; }
            if (s1 - s0 < tA) tA = s1 - s0;
            if (s2 - s1 < tB) tB = s2 - s1;
        }
        zdrop(zo, &za); zdrop(zo, &zb);
        rz = (nb > na && tB > tA) ? (double)(nb - na) / (tB - tA) : 1e12;
        t0 = tA - na / rz; if (t0 < 0) t0 = 0;
        for (int i = 0; i < nmat; i++) {       /* rates scale with row width; each 4096-column chunk is a call */
            const double k = (double)w->cols / c.v[i]->cols;
            nzs[i] = best_nz(c.v[i]->rows, rc * k, t0 * ((c.v[i]->cols + ZCOLS - 1) / ZCOLS), rz * k);
        }
        free(x); free(o);
    }
    /* the Zynq's free memory caps the total (int8: one byte a weight, rows padded to 8) */
    {
        uint64_t need = 0, have = (uint64_t)in.mem_free_mb << 20;
        for (int i = 0; i < nmat; i++)
            need += (uint64_t)nzs[i] * (wbits() == 4 ? (c.v[i]->cols + 15) / 16 * 8 : (c.v[i]->cols + 7) / 8 * 8);
        if (need > have * 95 / 100) {
            const double k = (double)(have * 95 / 100) / (double)need;
            for (int i = 0; i < nmat; i++) nzs[i] = (uint32_t)(nzs[i] * k);
        }
    }
    /* ---- upload ------------------------------------------------------------------------------ */
    long long weights = 0;
    for (int i = 0; i < nmat; i++) {
        char e2[128];
        int rc2 = zupload(zo, c.v[i], nzs[i], &zo->mats[zo->n], e2, sizeof e2);
        if (rc2 > 0) break;                                /* the Zynq is full: stop here */
        if (rc2 < 0) { snprintf(msg, msglen, "%s", e2); free(nzs); goto fail; }
        weights += (long long)zo->mats[zo->n].nz * c.v[i]->cols;
        zo->n++;
    }
    free(nzs);
    free(c.v);
    m->zo = zo;
    if (share >= 0.0)
        snprintf(msg, msglen, "%.1f%% of %llu weights on the Zynq (share %.3f fixed, engine %s, int%d per-row, Hadamard-rotated)",
                 100.0 * weights / (double)c.weights, (unsigned long long)c.weights, share, in.engine ? "pl" : "cpu", wbits());
    else
        snprintf(msg, msglen, "%.1f%% of %llu weights on the Zynq (measured: CPU %.0f rows/s, Zynq %.0f rows/s + %.0f us/call; engine %s, int%d)",
                 100.0 * weights / (double)c.weights, (unsigned long long)c.weights, rc, rz, t0 * 1e6, in.engine ? "pl" : "cpu", wbits());
    return weights;
fail:
    for (int i = 0; i < zo->n; i++) zdrop(zo, &zo->mats[i]);
    free(c.v); free(zo->mats); free(zo->xq); free(zo->xr); free(zo->sxp); free(zo->facc); free(zo->blk); free(zo->acc); free(zo->tmp);
    zaccel_close(zo->z); free(zo);
    m->zo = NULL;
    return -1;
}

void model_zaccel_detach(model_t *m)
{
    zoff_t *zo = (zoff_t *)m->zo;
    if (!zo) return;
    m->zo = NULL;
    if (!zo->dead) for (int i = 0; i < zo->n; i++) zdrop(zo, &zo->mats[i]);
    else for (int i = 0; i < zo->n; i++) free(zo->mats[i].ws);
    free(zo->mats); free(zo->xq); free(zo->xr); free(zo->sxp); free(zo->facc); free(zo->blk); free(zo->acc); free(zo->tmp);
    zaccel_close(zo->z);
    free(zo);
}
