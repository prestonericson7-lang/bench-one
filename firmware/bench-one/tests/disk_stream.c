/* ===========================================================================================
 *  disk_stream.c -- what a model actually costs to read off storage, with the cache taken away
 * ===========================================================================================
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  A 14B model at Q4_K is 7.33 GB of weights, and the Q4_K_M mixes that ship in practice keep some
 *  tensors at Q6_K and land nearer 8.9 GB. The machine has 3.10 GB of memory. Every plan that ends
 *  with "and the rest streams off the 2 TB SSD" rests on one number nobody has measured: how fast
 *  weights actually arrive from storage when they are not already in RAM.
 *
 *  run_model reports "loaded 1.79 GB in 0.5 s (3499 MB/s off this disk)". That figure is a lie of
 *  omission -- it is the page cache handing back a file read minutes earlier. Read the same bytes
 *  with the cache switched off and the honest number appears.
 *
 *  On Windows that means FILE_FLAG_NO_BUFFERING, which forbids the cache entirely and demands that
 *  offsets, lengths and buffer addresses are all multiples of the sector size. On Linux it is
 *  O_DIRECT with the same alignment rules, or posix_fadvise(DONTNEED) between passes.
 *
 *  WHAT IT MEASURES
 *  ----------------
 *    1. Sequential, uncached, large blocks -- the ceiling for streaming a layer at a time.
 *    2. Sequential, uncached, small blocks -- what happens if the reader asks in tensor-sized
 *       pieces instead of coalescing.
 *    3. Random, uncached, layer-sized reads -- the mixture-of-experts case, where which bytes are
 *       needed is decided one token at a time and cannot be prefetched in order.
 *    4. Cached, for contrast, so the gap between the honest and flattering figure is visible.
 *
 *  Then it turns each rate into tokens per second for a 14B model, which is the only form of the
 *  answer that decides anything.
 *
 *  RUN
 *      disk_stream <file> [seconds-per-test]
 * ======================================================================================== */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
  static double now_s(void)
  {
      LARGE_INTEGER f, t;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&t);
      return (double)t.QuadPart / (double)f.QuadPart;
  }
#else
  #include <fcntl.h>
  #include <time.h>
  #include <unistd.h>
  static double now_s(void)
  {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
  }
#endif

/* A 14B at Q4_K, from the architecture rather than a guess.
 *
 * Qwen2.5-14B: 48 layers, hidden 5120, intermediate 13824, 40 query heads and 8 key/value heads of
 * 128, vocabulary 152064. Q4_K is 144 bytes per 256 weights; the embedding and the output head are
 * tied, so the table is read once per token for the projection. */
static double bytes_q4k(double weights) { return weights * 144.0 / 256.0; }

static double model_14b_bytes(double *per_layer_out)
{
    const double H = 5120.0, F = 13824.0, KV = 8.0 * 128.0, V = 152064.0, L = 48.0;
    const double attn = H * H + H * KV * 2.0 + H * H;      /* q, k, v, o */
    const double mlp  = H * F * 3.0;                       /* gate, up, down */
    const double per_layer = bytes_q4k(attn + mlp);
    const double head = bytes_q4k(V * H);
    if (per_layer_out) *per_layer_out = per_layer;
    return per_layer * L + head;
}

/* ------------------------------------------------------------------------------------------- */

#if defined(_WIN32)

typedef struct { HANDLE h; long long size; DWORD sector; } dfile_t;

static int dopen(dfile_t *d, const char *path, int direct)
{
    DWORD flags = FILE_FLAG_SEQUENTIAL_SCAN;
    if (direct) flags |= FILE_FLAG_NO_BUFFERING;
    d->h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, flags, NULL);
    if (d->h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    GetFileSizeEx(d->h, &sz);
    d->size = sz.QuadPart;
    d->sector = 4096;   /* every drive this will meet is 512e or 4Kn; 4096 satisfies both */
    return 1;
}

static long long dread(dfile_t *d, long long off, void *buf, long long len)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset     = (DWORD)(off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)((unsigned long long)off >> 32);
    DWORD got = 0;
    if (!ReadFile(d->h, buf, (DWORD)len, &got, &ov)) return -1;
    return got;
}

static void dclose(dfile_t *d) { CloseHandle(d->h); }
static void *dalloc(size_t n) { return VirtualAlloc(NULL, n, MEM_COMMIT, PAGE_READWRITE); }
static void  dfree(void *p)   { VirtualFree(p, 0, MEM_RELEASE); }

