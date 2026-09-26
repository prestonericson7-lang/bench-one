/* test_lib -- tests for libzaccel and zaccel_cpu.
 *
 *   test_lib --unit                  packing, reference vs a naive int8 matrix, CPU kernel vs reference
 *   test_lib --server HOST PORT      the protocol against a server (mock_server.py in the tests)
 *   test_lib --nomem  HOST PORT      a server started with --mem-mb 1 must answer LOAD with status 2
 *   test_lib --malformed             the client against a local server that replies wrongly
 *
 * Exit 0 only if every check passed.
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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static int n_pass, n_fail;

#define CHECK(cond, ...) do { if (cond) n_pass++; else { n_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t rs = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }

static const uint32_t EDGE_COLS[] = { 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63, 64, 65,
                                      100, 255, 256, 257, 1000, 4081, 4095, 4096 };
#define NEDGE (sizeof EDGE_COLS / sizeof EDGE_COLS[0])

static uint32_t pick_cols(void)
{
    return (rnd() & 1) ? EDGE_COLS[rnd() % NEDGE] : 1 + rnd() % 4096;
}

static void rand_w(int8_t *w, size_t n, uint32_t mode)
{
    for (size_t i = 0; i < n; i++)
        w[i] = mode == ZACCEL_MODE_INT4 ? (int8_t)((int)(rnd() % 16) - 8) : (int8_t)((int)(rnd() % 256) - 128);
}
static void rand_a(int8_t *a, size_t n) { for (size_t i = 0; i < n; i++) a[i] = (int8_t)((int)(rnd() % 256) - 128); }

static uint8_t *pack(uint32_t mode, const int8_t *w, uint32_t rows, uint32_t cols)
{
    uint8_t *p = malloc((size_t)rows * zaccel_row_bytes(mode, cols));
    if (mode == ZACCEL_MODE_INT4) zaccel_pack_int4(w, rows, cols, p);
    else                          zaccel_pack_int8(w, rows, cols, p);
    return p;
}

/* Straight from the definition, on the unpacked int8 matrix. */
static void naive(const int8_t *w, uint32_t rows, uint32_t cols, uint32_t nb, const int8_t *a, int32_t *y)
{
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t v = 0; v < nb; v++) {
            long long s = 0;
            for (uint32_t k = 0; k < cols; k++) s += (long long)w[(size_t)r * cols + k] * a[(size_t)v * cols + k];
            y[(size_t)r * nb + v] = (int32_t)s;
        }
}

static int first_diff(const int32_t *a, const int32_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return (int)i;
    return -1;
}

/* ================================================================ unit tests */
static void unit_row_bytes(void)
{
    CHECK(zaccel_row_bytes(0, 1) == 8 && zaccel_row_bytes(0, 16) == 8 && zaccel_row_bytes(0, 17) == 16 &&
          zaccel_row_bytes(0, 4096) == 2048, "int4 row bytes");
    CHECK(zaccel_row_bytes(1, 1) == 8 && zaccel_row_bytes(1, 8) == 8 && zaccel_row_bytes(1, 9) == 16 &&
          zaccel_row_bytes(1, 4096) == 4096, "int8 row bytes");
    CHECK(zaccel_row_bytes(2, 16) == 0, "bad mode gives 0");
}

