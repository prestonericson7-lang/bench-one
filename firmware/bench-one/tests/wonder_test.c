/* ===========================================================================================
 *  wonder_test.c -- does it hold possibilities, and does it ask its own questions?
 * ===========================================================================================
 *
 *  Two claims, and the second is the one the whole project has been aiming at.
 *
 *  SUPERPOSITION has a capacity and nobody knows it until it is measured. A state holding k
 *  candidates keeps every one of them retrievable up to some k and then stops, and the point where
 *  it stops is a hard limit on how much ambiguity the machine can carry before it is forced to
 *  guess. That number is measured here, not assumed.
 *
 *  SELF-DIRECTION is tested by giving it a memory with known flaws and NOTHING ELSE. No query, no
 *  input, no goal. If it finds the two memories it cannot tell apart, and the one that relates to
 *  nothing, and turns them into questions on its own, then the mechanism works. If it needs to be
 *  told where to look, it does not.
 *
 *  BUILD
 *      gcc -O2 -std=gnu11 -I../shared -o wonder_test.exe wonder_test.c \
 *          ../shared/bench_wonder.c ../shared/bench_born.c ../shared/bench_hdc.c
 * ===========================================================================================
 */

#include "bench_wonder.h"
#include "bench_born.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP 64

static int fails = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-54s %s\n", what, ok ? "yes" : "NO");
    if (!ok) { printf("      %s\n", detail); fails++; }
}

/* A copy with `bits` positions flipped: the same thing, seen through noise. */
static void noisy(hd_t out, const hd_t src, uint32_t bits, uint32_t *rng)
{
    hd_copy(out, src);
    for (uint32_t i = 0; i < bits; i++) {
        const uint32_t b = hd_rand(rng) % HD_BITS;
        out[b >> 5] ^= (uint32_t)1u << (b & 31);
    }
}

