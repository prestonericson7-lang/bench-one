/* ===========================================================================================
 *  born_test.c -- does the natal frame actually have the geometry it claims?
 * ===========================================================================================
 *
 *  bench_born.h makes five claims. A frame that fails any of them is worse than no frame at all,
 *  because everything learned afterwards is positioned in it and a distorted space distorts every
 *  memory placed in it, silently and permanently.
 *
 *  BUILD
 *      gcc -O2 -std=gnu11 -I../shared -o born_test.exe born_test.c \
 *          ../shared/bench_born.c ../shared/bench_hdc.c
 * ===========================================================================================
 */

#include "bench_born.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-52s %s\n", what, ok ? "yes" : "NO");
    if (!ok) { printf("      %s\n", detail); fails++; }
}

int main(void)
{
    printf("\n===============================================================\n");
    printf("the natal frame: %d axes of %d bits\n", (int)BORN_N, HD_BITS);
    printf("===============================================================\n\n");

    born_t a, b;
    born_frame(&a);
    born_frame(&b);

    /* ---- 1. every node must derive the same frame -------------------------------------- */
    printf("[1] identical on every node, with nothing transmitted\n");
    {
        int same = 1;
        for (int i = 0; i < BORN_N && same; i++)
            if (hd_hamming(a.axis[i], b.axis[i]) != 0) same = 0;
        check(same, "two independent derivations agree bit for bit",
              "nodes would be comparing coordinates from different spaces");
    }

    /* ---- 2. the axes must be unrelated, except the one pair that must not be ----------- */
    printf("\n[2] axes are mutually unrelated, and LESS/MORE deliberately is not\n");
    {
        uint32_t worst = 0xFFFFFFFFu, best = 0;
        int wi = 0, wj = 0;
        for (int i = 0; i < BORN_N; i++)
            for (int j = i + 1; j < BORN_N; j++) {
                if ((i == BORN_LESS && j == BORN_MORE)) continue;
                const uint32_t d = hd_hamming(a.axis[i], a.axis[j]);
                if (d < worst) { worst = d; wi = i; wj = j; }
                if (d > best) best = d;
            }
        const uint32_t lm = hd_hamming(a.axis[BORN_LESS], a.axis[BORN_MORE]);
        printf("      closest unrelated pair: %s/%s at %u, furthest %u, random expectation %d\n",
               born_axis_name((born_axis_t)wi), born_axis_name((born_axis_t)wj),
               worst, best, HD_BITS / 2);
        printf("      LESS to MORE: %u, which is the continuum's whole length\n", lm);
        check(worst > (HD_BITS / 2) - 400u,
              "no two independent axes drifted together", "an axis pair is correlated");
        check(lm > 1200u && lm < 3000u,
              "LESS and MORE are related but distinct",
              "the magnitude continuum has no room to move, or too much");
    }

    /* ---- 3. THE claim: magnitude is a continuum, not a set of buckets ------------------ */
    printf("\n[3] magnitude is smooth and monotonic -- the claim everything else rests on\n");
    {
        hd_t m[11];
        for (int i = 0; i <= 10; i++) born_magnitude(&a, (uint32_t)i, 10u, m[i]);

        printf("      distance from value 0.0:");
        for (int i = 0; i <= 10; i += 2) printf(" %.1f=%u", i / 10.0, hd_hamming(m[0], m[i]));
        printf("\n");

        int monotonic = 1;
        for (int i = 1; i <= 10; i++)
            if (hd_hamming(m[0], m[i]) <= hd_hamming(m[0], m[i - 1])) monotonic = 0;
        check(monotonic, "further apart in value is further apart in space",
              "the continuum folds back on itself somewhere");

        check(hd_hamming(m[0], m[10]) == hd_hamming(a.axis[BORN_LESS], a.axis[BORN_MORE]),
              "the ends of the range ARE the LESS and MORE axes",
              "the walk does not reach its endpoints");

        /* Neighbouring values must be nearer than distant ones. This is what makes an unseen
         * quantity land near the ones already learned, which is generalisation. */
        const uint32_t near = hd_hamming(m[3], m[4]);
        const uint32_t far  = hd_hamming(m[3], m[9]);
        printf("      0.3 to 0.4 is %u, 0.3 to 0.9 is %u\n", near, far);
        check(near * 3u < far, "a small change in value is a small change in space",
              "every value is as unrelated as every other, which is the bucket failure");
    }

    /* ---- 4. position ------------------------------------------------------------------- */
    printf("\n[4] nearby positions are nearby vectors\n");
    {
        hd_t p000, p001, p009, p999;
        born_position(&a, 0, 0, 0, 10u, p000);
        born_position(&a, 0, 0, 1, 10u, p001);
        born_position(&a, 0, 0, 9, 10u, p009);
        born_position(&a, 9, 9, 9, 10u, p999);
        const uint32_t d1 = hd_hamming(p000, p001);
        const uint32_t d9 = hd_hamming(p000, p009);
        const uint32_t dd = hd_hamming(p000, p999);
        printf("      one step %u, nine steps on one axis %u, far corner %u\n", d1, d9, dd);
        check(d1 < d9 && d9 < dd, "distance in space tracks distance in the frame",
              "positions are not ordered");
    }

    /* ---- 5. time is a rotation that can be undone -------------------------------------- */
    printf("\n[5] a moment can be returned to the thing it was a moment of\n");
    {
        uint32_t rng = 0x1234u;
        hd_t thing, later, back;
        hd_random(thing, &rng);
        born_moment(&a, thing, 3, later);

        hd_t un;
        hd_bind(un, later, a.axis[BORN_AFTER]);     /* undo the tag  */
        hd_permute(back, un, -3);                   /* undo the time -- NOT in place */

        printf("      the thing three steps on sits %u from the thing itself\n",
               hd_hamming(thing, later));
        check(hd_hamming(thing, back) == 0, "unrotating recovers it exactly",
              "time is lossy, so nothing can be reasoned about across it");
        check(hd_hamming(thing, later) > (HD_BITS / 2) - 400u,
              "but a later moment is its own distinct thing",
              "moments are confusable with each other");
    }

    /* ---- 6. self and world ------------------------------------------------------------- */
    printf("\n[6] it can tell a fact about itself from a fact about the world\n");
    {
        uint32_t rng = 0xABCDu;
        int right = 0;
        uint32_t margin_sum = 0;
        for (int i = 0; i < 200; i++) {
            hd_t fact, tagged;
            uint32_t margin = 0;
            hd_random(fact, &rng);
            const int want_self = (int)(hd_rand(&rng) & 1u);
            if (want_self) born_as_self(&a, fact, tagged);
            else           born_as_world(&a, fact, tagged);
            if (born_is_self(&a, tagged, &margin) == want_self) right++;
            margin_sum += margin;
        }
        printf("      %d of 200 classified correctly, mean margin %u bits\n",
               right, margin_sum / 200u);
        check(right >= 195, "self-knowledge lands in its own region",
              "the machine cannot tell a fact about itself from a fact about anything else");
    }

    printf("\n===============================================================\n");
    printf(fails ? "%d CLAIM(S) FAILED\n" : "the frame holds\n", fails);
    printf("===============================================================\n\n");
    return fails ? 1 : 0;
}