static void unit_pack(void)
{
    for (int it = 0; it < 50; it++) {
        uint32_t rows = 1 + rnd() % 9, cols = pick_cols();
        size_t rb = zaccel_row_bytes(0, cols);
        int8_t *w = malloc((size_t)rows * cols);
        rand_w(w, (size_t)rows * cols, 0);
        uint8_t *p = malloc(rows * rb);
        memset(p, 0xEE, rows * rb);
        int rc = zaccel_pack_int4(w, rows, cols, p);
        int ok = rc == ZACCEL_OK;
        for (uint32_t r = 0; r < rows && ok; r++) {
            const uint8_t *row = p + r * rb;
            for (uint32_t k = 0; k < cols && ok; k++) {         /* SPEC §1: byte i = w[2i] | w[2i+1] << 4 */
                int nib = (row[k / 2] >> (4 * (k % 2))) & 15;
                ok = nib == (w[(size_t)r * cols + k] & 15);
            }
            for (size_t k = cols; k < 2 * rb && ok; k++)          /* unused tail nibbles are 0 */
                ok = ((row[k / 2] >> (4 * (k % 2))) & 15) == 0;
        }
        CHECK(ok, "pack_int4 layout rows %u cols %u", rows, cols);
        free(w); free(p);
    }
    int8_t bad[3] = { 7, 8, -8 };
    uint8_t out[8];
    CHECK(zaccel_pack_int4(bad, 1, 3, out) == ZACCEL_E_ARG, "pack_int4 flags a value outside -8..7");
    int8_t bad2[2] = { -9, 0 };
    CHECK(zaccel_pack_int4(bad2, 1, 2, out) == ZACCEL_E_ARG, "pack_int4 flags -9");

    for (int it = 0; it < 30; it++) {
        uint32_t rows = 1 + rnd() % 9, cols = pick_cols();
        size_t rb = zaccel_row_bytes(1, cols);
        int8_t *w = malloc((size_t)rows * cols);
        rand_w(w, (size_t)rows * cols, 1);
        uint8_t *p = malloc(rows * rb);
        memset(p, 0xEE, rows * rb);
        zaccel_pack_int8(w, rows, cols, p);
        int ok = 1;
        for (uint32_t r = 0; r < rows && ok; r++)
            for (size_t k = 0; k < rb && ok; k++)
                ok = p[r * rb + k] == (k < cols ? (uint8_t)w[(size_t)r * cols + k] : 0);
        CHECK(ok, "pack_int8 layout rows %u cols %u", rows, cols);
        free(w); free(p);
    }
}

static void unit_ref(void)
{
    for (int it = 0; it < 200; it++) {
        uint32_t mode = rnd() & 1, rows = 1 + rnd() % 24, cols = pick_cols(), nb = 1 + rnd() % 8;
        int8_t *w = malloc((size_t)rows * cols), *a = malloc((size_t)nb * cols);
        rand_w(w, (size_t)rows * cols, mode); rand_a(a, (size_t)nb * cols);
        uint8_t *p = pack(mode, w, rows, cols);
        int32_t *y0 = malloc((size_t)rows * nb * 4), *y1 = malloc((size_t)rows * nb * 4);
        naive(w, rows, cols, nb, a, y0);
        zaccel_ref_gemv(mode, rows, cols, p, nb, a, y1);
        int d = first_diff(y0, y1, (size_t)rows * nb);
        CHECK(d < 0, "ref vs naive mode %u %ux%u nb %u differs at %d", mode, rows, cols, nb, d);
        free(w); free(a); free(p); free(y0); free(y1);
    }
}

