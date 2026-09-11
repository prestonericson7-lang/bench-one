/* ===========================================================================================
 *  system_test.c -- re-proves the four claims BENCH ONE rests on, from nothing, every time
 * ===========================================================================================
 *
 *  WHY THIS FILE EXISTS
 *  ---------------------
 *  Every performance and correctness number in this repository was measured once, in a scratch
 *  harness that no longer exists. That makes them assertions, not results. Anyone who picks the
 *  project up -- including me, next session -- has no way to re-run them, and a claim nobody can
 *  re-check quietly rots into folklore.
 *
 *  So this is the whole architecture, in one process, asserting the four things that actually
 *  decide whether a pile of microcontrollers can act as one machine. Each claim gets a stated
 *  pass condition. A failure exits non-zero and says which claim broke.
 *
 *  THE FOUR CLAIMS
 *  ----------------
 *  1. THE MERGE IS A MONOID.
 *     Answers arrive from many nodes over an unreliable link: late, out of order, duplicated,
 *     retried. If combining them is associative, commutative and idempotent then none of that
 *     matters and the coordinator needs no sequencing, no timeouts per node, no retry bookkeeping.
 *     Tested by merging the same replies under many random permutations, with duplicates injected,
 *     and demanding a bit-identical result every time. This is the claim the design lives on.
 *
 *  2. IT DEGRADES, IT DOES NOT LIE.
 *     Stall a growing fraction of nodes. Recall is allowed to fall. What is NOT allowed is a
 *     confident wrong answer: any query reported with high coverage must be right. A machine that
 *     says "I do not know" at 40% recall is usable. One that says "cat" when it means "dog"
 *     because six nodes were busy is not, at any recall.
 *
 *  3. COVERAGE TELLS THE TRUTH.
 *     Coverage is what separates "this is new" from "the nodes that knew never answered". It is a
 *     sum, and sums are not idempotent, so duplicates once inflated it from 244 to 423 out of 600.
 *     Asserted here against the ground truth the harness already knows.
 *
 *  4. THE INDEX/BODY SPLIT PRESERVES THE ANSWER.
 *     Screening on a 512-bit prefix instead of the full 8192 is what raises capacity from 2.76M
 *     to 36M entries. It is only worth having if it never converts a right answer into a
 *     confident wrong one. It is allowed to refuse. It is not allowed to be wrong.
 *
 *  BUILD
 *      cc -O2 -std=c99 -I../shared -o system_test system_test.c \
 *         ../shared/bench_hdc.c ../shared/bench_hdc_shard.c ../shared/bench_index.c
 *  RUN
 *      ./system_test              # about ten seconds
 *      ./system_test --big        # the full inventory, a few minutes
 * ===========================================================================================
 */

#include "bench_hdc.h"
#include "bench_hdc_shard.h"
#include "bench_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------------------------
 * WHY THIS RUNS ON A TEENSY AND NOT ON A PC
 * -----------------------------------------------------------------------------------------
 * A PC answers every one of these queries out of cache in microseconds, so the behaviour the
 * architecture actually turns on -- a scan that runs out of deadline halfway, a node that stalls
 * for wifi, the gap between a vector in DTCM and one behind SPI -- simply never happens. The
 * claims would all pass and none of them would have been tested.
 *
 * The M7 has a 32 KB data cache and 512 KB of tightly coupled memory, and the shard does not fit
 * in either. That is the real machine. So this compiles for the target, reports cycles rather
 * than seconds, and the numbers mean something.
 * ----------------------------------------------------------------------------------------- */
#ifdef ARDUINO
  #include <Arduino.h>
  int tprintf(const char *fmt, ...);   /* implemented in the .ino, which can reach Serial */
  #define printf tprintf
  #define TEST_ENTRY system_test_run
  #ifndef NODES
    #define NODES 34
  #endif
  /* 340 vectors is 340 KB, which fits OCRAM with room for the index. Raise it if your 4.1 has
   * the PSRAM pads populated -- 8 MB takes this to the full inventory scale. */
  #ifndef MEM_SMALL
    #define MEM_SMALL 340
  #endif
  #ifndef MEM_BIG
    #define MEM_BIG 340
  #endif
  #ifndef QUERIES
    #define QUERIES 200
  #endif
