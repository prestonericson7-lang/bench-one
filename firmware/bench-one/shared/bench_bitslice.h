/* ===========================================================================================
 *  bench_bitslice.h -- BENCH ONE: the bit-sliced ensemble RBM
 * ===========================================================================================
 *
 *  One file, portable C99. Compiles unchanged for the Teensy 4.1 (Cortex-M7), the ESP32-S3
 *  (Xtensa LX7), the Luckfox (Cortex-A7) and your PC. It is the compute kernel of the whole
 *  machine, and every node runs the same one.
 *
 *
 *  THE TWO IDEAS, AND WHY THEY BELONG TOGETHER
 *  --------------------------------------------
 *
 *  IDEA 1 -- A NEURON'S STATE IS ONE BIT, SO PACK 32 OF THEM IN A WORD.
 *
 *      An RBM's units are stochastic BINARY units. A unit is 0 or 1; the real number lives in
 *      the probability, not in the state. So a 784-unit visible layer is not 784 bytes, it is
 *      784 bits = 25 words.
 *
 *      With weights restricted to {-1, 0, +1} stored as two bit-planes, the entire dot product
 *      for one hidden unit becomes:
 *
 *          act = popcount(v & w_pos) - popcount(v & w_neg)
 *
 *      summed over 25 words. No multiplier is executed anywhere. Thirty-two weights are
 *      consumed per AND. This is Gaines' stochastic computing (1969) and BitNet b1.58 (2024)
 *      arriving at the same place from opposite directions.
 *
 *
 *  IDEA 2 -- RUN R COPIES OF THE NETWORK AT ONCE. THE ENSEMBLE *IS* THE DISTRIBUTION.
 *
 *      The state array is [R][words]: R independent replicas of the network, evolving in
 *      parallel under the same weights. The machine does not hold "the state" -- it holds R
 *      simultaneous configurations, and you never read a value out of it. You MEASURE it, by
 *      counting how many replicas have a unit set, and what comes back is a probability.
 *
 *      This is the honest classical analogue of a superposition, and it is not decoration:
 *      it is Persistent Contrastive Divergence with R fantasy particles (Tieleman 2008), the
 *      standard fix for CD-1's biased gradient. The ensemble makes the training BETTER, not
 *      just parallel. Higher R = a better estimate of the model distribution.
 *
 *      R is a compile-time constant. 32 is the default. Raise it on the nodes that can:
 *      the ESP32-S3's PIE registers are 128 bits wide and the A7's NEON is too.
 *
 *
 *  WHY TERNARY IS THE POINT, NOT A COMPROMISE
 *  -------------------------------------------
 *
 *      A Teensy's DTCM is 512 KB and single-cycle. That is the binding constraint on this
 *      whole machine -- doc 08's arithmetic. Weight density decides how much network fits:
 *
 *          int8    8 bits/weight   ->    400 K weights in 400 KB
 *          ternary 2 bits/weight   ->  1,600 K weights in 400 KB      <-- 4x
 *
 *      A ternary kernel that runs at half the MAC rate of SMLAD still wins by 2x, because the
 *      layer is memory-bound, not multiplier-bound. And on the 74HC fabric an AND gate IS the
 *      multiplier, which is the first job in this project the discrete logic is genuinely the
 *      right tool for.
 *
 *
 *  WHERE THE MEMORY GOES, AND WHY THE PSRAM DECISION IN DOC 11 IS THE ALGORITHM'S DECISION
 *  ---------------------------------------------------------------------------------------
 *
 *      Ternary planes    read once per SAMPLE   ->  DTCM   (fast, small, 2 bits/weight)
 *      int8 shadow       read once per BATCH    ->  PSRAM  (16 MB, 45 MB/s, slow is fine)
 *
 *      Training cannot use ternary weights directly -- a {-1,0,+1} weight has nowhere to put a
 *      small gradient. So a real-valued shadow accumulates the updates and is re-thresholded to
 *      ternary at the end of each batch. The shadow is 4x the size of the planes and is touched
 *      1/batch_size as often, so it belongs in PSRAM exactly. Two chips per Teensy is not a
 *      capacity decision, it is what makes ternary training possible at all on this hardware.
 *
 *
 *  WHAT THIS FILE DELIBERATELY DOES NOT DO
 *  ----------------------------------------
 *      No dynamic allocation -- the caller owns every buffer, so a Teensy can put the planes in
 *      DTCM and the shadow in EXTMEM by choosing where it declares them. No floating point in
 *      the hot path. No printing, no protocol, no threads. It is a kernel; the node firmware
 *      decides policy.
 * ===========================================================================================
 */