/* CPU kernel vs the reference; tail padding optionally filled with garbage. */
static int cpu_case(zaccel_cpu_t *cpu, uint32_t mode, uint32_t rows, uint32_t cols, uint32_t nb,
                    int garbage_tail, int extreme)
{
    size_t rb = zaccel_row_bytes(mode, cols);
    int8_t *w = malloc((size_t)rows * cols), *a = malloc((size_t)nb * cols);
    if (extreme) {
        memset(w, mode == ZACCEL_MODE_INT4 ? -8 : -128, (size_t)rows * cols);
        memset(a, -128, (size_t)nb * cols);
    } else {
        rand_w(w, (size_t)rows * cols, mode); rand_a(a, (size_t)nb * cols);
    }
    uint8_t *p = pack(mode, w, rows, cols);
    if (garbage_tail)
        for (uint32_t r = 0; r < rows; r++) {
            uint8_t *row = p + r * rb;
            if (mode == ZACCEL_MODE_INT4) {
                if (cols & 1) row[cols / 2] |= 0xB0;
                for (size_t i = (cols + 1) / 2; i < rb; i++) row[i] = (uint8_t)(rnd() | 1);
            } else {
                for (size_t i = cols; i < rb; i++) row[i] = (uint8_t)(rnd() | 1);
            }
        }
    int32_t *y0 = malloc((size_t)rows * nb * 4), *y1 = malloc((size_t)rows * nb * 4), *y2 = malloc((size_t)rows * nb * 4);
    naive(w, rows, cols, nb, a, y0);
    zaccel_ref_gemv(mode, rows, cols, p, nb, a, y1);
    memset(y2, 0xA5, (size_t)rows * nb * 4);
    int rc = zaccel_cpu_gemv(cpu, mode, rows, cols, p, nb, a, y2);
    int d1 = first_diff(y0, y1, (size_t)rows * nb), d2 = first_diff(y1, y2, (size_t)rows * nb);
    int ok = rc == 0 && d1 < 0 && d2 < 0;
    if (!ok)
        printf("  cpu case mode %u %ux%u nb %u threads %d tail %d extreme %d: rc %d ref-naive %d cpu-ref %d\n",
               mode, rows, cols, nb, zaccel_cpu_threads(cpu), garbage_tail, extreme, rc, d1, d2);
    if (ok && extreme) ok = y2[0] == (int32_t)cols * (mode == ZACCEL_MODE_INT4 ? 1024 : 16384);
    free(w); free(a); free(p); free(y0); free(y1); free(y2);
    return ok;
}

static void unit_cpu(void)
{
    printf("cpu kernel: %s\n", zaccel_cpu_kernel());
    const int tcounts[] = { 1, 2, 3, 0 };
    for (int ti = 0; ti < 4; ti++) {
        zaccel_cpu_t *cpu = zaccel_cpu_create(tcounts[ti]);
        CHECK(cpu != NULL, "cpu pool create");
        if (!cpu) continue;
        int bad = 0, n = 0;
        for (uint32_t mode = 0; mode < 2; mode++)
            for (uint32_t nb = 1; nb <= 8; nb++) {
                for (size_t e = 0; e < NEDGE; e++) { bad += !cpu_case(cpu, mode, 1 + rnd() % 5, EDGE_COLS[e], nb, 0, 0); n++; }
                for (int it = 0; it < 4; it++) { bad += !cpu_case(cpu, mode, 1 + rnd() % 70, pick_cols(), nb, it & 1, 0); n++; }
            }
        CHECK(bad == 0, "cpu kernel vs reference, %d threads: %d of %d shapes wrong", zaccel_cpu_threads(cpu), bad, n);
        CHECK(cpu_case(cpu, 0, 3, 4096, 8, 0, 1) && cpu_case(cpu, 1, 3, 4096, 8, 0, 1),
              "cpu kernel at the extreme sums (-8/-128 x -128, 4096 cols)");
        CHECK(cpu_case(cpu, 0, 777, 333, 3, 1, 0) && cpu_case(cpu, 1, 1025, 4096, 8, 0, 0),
              "cpu kernel, many rows across all threads");
        int8_t a[16] = { 0 }; uint8_t p[16] = { 0 }; int32_t y[16];
        CHECK(zaccel_cpu_gemv(cpu, 0, 1, 16, p, 0, a, y) == ZACCEL_E_ARG &&
              zaccel_cpu_gemv(cpu, 0, 1, 16, p, 9, a, y) == ZACCEL_E_ARG &&
              zaccel_cpu_gemv(cpu, 0, 1, 0, p, 1, a, y) == ZACCEL_E_ARG &&
              zaccel_cpu_gemv(cpu, 0, 1, 4097, p, 1, a, y) == ZACCEL_E_ARG &&
              zaccel_cpu_gemv(cpu, 2, 1, 16, p, 1, a, y) == ZACCEL_E_ARG, "cpu kernel rejects bad arguments");
        zaccel_cpu_destroy(cpu);
    }
}

