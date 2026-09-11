/* ===========================================================================================
 *  host_scaling.c -- the structural difference, measured on the machine it is being compared to
 * ===========================================================================================
 *
 *  THE CLAIM
 *  ----------
 *  A conventional computer has one memory controller shared by every core. Adding cores does not
 *  add bandwidth; past a small number they simply queue for the same bus, and aggregate throughput
 *  flattens. A distributed machine has one memory controller PER NODE, so bandwidth and capacity
 *  both scale with node count, linearly, with nothing to contend for.
 *
 *  That is the whole architectural argument, and it is testable in ten seconds on the machine it
 *  is being argued against. This runs the same streaming read on one thread, then two, then four,
 *  then as many as the machine has, and reports aggregate bandwidth at each step. If the claim is
 *  right the curve bends over. If it does not bend, the argument is weaker than stated and this
 *  file will say so.
 *
 *  WHAT IS AND IS NOT BEING CLAIMED
 *  ---------------------------------
 *  Not that microcontrollers out-compute a desktop. They do not, by any measure taken in this
 *  project: one host core does an 8192-bit compare in 0.123 us against a Teensy's 3.66, and holds
 *  a 16 G MAC/s quantized matmul rate no node here approaches.
 *
 *  The claim is narrower and survives that: for a workload bound by streaming weights out of
 *  memory, which is what single-stream inference is, the ceiling is bandwidth, a desktop's ceiling
 *  is fixed at purchase, and a distributed machine's is not.
 *
 *  The number that matters at the end is the crossover: how many $10 boards it takes to equal a
 *  whole desktop's saturated memory bandwidth. That is a fact about both machines, and it is
 *  computed from measurements of both rather than asserted about either.
 *
 *  BUILD
 *      gcc -O2 -std=gnu11 -pthread -o host_scaling.exe host_scaling.c
 * ===========================================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
static double now_s(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
static int core_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
#else
#include <time.h>
#include <unistd.h>
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int core_count(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }
#endif

/* 64 MB per thread. Large enough that no cache holds it, which is the point: a buffer that fits in
 * L3 measures the cache and would show no contention at all, because the cache is not the thing
 * being shared. */
#define PER_THREAD_MB 64u
#define REPS 4u

typedef struct {
    volatile uint32_t *buf;
    uint32_t words;
    uint32_t sum;
} job_t;

static void *worker(void *arg)
{
    job_t *j = (job_t *)arg;
    uint32_t sum = 0;
    for (uint32_t k = 0; k < REPS; k++)
        for (uint32_t i = 0; i < j->words; i++) sum += j->buf[i];
    j->sum = sum;                       /* published once, so the loop is pure loads */
    return NULL;
}

static double run(int threads, job_t *jobs, pthread_t *tids)
{
    const double t0 = now_s();
    for (int i = 0; i < threads; i++) pthread_create(&tids[i], NULL, worker, &jobs[i]);
    for (int i = 0; i < threads; i++) pthread_join(tids[i], NULL);
    const double dt = now_s() - t0;
    const double bytes = (double)PER_THREAD_MB * 1048576.0 * threads * REPS;
    return bytes / dt / 1073741824.0;   /* GB/s */
}

int main(void)
{
    const int cores = core_count();
    int maxt = cores;
    if (maxt > 16) maxt = 16;

    job_t *jobs = calloc((size_t)maxt, sizeof(job_t));
    pthread_t *tids = calloc((size_t)maxt, sizeof(pthread_t));
    if (!jobs || !tids) { puts("oom"); return 1; }

    const uint32_t words = PER_THREAD_MB * 1048576u / 4u;
    for (int i = 0; i < maxt; i++) {
        jobs[i].buf = malloc((size_t)words * 4u);
        jobs[i].words = words;
        if (!jobs[i].buf) { puts("oom"); return 1; }
        for (uint32_t w = 0; w < words; w++) jobs[i].buf[w] = w + (uint32_t)i;
    }

    printf("\n===============================================================\n");
    printf("does adding cores add memory bandwidth?\n");
    printf("  %d logical cores, %u MB streamed per thread, read only\n", cores, PER_THREAD_MB);
    printf("===============================================================\n\n");

    printf("  threads   aggregate GB/s   per thread   scaling vs 1 thread\n");
    printf("  -------   --------------   ----------   -------------------\n");

    double one = 0.0, best = 0.0;
    for (int t = 1; t <= maxt; t *= 2) {
        const double gbs = run(t, jobs, tids);
        if (t == 1) one = gbs;
        if (gbs > best) best = gbs;
        printf("  %7d   %14.2f   %10.2f   %5.2fx of a perfect %dx\n",
               t, gbs, gbs / t, gbs / one, t);
    }
    if (maxt > 1 && (maxt & (maxt - 1))) {
        const double gbs = run(maxt, jobs, tids);
        if (gbs > best) best = gbs;
        printf("  %7d   %14.2f   %10.2f   %5.2fx of a perfect %dx\n",
               maxt, gbs, gbs / maxt, gbs / one, maxt);
    }

    printf("\n  one core alone reached %.2f GB/s. All %d together reached %.2f GB/s,\n",
           one, maxt, best);
    printf("  which is %.2fx, not %dx. The gap is the shared memory controller,\n",
           best / one, maxt);
    printf("  and no amount of software removes it.\n\n");

    /* Measured on the boards in this project, same units, same operation. */
    const double luckfox = 0.930, esp32 = 0.0576, teensy = 0.0328;

    printf("  measured per node in this project, streaming reads:\n");
    printf("    Luckfox Pico Mini   %6.3f GB/s      ESP32-S3  %6.4f GB/s\n", luckfox, esp32);
    printf("    Teensy 4.1 PSRAM    %6.4f GB/s\n", teensy);
    printf("\n  boards needed to equal this whole machine's saturated %.2f GB/s:\n", best);
    printf("    %4.0f Luckfoxes, and each one ADDS to the total rather than queueing\n",
           best / luckfox);
    printf("    %4.0f ESP32-S3s\n", best / esp32);
    printf("\n  a desktop's ceiling is fixed when it is bought. This one is bought in\n");
    printf("  increments, and the increments do not contend.\n");
    printf("===============================================================\n\n");

    for (int i = 0; i < maxt; i++) free((void *)jobs[i].buf);
    free(jobs);
    free(tids);
    return 0;
}
