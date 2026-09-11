/* ===========================================================================================
 *  bench_born.h -- the frame the machine is born into
 * ===========================================================================================
 *
 *  A mind does not arrive in an empty space. A newborn animal has never seen anything, and yet
 *  the space it sees into already has axes: near and far, before and after, inside and outside,
 *  self and not-self, more and less. None of that is knowledge. It is geometry, laid down long
 *  before the animal, and it is the reason the first thing it experiences lands SOMEWHERE rather
 *  than nowhere.
 *
 *  This file is that geometry. It is what the difference looks like between assembling a machine
 *  and then animating it, and letting something come into being in a place that already has shape.
 *
 *
 *  WHY A FRAME IS NOT CHEATING, AND NOT PRE-LOADED KNOWLEDGE
 *  ----------------------------------------------------------
 *  Nothing here asserts a single fact about the world. There is no "fire is hot", no "dogs have
 *  four legs", not one claim that could be true or false. What the frame provides is a set of
 *  directions along which things are ABLE to differ.
 *
 *  Without it, every experience is an independent random vector, every pair is equally unrelated,
 *  and the only structure the machine can ever find is structure it has seen enough repetitions to
 *  extract from nothing. With it, "slightly nearer" is a small step and "much later" is a large
 *  one, before a single observation has been made. That is not an answer supplied in advance. It
 *  is a space in which answers can be close to each other, which is the precondition for
 *  generalising at all.
 *
 *  The honest way to put it: this is a prior over SIMILARITY, not over truth.
 *
 *
 *  THE FRAME MUST BE IDENTICAL ON EVERY NODE, AND IT IS
 *  -----------------------------------------------------
 *  Thirty-six boards that each invented their own axes would produce vectors that cannot be
 *  compared, and every merge in this system would be combining coordinates from different spaces
 *  while looking like it worked. So the frame is derived deterministically from one fixed
 *  constant. Every node computes the same 8192-bit axes from that constant at boot, in about a
 *  millisecond, and nothing is transmitted. No handshake, no distribution step, nothing to get out
 *  of sync -- a node that reboots mid-run comes back into exactly the same space it left.
 *
 *  BORN_SEED is therefore not a tuning parameter. Changing it invalidates every vector ever stored.
 *
 *
 *  WHAT THE AXES ARE, AND WHY THESE ONES
 *  --------------------------------------
 *  These are the distinctions that have to exist before experience can be organised at all, rather
 *  than a catalogue of things worth knowing. Each earns its place by being a distinction no amount
 *  of observation can bootstrap from nothing:
 *
 *      SELF / WORLD          the boundary. Without it there is no difference between a thing the
 *                            machine observed and a thing the machine IS, and self-knowledge is
 *                            indistinguishable from any other memory.
 *      BEFORE / NOW / AFTER  order. Sequence is carried by permutation, which is why time is a
 *                            rotation here and not a stored number: a rotated vector is
 *                            recognisably a rotation of the original, so "the same thing, later"
 *                            stays related to "the same thing".
 *      HERE / NEAR / FAR     proximity, as a continuum rather than three labels.
 *      INSIDE / OUTSIDE      containment, which is what makes a boundary mean anything.
 *      SAME / OTHER          identity across time, which is object permanence.
 *      CAUSE / EFFECT        asymmetric relation. Correlation is symmetric and is not enough.
 *      PART / WHOLE          composition, so an assembly relates to its pieces.
 *      MORE / LESS           magnitude, the axis every measurable quantity is projected onto.
 *      X / Y / Z             three orthogonal spatial directions, so a position is one vector and
 *                            nearby positions are nearby vectors.
 *
 *
 *  MAGNITUDE IS A CONTINUUM, NOT A SET OF BUCKETS
 *  -----------------------------------------------
 *  born_magnitude walks from one endpoint vector to the other by flipping a proportion of bits, so
 *  0.30 and 0.31 land near each other and 0.30 and 0.90 land far apart, automatically. Random
 *  vectors per bucket would make "slightly warmer" exactly as different as "freezing", and every
 *  quantity the machine ever meets would have to be learned value by value.
 * ===========================================================================================
 */

#ifndef BENCH_BORN_H
#define BENCH_BORN_H

#include "bench_hdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed for the life of the project. Every node derives the same frame from it, with no
 * communication. Changing it invalidates every vector ever stored anywhere in the system. */
#define BORN_SEED 0x8E17C0DEu

typedef enum {
    BORN_SELF = 0, BORN_WORLD,
    BORN_BEFORE,   BORN_NOW,     BORN_AFTER,
    BORN_HERE,     BORN_NEAR,    BORN_FAR,
    BORN_INSIDE,   BORN_OUTSIDE,
    BORN_SAME,     BORN_OTHER,
    BORN_CAUSE,    BORN_EFFECT,
    BORN_PART,     BORN_WHOLE,
    BORN_LESS,     BORN_MORE,
    BORN_X,        BORN_Y,       BORN_Z,
    BORN_N
} born_axis_t;

typedef struct {
    hd_t axis[BORN_N];
} born_t;

/* Derive the frame. Deterministic, identical on every node, no communication. */
void born_frame(born_t *b);

const char *born_axis_name(born_axis_t a);

/* Place a thing on an axis: out = what XOR axis. Binding is its own inverse, so the thing can be
 * recovered from the placement and the axis, and the placement is far from both. */
void born_place(const born_t *b, const hd_t what, born_axis_t where, hd_t out);

/* A quantity as a point on the LESS..MORE continuum. Nearby values give nearby vectors. */
void born_magnitude(const born_t *b, uint32_t value, uint32_t max, hd_t out);

/* A position in space: the three axes weighted by three magnitudes and bundled. */
void born_position(const born_t *b, uint32_t x, uint32_t y, uint32_t z, uint32_t max, hd_t out);

/* The same thing at a different moment. Time is rotation: `when` steps of permutation, negative
 * for the past. A rotated vector stays recognisably a rotation, so "this, later" remains related
 * to "this" while being distinct from it. */
void born_moment(const born_t *b, const hd_t what, int32_t when, hd_t out);

/* Mark something as belonging to the machine rather than the world, or the reverse. This is what
 * keeps self-knowledge in its own region of the space while remaining queryable by the same scan
 * as everything else. */
void born_as_self (const born_t *b, const hd_t what, hd_t out);
void born_as_world(const born_t *b, const hd_t what, hd_t out);

/* Is this vector nearer the self region or the world region? Returns 1 for self, 0 for world.
 * `margin_out` receives how decisive that was, which is the machine's confidence about whether it
 * is looking at itself. */
int born_is_self(const born_t *b, const hd_t v, uint32_t *margin_out);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_BORN_H */