/* ================================================================ protocol tests */
static int load_gemv_check(zaccel_t *z, uint32_t mode, uint32_t rows, uint32_t cols, uint32_t nb,
                           uint32_t *engine)
{
    int8_t *w = malloc((size_t)rows * cols), *a = malloc((size_t)nb * cols);
    rand_w(w, (size_t)rows * cols, mode); rand_a(a, (size_t)nb * cols);
    uint8_t *p = pack(mode, w, rows, cols);
    int32_t *y0 = malloc((size_t)rows * nb * 4), *y1 = malloc((size_t)rows * nb * 4);
    zaccel_ref_gemv(mode, rows, cols, p, nb, a, y0);
    memset(y1, 0xA5, (size_t)rows * nb * 4);
    zaccel_tensor_t t;
    int ok = 0, rc = zaccel_load(z, mode, rows, cols, p, &t);
    if (rc == 0) {
        uint32_t cyc = 0;
        rc = zaccel_gemv(z, &t, nb, a, y1, &cyc, engine);
        int d = rc ? -2 : first_diff(y0, y1, (size_t)rows * nb);
        int rc2 = zaccel_free(z, &t);
        ok = rc == 0 && d < 0 && rc2 == 0;
        if (!ok) printf("  mode %u %ux%u nb %u: gemv rc %d diff %d free rc %d\n", mode, rows, cols, nb, rc, d, rc2);
    } else {
        printf("  mode %u %ux%u: load rc %d (%s)\n", mode, rows, cols, rc, zaccel_strerror(rc));
    }
    free(w); free(a); free(p); free(y0); free(y1);
    return ok;
}

