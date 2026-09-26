/* zaccel-bench -- does the Zynq make the Orange Pi faster?  (contract: accel/SPEC.md §5)
 *
 * For each shape: build random weights and activations, compute the reference, then
 *   (1) Pi CPU alone  -- every core, NEON SDOT (zaccel_cpu)
 *   (2) Zynq alone    -- weights resident on the Zynq, activations over the wire (libzaccel)
 *   (3) both at once  -- rows [0,s) on the Pi's cores while rows [s,N) run on the Zynq; s starts
 *                        from the rates of (1) and (2) and is re-tuned from the split's own timings
 * Every answer, timed or not, is compared bit for bit with zaccel_ref_gemv() before it counts.
 *
 *   zaccel-bench [-H host] [-p port] [-t threads] [-r reps] [-n] [-s ROWSxCOLS:int4|int8:NB]...
 *
 * NUMBERS ONLY COUNT when this runs ON THE ORANGE PI 4 PRO against the REAL ZYNQ with the PL engine.
 * Exit: 0 every answer exact, 1 a wrong answer, 2 the Zynq was unreachable or failed, 3 usage.
 */
#define _POSIX_C_SOURCE 200809L

#include "libzaccel.h"
#include "zaccel_cpu.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { uint32_t rows, cols, mode, nb; } shape_t;

typedef struct {
    shape_t sh;
    int pi_ok, z_ok, both_ok;          /* measured and every answer exact */
    double pi_ms, z_ms, both_ms, pl_ms;
    uint32_t split, engine;
} result_t;

static long g_checks, g_wrong;
static int  g_zynq_failed, g_saw_cpu_fallback, g_saw_pl;

/* ---------------------------------------------------------------- small helpers */
static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static double median(double *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_d);
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* Compare with the reference; report the first difference. */
static int check(const char *who, const int32_t *y, const int32_t *ref, size_t n, uint32_t nb)
{
    g_checks++;
    if (memcmp(y, ref, n * sizeof *y) == 0) return 1;
    g_wrong++;
    for (size_t i = 0; i < n; i++)
        if (y[i] != ref[i]) {
            printf("  MISMATCH  %-10s row %zu vec %zu: got %d, reference %d\n",
                   who, i / nb, i % nb, y[i], ref[i]);
            break;
        }
    return 0;
}

static void poison(int32_t *y, size_t n) { memset(y, 0xA5, n * sizeof *y); }

static const char *mode_name(uint32_t m) { return m == ZACCEL_MODE_INT4 ? "int4" : "int8"; }

/* How many Cortex-A76 (0xd0b) and Cortex-A55 (0xd05) cores does /proc/cpuinfo list? */
static void cpu_parts(int *a76, int *a55, int *other)
{
    *a76 = *a55 = *other = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "CPU part", 8) != 0) continue;
        char *c = strchr(line, ':');
        if (!c) continue;
        unsigned long part = strtoul(c + 1, NULL, 0);
        if (part == 0xd0b) (*a76)++; else if (part == 0xd05) (*a55)++; else (*other)++;
    }
    fclose(f);
}

/* ---------------------------------------------------------------- the Zynq side of "both" */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int go, done, quit;
    zaccel_t *z;
    zaccel_tensor_t t;
    uint32_t nb;
    const int8_t *A;
    int32_t *Y;
    int rc;
    uint32_t engine;
    double t_end;
} net_t;

static void *net_main(void *arg)
{
    net_t *n = arg;
    pthread_mutex_lock(&n->mu);
    for (;;) {
        while (!n->go && !n->quit) pthread_cond_wait(&n->cv, &n->mu);
        if (n->quit) break;
        n->go = 0;
        pthread_mutex_unlock(&n->mu);
        uint32_t eng = 0;
        int rc = zaccel_gemv(n->z, &n->t, n->nb, n->A, n->Y, NULL, &eng);
        double te = now();
        pthread_mutex_lock(&n->mu);
        n->rc = rc; n->engine = eng; n->t_end = te; n->done = 1;
        pthread_cond_broadcast(&n->cv);
    }
    pthread_mutex_unlock(&n->mu);
    return NULL;
}