#else

typedef struct { int h; long long size; unsigned sector; } dfile_t;

static int dopen(dfile_t *d, const char *path, int direct)
{
    int fl = O_RDONLY;
  #ifdef O_DIRECT
    if (direct) fl |= O_DIRECT;
  #else
    (void)direct;
  #endif
    d->h = open(path, fl);
    if (d->h < 0) return 0;
    d->size = lseek(d->h, 0, SEEK_END);
    d->sector = 4096;
    return 1;
}

static long long dread(dfile_t *d, long long off, void *buf, long long len)
{
    return pread(d->h, buf, (size_t)len, (off_t)off);
}

static void dclose(dfile_t *d) { close(d->h); }
static void *dalloc(size_t n)
{
    void *p = NULL;
    if (posix_memalign(&p, 4096, n) != 0) return NULL;
    return p;
}
static void dfree(void *p) { free(p); }

#endif

/* ------------------------------------------------------------------------------------------- */

static double bench(const char *path, int direct, long long block, int random, double budget)
{
    dfile_t d;
    if (!dopen(&d, path, direct)) { printf("    cannot open %s\n", path); return 0.0; }

    void *buf = dalloc((size_t)block);
    if (!buf) { dclose(&d); return 0.0; }

    /* Stay clear of the last block so an aligned read never runs off the end. */
    const long long span = (d.size / block) * block - block;
    if (span <= 0) { dfree(buf); dclose(&d); return 0.0; }

    uint64_t rs = 0x2545F4914F6CDD1Dull;
    long long total = 0, off = 0;
    const double t0 = now_s();
    double dt;
    do {
        if (random) {
            rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
            off = (long long)((rs % (uint64_t)(span / block)) * (uint64_t)block);
        } else {
            off += block;
            if (off >= span) off = 0;
        }
        const long long got = dread(&d, off, buf, block);
        if (got <= 0) break;
        total += got;
        dt = now_s() - t0;
    } while (dt < budget);

    dt = now_s() - t0;
    dfree(buf);
    dclose(&d);
    return (double)total / dt / (1024.0 * 1024.0);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: disk_stream <file> [seconds-per-test]\n"); return 1; }
    const char *path = argv[1];
    const double budget = (argc > 2) ? atof(argv[2]) : 2.0;

    double per_layer = 0.0;
    const double total14 = model_14b_bytes(&per_layer);

    printf("\n========================================================================\n");
    printf("  what storage really gives, with the page cache taken out of the way\n");
    printf("========================================================================\n");
    printf("  file: %s\n", path);
    printf("  each test runs for %.1f s\n\n", budget);

    const struct { const char *tag; int direct; long long block; int random; } T[] = {
        { "cached, 4 MB blocks       ", 0, 4 << 20, 0 },
        { "UNCACHED, 4 MB blocks     ", 1, 4 << 20, 0 },
        { "UNCACHED, 1 MB blocks     ", 1, 1 << 20, 0 },
        { "UNCACHED, 128 KB blocks   ", 1, 128 << 10, 0 },
        { "UNCACHED, 64 KB random    ", 1, 64 << 10, 1 },
        { "UNCACHED, 4 MB random     ", 1, 4 << 20, 1 },
    };
    double seq_big = 0.0, rnd_big = 0.0;

    for (int i = 0; i < (int)(sizeof(T) / sizeof(T[0])); i++) {
        const double mbs = bench(path, T[i].direct, T[i].block, T[i].random, budget);
        printf("    %s %8.1f MB/s\n", T[i].tag, mbs);
        if (i == 1) seq_big = mbs;
        if (i == 5) rnd_big = mbs;
    }

    printf("\n  a 14B at Q4_K, from the architecture:\n");
    printf("    per layer                  %8.1f MB  x48\n", per_layer / (1024.0 * 1024.0));
    printf("    whole model                %8.2f GB\n", total14 / (1024.0 * 1024.0 * 1024.0));
    printf("    this machine holds            3.10 GB, so %.2f GB has to come from storage\n",
           total14 / (1024.0 * 1024.0 * 1024.0) - 3.10);

    if (seq_big > 0.0) {
        const double resident = 3.10 * 1024.0 * 1024.0 * 1024.0;
        const double streamed = total14 - resident;
        printf("\n  decode, if the streamed part is read once per token at %.0f MB/s:\n", seq_big);
        printf("    %8.2f s per token   ->  %.2f tokens/s\n",
               streamed / (seq_big * 1024.0 * 1024.0),
               1.0 / (streamed / (seq_big * 1024.0 * 1024.0)));
        printf("  and if none of it is resident:\n");
        printf("    %8.2f s per token   ->  %.2f tokens/s\n",
               total14 / (seq_big * 1024.0 * 1024.0),
               1.0 / (total14 / (seq_big * 1024.0 * 1024.0)));
        printf("\n  VERDICT for a dense model: streaming weights off storage every token is dead.\n");
        printf("  Not slow -- dead. A dense 14B needs the resident memory, which means more boards.\n");

        /* Mixture-of-experts is the one shape where streaming is not obviously hopeless, because
         * only the experts that fire are read. Qwen3-30B-A3B: 48 layers, 128 experts of
         * intermediate 768 at hidden 2048, 8 firing per token, and the attention plus the router
         * are small enough to stay resident.
         *
         * The right rate to use is the LARGE-BLOCK RANDOM one. An expert is a contiguous 2.65 MB,
         * so each read is big enough to reach streaming speed, but which experts are wanted is
         * decided by the router one layer at a time and cannot be issued in advance. */
        const double eh = 2048.0, ei = 768.0, elayers = 48.0, efire = 8.0, ecount = 128.0;
        const double expert_bytes = bytes_q4k(eh * ei * 3.0);
        const double per_token = expert_bytes * efire * elayers;
        const double all_experts = expert_bytes * ecount * elayers;
        printf("\n  a 30B mixture-of-experts, 8 of 128 firing:\n");
        printf("    one expert                 %8.2f MB\n", expert_bytes / (1024.0 * 1024.0));
        printf("    every expert               %8.2f GB   (does not fit, and never will)\n",
               all_experts / (1024.0 * 1024.0 * 1024.0));
        printf("    read per token             %8.1f MB   (8 x 48 of them)\n",
               per_token / (1024.0 * 1024.0));
        if (rnd_big > 0.0) {
            const double t = per_token / (rnd_big * 1024.0 * 1024.0);
            printf("    at %.0f MB/s random        %8.2f s per token  ->  %.2f tokens/s\n",
                   rnd_big, t, 1.0 / t);
            /* 3.10 GB of memory holds this many experts, and expert use is heavily skewed, so a
             * cache of the hottest ones removes most of the reads. The hit rate is the one number
             * here that has not been measured and it decides the whole thing. */
            /* Not all 3.10 GB is available to cache experts. Attention, the routers and the
             * embedding table have to be resident or every token pays for them too, and unlike an
             * expert they are needed on every single token. */
            const double qh = 32.0 * 128.0, kvh = 4.0 * 128.0, V30 = 151936.0;
            const double attn30 = bytes_q4k((eh * qh + eh * kvh * 2.0 + qh * eh) * elayers);
            const double route30 = bytes_q4k(eh * ecount * elayers);
            const double embed30 = bytes_q4k(V30 * eh);
            const double fixed30 = attn30 + route30 + embed30;
            const double cache_bytes = 3.10 * 1024.0 * 1024.0 * 1024.0 - fixed30;
            printf("    attention + routers        %8.2f GB   resident, needed every token\n",
                   (attn30 + route30) / (1024.0 * 1024.0 * 1024.0));
            printf("    embedding and output head  %8.2f GB   resident, tied, read every token\n",
                   embed30 / (1024.0 * 1024.0 * 1024.0));
            printf("    left for an expert cache   %8.2f GB\n",
                   cache_bytes / (1024.0 * 1024.0 * 1024.0));
            const double cacheable = cache_bytes / expert_bytes;
            printf("    which holds                %8.0f experts of %.0f (%.0f%%)\n",
                   cacheable, ecount * elayers, 100.0 * cacheable / (ecount * elayers));
            for (int h = 50; h <= 90; h += 20) {
                const double tt = t * (1.0 - h / 100.0);
                printf("    at a %d%% cache hit rate    %8.2f s per token  ->  %.2f tokens/s\n",
                       h, tt, 1.0 / tt);
            }
            printf("\n  The hit rate is the unmeasured number and it decides everything. Measure it\n");
            printf("  by logging which experts a real 30B router picks over a few thousand tokens.\n");
        }
    }
    printf("\n");
    return 0;
}
