/* test_kernels.c -- the offload against the CPU on the SAME matrices and inputs, element by element,
 * for decode (matvec) and prefill (matmul, n positions). Separates a bug (large, structured error)
 * from the approximation (small, spread error).
 *   test_kernels <model.gguf> HOST:PORT [share]            (host build, correctness only) */
#include "gguf.h"
#include "model_q.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double rel_err(const float *a, const float *b, size_t n, double *maxabs)
{
    double num = 0, den = 0; *maxabs = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)a[i] - b[i];
        num += d * d; den += (double)a[i] * a[i];
        if (fabs(d) > *maxabs) *maxabs = fabs(d);
    }
    return den > 0 ? sqrt(num / den) : sqrt(num);
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("test_kernels <model.gguf> HOST:PORT [share]\n"); return 2; }
    const double share = argc > 3 ? atof(argv[3]) : 0.5;
    gguf_t g;
    if (gguf_open(&g, argv[1])) { printf("open: %s\n", g.err); return 1; }
    model_t m;
    if (model_load(&m, &g, 64)) { printf("load: %s\n", g.err); return 1; }
    model_set_fast(&m, 1);
    char msg[256];
    if (model_zaccel_attach(&m, argv[2], share, msg, sizeof msg) < 0) { printf("attach: %s\n", msg); return 1; }
    printf("attach: %s\n", msg);
    const qten_t *ws[] = { &m.L[0].wq, &m.L[0].w_up, &m.L[0].w_down, &m.L[17].wk };
    const char *nm[] = { "L0 wq", "L0 w_up", "L0 w_down (3 column chunks)", "L17 wk" };
    int bad = 0;
    srand(7);
    for (int t = 0; t < 4; t++) {
        const qten_t *w = ws[t];
        const int n = 13;                                  /* odd, > 8: two engine batches */
        float *X = malloc(sizeof(float) * n * w->cols);
        for (size_t i = 0; i < (size_t)n * w->cols; i++) X[i] = (float)((rand() / (double)RAND_MAX - 0.5) * 2.0);
        X[5] = 40.0f;                                      /* one outlier channel, as real activations have */
        float *ref = malloc(sizeof(float) * n * w->rows), *got = malloc(sizeof(float) * n * w->rows);
        /* decode: one vector */
        model_matvec_rows(&m, w, X, ref, 0, (int)w->rows);
        if (!model_zaccel_matvec(&m, w, X, got, (int)w->rows)) { printf("%s: not offloaded\n", nm[t]); bad = 1; continue; }
        double mx, e = rel_err(ref, got, w->rows, &mx);
        printf("%-28s decode  rel err %.4f  max |d| %.4f\n", nm[t], e, mx);
        if (e > 0.05) bad = 1;
        /* prefill: n positions */
        model_matmul_rows(w, X, n, ref, 0, (int)w->rows);
        if (!model_zaccel_matmul(&m, w, X, n, got)) { printf("%s: prefill not offloaded\n", nm[t]); bad = 1; continue; }
        e = rel_err(ref, got, (size_t)n * w->rows, &mx);
        printf("%-28s prefill rel err %.4f  max |d| %.4f\n", nm[t], e, mx);
        if (e > 0.05) bad = 1;
        free(X); free(ref); free(got);
    }
    model_free(&m);
    printf(bad ? "KERNELS: FAIL\n" : "KERNELS: PASS\n");
    return bad;
}