/* One job split at row s: Pi rows [0,s), Zynq rows [s,N) (already loaded as n->t). */
static int both_once(net_t *n, zaccel_cpu_t *cpu, const shape_t *sh, uint32_t s,
                     const uint8_t *P, const int8_t *A, int32_t *Y,
                     double *t_pi, double *t_z, double *t_all)
{
    double t0 = now();
    pthread_mutex_lock(&n->mu);
    n->done = 0; n->go = 1;
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
    zaccel_cpu_gemv(cpu, sh->mode, s, sh->cols, P, sh->nb, A, Y);
    double tp = now();
    pthread_mutex_lock(&n->mu);
    while (!n->done) pthread_cond_wait(&n->cv, &n->mu);
    pthread_mutex_unlock(&n->mu);
    double te = now();
    *t_pi = tp - t0; *t_z = n->t_end - t0; *t_all = te - t0;
    return n->rc;
}

/* ---------------------------------------------------------------- one shape */
static void bench_shape(result_t *res, const shape_t *sh, int reps, zaccel_cpu_t *cpu,
                        zaccel_t **zp, uint64_t seed)
{
    const uint32_t N = sh->rows, K = sh->cols, nb = sh->nb;
    const size_t rb = zaccel_row_bytes(sh->mode, K), ny = (size_t)N * nb;
    const double macs = (double)N * K * nb;
    memset(res, 0, sizeof *res);
    res->sh = *sh;

    printf("\n== %ux%u %s nb=%u   (%.1f M MAC per job, weights %.2f MB, seed %llu)\n",
           N, K, mode_name(sh->mode), nb, macs / 1e6, (double)N * rb / 1048576.0,
           (unsigned long long)seed);

    int8_t  *W = malloc((size_t)N * K);
    uint8_t *P = malloc((size_t)N * rb);
    int8_t  *A = malloc((size_t)nb * K);
    int32_t *R = malloc(ny * sizeof *R), *Y = malloc(ny * sizeof *Y);
    double  *tv = malloc((size_t)(reps > 8 ? reps : 8) * 3 * sizeof *tv);
    if (!W || !P || !A || !R || !Y || !tv) {
        printf("  out of memory for this shape\n");
        goto out;
    }
    rng_state = seed ? seed : 1;
    for (size_t i = 0; i < (size_t)N * K; i++)
        W[i] = sh->mode == ZACCEL_MODE_INT4 ? (int8_t)((int)(rnd() % 16) - 8)
                                            : (int8_t)((int)(rnd() % 256) - 128);
    for (size_t i = 0; i < (size_t)nb * K; i++) A[i] = (int8_t)((int)(rnd() % 256) - 128);
    if (sh->mode == ZACCEL_MODE_INT4) zaccel_pack_int4(W, N, K, P);
    else                              zaccel_pack_int8(W, N, K, P);
    zaccel_ref_gemv(sh->mode, N, K, P, nb, A, R);

    /* (1) Pi CPU alone ------------------------------------------------------------------ */
    poison(Y, ny);
    zaccel_cpu_gemv(cpu, sh->mode, N, K, P, nb, A, Y);
    if (!check("Pi CPU", Y, R, ny, nb)) {
        printf("  Pi CPU    WRONG ANSWER -- not timed\n");
    } else {
        int good = 1;
        for (int i = 0; i < reps; i++) {
            poison(Y, ny);
            double t = now();
            zaccel_cpu_gemv(cpu, sh->mode, N, K, P, nb, A, Y);
            tv[i] = now() - t;
            good &= check("Pi CPU", Y, R, ny, nb);
        }
        if (good) { res->pi_ok = 1; res->pi_ms = median(tv, reps) * 1e3; }
        printf("  check  Pi CPU alone ....... %s\n", good ? "bit-exact" : "WRONG during timing");
    }

    /* (2) Zynq alone --------------------------------------------------------------------- */
    zaccel_tensor_t tf;
    int rc;
    if (*zp) {
        rc = zaccel_load(*zp, sh->mode, N, K, P, &tf);
        if (rc) {
            printf("  Zynq LOAD failed: %s\n", zaccel_strerror(rc));
            g_zynq_failed = 1;
        } else {
            uint32_t cyc = 0, eng = 0;
            poison(Y, ny);
            rc = zaccel_gemv(*zp, &tf, nb, A, Y, &cyc, &eng);
            if (rc) {
                printf("  Zynq GEMV failed: %s\n", zaccel_strerror(rc));
                g_zynq_failed = 1;
            } else if (!check("Zynq", Y, R, ny, nb)) {
                printf("  Zynq      WRONG ANSWER -- not timed\n");
            } else {
                int good = 1;
                double *cv = tv + reps;
                for (int i = 0; i < reps && good && rc == 0; i++) {
                    poison(Y, ny);
                    double t = now();
                    rc = zaccel_gemv(*zp, &tf, nb, A, Y, &cyc, &eng);
                    tv[i] = now() - t;
                    cv[i] = cyc / 100e6;                  /* FCLK0 = 100 MHz (SPEC §1) */
                    if (rc == 0) good &= check("Zynq", Y, R, ny, nb);
                }
                if (rc) { printf("  Zynq GEMV failed: %s\n", zaccel_strerror(rc)); g_zynq_failed = 1; }
                else if (good) {
                    res->z_ok = 1; res->z_ms = median(tv, reps) * 1e3;
                    res->pl_ms = median(cv, reps) * 1e3;
                }
                res->engine = eng;
                if (eng == 1) g_saw_pl = 1; else g_saw_cpu_fallback = 1;
                printf("  check  Zynq alone ......... %s  (engine_used=%s)\n",
                       rc ? "failed" : good ? "bit-exact" : "WRONG during timing",
                       eng == 1 ? "PL" : "CPU FALLBACK, not the PL");
            }
            zaccel_free(*zp, &tf);   /* a dead connection is caught by the ping after the shape */
        }
    }

    /* (3) both at once ------------------------------------------------------------------ */
    if (res->pi_ok && res->z_ok && N >= 2 && *zp) {
        double rp = N / res->pi_ms, rz = N / res->z_ms;
        uint32_t s = (uint32_t)(N * rp / (rp + rz) + 0.5);
        net_t n;
        memset(&n, 0, sizeof n);
        pthread_mutex_init(&n.mu, NULL);
        pthread_cond_init(&n.cv, NULL);
        n.z = *zp; n.nb = nb; n.A = A;
        pthread_t th;
        if (pthread_create(&th, NULL, net_main, &n) != 0) {
            printf("  no thread for the split\n");
            pthread_mutex_destroy(&n.mu);
            pthread_cond_destroy(&n.cv);
            goto report;
        }
        int good = 1, loaded = 0;
        double tp[16], tz[16], ta[16];
        for (int round = 0; round < 5 && good; round++) {
            if (s < 1) s = 1;
            if (s > N - 1) s = N - 1;
            rc = zaccel_load(*zp, sh->mode, N - s, K, P + (size_t)s * rb, &n.t);
            if (rc) { printf("  Zynq LOAD (split) failed: %s\n", zaccel_strerror(rc)); g_zynq_failed = 1; good = 0; break; }
            loaded = 1;
            n.Y = Y + (size_t)s * nb;
            for (int i = 0; i < 3 && good; i++) {
                poison(Y, ny);
                rc = both_once(&n, cpu, sh, s, P, A, Y, &tp[i], &tz[i], &ta[i]);
                if (rc) { printf("  Zynq GEMV (split) failed: %s\n", zaccel_strerror(rc)); g_zynq_failed = 1; good = 0; }
                else good &= check("both", Y, R, ny, nb);
            }
            if (!good) break;
            double mp = median(tp, 3), mz = median(tz, 3);
            double np_ = s / mp, nz = (N - s) / mz;
            uint32_t s2 = (uint32_t)(N * np_ / (np_ + nz) + 0.5);
            if (s2 < 1) s2 = 1;
            if (s2 > N - 1) s2 = N - 1;
            uint32_t tol = N / 256 ? N / 256 : 1;
            if ((s2 > s ? s2 - s : s - s2) <= tol || round == 4) break;
            zaccel_free(*zp, &n.t);
            loaded = 0;
            s = s2;
        }
        if (good && loaded) {
            for (int i = 0; i < reps && good; i++) {
                poison(Y, ny);
                double a, b;
                rc = both_once(&n, cpu, sh, s, P, A, Y, &a, &b, &tv[i]);
                if (rc) { printf("  Zynq GEMV (split) failed: %s\n", zaccel_strerror(rc)); g_zynq_failed = 1; good = 0; }
                else good &= check("both", Y, R, ny, nb);
            }
            if (good) { res->both_ok = 1; res->both_ms = median(tv, reps) * 1e3; res->split = s; }
            printf("  check  both at once ....... %s  (Pi rows 0..%u, Zynq rows %u..%u)\n",
                   good ? "bit-exact" : "WRONG or failed", s - 1, s, N - 1);
        }
        if (loaded) zaccel_free(*zp, &n.t);
        pthread_mutex_lock(&n.mu);
        n.quit = 1;
        pthread_cond_broadcast(&n.cv);
        pthread_mutex_unlock(&n.mu);
        pthread_join(th, NULL);
        pthread_mutex_destroy(&n.mu);
        pthread_cond_destroy(&n.cv);
    }

    /* report -------------------------------------------------------------------------------- */
report:
    printf("                              ms/job    G MAC/s   vs Pi alone\n");
    if (res->pi_ok)
        printf("  Pi CPU alone            %10.3f %10.2f      1.00x\n", res->pi_ms, macs / res->pi_ms / 1e6);
    if (res->z_ok) {
        printf("  Zynq alone              %10.3f %10.2f %9.2fx", res->z_ms, macs / res->z_ms / 1e6,
               res->pi_ok ? res->pi_ms / res->z_ms : 0.0);
        if (res->engine == 1) printf("   (PL busy %.3f ms of it)\n", res->pl_ms);
        else                  printf("   (Zynq CPU fallback, NOT the PL)\n");
    }
    if (res->both_ok) {
        printf("  both, split %5u/%-5u %10.3f %10.2f %9.2fx   <- the boost\n", res->split,
               N - res->split, res->both_ms, macs / res->both_ms / 1e6, res->pi_ms / res->both_ms);
        if (res->both_ms > res->pi_ms)
            printf("  note: at this shape the split is SLOWER than the Pi alone: even one row's round\n"
                   "        trip to the Zynq outlasts the Pi's whole job, so the Zynq should get no rows\n");
    }
out:
    free(W); free(P); free(A); free(R); free(Y); free(tv);
}

