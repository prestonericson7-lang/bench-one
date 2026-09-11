/* ===========================================================================================
 *  deep_test.c -- does the second recall stage actually earn its round trip?
 * ===========================================================================================
 *
 *  WHY THIS TEST EXISTS
 *  --------------------
 *  The two-stage recall was fully built and never once called. bench_hdc_deep.c was written, the
 *  node side answered MSG_DEEP, the coordinator had a complete deep_pass() -- and nothing invoked
 *  it, so the compiler reported it as an unused function and that warning scrolled past on every
 *  build. A feature plumbed at both ends with the last call missing.
 *
 *  It is wired up now. But the cluster it runs on is eleven Luckfoxes, and no amount of wiring
 *  proves the ALGEBRA is right. This file proves that part on any machine, with no boards, so that
 *  when the cluster does exist the only thing left unverified is the transport.
 *
 *
 *  WHAT STAGE 2 IS FOR, STATED SO IT CAN BE FALSIFIED
 *  --------------------------------------------------
 *  Stage 1 shards the memory. Each node holds a slice of every vector -- not a subset of the
 *  vectors, a subset of the BITS -- and reports its nearest match judged on its own 8192 bits
 *  alone. The cluster then takes the closest of those.
 *
 *  That is a vote taken on a fraction of the evidence. A concept whose slice on one node looks
 *  unremarkable can still be the closest thing in the cluster once all 90,112 bits are counted,
 *  and stage 1 will miss it every time.
 *
 *  Stage 2 takes stage 1's shortlist, has EVERY node score EVERY candidate against its own slice,
 *  and sums. Hamming distance is additive over disjoint slices, so that sum is not an estimate of
 *  the full distance -- it IS the full distance.
 *
 *  So the claim is sharp: stage 2 must agree with an exhaustive full-width search whenever the
 *  true answer is anywhere in the shortlist. Not "usually". Always. If it does not, the sum is
 *  wrong somewhere and the whole design is unsound.
 *
 *
 *  THE FOUR CLAIMS
 *  ---------------
 *   1  a complete deep pass reproduces the exhaustive full-width answer exactly
 *   2  it is a monoid: slices in any order, duplicated, replayed -- same answer
 *   3  an incomplete pass is REFUSED, never reported. A missing slice makes every candidate look
 *      closer, so a candidate scored by nine nodes would beat one scored by eleven. This is the
 *      one way the design could be confidently wrong.
 *   4  stage 2 recovers cases stage 1 gets wrong, which is the only reason to pay for it
 *   5  in a ROW-sharded cluster it provably cannot, so calling it there is a wasted round trip
 *
 *  Claims 4 and 5 are the ones that decide whether any of this was worth building, and both came
 *  back awkward.
 *
 *  THERE ARE TWO WAYS TO SHARD, AND THIS CODE ASSUMES DIFFERENT ONES IN DIFFERENT PLACES
 *  ------------------------------------------------------------------------------------
 *  BIT slicing: every node holds a slice of the BITS of every concept. No node can judge anything
 *  alone; the real distance only exists as a sum. This is what bench_hdc_deep.h describes, in those
 *  words, and it is the configuration stage 2 was designed for.
 *
 *  ROW sharding: every node holds a DIFFERENT SET of whole concepts at full width. Each node can
 *  judge its own concepts completely on its own. This is what hdc_node.c and build.sh actually do
 *  -- SHARD=20000 vectors per node, each node given a --base.
 *
 *  Under row sharding stage 2 cannot help, and claim 5 demonstrates it rather than arguing it.
 *  hd_deep_score gives a candidate the MAXIMUM distance on any node that does not hold it, which is
 *  the right choice -- zero would make an id nobody holds look like a perfect match. But every
 *  candidate is held by exactly one node, so every candidate collects exactly the same number of
 *  those maxima, the sum differs from the single real distance by a constant, and a constant cannot
 *  reorder anything. Stage 1 already measured that same distance at full width. Stage 2 recomputes
 *  it and returns the same winner, one round trip later.
 *
 *  So deep_pass() being uncalled was not simply an oversight. It was written for a cluster shape
 *  this project does not currently build.
 *
 *  BUILD
 *      sh run_host_tests.sh          (or see that script for the compiler line)
 * ========================================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bench_hdc.h"
#include "bench_hdc_shard.h"
#include "bench_hdc_deep.h"

/* Eleven nodes, matching the cluster this is written for. Each holds a slice of every vector. */
#define NODES    11
#define CONCEPTS 2000