int main(void)
{
    hd_t     *store  = (hd_t *)malloc(sizeof(hd_t) * CAP);
    uint16_t *labels = (uint16_t *)malloc(sizeof(uint16_t) * CAP);
    hd_acc_t *scratch = (hd_acc_t *)malloc(sizeof(hd_acc_t));
    if (!store || !labels || !scratch) { printf("out of memory\n"); return 1; }

    hd_mem_t mem;
    hd_mem_init(&mem, store, labels, CAP);

    uint32_t rng = 0x5A17E33Du;

    printf("\n===============================================================\n");
    printf("holding possibilities, and asking its own questions\n");
    printf("===============================================================\n\n");

    /* ---- 1. how many things can it hold at once? ---------------------------------------- */
    printf("[1] superposition capacity -- how much ambiguity it can carry\n");
    for (uint16_t i = 0; i < 20; i++) { hd_t v; hd_random(v, &rng); hd_mem_add(&mem, v, i); }

    printf("      members   distance to a member   noise floor is %u\n", HD_BITS / 2);
    uint8_t last_good = 0;
    for (uint8_t k = 1; k <= HD_SUPER_MAX; k++) {
        hd_super_t s;
        hd_super_clear(&s);
        for (uint8_t i = 0; i < k; i++) hd_super_add(&s, i);
        hd_super_settle(&s, &mem, scratch);

        uint32_t worst = 0;
        for (uint8_t i = 0; i < k; i++) {
            const uint32_t d = hd_super_dist(&s, mem.v[i]);
            if (d > worst) worst = d;
        }
        const int readable = (worst < hd_super_far(&s));
        if (readable) last_good = k;
        if (k <= 2 || k == 4 || k == 8 || k == 12 || k == 14 || k == 15 || k == 16)
            printf("      %7u   %20u   %s\n", k, worst, readable ? "all still readable" : "LOST");
    }
    printf("      it can hold %u candidates at once before members fall into the noise\n",
           last_good);
    check(last_good >= 4, "it can hold several possibilities at once",
          "the superposition collapses to unreadable almost immediately");

    /* ---- 2. evidence collapses it to the right one --------------------------------------- */
    printf("\n[2] collapse -- evidence picks, and nothing had to be decided before it arrived\n");
    {
        int right = 0;
        const int trials = 200;
        for (int t = 0; t < trials; t++) {
            hd_super_t s;
            hd_super_clear(&s);
            const uint16_t k = 5;
            uint16_t base = (uint16_t)(hd_rand(&rng) % 10u);
            for (uint16_t i = 0; i < k; i++) hd_super_add(&s, (uint16_t)(base + i));
            hd_super_settle(&s, &mem, scratch);

            const uint16_t want = (uint16_t)(base + (hd_rand(&rng) % k));
            hd_t probe;
            noisy(probe, mem.v[want], 400, &rng);

            uint32_t d = 0;
            if (hd_super_collapse(&s, &mem, probe, &d) == (int32_t)want) right++;
        }
        printf("      %d of %d collapsed to the candidate the evidence pointed at\n",
               right, trials);
        check(right >= trials - 2, "evidence selects the right member",
              "the state cannot be resolved by what arrives later");
    }

    /* ---- 3. THE ONE THAT MATTERS: does it ask without being asked? ----------------------- */
    printf("\n[3] self-direction -- a flawed memory, no query, no input, no goal\n");
    {
        hd_mem_t m2;
        hd_t     *s2 = (hd_t *)malloc(sizeof(hd_t) * CAP);
        uint16_t *l2 = (uint16_t *)malloc(sizeof(uint16_t) * CAP);
        hd_mem_init(&m2, s2, l2, CAP);

        /* Twelve ordinary unrelated memories. */
        for (uint16_t i = 0; i < 12; i++) { hd_t v; hd_random(v, &rng); hd_mem_add(&m2, v, i); }

        /* Slot 12 is a near-copy of slot 3. It cannot tell these two apart, and it has not been
         * told so. */
        hd_t twin;
        noisy(twin, m2.v[3], 300, &rng);
        hd_mem_add(&m2, twin, 12);

        /* Slot 13 is unlike anything -- but so are the twelve random ones, which are all mutually
         * distant. So ORPHAN legitimately picks whichever is marginally loneliest, and this test
         * exercises that path WITHOUT verifying it against a known answer. Doing that properly
         * needs a memory where most items are clustered and one genuinely is not, which is the
         * shape real learned memory takes and this synthetic one does not. Stated rather than
         * papered over: the confusion path below is verified, the orphan path is only exercised. */
        hd_t lone;
        hd_random(lone, &rng);
        hd_mem_add(&m2, lone, 13);

        wonder_t w;
        wonder_init(&w, 0xC0FFEEu);

        printf("      running idle ticks with nothing else happening\n");
        for (int t = 0; t < 8; t++) wonder_tick(&w, &m2, 64);

        printf("      formed %u question(s) from %u ticks, %u still open\n",
               w.formed, w.ticks, w.n_open);

        const wonder_q_t *top = wonder_top(&w);
        if (top) {
            printf("      most urgent: %s about slots %u and %u, tension %u\n",
                   wonder_kind_name(top->kind), top->a, top->b, top->tension);
        }
        check(w.formed > 0, "it generated questions with nothing asking it anything",
              "it is inert when idle, which is the whole thing this was for");

        int found_confusion = 0;
        for (uint8_t i = 0; i < WONDER_MAX_OPEN; i++) {
            if (!w.open[i].live) continue;
            if (w.open[i].kind == WQ_CONFUSION &&
                ((w.open[i].a == 3 && w.open[i].b == 12) ||
                 (w.open[i].a == 12 && w.open[i].b == 3))) found_confusion = 1;
        }
        check(found_confusion, "it found the exact pair it cannot tell apart",
              "it noticed something, but not the flaw that was actually planted");

        /* ---- 4. a question is a standing request against the future ---------------------- */
        printf("\n[4] an arriving experience answers what it wanted to know\n");
        {
            /* Something clearly slot 3 and clearly not slot 12: it tells the pair apart. */
            hd_t evidence;
            noisy(evidence, m2.v[3], 100, &rng);
            const uint8_t closed = wonder_offer(&w, &m2, evidence, 3824u);
            printf("      experience closed %u open question(s), %u remain\n", closed, w.n_open);
            check(closed > 0, "experience it was waiting for resolved a question",
                  "questions are kept but never answered, which makes them decoration");
        }

        free(s2);
        free(l2);
    }

    /* ---- 5. it can hold a fact about itself ---------------------------------------------- */
    printf("\n[5] a fact about itself is an ordinary memory in its own region\n");
    {
        born_t b;
        born_frame(&b);
        wonder_t w;
        wonder_init(&w, 0xBEEFu);

        hd_t role, value, fact, tagged;
        hd_random(role, &rng);
        hd_random(value, &rng);
        wonder_self_fact(&w, role, value, fact);
        born_as_self(&b, fact, tagged);

        uint32_t margin = 0;
        const int is_self = born_is_self(&b, tagged, &margin);
        printf("      the machine places its own fact in the self region by %u bits\n", margin);
        check(is_self, "a fact about itself is recognisably about itself",
              "self-knowledge is indistinguishable from a fact about the world");

        hd_t other;
        hd_random(other, &rng);
        check(hd_hamming(fact, other) > 3824u,
              "and it is not confusable with an unrelated memory",
              "self-facts collide with ordinary ones");
    }

    printf("\n===============================================================\n");
    if (fails) printf("%d CLAIM(S) FAILED\n", fails);
    else       printf("it holds possibilities, and it asks its own questions\n");
    printf("===============================================================\n\n");

    free(store);
    free(labels);
    free(scratch);
    return fails ? 1 : 0;
}