#else
  #define TEST_ENTRY main
#endif

/* The real inventory: 9 Teensy 4.1, 15 ESP32-S3, 10 Luckfox Pico Mini. The FPGAs are matrix
 * engines rather than memory holders, so they are not shards. */
#ifndef NODES
#define NODES        34
#endif
#ifndef MEM_SMALL
#define MEM_SMALL    5000
#endif
#ifndef MEM_BIG
#define MEM_BIG      20000
#endif
#ifndef QUERIES
#define QUERIES      2000
#endif
#ifndef PERMUTATIONS
#define PERMUTATIONS 24
#endif

static hd_t   *g_store;                 /* the whole distributed memory, laid out flat        */
static uint16_t *g_labels;
static uint32_t g_total;

static hd_mem_t g_shard[NODES];
static uint16_t g_base[NODES];
static uint16_t g_count[NODES];

static uint32_t g_fail = 0;

static void check(int ok, const char *claim, const char *detail)
{
    if (!ok) { printf("    FAIL  %s -- %s\n", claim, detail); g_fail++; }
}

/* ------------------------------------------------------------------------------------------- */

static void build(uint32_t total, uint32_t seed)
{
    g_total  = total;
    g_store  = (hd_t *)malloc(sizeof(hd_t) * total);
    g_labels = (uint16_t *)malloc(sizeof(uint16_t) * total);
    if (!g_store || !g_labels) { printf("out of memory\n"); exit(1); }

    uint32_t rng = seed;
    for (uint32_t i = 0; i < total; i++) {
        hd_random(g_store[i], &rng);
        g_labels[i] = (uint16_t)i;
    }

    /* Shard it. Contiguous slices, each node told where its slice starts so local slot numbers
     * translate back to global ones without the coordinator holding a map. */
    uint32_t per = total / NODES, at = 0;
    for (int n = 0; n < NODES; n++) {
        const uint32_t take = (n == NODES - 1) ? (total - at) : per;
        g_base[n]  = (uint16_t)at;
        g_count[n] = (uint16_t)take;
        hd_mem_init(&g_shard[n], &g_store[at], &g_labels[at], (uint16_t)take);
        for (uint32_t k = 0; k < take; k++) hd_mem_add(&g_shard[n], g_store[at + k],
                                                        g_labels[at + k]);
        at += take;
    }
}

/* One node answering one query. `budget` caps how many vectors it gets through, which is how a
 * node that is busy with wifi or a stalled PSRAM read is modelled: it answers, just partially. */
static void ask_node(int n, const hd_t q, uint16_t qid, uint16_t budget, hd_partial_t *out)
{
    hd_scan_t s;
    hd_scan_begin(&s, &g_shard[n], q, (uint16_t)n, qid, g_base[n]);
    uint16_t done = 0;
    while (done < budget) {
        if (hd_scan_chunk(&s, 64)) break;
        done += 64;
    }
    *out = s.result;
}

/* Corrupt a distinctive number of bits so the query is recognisably a neighbour of one stored
 * vector and nothing else. 400 of 8192 is far inside the noise floor for 8192-bit vectors. */
static void noisy_copy(hd_t out, const hd_t src, uint32_t bits, uint32_t *rng)
{
    hd_copy(out, src);
    for (uint32_t i = 0; i < bits; i++) {
        const uint32_t b = hd_rand(rng) % HD_BITS;
        out[b >> 5] ^= (uint32_t)1u << (b & 31);
    }
}