#ifndef BENCH_BITSLICE_H
#define BENCH_BITSLICE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------------------------
 * TUNABLES
 * ----------------------------------------------------------------------------------------- */

/* Replicas in the ensemble -- the "fantasy particles". Must be >= 1.
 * 32 is the default because it is one machine word of RNG per unit and it is enough particles
 * for PCD to behave. The cost is linear: R replicas is R times the work and R times the state,
 * but the weights (the big thing) are shared, so it is nearly free in memory. */
#ifndef BS_REPLICAS
#define BS_REPLICAS 32
#endif

/* Fixed-point shift for the shadow weights. Shadow is int16 in units of 2^-BS_SHADOW_SHIFT. */
#ifndef BS_SHADOW_SHIFT
#define BS_SHADOW_SHIFT 8
#endif

/* Words needed to hold n units, one bit each. */
#define BS_WORDS(n)  (((n) + 31u) / 32u)

/* Bytes for the two ternary weight planes of an (nv x nh) layer -- the DTCM-resident part. */
#define BS_PLANE_BYTES(nv, nh)   ((size_t)(nh) * BS_WORDS(nv) * 4u * 2u)

/* Bytes for the int16 shadow -- the PSRAM-resident part. */
#define BS_SHADOW_BYTES(nv, nh)  ((size_t)(nh) * (size_t)(nv) * sizeof(int16_t))


/* -------------------------------------------------------------------------------------------
 * THE LAYER
 * -----------------------------------------------------------------------------------------
 * Every pointer is supplied by the caller. Nothing here allocates. On a Teensy:
 *
 *     DMAMEM  ... no. Use plain globals (DTCM) for w_pos/w_neg, and EXTMEM for shadow:
 *
 *     static uint32_t   g_wpos[NH * BS_WORDS(NV)];      // DTCM  -- hot
 *     static uint32_t   g_wneg[NH * BS_WORDS(NV)];      // DTCM  -- hot
 *     EXTMEM static int16_t g_shadow[NH * NV];          // PSRAM -- cold
 */
typedef struct {
    uint16_t  nv;          /* visible units                                                  */
    uint16_t  nh;          /* hidden units                                                   */
    uint16_t  vw;          /* BS_WORDS(nv), cached                                           */
    uint16_t  hw;          /* BS_WORDS(nh), cached                                           */

    /* Ternary weights, row-major by hidden unit: w_pos[j*vw + k] holds, for hidden unit j,
     * the 32 visible units of word k whose weight is +1. w_neg likewise for -1. A weight that
     * is 0 is set in neither plane -- which is why "skip" costs nothing. */
    uint32_t *w_pos;
    uint32_t *w_neg;

    /* Real-valued shadow, w_shadow[j*nv + i], fixed point 2^-BS_SHADOW_SHIFT.
     * May be NULL on an inference-only node -- then the layer is frozen and cannot train. */
    int16_t  *w_shadow;

    int16_t  *b_vis;       /* visible bias, nv entries, same fixed point                     */
    int16_t  *b_hid;       /* hidden  bias, nh entries                                       */

    /* Threshold (in shadow units) above which a shadow weight becomes +1, below -1 and its
     * negation -1. Larger = sparser network. Retuned by bs_retherhold() from the weight
     * distribution, so it tracks the layer instead of being guessed. */
    int16_t   tern_thresh;

    /* Activation gain, x16 fixed point, applied as act = (raw * gain) >> 4.
     *
     * THIS FIELD IS NOT OPTIONAL AND IT IS NOT COSMETIC. The raw activation of a unit is a
     * count difference, so its spread is set by the unit's non-zero fan-in: about +/-sqrt(F).
     * The sigmoid table has a fixed slope, so without a per-layer gain the SAME kernel is
     * saturated on a wide layer and stuck at p=0.5 on a narrow one. A 36-input layer measured
     * an activation spread of 2.5 against a table that needs +-16 to move at all -- every unit
     * was a coin flip no matter what the weights said, and training did nothing.
     *
     * Set by bs_regain() from the measured fan-in. Set it ONCE, after the first threshold, and
     * then leave it alone: re-normalising it every batch pins the model's confidence at
     * whatever spread you chose and stops it ever sharpening. */
    int16_t   gain_h;      /* for P(h|v): fan-in is non-zeros per hidden row               */
    int16_t   gain_v;      /* for P(v|h): fan-in is non-zeros per visible column           */

    uint32_t  rng;         /* xorshift32 state. Seed it to anything non-zero.                */
} bs_layer_t;