/* ---------------------------------------------------------------- main */
static int parse_shape(const char *s, shape_t *sh)
{
    char m[8];
    unsigned r, c, nb;
    if (sscanf(s, "%ux%u:%4[a-z0-9]:%u", &r, &c, m, &nb) != 4) return 0;
    if (!strcmp(m, "int4")) sh->mode = ZACCEL_MODE_INT4;
    else if (!strcmp(m, "int8")) sh->mode = ZACCEL_MODE_INT8;
    else return 0;
    if (r < 1 || c < 1 || c > ZACCEL_MAX_COLS || nb < 1 || nb > ZACCEL_MAX_BATCH) return 0;
    sh->rows = r; sh->cols = c; sh->nb = nb;
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: zaccel-bench [-H host] [-p port] [-t threads] [-r reps] [-n] [-s shape]...\n"
        "  -H host    Zynq address (default $ZACCEL_HOST, else %s, then " ZACCEL_ALT_HOST ")\n"
        "  -p port    default %d\n"
        "  -t n       Pi CPU threads (default: every core)\n"
        "  -r n       timed repetitions per measurement (default 20)\n"
        "  -n         Pi CPU only, do not contact the Zynq\n"
        "  -s shape   ROWSxCOLS:int4|int8:NB, repeatable (default 4096x4096:int4:1,\n"
        "             4096x4096:int4:8, 2048x2048:int8:1, 2048x2048:int8:8)\n",
        ZACCEL_DEFAULT_HOST, ZACCEL_DEFAULT_PORT);
}