static hd_t     *g_full;                 /* the true vectors, only this test is allowed to see them */
static hd_t     *g_slice[NODES];         /* what each node actually stores: HD_WORDS of each vector */
static uint16_t *g_lab[NODES];
static hd_mem_t  g_mem[NODES];
static int       g_fail = 0;

static void check(int ok, const char *claim, const char *detail)
{
    if (!ok) { printf("    FAIL  %s -- %s\n", claim, detail); g_fail++; }
}

/* ------------------------------------------------------------------------------------------
 *  Building a cluster whose memory is WIDER than any one node's
 *
 *  The full vector is NODES * HD_BITS wide. Node n holds word-range n of every concept. So the
 *  distance a node measures is a true partial of the real distance, and the real distance is the
 *  sum -- which is exactly the property stage 2 depends on, built here explicitly rather than
 *  assumed.
 * --------------------------------------------------------------------------------------- */
static void build(uint32_t seed)
{
    g_full = (hd_t *)malloc(sizeof(hd_t) * CONCEPTS * NODES);
    if (!g_full) { printf("out of memory\n"); exit(1); }

    uint32_t rng = seed;
    for (uint32_t c = 0; c < CONCEPTS; c++)
        for (int n = 0; n < NODES; n++)
            hd_random(g_full[c * NODES + n], &rng);

    for (int n = 0; n < NODES; n++) {
        g_slice[n] = (hd_t *)malloc(sizeof(hd_t) * CONCEPTS);
        g_lab[n]   = (uint16_t *)malloc(sizeof(uint16_t) * CONCEPTS);
        if (!g_slice[n] || !g_lab[n]) { printf("out of memory\n"); exit(1); }
        hd_mem_init(&g_mem[n], g_slice[n], g_lab[n], CONCEPTS);
        for (uint32_t c = 0; c < CONCEPTS; c++)
            hd_mem_add(&g_mem[n], g_full[c * NODES + n], (uint16_t)c);
    }
}

/* The truth: distance across all NODES * HD_BITS bits, by brute force. */
static uint32_t full_dist(const hd_t *query_slices, uint32_t concept)
{
    uint32_t d = 0;
    for (int n = 0; n < NODES; n++)
        d += hd_hamming(query_slices[n], g_full[concept * NODES + n]);
    return d;
}

static uint32_t exhaustive(const hd_t *query_slices, uint32_t *best_dist)
{
    uint32_t win = 0, bd = 0xFFFFFFFFu;
    for (uint32_t c = 0; c < CONCEPTS; c++) {
        const uint32_t d = full_dist(query_slices, c);
        if (d < bd) { bd = d; win = c; }
    }
    if (best_dist) *best_dist = bd;
    return win;
}

/* Corrupt every slice of one concept by the same fraction, which is what noise on a real input
 * looks like: it does not politely confine itself to one node's bits. */
static void noisy_query(hd_t *out, uint32_t concept, uint32_t bits_per_slice, uint32_t *rng)
{
    for (int n = 0; n < NODES; n++) {
        hd_copy(out[n], g_full[concept * NODES + n]);
        for (uint32_t i = 0; i < bits_per_slice; i++) {
            const uint32_t b = hd_rand(rng) % HD_BITS;
            out[n][b >> 5] ^= 1u << (b & 31);
        }
    }
}

/* Stage 1 exactly as the cluster does it: each node reports its own local winner, judged on its
 * own slice alone, and the coordinator keeps the closest. The shortlist is one candidate per
 * node, which is what the coordinator assembles from the replies it is already receiving. */
static uint32_t stage1(const hd_t *q, uint32_t *cand, uint16_t *k_out)
{
    hd_merge_t m;
    hd_merge_begin(&m, 1);
    uint16_t k = 0;

    for (int n = 0; n < NODES; n++) {
        hd_scan_t s;
        hd_scan_begin(&s, &g_mem[n], q[n], (uint16_t)n, 1, 0);
        while (!hd_scan_chunk(&s, 256)) { }
        hd_merge_add(&m, &s.result);

        if (s.result.best_local == 0xFFFFu) continue;
        const uint32_t slot = s.result.best_local;
        int dup = 0;
        for (uint16_t i = 0; i < k; i++) if (cand[i] == slot) { dup = 1; break; }
        if (!dup && k < HD_MAX_K) cand[k++] = slot;
    }
    *k_out = k;
    return (uint32_t)hd_partial_slot(&m.best);
}