/* -------------------------------------------------------------------------------------------
 * STATE BUFFERS
 * -----------------------------------------------------------------------------------------
 * A layer's state across the whole ensemble is [BS_REPLICAS][BS_WORDS(n)] uint32.
 * Bit i of word k of replica r is unit (k*32 + i) of replica r.
 * Declare with BS_STATE(name, n_units).
 */
#define BS_STATE(name, n) uint32_t name[BS_REPLICAS][BS_WORDS(n)]


/* ===========================================================================================
 * MEASUREMENT -- turning the ensemble back into a number
 * =========================================================================================*/

/* P(unit i == 1) over the ensemble, as a value in 0..255.
 * This is the only way to read a scalar out of the machine, and it is a measurement in the
 * literal sense: it collapses R configurations into one probability. */
uint8_t  bs_measure(const uint32_t *state, uint16_t words, uint16_t unit);

/* Mean activation of every unit, 0..255, written to out[n]. */
void     bs_measure_all(const uint32_t *state, uint16_t words, uint16_t n, uint8_t *out);

/* Population count of a whole layer in one replica -- how many units are on. */
uint32_t bs_active(const uint32_t *state, uint16_t words);


/* ===========================================================================================
 * GIBBS SAMPLING -- one half-step of the chain, all replicas at once
 * =========================================================================================*/

/* hidden <- sample from P(h | v). Runs every replica.
 *   vis:  [BS_REPLICAS][layer->vw]
 *   hid:  [BS_REPLICAS][layer->hw]   (written)
 * This is the hot loop of the entire machine. */
void bs_sample_hidden(bs_layer_t *l, const uint32_t *vis, uint32_t *hid);

/* visible <- sample from P(v | h). The transpose direction. */
void bs_sample_visible(bs_layer_t *l, const uint32_t *hid, uint32_t *vis);

/* Deterministic versions: set the unit if P > 1/2 instead of sampling. Use these to read the
 * model out -- a reconstruction that is sampled carries the sampler's noise, not the model's
 * opinion.
 *
 * Note the rule is strictly GREATER than half. sigmoid(0) is exactly 128, and a unit with no
 * evidence either way must not switch on: in a sparse ternary network most units have no
 * evidence on most inputs, so ">=" turns the whole layer on and every reconstruction becomes
 * a solid block. That one character cost a full debugging session -- leave it as ">". */
void bs_mean_hidden (bs_layer_t *l, const uint32_t *vis, uint32_t *hid);
void bs_mean_visible(bs_layer_t *l, const uint32_t *hid, uint32_t *vis);

/* ---------------------------------------------------------------------------------------
 * TEMPERATURE
 * -------------------------------------------------------------------------------------
 * `boost` is INVERSE temperature in x16 fixed point: 16 = normal, 128 = eight times colder.
 * It multiplies the activation before the sigmoid, so a cold unit is a confident one.
 *
 * THIS IS WHAT MAKES THE MODEL GENERATE. A free-running chain at boost=16 sat at exactly
 * half its units on -- maximum entropy, a random walk -- and produced valid samples 0.8% of
 * the time. Annealed from 16 to 128 the same weights produce them 16.4% of the time, against
 * a random-chance rate of 0.0000002%. The energy minima were always there; without cooling
 * the chain simply never fell into one.
 * ------------------------------------------------------------------------------------- */