static void server_tests(const char *host, int port)
{
    zaccel_t *z = zaccel_connect(host, port);
    CHECK(z != NULL, "connect %s:%d (%s)", host, port, strerror(errno));
    if (!z) return;

    zaccel_info_t in;
    int rc = zaccel_info(z, &in);
    CHECK(rc == 0, "INFO rc %d", rc);
    CHECK(in.version == 1 && in.max_cols == 4096 && in.max_batch == 8 && in.selftest == 0 &&
          in.engine <= 1 && in.mem_free_mb <= in.mem_total_mb,
          "INFO fields: version %u engine %u mem %u/%u cols %u batch %u selftest %u", in.version,
          in.engine, in.mem_free_mb, in.mem_total_mb, in.max_cols, in.max_batch, in.selftest);

    const uint32_t psz[] = { 0, 1, 7, 64, 1000, 65536, 1u << 20 };
    for (size_t i = 0; i < sizeof psz / sizeof psz[0]; i++) {
        uint8_t *b = malloc(psz[i] + 1);
        for (uint32_t k = 0; k < psz[i]; k++) b[k] = (uint8_t)rnd();
        rc = zaccel_ping(z, b, psz[i]);
        CHECK(rc == 0, "PING %u bytes rc %d", psz[i], rc);
        free(b);
    }

    /* LOAD / GEMV / FREE over random shapes, every nb, both modes, odd columns */
    uint32_t engine = 9;
    int bad = 0, n = 0;
    for (uint32_t mode = 0; mode < 2; mode++)
        for (uint32_t nb = 1; nb <= 8; nb++)
            for (int it = 0; it < 3; it++) {
                uint32_t cols = it == 0 ? EDGE_COLS[rnd() % NEDGE] : it == 1 ? (1 + rnd() % 600) | 1 : pick_cols();
                uint32_t rows = 1 + rnd() % (cols > 2000 ? 12 : 60);
                bad += !load_gemv_check(z, mode, rows, cols, nb, &engine);
                n++;
            }
    CHECK(bad == 0, "LOAD/GEMV/FREE vs reference: %d of %d shapes wrong", bad, n);
    CHECK(engine <= 1, "engine_used is 0 or 1 (got %u)", engine);
    printf("server engine_used = %u (%s)\n", engine, engine == 1 ? "PL" : "CPU");

    /* several tensors resident at once, used out of order, and a second connection */
    {
        zaccel_t *z2 = zaccel_connect(host, port);
        CHECK(z2 != NULL, "second connection");
        enum { NT = 5 };
        zaccel_tensor_t t[NT];
        int8_t *a[NT]; uint8_t *p[NT]; int32_t *ref[NT]; uint32_t nbs[NT];
        int ok = 1;
        for (int i = 0; i < NT; i++) {
            uint32_t mode = (uint32_t)i & 1, rows = 5 + (uint32_t)i * 7, cols = 33 + (uint32_t)i * 101;
            nbs[i] = 1 + (uint32_t)i;
            int8_t *w = malloc((size_t)rows * cols);
            a[i] = malloc((size_t)nbs[i] * cols);
            rand_w(w, (size_t)rows * cols, mode); rand_a(a[i], (size_t)nbs[i] * cols);
            p[i] = pack(mode, w, rows, cols);
            ref[i] = malloc((size_t)rows * nbs[i] * 4);
            zaccel_ref_gemv(mode, rows, cols, p[i], nbs[i], a[i], ref[i]);
            ok &= zaccel_load((i & 2) && z2 ? z2 : z, mode, rows, cols, p[i], &t[i]) == 0;
            free(w);
        }
        CHECK(ok, "LOAD %d tensors", NT);
        int ids_unique = 1;
        for (int i = 0; i < NT; i++) for (int j = i + 1; j < NT; j++) ids_unique &= t[i].id != t[j].id;
        CHECK(ids_unique, "tensor ids are distinct");
        const int order[] = { 3, 0, 4, 1, 2, 0, 3, 4 };
        for (size_t k = 0; k < sizeof order / sizeof order[0] && ok; k++) {
            int i = order[k];
            int32_t *y = malloc((size_t)t[i].rows * nbs[i] * 4);
            ok &= zaccel_gemv((i & 2) && z2 ? z2 : z, &t[i], nbs[i], a[i], y, NULL, NULL) == 0 &&
                  first_diff(y, ref[i], (size_t)t[i].rows * nbs[i]) < 0;
            free(y);
        }
        CHECK(ok, "GEMV on resident tensors out of order, two connections");
        for (int i = 0; i < NT; i++) {
            CHECK(zaccel_free((i & 2) && z2 ? z2 : z, &t[i]) == 0, "FREE tensor %d", i);
            free(a[i]); free(p[i]); free(ref[i]);
        }
        zaccel_close(z2);
    }

    /* error statuses, and the connection survives each of them */
    {
        int8_t w[64 * 20], a[9 * 64];
        rand_w(w, sizeof w, 0); rand_a(a, sizeof a);
        uint8_t *p = pack(0, w, 20, 64);
        uint8_t *wide = calloc(zaccel_row_bytes(0, 4097), 1);
        int32_t y[20 * 9];
        zaccel_tensor_t t;
        rc = zaccel_load(z, 0, 20, 64, p, &t);
        CHECK(rc == 0, "load for error tests");
        zaccel_tensor_t wrong = t;
        wrong.cols = 60;                                      /* payload length disagrees with the tensor */
        rc = zaccel_gemv(z, &wrong, 1, a, y, NULL, NULL);
        CHECK(rc == ZACCEL_ST_BADREQ, "GEMV with the wrong column count -> status 1 (got %d)", rc);
        CHECK(zaccel_ping(z, "ok", 2) == 0, "connection alive after status 1");
        rc = zaccel_gemv(z, &t, 9, a, y, NULL, NULL);        /* the lib passes nb=9 through */
        CHECK(rc == ZACCEL_ST_BADREQ, "GEMV nb=9 -> status 1 (got %d)", rc);
        rc = zaccel_load(z, 0, 1, 4097, wide, &wrong);
        CHECK(rc == ZACCEL_ST_BADREQ, "LOAD cols=4097 -> status 1 (got %d)", rc);
        CHECK(zaccel_free(z, &t) == 0, "free");
        rc = zaccel_gemv(z, &t, 1, a, y, NULL, NULL);
        CHECK(rc == ZACCEL_ST_NOTENSOR, "GEMV on a freed tensor -> status 4 (got %d)", rc);
        rc = zaccel_free(z, &t);
        CHECK(rc == ZACCEL_ST_NOTENSOR, "FREE twice -> status 4 (got %d)", rc);
        wrong.id = 0xDEADBEEF;
        rc = zaccel_free(z, &wrong);
        CHECK(rc == ZACCEL_ST_NOTENSOR, "FREE of an id never issued -> status 4 (got %d)", rc);
        CHECK(zaccel_ping(z, "ok", 2) == 0, "connection alive after status 4");
        CHECK(zaccel_load(z, 2, 1, 16, p, &wrong) == ZACCEL_E_ARG, "LOAD mode 2 rejected locally");
        CHECK(zaccel_gemv(z, &t, 0, a, y, NULL, NULL) == ZACCEL_E_ARG, "GEMV nb=0 rejected locally");
        CHECK(zaccel_info(z, &in) == 0, "INFO still answers");
        free(p); free(wide);
    }
    zaccel_close(z);

    /* host defaults and failures */
    {
        setenv("ZACCEL_HOST", host, 1);
        CHECK(strcmp(zaccel_default_host(), host) == 0, "ZACCEL_HOST picked up");
        zaccel_t *z3 = zaccel_connect(NULL, port);
        CHECK(z3 != NULL && zaccel_ping(z3, "h", 1) == 0, "connect(NULL) uses $ZACCEL_HOST");
        zaccel_close(z3);
        unsetenv("ZACCEL_HOST");
        CHECK(strcmp(zaccel_default_host(), "10.20.0.2") == 0, "default host is 10.20.0.2");
        errno = 0;
        z3 = zaccel_connect("127.0.0.1", 1);
        CHECK(z3 == NULL && errno == ECONNREFUSED, "closed port -> NULL, ECONNREFUSED (errno %d)", errno);
        zaccel_close(z3);
        /* RFC 5737 TEST-NET-1: never routed, so the connect deadline has to fire */
        setenv("ZACCEL_TIMEOUT_MS", "400", 1);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        z3 = zaccel_connect("192.0.2.1", 8093);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double el = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
        CHECK(z3 == NULL && el < 3.0, "unreachable host gives up (%.2f s, errno %d)", el, errno);
        zaccel_close(z3);
        unsetenv("ZACCEL_TIMEOUT_MS");
    }
}