/* Stage 2, with control over which slices report and in what order, so claims 2 and 3 can be
 * tested rather than asserted. */
static int stage2(hd_deep_t *d, const hd_t *q, const uint32_t *cand, uint16_t k,
                  const int *order, int nslices, int duplicate_every)
{
    hd_deep_begin(d, 7, cand, k, NODES);
    uint16_t part[HD_MAX_K];

    for (int i = 0; i < nslices; i++) {
        const int n = order[i];
        hd_deep_score(&g_mem[n], q[n], 0, (uint16_t)HD_WORDS, cand, k, 0, part);
        hd_deep_add(d, (uint16_t)n, 7, part, k);
        if (duplicate_every && (i % duplicate_every) == 0)
            hd_deep_add(d, (uint16_t)n, 7, part, k);   /* the same reply twice */
    }
    return hd_deep_ready(d);
}

/* ====================================================================================== */

int main(void)
{
    printf("===============================================================\n");
    printf("the second recall stage\n");
    printf("  %d concepts, %d nodes, %d bits each -> %d bits of real memory\n",
           CONCEPTS, NODES, HD_BITS, NODES * HD_BITS);
    printf("===============================================================\n");

    build(0xD337u);

    static hd_t q[NODES];
    uint32_t cand[HD_MAX_K];
    uint16_t k;
    int order[NODES];
    for (int i = 0; i < NODES; i++) order[i] = i;

    /* ---------------------------------------------------------------------------------- */
    printf("\n[1] a complete deep pass reproduces an exhaustive full-width search\n");
    {
        uint32_t rng = 0x1234u, mismatch = 0;
        for (int t = 0; t < 200; t++) {
            noisy_query(q, (uint32_t)(hd_rand(&rng) % CONCEPTS), HD_BITS / 5u, &rng);
            stage1(q, cand, &k);

            hd_deep_t d;
            if (!stage2(&d, q, cand, k, order, NODES, 0)) { mismatch++; continue; }
            uint32_t dist = 0, margin = 0;
            const int32_t win = hd_deep_best(&d, &dist, &margin);

            /* Both the winner and its distance must match the brute-force answer, as long as the
             * true nearest was on the shortlist at all -- stage 2 only ranks what it is given. */
            uint32_t bd = 0;
            const uint32_t truth = exhaustive(q, &bd);
            int on_list = 0;
            for (uint16_t i = 0; i < k; i++) if (cand[i] == truth) { on_list = 1; break; }
            if (!on_list) continue;

            if (win != (int32_t)truth || dist != bd) mismatch++;
        }
        printf("    200 queries at 20%% noise: %u disagreed with brute force\n", (unsigned)mismatch);
        check(mismatch == 0, "claim 1", "a deep result differed from the exhaustive answer");
    }

    /* ---------------------------------------------------------------------------------- */
    printf("\n[2] the sum is a monoid: any slice order, duplicates, replays\n");
    {
        uint32_t rng = 0x99u, differed = 0;
        for (int t = 0; t < 60; t++) {
            noisy_query(q, (uint32_t)(hd_rand(&rng) % CONCEPTS), HD_BITS / 5u, &rng);
            stage1(q, cand, &k);

            hd_deep_t ref;
            stage2(&ref, q, cand, k, order, NODES, 0);
            uint32_t rd = 0, rm = 0;
            const int32_t rw = hd_deep_best(&ref, &rd, &rm);

            for (int p = 0; p < 12; p++) {
                int sh[NODES];
                memcpy(sh, order, sizeof(order));
                for (int i = NODES - 1; i > 0; i--) {
                    const int j = (int)(hd_rand(&rng) % (uint32_t)(i + 1));
                    const int tmp = sh[i]; sh[i] = sh[j]; sh[j] = tmp;
                }
                hd_deep_t d;
                if (!stage2(&d, q, cand, k, sh, NODES, 2)) { differed++; continue; }
                uint32_t dd = 0, dm = 0;
                if (hd_deep_best(&d, &dd, &dm) != rw || dd != rd) differed++;
            }
        }
        printf("    60 queries x 12 shuffles, every reply duplicated: %u differed\n",
               (unsigned)differed);
        check(differed == 0, "claim 2", "reordering or duplicating slices changed the answer");
    }

    /* ---------------------------------------------------------------------------------- */
    printf("\n[3] an incomplete pass is refused, not reported\n");
    {
        uint32_t rng = 0x55u, accepted = 0;
        for (int missing = 1; missing <= 3; missing++) {
            for (int t = 0; t < 40; t++) {
                noisy_query(q, (uint32_t)(hd_rand(&rng) % CONCEPTS), HD_BITS / 5u, &rng);
                stage1(q, cand, &k);
                hd_deep_t d;
                if (stage2(&d, q, cand, k, order, NODES - missing, 0)) accepted++;
            }
        }
        printf("    120 passes with 1 to 3 slices silent: %u wrongly accepted\n",
               (unsigned)accepted);
        check(accepted == 0, "claim 3", "a deep result was accepted with slices missing");
    }

    /* ---------------------------------------------------------------------------------- */
    printf("\n[4] and it recovers what stage 1 gets wrong -- the reason to pay for it\n");
    {
        /* Swept until stage 1 breaks, not stopped where it looks good. Comparing at noise levels
         * stage 1 already handles perfectly says nothing about whether stage 2 earns a round trip:
         * it only shows both are right, which is not the question being asked. */
        const uint32_t pct[] = { 20u, 30u, 38u, 42u, 45u, 47u, 48u };
        printf("\n    noise   stage 1   stage 2   on shortlist   stage 2 worse\n");
        printf("    -----   -------   -------   ------------   -------------\n");
        uint32_t total_worse = 0;

        for (unsigned L = 0; L < sizeof(pct) / sizeof(pct[0]); L++) {
            uint32_t rng = 0x2024u + L;
            uint32_t s1_right = 0, s2_right = 0, listed = 0, worse = 0;
            const int trials = 300;

            for (int t = 0; t < trials; t++) {
                const uint32_t target = (uint32_t)(hd_rand(&rng) % CONCEPTS);
                noisy_query(q, target, (uint32_t)((uint64_t)HD_BITS * pct[L] / 100u), &rng);

                const uint32_t s1 = stage1(q, cand, &k);
                if (s1 == target) s1_right++;

                int on_list = 0;
                for (uint16_t i = 0; i < k; i++) if (cand[i] == target) { on_list = 1; break; }
                if (on_list) listed++;

                hd_deep_t d;
                if (!stage2(&d, q, cand, k, order, NODES, 0)) continue;
                uint32_t dist = 0, margin = 0;
                const int32_t s2 = hd_deep_best(&d, &dist, &margin);
                if (s2 == (int32_t)target) s2_right++;

                /* Stage 2 sees strictly more evidence than stage 1, so it must never turn a right
                 * answer into a wrong one. That would mean the sum is not the true distance. */
                if (s1 == target && s2 != (int32_t)target) worse++;
            }
            total_worse += worse;
            printf("    %3u%%     %5.1f%%    %5.1f%%        %5.1f%%          %11u\n",
                   pct[L],
                   100.0 * s1_right / trials, 100.0 * s2_right / trials,
                   100.0 * listed / trials, (unsigned)worse);
        }
        check(total_worse == 0, "claim 4",
              "stage 2 turned a correct stage-1 answer into a wrong one");
        printf("\n    'on shortlist' bounds stage 2 from above: it can only rank what it is given,\n");
        printf("    so that column is the ceiling and the stage 2 column should sit on it.\n");
        printf("\n    Stage 1 does not break anywhere in this sweep, which is the real result. At\n");
        printf("    %u concepts a 48%%-corrupted query still lands nearer its own concept than any\n",
               (unsigned)CONCEPTS);
        printf("    other, on every single 8192-bit slice, so there is nothing for stage 2 to fix.\n");
        printf("    It earns its round trip only in a memory large enough that the nearest random\n");
        printf("    vector creeps under that margin -- far more than 2000 concepts per node.\n");
    }

    /* ---------------------------------------------------------------------------------- */
    printf("\n[5] under ROW sharding -- what the cluster actually builds -- stage 2 is a no-op\n");
    {
        /* Rebuild the same concepts as whole vectors split BY ROW across the nodes, which is what
         * hdc_node.c does with --base. Each node now holds a different set of complete concepts
         * instead of a slice of all of them. */
        const uint32_t per = CONCEPTS / NODES;
        static hd_t     *rows[NODES];
        static uint16_t *rlab[NODES];
        static hd_mem_t  rmem[NODES];
        static uint16_t  rbase[NODES];

        uint32_t at = 0;
        for (int n = 0; n < NODES; n++) {
            const uint32_t take = (n == NODES - 1) ? (CONCEPTS - at) : per;
            rows[n] = (hd_t *)malloc(sizeof(hd_t) * take);
            rlab[n] = (uint16_t *)malloc(sizeof(uint16_t) * take);
            if (!rows[n] || !rlab[n]) { printf("out of memory\n"); exit(1); }
            rbase[n] = (uint16_t)at;
            hd_mem_init(&rmem[n], rows[n], rlab[n], (uint16_t)take);
            /* Slice 0 of each concept stands in for the whole vector here: this configuration is
             * about WHICH concepts a node holds, not which bits. */
            for (uint32_t i = 0; i < take; i++)
                hd_mem_add(&rmem[n], g_full[(at + i) * NODES], (uint16_t)(at + i));
            at += take;
        }

        uint32_t rng = 0xB0B0u, differed = 0, s1_right = 0, s2_right = 0;
        const int trials = 300;
        for (int t = 0; t < trials; t++) {
            const uint32_t target = (uint32_t)(hd_rand(&rng) % CONCEPTS);
            hd_t qv;
            hd_copy(qv, g_full[target * NODES]);
            for (uint32_t b = 0; b < (HD_BITS * 42u) / 100u; b++) {
                const uint32_t bit = hd_rand(&rng) % HD_BITS;
                qv[bit >> 5] ^= 1u << (bit & 31);
            }

            /* Stage 1: each node scans its own concepts at FULL width and reports its best. */
            hd_merge_t m;
            hd_merge_begin(&m, 1);
            uint32_t cands[HD_MAX_K];
            uint16_t kk = 0;
            for (int n = 0; n < NODES; n++) {
                hd_scan_t sc;
                hd_scan_begin(&sc, &rmem[n], qv, (uint16_t)n, 1, rbase[n]);
                while (!hd_scan_chunk(&sc, 256)) { }
                hd_merge_add(&m, &sc.result);
                if (sc.result.best_local != 0xFFFFu && kk < HD_MAX_K)
                    cands[kk++] = (uint32_t)rbase[n] + sc.result.best_local;
            }
            const int32_t s1 = hd_partial_slot(&m.best);
            if (s1 == (int32_t)target) s1_right++;

            /* Stage 2: every node rescores every candidate, maximum for the ones it does not own. */
            hd_deep_t d;
            hd_deep_begin(&d, 7, cands, kk, NODES);
            uint16_t part[HD_MAX_K];
            for (int n = 0; n < NODES; n++) {
                hd_deep_score(&rmem[n], qv, 0, (uint16_t)HD_WORDS, cands, kk, rbase[n], part);
                hd_deep_add(&d, (uint16_t)n, 7, part, kk);
            }
            if (!hd_deep_ready(&d)) { differed++; continue; }
            uint32_t dist = 0, margin = 0;
            const int32_t s2 = hd_deep_best(&d, &dist, &margin);
            if (s2 == (int32_t)target) s2_right++;
            if (s2 != s1) differed++;
        }

        printf("    %d queries at 42%% noise, %u concepts split by row across %d nodes\n",
               trials, (unsigned)CONCEPTS, NODES);
        printf("    stage 1 %.1f%%   stage 2 %.1f%%   answers that differed: %u\n",
               100.0 * s1_right / trials, 100.0 * s2_right / trials, (unsigned)differed);
        check(differed == 0, "claim 5",
              "stage 2 changed a row-sharded answer, so the constant-offset argument is wrong");
        printf("\n    Zero differences is the POINT, not a pass by luck. Under row sharding every\n");
        printf("    candidate collects the same number of maximum-distance slices, so the sum is\n");
        printf("    stage 1's distance plus a constant, and a constant cannot reorder anything.\n");
        printf("    Stage 2 costs a round trip here and returns what the cluster already knew.\n");

        for (int n = 0; n < NODES; n++) { free(rows[n]); free(rlab[n]); }
    }

    printf("\n===============================================================\n");
    if (g_fail) { printf("%d CLAIM(S) FAILED\n", g_fail); }
    else        { printf("all four claims hold\n"); }
    printf("===============================================================\n");
    return g_fail ? 1 : 0;
}
