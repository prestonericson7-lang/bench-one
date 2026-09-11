/* ===========================================================================================
 *  bench_born.c -- see bench_born.h
 * ===========================================================================================
 */

#include "bench_born.h"

static const char *k_names[BORN_N] = {
    "SELF", "WORLD",
    "BEFORE", "NOW", "AFTER",
    "HERE", "NEAR", "FAR",
    "INSIDE", "OUTSIDE",
    "SAME", "OTHER",
    "CAUSE", "EFFECT",
    "PART", "WHOLE",
    "LESS", "MORE",
    "X", "Y", "Z"
};

const char *born_axis_name(born_axis_t a)
{
    return (a < BORN_N) ? k_names[a] : "?";
}

void born_frame(born_t *b)
{
    /* One seed, walked forward. Every node runs this identical loop and lands on identical axes
     * without exchanging a byte. That is the whole reason the frame can be assumed rather than
     * agreed: there is no distribution step to fail, and a node that reboots mid-run comes back
     * into exactly the space it left. */
    uint32_t rng = BORN_SEED;
    for (uint32_t i = 0; i < BORN_N; i++) hd_random(b->axis[i], &rng);

    /* LESS and MORE are the only pair that must NOT be independent.
     *
     * Every other axis wants to be as far from the others as possible, and random vectors give
     * that for free -- two random 8192-bit vectors sit 4096 apart, which is as unrelated as this
     * representation gets. But a magnitude is a walk from one end to the other, and if the ends
     * are unrelated then the midpoint is equally far from both and "half" means nothing. Making
     * MORE a corrupted copy of LESS puts them a known distance apart and gives the walk between
     * them somewhere to go.
     *
     * A quarter of the bits is the right amount. Much less and the two ends are barely
     * distinguishable, so the whole continuum is compressed into noise. Much more, and the
     * midpoint drifts far enough from both ends to be its own unrelated concept, which is exactly
     * the failure mode of using a random vector per bucket. */
    hd_copy(b->axis[BORN_MORE], b->axis[BORN_LESS]);
    for (uint32_t i = 0; i < HD_BITS / 4; i++) {
        const uint32_t bit = hd_rand(&rng) % HD_BITS;
        b->axis[BORN_MORE][bit >> 5] ^= (uint32_t)1u << (bit & 31);
    }
}

void born_place(const born_t *b, const hd_t what, born_axis_t where, hd_t out)
{
    hd_bind(out, what, b->axis[where]);
}

void born_magnitude(const born_t *b, uint32_t value, uint32_t max, hd_t out)
{
    /* Interpolate directly along the LESS..MORE difference.
     *
     * The first version of this called hd_encode_level on LESS and then bound the top half of the
     * range against the difference, which was two mistakes. hd_encode_level walks in a direction
     * fixed by its base, so it walked AWAY from LESS rather than TOWARD MORE, and the conditional
     * bind put a discontinuity in the middle of a continuum -- 0.49 and 0.51 landed in different
     * regions, which is precisely what a magnitude must never do.
     *
     * Walking the differing bits is exact. LESS and MORE disagree on about a quarter of the space;
     * flip that share of them in proportion and value 0 IS the LESS vector, value max IS the MORE
     * vector, and everything between moves smoothly and monotonically from one to the other. */
    if (max == 0) max = 1;
    if (value > max) value = max;

    hd_copy(out, b->axis[BORN_LESS]);

    /* Deterministic order, so the same value always gives the same vector on every node. */
    uint32_t flipped = 0, want = 0;
    uint32_t total = 0;
    for (uint32_t w = 0; w < HD_WORDS; w++)
        total += hd_popcount(b->axis[BORN_LESS][w] ^ b->axis[BORN_MORE][w]);
    want = (uint32_t)(((uint64_t)total * value) / max);

    for (uint32_t w = 0; w < HD_WORDS && flipped < want; w++) {
        uint32_t diff = b->axis[BORN_LESS][w] ^ b->axis[BORN_MORE][w];
        while (diff && flipped < want) {
            const uint32_t lowest = diff & (uint32_t)(-(int32_t)diff);
            out[w] ^= lowest;
            diff &= ~lowest;
            flipped++;
        }
    }
}