static void banner(void)
{
    printf("==========================================================================\n"
           " NUMBERS ONLY COUNT when this runs ON THE ORANGE PI 4 PRO against the\n"
           " REAL ZYNQ with engine_used=PL.  On a PC, under qemu, or against\n"
           " mock_server.py this is a CORRECTNESS CHECK and its timings mean nothing.\n"
           "==========================================================================\n");
}

int main(int argc, char **argv)
{
    const char *host = NULL;
    int port = 0, threads = 0, reps = 20, no_zynq = 0, nsh = 0;
    shape_t shapes[32];
    int o;
    while ((o = getopt(argc, argv, "H:p:t:r:ns:h")) != -1) {
        switch (o) {
        case 'H': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 't': threads = atoi(optarg); break;
        case 'r': reps = atoi(optarg); break;
        case 'n': no_zynq = 1; break;
        case 's':
            if (nsh >= 32 || !parse_shape(optarg, &shapes[nsh])) {
                fprintf(stderr, "bad shape '%s'\n", optarg); usage(); return 3;
            }
            nsh++;
            break;
        default: usage(); return 3;
        }
    }
    if (reps < 1 || reps > 1000) { fprintf(stderr, "reps must be 1..1000\n"); return 3; }
    if (!nsh) {
        const shape_t d[4] = { { 4096, 4096, ZACCEL_MODE_INT4, 1 }, { 4096, 4096, ZACCEL_MODE_INT4, 8 },
                               { 2048, 2048, ZACCEL_MODE_INT8, 1 }, { 2048, 2048, ZACCEL_MODE_INT8, 8 } };
        memcpy(shapes, d, sizeof d);
        nsh = 4;
    }
    const char *req_host = host;   /* NULL: libzaccel tries both of the Zynq's addresses */
    if (!host || !*host)
        host = (getenv("ZACCEL_HOST") && *getenv("ZACCEL_HOST")) ? getenv("ZACCEL_HOST")
                                                                 : ZACCEL_DEFAULT_HOST " or " ZACCEL_ALT_HOST;

    setvbuf(stdout, NULL, _IOLBF, 0);
    banner();
    int a76, a55, other;
    cpu_parts(&a76, &a55, &other);
    int on_pi = (a76 == 2 && a55 == 6 && other == 0);
#if defined(__aarch64__)
    const char *arch = "aarch64";
#elif defined(__x86_64__)
    const char *arch = "x86_64";
#else
    const char *arch = "other";
#endif
    zaccel_cpu_t *cpu = zaccel_cpu_create(threads);
    if (!cpu) { fprintf(stderr, "cannot start the CPU thread pool\n"); return 3; }
    printf("build : %s, kernel %s, %d threads\n", arch, zaccel_cpu_kernel(), zaccel_cpu_threads(cpu));
    printf("cpu   : %d x Cortex-A76 + %d x Cortex-A55 + %d other -> %s\n", a76, a55, other,
           on_pi ? "Orange Pi 4 Pro class (A733)" : "NOT the Orange Pi's A733: timings do not count");

    zaccel_t *z = NULL;
    if (!no_zynq) {
        z = zaccel_connect(req_host, port);
        if (!z) {
            printf("zynq  : %s:%d NOT REACHABLE (%s) -- Pi CPU only\n", host,
                   port > 0 ? port : ZACCEL_DEFAULT_PORT, strerror(errno));
            g_zynq_failed = 1;
        } else {
            zaccel_info_t in;
            int rc = zaccel_info(z, &in);
            if (rc) {
                printf("zynq  : INFO failed: %s -- Pi CPU only\n", zaccel_strerror(rc));
                zaccel_close(z); z = NULL; g_zynq_failed = 1;
            } else {
                printf("zynq  : %s:%d  version %u  engine %s  mem %u/%u MB free  max_cols %u  "
                       "max_batch %u  selftest %s(%u)\n", host, port > 0 ? port : ZACCEL_DEFAULT_PORT,
                       in.version, in.engine == 1 ? "PL" : "CPU-fallback", in.mem_free_mb,
                       in.mem_total_mb, in.max_cols, in.max_batch, in.selftest ? "FAIL" : "pass",
                       in.selftest);
                uint8_t pb[64];
                for (int i = 0; i < 64; i++) pb[i] = (uint8_t)(i * 37 + 1);
                double pt[20];
                int ok = 1;
                for (int i = 0; i < 20 && ok; i++) {
                    double t = now();
                    ok = zaccel_ping(z, pb, sizeof pb) == 0;
                    pt[i] = now() - t;
                }
                if (ok) printf("link  : PING 64 B round trip, median of 20: %.3f ms\n", median(pt, 20) * 1e3);
                else { printf("link  : PING failed -- Pi CPU only\n"); zaccel_close(z); z = NULL; g_zynq_failed = 1; }
            }
        }
    } else {
        printf("zynq  : skipped (-n)\n");
    }

    result_t res[32];
    for (int i = 0; i < nsh; i++) {
        bench_shape(&res[i], &shapes[i], reps, cpu, &z, 0x9E3779B97F4A7C15ull + (uint64_t)i * 7919u);
        if (z && g_zynq_failed) {
            /* a failed call may have closed the connection; make sure it still answers */
            if (zaccel_ping(z, "x", 1) != 0) { zaccel_close(z); z = NULL; }
        }
    }

    printf("\n==========================================================================\n");
    printf("SUMMARY  (ms per job / G MAC/s, median of %d; boost = Pi alone / both)\n", reps);
    printf("  %-22s %-18s %-18s %-24s %s\n", "shape", "Pi CPU alone", "Zynq alone", "both at once", "boost");
    for (int i = 0; i < nsh; i++) {
        const result_t *r = &res[i];
        double macs = (double)r->sh.rows * r->sh.cols * r->sh.nb;
        char s0[40], a[32], b[32], c[40], d[16];
        snprintf(s0, sizeof s0, "%ux%u %s nb=%u", r->sh.rows, r->sh.cols, mode_name(r->sh.mode), r->sh.nb);
        if (r->pi_ok) snprintf(a, sizeof a, "%.3f / %.2f", r->pi_ms, macs / r->pi_ms / 1e6); else snprintf(a, sizeof a, "-");
        if (r->z_ok)  snprintf(b, sizeof b, "%.3f / %.2f%s", r->z_ms, macs / r->z_ms / 1e6, r->engine == 1 ? "" : " cpu");
        else snprintf(b, sizeof b, "-");
        if (r->both_ok) snprintf(c, sizeof c, "%.3f / %.2f", r->both_ms, macs / r->both_ms / 1e6); else snprintf(c, sizeof c, "-");
        if (r->both_ok) snprintf(d, sizeof d, "%.2fx", r->pi_ms / r->both_ms); else snprintf(d, sizeof d, "-");
        printf("  %-22s %-18s %-18s %-24s %s\n", s0, a, b, c, d);
    }
    printf("\nanswers checked: %ld, wrong: %ld\n", g_checks, g_wrong);
    printf("do these numbers count?\n");
    printf("  ran on the Orange Pi 4 Pro (2 x A76 + 6 x A55) ... %s\n", on_pi ? "yes" : "NO");
    printf("  aarch64 build with NEON SDOT ...................... %s\n",
           strcmp(zaccel_cpu_kernel(), "neon-sdot") == 0 ? "yes" : "NO");
    printf("  Zynq reached and ran on the PL .................... %s\n",
           no_zynq ? "NO (skipped)" : g_saw_pl && !g_saw_cpu_fallback && !g_zynq_failed ? "yes"
           : g_saw_cpu_fallback ? "NO (CPU fallback ran)" : "NO (unreachable or failed)");
    int counts = on_pi && !strcmp(zaccel_cpu_kernel(), "neon-sdot") && !no_zynq && g_saw_pl &&
                 !g_saw_cpu_fallback && !g_zynq_failed && !g_wrong;
    if (g_wrong)
        printf("RESULT: FAIL -- %ld wrong answer(s). No number above counts.\n", g_wrong);
    else if (counts)
        printf("RESULT: every answer bit-exact; these are real Orange Pi + Zynq numbers.\n");
    else
        printf("RESULT: every answer bit-exact; timings DO NOT COUNT (see above).\n");

    zaccel_close(z);
    zaccel_cpu_destroy(cpu);
    if (g_wrong) return 1;
    if (!no_zynq && g_zynq_failed) return 2;
    return 0;
}