static void nomem_tests(const char *host, int port)
{
    zaccel_t *z = zaccel_connect(host, port);
    CHECK(z != NULL, "connect");
    if (!z) return;
    uint32_t rows = 2048, cols = 2048;                        /* 4 MB of int8 */
    uint8_t *p = calloc((size_t)rows * cols, 1);
    zaccel_tensor_t t;
    int rc = zaccel_load(z, 1, rows, cols, p, &t);
    CHECK(rc == ZACCEL_ST_NOMEM, "LOAD bigger than the server's memory -> status 2 (got %d)", rc);
    zaccel_info_t in;
    CHECK(zaccel_info(z, &in) == 0 && in.mem_total_mb == 1, "INFO after NOMEM (total %u MB)", in.mem_total_mb);
    uint32_t eng;
    CHECK(load_gemv_check(z, 0, 16, 256, 4, &eng), "a small tensor still loads and runs");
    free(p);
    zaccel_close(z);
}

/* ================================================================ a server that replies wrongly */
typedef struct { int lfd; int kind; } evil_t;

static void *evil_main(void *arg)
{
    evil_t *e = arg;
    for (;;) {
        int fd = accept(e->lfd, NULL, NULL);
        if (fd < 0) return NULL;
        uint8_t h[16];
        size_t got = 0;
        while (got < 16) { ssize_t r = recv(fd, h + got, 16 - got, 0); if (r <= 0) break; got += (size_t)r; }
        if (got == 16) {
            uint32_t len = h[12] | h[13] << 8 | h[14] << 16 | (uint32_t)h[15] << 24;
            uint8_t tmp[4096];
            while (len) { ssize_t r = recv(fd, tmp, len < sizeof tmp ? len : sizeof tmp, 0); if (r <= 0) break; len -= (uint32_t)r; }
            uint32_t rep[4] = { 0x3152415A, 0, h[8] | h[9] << 8 | h[10] << 16 | (uint32_t)h[11] << 24, 0 };
            uint8_t extra[16] = { 0 };
            size_t elen = 0;
            if (e->kind == 0) rep[0] = 0x12345678;            /* bad magic                  */
            if (e->kind == 1) rep[2] += 1;                    /* wrong sequence number      */
            if (e->kind == 2) { rep[3] = 12; elen = 12; }     /* GEMV reply of the wrong size */
            if (e->kind == 3) { rep[3] = 3; elen = 3; }       /* PING echo of the wrong size */
            send(fd, rep, sizeof rep, MSG_NOSIGNAL);
            if (elen) send(fd, extra, elen, MSG_NOSIGNAL);
        }
        close(fd);
    }
}