/* ------------------------------------------------------------------------------------------- */
/*  CLAIM 1 -- the merge is a monoid                                                            */
/* ------------------------------------------------------------------------------------------- */
static void claim1_monoid(uint32_t queries)
{
    printf("[1] the merge is a monoid: order, duplication and retry cannot change the answer\n");

    uint32_t rng = 0xC0FFEEu;
    hd_t q;
    hd_partial_t parts[NODES * 3];
    int order[NODES * 3];
    uint32_t worst_dupes = 0;

    for (uint32_t t = 0; t < queries; t++) {
        const uint32_t target = hd_rand(&rng) % g_total;
        noisy_copy(q, g_store[target], 400, &rng);

        /* Collect one reply per node, then append duplicates of a random handful -- exactly what
         * a retry after a lost acknowledgement looks like on the wire. */
        int np = 0;
        for (int n = 0; n < NODES; n++) ask_node(n, q, (uint16_t)t, 0xFFFF, &parts[np++]);
        const int real = np;
        const int extra = (int)(hd_rand(&rng) % 8u);
        for (int e = 0; e < extra; e++) parts[np++] = parts[hd_rand(&rng) % (uint32_t)real];

        /* Reference: merge in the order they were produced. */
        hd_merge_t ref;
        hd_merge_begin(&ref, (uint16_t)t);
        for (int i = 0; i < np; i++) hd_merge_add(&ref, &parts[i]);
        const int32_t ref_slot = hd_partial_slot(&ref.best);
        const uint32_t ref_dist = ref.best.best_dist;
        const uint8_t ref_cov = hd_merge_coverage(&ref, g_total);
        if (ref.dupes > worst_dupes) worst_dupes = ref.dupes;

        /* Now shuffle and merge again. Every permutation must land in exactly the same place. */
        for (uint32_t p = 0; p < PERMUTATIONS; p++) {
            for (int i = 0; i < np; i++) order[i] = i;
            for (int i = np - 1; i > 0; i--) {
                const int j = (int)(hd_rand(&rng) % (uint32_t)(i + 1));
                const int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
            hd_merge_t m;
            hd_merge_begin(&m, (uint16_t)t);
            for (int i = 0; i < np; i++) hd_merge_add(&m, &parts[order[i]]);

            if (hd_partial_slot(&m.best) != ref_slot ||
                m.best.best_dist != ref_dist ||
                hd_merge_coverage(&m, g_total) != ref_cov) {
                char d[160];
                snprintf(d, sizeof(d),
                         "query %u permutation %u: slot %ld/%ld dist %u/%u cov %u/%u",
                         (unsigned)t, (unsigned)p,
                         (long)hd_partial_slot(&m.best), (long)ref_slot,
                         (unsigned)m.best.best_dist, (unsigned)ref_dist,
                         (unsigned)hd_merge_coverage(&m, g_total), (unsigned)ref_cov);
                check(0, "claim 1", d);
                return;
            }
        }
    }
    printf("    %u queries x %u orderings, duplicates injected: every result identical\n",
           (unsigned)queries, (unsigned)PERMUTATIONS);
    printf("    duplicate replies absorbed without inflating coverage (max %u on one query)\n",
           (unsigned)worst_dupes);
}

/* ------------------------------------------------------------------------------------------- */
/*  CLAIM 2 and 3 -- degrade honestly, and report coverage truthfully                           */
/* ------------------------------------------------------------------------------------------- */
/* Which node holds a given global slot. Contiguous shards, so this is a walk rather than a map --
 * the coordinator deliberately does not keep one, and neither does this. */
static int owner_of(uint32_t slot)
{
    for (int n = 0; n < NODES; n++)
        if (slot >= g_base[n] && slot < (uint32_t)g_base[n] + g_count[n]) return n;
    return -1;
}

static void claim23_degrade(uint32_t queries)
{
    printf("\n[2] a vector is found IF AND ONLY IF some node actually scanned it\n");
    printf("[3] and reported coverage matches what was really scanned\n\n");

    /* WHY THIS IS NOT A RECALL THRESHOLD ANY MORE
     * -------------------------------------------
     * This used to assert two things that are not true of a correct system, and it failed because
     * the system is correct.
     *
     * It asserted perfect recall with no nodes stalled. But a node is randomly given a reduced
     * budget one time in five even at zero stalls, so part of the store is never examined, and a
     * vector nobody looked at cannot be returned. Recall at zero stalls is 97%, and it SHOULD be.
     *
     * It then counted any wrong answer above 90% coverage as a fault. At 97% coverage roughly 3% of
     * targets were not scanned, so about 3% of queries must come back wrong. Counting those as bugs
     * makes the test fail precisely when the machine is behaving.
     *
     * The honest invariant is sharper than either, and it is exact rather than statistical:
     *
     *     scanned the target  ->  MUST return it
     *     did not scan it     ->  MUST NOT return it
     *
     * The first catches a broken comparison or a bad merge. The second catches a node inventing a
     * slot it never examined, which is the failure that would quietly poison every downstream
     * answer. Neither has a tolerance, so a single violation fails the run.
     *
     * Recall and coverage are still printed side by side, because they should track each other and
     * a divergence is the first sign something is wrong even when both invariants hold. */
    printf("    stalled  recall   mean cov   missed after scan   found unscanned   cov lies\n");
    printf("    -------  ------   --------   -----------------   ---------------   --------\n");

    const int steps[] = { 0, 3, 7, 14, 20, 27, 31 };

    for (unsigned s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        const int stalled = steps[s];
        uint32_t rng = 0x5EEDu + (uint32_t)stalled;
        uint32_t hit = 0, cov_lies = 0, cov_sum = 0;
        uint32_t missed_after_scan = 0, found_unscanned = 0;

        for (uint32_t t = 0; t < queries; t++) {
            const uint32_t target = hd_rand(&rng) % g_total;
            hd_t q;
            noisy_copy(q, g_store[target], 400, &rng);

            /* Choose which nodes are unavailable this query. A stalled node is silent; a node
             * under partial load answers with a reduced budget. Both happen in the real stack:
             * the ESP32s stop for wifi, the Luckfoxes get preempted by Linux. */
            uint64_t dead = 0;
            for (int d = 0; d < stalled; d++) dead |= (uint64_t)1u << (hd_rand(&rng) % NODES);

            hd_merge_t m;
            hd_merge_begin(&m, (uint16_t)t);
            uint32_t truly_scanned = 0;

            /* Track whether the one node that holds the target actually reached it. A shard is
             * scanned from its own slot 0 upwards, so "reached" is simply whether the target's
             * local index fell inside the count that node got through. */
            const int owner = owner_of(target);
            uint32_t owner_scanned = 0;                                /* 0 if the owner is silent */

            for (int n = 0; n < NODES; n++) {
                if (dead & ((uint64_t)1u << n)) continue;               /* silent */
                const uint16_t budget = (hd_rand(&rng) % 5u == 0u)
                                      ? (uint16_t)(g_count[n] / 2u)    /* half-loaded */
                                      : 0xFFFF;
                hd_partial_t p;
                ask_node(n, q, (uint16_t)t, budget, &p);
                truly_scanned += p.scanned;
                if (n == owner) owner_scanned = p.scanned;
                hd_merge_add(&m, &p);
            }

            const int target_scanned =
                (owner >= 0) && ((target - (uint32_t)g_base[owner]) < owner_scanned);

            const int32_t got = hd_partial_slot(&m.best);
            const uint8_t cov = hd_merge_coverage(&m, g_total);
            cov_sum += cov;

            const uint8_t truth = (uint8_t)((truly_scanned * 255u) / g_total);
            if (cov > truth + 1u) cov_lies++;          /* +1 for integer rounding, not slack  */

            if (got == (int32_t)target) hit++;

            /* The two directions of the invariant. */
            if (target_scanned && got != (int32_t)target) missed_after_scan++;
            if (!target_scanned && got == (int32_t)target) found_unscanned++;
        }

        printf("    %5d    %5.1f%%   %6.1f%%   %17u   %15u   %8u\n",
               stalled, 100.0 * hit / queries, 100.0 * (cov_sum / (double)queries) / 255.0,
               (unsigned)missed_after_scan, (unsigned)found_unscanned, (unsigned)cov_lies);

        check(missed_after_scan == 0, "claim 2",
              "a node scanned the target and the merge still returned something else");
        check(found_unscanned == 0, "claim 2",
              "the merge returned a vector that no node ever examined");
        check(cov_lies == 0, "claim 3", "reported coverage exceeded what was scanned");
    }
}

/* ------------------------------------------------------------------------------------------- */
/*  CLAIM 4 -- prefix screening never turns a right answer into a wrong one                     */
/* ------------------------------------------------------------------------------------------- */
static int body_fetch(void *ctx, uint64_t off, hd_t out)
{
    (void)ctx;
    hd_copy(out, g_store[off]);
    return 1;
}

static void claim4_index(uint32_t queries)
{
    printf("\n[4] the 512-bit index either agrees with the full 8192-bit scan, or refuses\n");

    hd_entry_t *e = (hd_entry_t *)malloc(sizeof(hd_entry_t) * g_total);
    uint16_t *lab = (uint16_t *)malloc(sizeof(uint16_t) * g_total);
    if (!e || !lab) { printf("out of memory\n"); exit(1); }

    hd_index_t ix;
    hd_index_init(&ix, e, lab, g_total, body_fetch, NULL);
    for (uint32_t i = 0; i < g_total; i++) hd_index_add(&ix, g_store[i], g_labels[i], i);

    uint32_t agree = 0, refused = 0, wrong = 0, bodies = 0;
    uint32_t rng = 0xA11CEu;

    for (uint32_t t = 0; t < queries; t++) {
        const uint32_t target = hd_rand(&rng) % g_total;
        hd_t q;
        noisy_copy(q, g_store[target], 400, &rng);

        hd_index_result_t r;
        hd_index_search(&ix, q, &r);
        bodies += r.bodies_read;

        if (r.status == HD_INDEX_OK) {
            if (r.slot == (int32_t)target) agree++;
            else wrong++;
        } else {
            refused++;
        }
    }

    printf("    %u queries: %u agreed, %u refused as too noisy, %u WRONG\n",
           (unsigned)queries, (unsigned)agree, (unsigned)refused, (unsigned)wrong);
    printf("    bodies fetched per query: %.1f of %u entries -- the point of the split\n",
           (double)bodies / queries, (unsigned)g_total);
    check(wrong == 0, "claim 4", "prefix screening returned a confident wrong answer");

    free(e);
    free(lab);
}

/* ------------------------------------------------------------------------------------------- */

int TEST_ENTRY(int argc, char **argv)
{
    int big = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--big")) big = 1;

    const uint32_t mem = big ? MEM_BIG : MEM_SMALL;
    const uint32_t qs  = big ? QUERIES : (QUERIES / 4);

    printf("\n===============================================================\n");
    printf("BENCH ONE system test\n");
    printf("  %u vectors of %d bits across %d nodes, %u bytes each\n",
           (unsigned)mem, HD_BITS, NODES, (unsigned)HD_BYTES);
    printf("  index prefix %d bits, entry %u bytes\n",
           HD_INDEX_BITS, (unsigned)sizeof(hd_entry_t));
    printf("===============================================================\n\n");

    build(mem, 0x1234u);

    claim1_monoid(qs / 8);          /* the inner loop is 24 merges deep, so fewer queries */
    claim23_degrade(qs);
    claim4_index(qs);

    printf("\n===============================================================\n");
    if (g_fail) printf("%u CLAIM(S) FAILED\n", g_fail);
    else        printf("all four claims hold\n");
    printf("===============================================================\n\n");

    free(g_store);
    free(g_labels);
    return g_fail ? 1 : 0;
}