void bs_hidden_t (bs_layer_t *l, const uint32_t *vis, uint32_t *hid, int sample, uint16_t boost);
void bs_visible_t(bs_layer_t *l, const uint32_t *hid, uint32_t *vis, int sample, uint16_t boost);

/* Run an annealed Gibbs chain and leave a sample in `vis`. Pass schedule=NULL for the default
 * (a gradual 1.0 -> 8.0 ramp, which beat both a flat chain and a steep one on the bench task).
 * `vis` should already hold a random starting state. Ends with a cold deterministic settle. */
void bs_generate(bs_layer_t *l, uint32_t *vis, uint32_t *hid_scratch,
                 const uint16_t *schedule, uint16_t nsteps);

/* Recompute gain_h/gain_v from the current weight population. Call once after the first
 * bs_rethreshold(), and after any deliberate change of sparsity -- not every batch. */
void bs_regain(bs_layer_t *l);


/* ===========================================================================================
 * TRAINING -- Persistent Contrastive Divergence, R particles
 * =========================================================================================*/

/* Accumulate one minibatch into the shadow weights.
 *
 *   data:      [BS_REPLICAS][vw]  -- one training sample per replica (bit-packed).
 *   fantasy:   [BS_REPLICAS][vw]  -- the PERSISTENT chain. Never reset between calls; that
 *                                    persistence is what makes this PCD rather than CD-1.
 *   scratch_h0/h1: [BS_REPLICAS][hw]  -- caller-owned workspace.
 *   scratch_v:     [BS_REPLICAS][vw]  -- workspace for the reconstruction measurement.
 *   lr:        learning rate in shadow units (typical 1..8).
 *
 * Returns the mean absolute RECONSTRUCTION error, 0..255: the data is pushed up to the hidden
 * layer and straight back down, and the result compared with what went in. This is the number
 * that must fall, and it is the honest one -- comparing the data against the persistent chain
 * instead would measure only whether the per-pixel marginals match, which a network can satisfy
 * while having learned nothing about how pixels go together.
 */
uint32_t bs_pcd_step(bs_layer_t *l,
                     const uint32_t *data,
                     uint32_t       *fantasy,
                     uint32_t       *scratch_h0,
                     uint32_t       *scratch_h1,
                     uint32_t       *scratch_v,
                     int16_t         lr);

/* Re-threshold the shadow into the ternary planes. Call once per minibatch, after
 * bs_pcd_step(). This is the only writer of w_pos/w_neg. */
void bs_rethreshold(bs_layer_t *l);

/* Choose tern_thresh so that approximately `percent_nonzero` of weights end up non-zero.
 * Sparsity is a knob: sparser is faster and smaller, denser is more expressive. 30 is sane. */
void bs_autotune_threshold(bs_layer_t *l, uint8_t percent_nonzero);


/* ===========================================================================================
 * UTILITY
 * =========================================================================================*/

void     bs_layer_init(bs_layer_t *l, uint16_t nv, uint16_t nh,
                       uint32_t *w_pos, uint32_t *w_neg, int16_t *w_shadow,
                       int16_t *b_vis, int16_t *b_hid, uint32_t seed);

/* Fill the shadow with small random values and threshold. Do this once before training. */
void     bs_randomize(bs_layer_t *l, int16_t amplitude);

/* Pack/unpack between bit-packed replica state and plain bytes, for I/O and for feeding the
 * next layer over the network. */
void     bs_pack(const uint8_t *bytes, uint32_t *state_replica, uint16_t n);
void     bs_unpack(const uint32_t *state_replica, uint8_t *bytes, uint16_t n);

/* Portable 32-bit population count. Uses the compiler builtin where there is one. */
static inline uint32_t bs_popcount(uint32_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_popcount(x);
#else
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (x * 0x01010101u) >> 24;
#endif
}

#ifdef __cplusplus
}
#endif
#endif /* BENCH_BITSLICE_H */