static void malformed_tests(void)
{
    const char *what[] = { "bad reply magic", "wrong reply seq", "GEMV reply of the wrong size",
                           "PING echo of the wrong size" };
    for (int kind = 0; kind < 4; kind++) {
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t sl = sizeof sa;
        if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) || listen(lfd, 4) ||
            getsockname(lfd, (struct sockaddr *)&sa, &sl)) { CHECK(0, "evil server socket"); return; }
        evil_t e = { lfd, kind };
        pthread_t th;
        pthread_create(&th, NULL, evil_main, &e);
        zaccel_t *z = zaccel_connect("127.0.0.1", ntohs(sa.sin_port));
        int rc = -99;
        if (z) {
            if (kind == 2) {
                zaccel_tensor_t t = { 1, 0, 4, 16 };
                int8_t a[16] = { 0 };
                int32_t y[4];
                rc = zaccel_gemv(z, &t, 1, a, y, NULL, NULL);
            } else if (kind == 3) {
                rc = zaccel_ping(z, "abcd", 4);
            } else {
                zaccel_info_t in;
                rc = zaccel_info(z, &in);
            }
        }
        CHECK(z && rc == ZACCEL_E_PROTO, "%s -> ZACCEL_E_PROTO (got %d)", what[kind], rc);
        CHECK(z && zaccel_ping(z, "x", 1) == ZACCEL_E_CLOSED, "%s: connection closed afterwards", what[kind]);
        zaccel_close(z);
        shutdown(lfd, SHUT_RDWR);
        close(lfd);
        pthread_join(th, NULL);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "--unit")) {
        unit_row_bytes(); unit_pack(); unit_ref(); unit_cpu();
    } else if (argc >= 4 && !strcmp(argv[1], "--server")) {
        server_tests(argv[2], atoi(argv[3]));
    } else if (argc >= 4 && !strcmp(argv[1], "--nomem")) {
        nomem_tests(argv[2], atoi(argv[3]));
    } else if (argc >= 2 && !strcmp(argv[1], "--malformed")) {
        malformed_tests();
    } else {
        fprintf(stderr, "usage: test_lib --unit | --server HOST PORT | --nomem HOST PORT | --malformed\n");
        return 2;
    }
    printf("test_lib %s: %d passed, %d failed\n", argv[1], n_pass, n_fail);
    return n_fail ? 1 : 0;
}