void born_position(const born_t *b, uint32_t x, uint32_t y, uint32_t z, uint32_t max, hd_t out)
{
    /* Each coordinate is a magnitude bound to its axis, and the three are bundled. Bundling is
     * what makes the result one vector that is simultaneously near all three components, so two
     * nearby positions stay near each other while a distant one does not. */
    hd_t mx, my, mz, bx, by, bz;
    born_magnitude(b, x, max, mx);
    born_magnitude(b, y, max, my);
    born_magnitude(b, z, max, mz);
    hd_bind(bx, mx, b->axis[BORN_X]);
    hd_bind(by, my, b->axis[BORN_Y]);
    hd_bind(bz, mz, b->axis[BORN_Z]);

    hd_acc_t acc;
    hd_acc_clear(&acc);
    hd_acc_add(&acc, bx);
    hd_acc_add(&acc, by);
    hd_acc_add(&acc, bz);
    hd_acc_result(&acc, out);
}

void born_moment(const born_t *b, const hd_t what, int32_t when, hd_t out)
{
    /* Time is rotation, not a stored number.
     *
     * A permuted vector is recognisably a permutation of the original: it sits far enough away to
     * be a different thing, and close enough in structure that unrotating it recovers the original
     * exactly. So "this, three steps later" is its own vector, is not confusable with "this", and
     * costs nothing to undo. A timestamp stored alongside would give none of that -- two moments
     * of the same thing would be identical vectors with different metadata, and every comparison
     * would have to special-case time. */
    if (when == 0) {
        hd_copy(out, what);
    } else {
        hd_permute(out, what, when);
    }
    /* Tag which side of now it falls on, so a query can ask for the past without knowing how far
     * back it is looking. */
    hd_bind(out, out, b->axis[(when < 0) ? BORN_BEFORE : (when > 0) ? BORN_AFTER : BORN_NOW]);
}

/* SELF and WORLD are marked by MOVING the vector a fixed fraction toward the axis.
 *
 * Two earlier attempts failed, and both failed silently, which is the point of testing this.
 *
 * Binding cannot work: the distance from `what XOR SELF` back to `what` is the popcount of SELF, a
 * constant for every input, so a test built on it discriminates nothing while appearing to.
 *
 * Weighted bundling cannot work either, and the reason is arithmetic rather than conceptual. A
 * majority over two contributors weighted 3 and 1 is decided entirely by the heavier one -- every
 * bit comes out as `what` and the axis contributes literally nothing. The classifier scored 101 of
 * 200, which is precisely chance, and chance is what a coin flip looks like when the code is doing
 * nothing at all.
 *
 * Moving a controlled share of the differing bits works because the share is the parameter. At a
 * quarter, the tagged vector sits about 3072 from its own axis and about 4096 from the other, so
 * the margin is roughly a thousand bits and the decision is not close. Meanwhile it stays about
 * 1024 from the untagged original, far nearer than anything unrelated, so the thing itself is
 * still recognisable through the tag. */
static void toward(hd_t out, const hd_t from, const hd_t target, uint32_t numer, uint32_t denom)
{
    hd_copy(out, from);

    uint32_t total = 0;
    for (uint32_t w = 0; w < HD_WORDS; w++) total += hd_popcount(from[w] ^ target[w]);
    const uint32_t want = (uint32_t)(((uint64_t)total * numer) / denom);

    uint32_t flipped = 0;
    for (uint32_t w = 0; w < HD_WORDS && flipped < want; w++) {
        uint32_t diff = from[w] ^ target[w];
        while (diff && flipped < want) {
            const uint32_t lowest = diff & (uint32_t)(-(int32_t)diff);
            out[w] ^= lowest;
            diff &= ~lowest;
            flipped++;
        }
    }
}

void born_as_self(const born_t *b, const hd_t what, hd_t out)
{
    toward(out, what, b->axis[BORN_SELF], 1u, 4u);
}

void born_as_world(const born_t *b, const hd_t what, hd_t out)
{
    toward(out, what, b->axis[BORN_WORLD], 1u, 4u);
}

int born_is_self(const born_t *b, const hd_t v, uint32_t *margin_out)
{
    const uint32_t ds = hd_hamming(v, b->axis[BORN_SELF]);
    const uint32_t dw = hd_hamming(v, b->axis[BORN_WORLD]);
    if (margin_out) *margin_out = (ds < dw) ? (dw - ds) : (ds - dw);
    return (ds < dw) ? 1 : 0;
}
