/* ===========================================================================================
 *  bench_bitslice.c -- implementation of the bit-sliced ensemble RBM
 * ===========================================================================================
 *
 *  TWO KERNELS, ONE WEIGHT LAYOUT
 *  -------------------------------
 *  Weights are stored row-major by HIDDEN unit: row j is a bitmask over the visible units.
 *  Both directions of the Gibbs chain are computed from that one layout, so there is never a
 *  transpose and never a second copy of the weights. That is the whole reason the layout was
 *  chosen, and it is what lets 1.6 M weights live in a Teensy's DTCM.
 *
 *    h | v   POPCOUNT.  act_j = sum_k popcount(v & wpos_j) - popcount(v & wneg_j).
 *                       Row j is read once and used for all R replicas, so the big thing
 *                       (weights) streams exactly once per Gibbs half-step.
 *
 *    v | h   BIT-SLICED CARRY-SAVE.  act_i = sum over active j of w_ij. Row j is already a
 *                       mask over i, so an active hidden unit contributes by being ADDED into
 *                       a bit-plane accumulator: a ripple carry across BS_ACC_PLANES words,
 *                       32 visible units at a time, with an early exit the moment the carry
 *                       dies. Average cost is about two word-ops per 32 weights.
 *
 *  The carry-save adder is also exactly what the FPGA version and the 74HC version are: an
 *  adder tree fed by AND gates. Same algorithm at three scales, which is the point.
 * ===========================================================================================
 */

#include "bench_bitslice.h"

/* Bit-planes in the carry-save accumulator. 10 planes counts to 1023 active inputs, which
 * covers any layer this machine will hold in one node. */
#define BS_ACC_PLANES 10

/* Largest layer the fixed-size scratch below can serve. 1024 units = 32 words. */
#define BS_MAX_WORDS  32

/* -------------------------------------------------------------------------------------------
 * sigmoid, 0..255, over act in [-64, +63], slope 1/8.
 * A ternary layer with ~30% of 784 inputs non-zero has an activation stddev near 11, so the
 * interesting part of the curve is +/-32 and this table spends its resolution there.
 * Integer only -- there is no floating point anywhere in this file.
 * ----------------------------------------------------------------------------------------- */
static const uint8_t bs_sig[128] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   2,
      2,   2,   2,   2,   3,   3,   4,   4,   5,   5,   6,   7,
      7,   8,  10,  11,  12,  14,  15,  17,  19,  22,  24,  27,
     30,  34,  38,  42,  47,  51,  57,  62,  69,  75,  82,  89,
     96, 104, 112, 120, 128, 135, 143, 151, 159, 166, 173, 180,
    186, 193, 198, 204, 208, 213, 217, 221, 225, 228, 231, 233,
    236, 238, 240, 241, 243, 244, 245, 247, 248, 248, 249, 250,
    250, 251, 251, 252, 252, 253, 253, 253, 253, 253, 254, 254,
    254, 254, 254, 254, 254, 254, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
};

static inline uint8_t bs_sigmoid(int32_t act)
{
    int32_t i = act + 64;
    if (i < 0)   i = 0;
    if (i > 127) i = 127;
    return bs_sig[i];
}

/* xorshift32. Cheap, adequate for Gibbs sampling, and identical on every target so a training
 * run is reproducible across the whole cluster from one seed. */
static inline uint32_t bs_rand(bs_layer_t *l)
{
    uint32_t x = l->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    l->rng = x;
    return x;
}


/* ===========================================================================================
 * MEASUREMENT
 * =========================================================================================*/

uint8_t bs_measure(const uint32_t *state, uint16_t words, uint16_t unit)
{
    const uint32_t w    = unit >> 5;
    const uint32_t mask = 1u << (unit & 31u);
    uint32_t hits = 0;
    for (uint32_t r = 0; r < BS_REPLICAS; r++) {
        if (state[r * words + w] & mask) hits++;
    }
    /* Collapse R configurations to one probability. This is the only way a scalar leaves the
     * ensemble, and rounding is deliberate: 255 * hits / R, not a truncating divide. */
    return (uint8_t)((hits * 255u + (BS_REPLICAS / 2u)) / BS_REPLICAS);
}

void bs_measure_all(const uint32_t *state, uint16_t words, uint16_t n, uint8_t *out)
{
    for (uint16_t i = 0; i < n; i++) out[i] = bs_measure(state, words, i);
}

uint32_t bs_active(const uint32_t *state, uint16_t words)
{
    uint32_t n = 0;
    for (uint16_t k = 0; k < words; k++) n += bs_popcount(state[k]);
    return n;
}


/* ===========================================================================================
 * h | v  -- the popcount kernel. This is the hot loop of the machine.
 * =========================================================================================*/

static void bs_hidden_core(bs_layer_t *l, const uint32_t *vis, uint32_t *hid, int sample,
                           uint16_t boost)
{
    const uint16_t vw = l->vw, hw = l->hw, nh = l->nh;

    for (uint32_t r = 0; r < BS_REPLICAS; r++) {
        for (uint16_t k = 0; k < hw; k++) hid[r * hw + k] = 0;
    }

    for (uint16_t j = 0; j < nh; j++) {
        const uint32_t *wp = l->w_pos + (size_t)j * vw;
        const uint32_t *wn = l->w_neg + (size_t)j * vw;
        /* Bias expressed in TERNARY-WEIGHT units. A ternary weight contributes exactly +-1
         * to the raw activation, so a bias must be divided by the same threshold that turns a
         * shadow weight into a +-1. Shifting by BS_SHADOW_SHIFT instead lets the bias reach
         * +-125 while every weight is worth 1, and the bias then decides every unit by itself. */
        const int32_t   bj = (l->b_hid && l->tern_thresh) ? (l->b_hid[j] / l->tern_thresh) : 0;
        const uint16_t  jw = j >> 5;
        const uint32_t  jm = 1u << (j & 31u);

        /* Row j is now in cache. Spend it on every replica before moving on -- this ordering
         * is why the weights are read exactly once per half-step instead of R times. */
        for (uint32_t r = 0; r < BS_REPLICAS; r++) {
            const uint32_t *v = vis + (size_t)r * vw;
            int32_t acc = 0;
            for (uint16_t k = 0; k < vw; k++) {
                const uint32_t vk = v[k];
                acc += (int32_t)bs_popcount(vk & wp[k]);
                acc -= (int32_t)bs_popcount(vk & wn[k]);
            }
            acc += bj;

            const uint8_t p = bs_sigmoid((acc * l->gain_h * (int32_t)boost) >> 8);
            uint32_t on;
            if (sample) {
                on = ((bs_rand(l) & 0xFFu) < p);
            } else {
                on = (p > 128u);          /* strictly greater -- see bs_mean_hidden() */
            }
            if (on) hid[(size_t)r * hw + jw] |= jm;
        }
    }
}

void bs_sample_hidden(bs_layer_t *l, const uint32_t *vis, uint32_t *hid)
{
    bs_hidden_core(l, vis, hid, 1, 16);
}

void bs_mean_hidden(bs_layer_t *l, const uint32_t *vis, uint32_t *hid)
{
    bs_hidden_core(l, vis, hid, 0, 16);
}

void bs_hidden_t(bs_layer_t *l, const uint32_t *vis, uint32_t *hid, int sample, uint16_t boost)
{
    bs_hidden_core(l, vis, hid, sample, boost);
}


/* ===========================================================================================
 * v | h  -- the carry-save kernel.
 * -------------------------------------------------------------------------------------------
 * Add a 1-bit-per-lane mask into a bit-plane accumulator. Each plane holds one binary digit of
 * the running total for 32 lanes at once; the carry ripples upward and almost always dies in
 * the first two planes, so the early exit is what makes this cheap.
 * =========================================================================================*/
static inline void bs_ripple_add(uint32_t planes[][BS_MAX_WORDS], uint16_t k, uint32_t m)
{
    for (int b = 0; b < BS_ACC_PLANES && m; b++) {
        const uint32_t carry = planes[b][k] & m;
        planes[b][k] ^= m;
        m = carry;
    }
}

static void bs_visible_core(bs_layer_t *l, const uint32_t *hid, uint32_t *vis, int sample,
                            uint16_t boost)
{
    const uint16_t vw = l->vw, hw = l->hw, nh = l->nh, nv = l->nv;

    static uint32_t P[BS_ACC_PLANES][BS_MAX_WORDS];
    static uint32_t N[BS_ACC_PLANES][BS_MAX_WORDS];

    for (uint32_t r = 0; r < BS_REPLICAS; r++) {
        const uint32_t *h = hid + (size_t)r * hw;
        uint32_t       *v = vis + (size_t)r * vw;

        for (int b = 0; b < BS_ACC_PLANES; b++)
            for (uint16_t k = 0; k < vw; k++) { P[b][k] = 0; N[b][k] = 0; }

        /* Walk only the hidden units that are ON. A sparse hidden layer costs proportionally
         * less, which is the same reason Mixture-of-Experts works: unread weights are free. */
        for (uint16_t jw = 0; jw < hw; jw++) {
            uint32_t bits = h[jw];
            while (bits) {
                const uint32_t b0 = bits & (uint32_t)(-(int32_t)bits);   /* lowest set bit    */
                const uint16_t j  = (uint16_t)((jw << 5) + bs_popcount(b0 - 1u));
                bits ^= b0;
                if (j >= nh) break;

                const uint32_t *wp = l->w_pos + (size_t)j * vw;
                const uint32_t *wn = l->w_neg + (size_t)j * vw;
                for (uint16_t k = 0; k < vw; k++) {
                    if (wp[k]) bs_ripple_add(P, k, wp[k]);
                    if (wn[k]) bs_ripple_add(N, k, wn[k]);
                }
            }
        }

        /* Read the accumulators back one visible unit at a time and sample. */
        for (uint16_t k = 0; k < vw; k++) v[k] = 0;

        for (uint16_t i = 0; i < nv; i++) {
            const uint16_t k  = i >> 5;
            const uint32_t sh = i & 31u;
            int32_t pos = 0, neg = 0;
            for (int b = 0; b < BS_ACC_PLANES; b++) {
                pos |= (int32_t)((P[b][k] >> sh) & 1u) << b;
                neg |= (int32_t)((N[b][k] >> sh) & 1u) << b;
            }
            int32_t acc = pos - neg;
            if (l->b_vis && l->tern_thresh) acc += (l->b_vis[i] / l->tern_thresh);

            const uint8_t p = bs_sigmoid((acc * l->gain_v * (int32_t)boost) >> 8);
            const uint32_t on = sample ? ((bs_rand(l) & 0xFFu) < p) : (p > 128u);
            if (on) v[k] |= (1u << sh);
        }
    }
}

void bs_sample_visible(bs_layer_t *l, const uint32_t *hid, uint32_t *vis)
{
    bs_visible_core(l, hid, vis, 1, 16);
}

void bs_mean_visible(bs_layer_t *l, const uint32_t *hid, uint32_t *vis)
{
    bs_visible_core(l, hid, vis, 0, 16);
}

void bs_visible_t(bs_layer_t *l, const uint32_t *hid, uint32_t *vis, int sample, uint16_t boost)
{
    bs_visible_core(l, hid, vis, sample, boost);
}

/* The measured-best schedule: a long warm exploration, then a steady squeeze. A steep ramp
 * (1.0 -> 32.0 in four stages) scored 9.4% against this one's 16.4% -- cooling too fast traps
 * the chain in whatever basin it happened to be in, which is the same failure simulated
 * annealing has had since Kirkpatrick 1983. */
static const uint16_t bs_default_schedule[] = {
    16,16,16,16,16,16,16,16,16,16,
    24,24,24,24,24,24,24,24,
    32,32,32,32,32,32,32,32,
    48,48,48,48,48,48,48,48,
    64,64,64,64,64,64,64,64,
    96,96,96,96,96,96,96,96,
    128,128,128,128,128,128,128,128,
};

void bs_generate(bs_layer_t *l, uint32_t *vis, uint32_t *hid_scratch,
                 const uint16_t *schedule, uint16_t nsteps)
{
    if (!schedule) {
        schedule = bs_default_schedule;
        nsteps   = (uint16_t)(sizeof(bs_default_schedule) / sizeof(bs_default_schedule[0]));
    }
    uint16_t last = 16;
    for (uint16_t s = 0; s < nsteps; s++) {
        last = schedule[s];
        bs_hidden_core (l, vis, hid_scratch, 1, last);
        bs_visible_core(l, hid_scratch, vis, 1, last);
    }
    /* Cold deterministic settle, so what comes out is the model's opinion and not the
     * sampler's last coin flip. */
    bs_hidden_core (l, vis, hid_scratch, 0, last);
    bs_visible_core(l, hid_scratch, vis, 0, last);
}


/* ===========================================================================================
 * TRAINING -- Persistent Contrastive Divergence with BS_REPLICAS fantasy particles
 * -------------------------------------------------------------------------------------------
 * The gradient of an RBM's log-likelihood is  <v h>_data - <v h>_model. The first term is easy.
 * The second needs samples from the model, and CD-1 approximates it by starting a chain at the
 * data -- which biases it. PCD instead keeps a chain running ACROSS calls (the `fantasy` buffer
 * is never reset), so it mixes toward the true model distribution over many minibatches.
 *
 * The ensemble is what makes this practical: R independent particles are R independent chains,
 * and they cost one pass because they share the weights.
 * =========================================================================================*/

uint32_t bs_pcd_step(bs_layer_t *l,
                     const uint32_t *data,
                     uint32_t       *fantasy,
                     uint32_t       *h_data,
                     uint32_t       *h_model,
                     uint32_t       *recon,
                     int16_t         lr)
{
    const uint16_t vw = l->vw, hw = l->hw, nv = l->nv, nh = l->nh;

    if (!l->w_shadow) return 0;             /* frozen layer: inference only */

    /* Positive phase: hidden units driven by the real data. */
    bs_hidden_core(l, data, h_data, 1, 16);

    /* Negative phase: advance the persistent chain one full Gibbs step. */
    bs_sample_hidden(l, fantasy, h_model);
    bs_sample_visible(l, h_model, fantasy);
    bs_sample_hidden(l, fantasy, h_model);

    /* Update. For each (i, j) the gradient is the difference in how often the pair fired
     * together under the data versus under the model, counted across the ensemble. Both counts
     * come from popcounts of the packed states, so the update loop never unpacks a bit. */
    for (uint16_t j = 0; j < nh; j++) {
        const uint16_t jw = j >> 5;
        const uint32_t jm = 1u << (j & 31u);

        /* Which replicas have hidden unit j on, in each phase. */
        uint32_t on_d = 0, on_m = 0;
        for (uint32_t r = 0; r < BS_REPLICAS; r++) {
            if (h_data [(size_t)r * hw + jw] & jm) on_d |= (1u << r);
            if (h_model[(size_t)r * hw + jw] & jm) on_m |= (1u << r);
        }
        if (!on_d && !on_m) continue;       /* dead unit this batch -- skip its whole row */

        int16_t *row = l->w_shadow + (size_t)j * nv;

        for (uint16_t i = 0; i < nv; i++) {
            const uint16_t k  = i >> 5;
            const uint32_t im = 1u << (i & 31u);
            int32_t pos = 0, neg = 0;

            for (uint32_t r = 0; r < BS_REPLICAS; r++) {
                if ((on_d >> r) & 1u) { if (data   [(size_t)r * vw + k] & im) pos++; }
                if ((on_m >> r) & 1u) { if (fantasy[(size_t)r * vw + k] & im) neg++; }
            }
            if (pos == neg) continue;

            int32_t w = row[i] + (int32_t)lr * (pos - neg);
            if (w >  32000) w =  32000;     /* keep the shadow inside int16 with headroom */
            if (w < -32000) w = -32000;
            row[i] = (int16_t)w;
        }

        if (l->b_hid) {
            int32_t b = l->b_hid[j] + (int32_t)lr * ((int32_t)bs_popcount(on_d) - (int32_t)bs_popcount(on_m));
            if (b >  32000) b =  32000;
            if (b < -32000) b = -32000;
            l->b_hid[j] = (int16_t)b;
        }
    }

    if (l->b_vis) {
        for (uint16_t i = 0; i < nv; i++) {
            const uint16_t k  = i >> 5;
            const uint32_t im = 1u << (i & 31u);
            int32_t pos = 0, neg = 0;
            for (uint32_t r = 0; r < BS_REPLICAS; r++) {
                if (data   [(size_t)r * vw + k] & im) pos++;
                if (fantasy[(size_t)r * vw + k] & im) neg++;
            }
            int32_t b = l->b_vis[i] + (int32_t)lr * (pos - neg);
            if (b >  32000) b =  32000;
            if (b < -32000) b = -32000;
            l->b_vis[i] = (int16_t)b;
        }
    }

    /* Reconstruction error. Push the data up and straight back down through the weights we
     * just updated, and compare. h_data is already P(h|data) from the positive phase, so this
     * costs one extra downward pass and nothing else. */
    bs_sample_visible(l, h_data, recon);

    uint32_t err = 0;
    for (uint16_t k = 0; k < vw; k++) {
        for (uint32_t r = 0; r < BS_REPLICAS; r++) {
            uint32_t diff = data[(size_t)r * vw + k] ^ recon[(size_t)r * vw + k];
            if (k == (uint16_t)(vw - 1) && (nv & 31u))
                diff &= (1u << (nv & 31u)) - 1u;   /* ignore padding bits in the last word */
            err += bs_popcount(diff);
        }
    }
    return (err * 255u) / ((uint32_t)nv * BS_REPLICAS);
}


/* ===========================================================================================
 * SHADOW  ->  TERNARY
 * =========================================================================================*/

void bs_rethreshold(bs_layer_t *l)
{
    const uint16_t vw = l->vw, nv = l->nv, nh = l->nh;
    const int16_t  t  = l->tern_thresh;

    for (uint16_t j = 0; j < nh; j++) {
        uint32_t       *wp  = l->w_pos + (size_t)j * vw;
        uint32_t       *wn  = l->w_neg + (size_t)j * vw;
        const int16_t  *row = l->w_shadow + (size_t)j * nv;

        for (uint16_t k = 0; k < vw; k++) { wp[k] = 0; wn[k] = 0; }

        for (uint16_t i = 0; i < nv; i++) {
            const int16_t w = row[i];
            if      (w >=  t) wp[i >> 5] |= (1u << (i & 31u));
            else if (w <= -t) wn[i >> 5] |= (1u << (i & 31u));
            /* everything between is a genuine zero: no bit in either plane, and therefore
             * no work at all in the forward pass. Sparsity is free speed here. */
        }
    }
}

static uint32_t bs_isqrt(uint32_t n)
{
    if (n < 2) return n ? n : 1;
    uint32_t x = n, y = (x + 1u) / 2u;
    while (y < x) { x = y; y = (x + n / x) / 2u; }
    return x ? x : 1u;
}

void bs_regain(bs_layer_t *l)
{
    /* Map a unit's FULL activation range onto the useful width of the sigmoid table (about
     * +-24 entries, since the slope is 1/8 and +-3 logits is where it flattens). Mapping the
     * standard deviation instead makes a 3-count and a 6-count both saturate to p=255, which
     * throws away exactly the gradation the model needs to express confidence. */
    const size_t nplane = (size_t)l->nh * l->vw;
    uint32_t nz = 0;
    for (size_t i = 0; i < nplane; i++) nz += bs_popcount(l->w_pos[i]) + bs_popcount(l->w_neg[i]);
    if (nz < 1u) nz = 1u;

    uint32_t fan_h = nz / (l->nh ? l->nh : 1u);
    uint32_t fan_v = nz / (l->nv ? l->nv : 1u);
    if (fan_h < 1u) fan_h = 1u;
    if (fan_v < 1u) fan_v = 1u;

    uint32_t gh = 384u / fan_h;
    uint32_t gv = 384u / fan_v;
    if (gh < 1u)    gh = 1u;
    if (gv < 1u)    gv = 1u;
    if (gh > 1024u) gh = 1024u;
    if (gv > 1024u) gv = 1024u;
    l->gain_h = (int16_t)gh;
    l->gain_v = (int16_t)gv;
    (void)bs_isqrt;
}

void bs_autotune_threshold(bs_layer_t *l, uint8_t percent_nonzero)
{
    /* Histogram |shadow| into 256 buckets and pick the threshold that leaves roughly the
     * requested fraction non-zero. Two passes, no sort, no float. */
    uint32_t hist[256];
    for (int i = 0; i < 256; i++) hist[i] = 0;

    const size_t n = (size_t)l->nv * l->nh;
    for (size_t i = 0; i < n; i++) {
        int32_t a = l->w_shadow[i];
        if (a < 0) a = -a;
        a >>= 6;                                  /* 32000 -> 500, then clamp */
        hist[a > 255 ? 255 : a]++;
    }

    const uint32_t want = (uint32_t)((n * percent_nonzero) / 100u);
    uint32_t seen = 0;
    int b = 255;
    for (; b >= 0; b--) {
        seen += hist[b];
        if (seen >= want) break;
    }
    if (b < 0) b = 0;
    int32_t t = (int32_t)b << 6;
    if (t < 1) t = 1;
    l->tern_thresh = (int16_t)t;
}


/* ===========================================================================================
 * SETUP AND I/O
 * =========================================================================================*/

void bs_layer_init(bs_layer_t *l, uint16_t nv, uint16_t nh,
                   uint32_t *w_pos, uint32_t *w_neg, int16_t *w_shadow,
                   int16_t *b_vis, int16_t *b_hid, uint32_t seed)
{
    l->nv = nv;
    l->nh = nh;
    l->vw = (uint16_t)BS_WORDS(nv);
    l->hw = (uint16_t)BS_WORDS(nh);
    l->w_pos = w_pos;
    l->w_neg = w_neg;
    l->w_shadow = w_shadow;
    l->b_vis = b_vis;
    l->b_hid = b_hid;
    l->tern_thresh = 1 << (BS_SHADOW_SHIFT - 1);
    l->gain_h = 16;
    l->gain_v = 16;
    l->rng = seed ? seed : 0xB3C41011u;
}

void bs_randomize(bs_layer_t *l, int16_t amplitude)
{
    if (!l->w_shadow) return;
    const size_t n = (size_t)l->nv * l->nh;
    for (size_t i = 0; i < n; i++) {
        const int32_t r = (int32_t)(bs_rand(l) % (uint32_t)(2 * amplitude + 1)) - amplitude;
        l->w_shadow[i] = (int16_t)r;
    }
    if (l->b_vis) for (uint16_t i = 0; i < l->nv; i++) l->b_vis[i] = 0;
    if (l->b_hid) for (uint16_t j = 0; j < l->nh; j++) l->b_hid[j] = 0;
    /* 15% measured better than 30% on the bench task: a sparser ternary layer both trains
     * better and runs faster, because a zero weight costs nothing in the forward pass. */
    bs_autotune_threshold(l, 15);
    bs_rethreshold(l);
    bs_regain(l);
}

void bs_pack(const uint8_t *bytes, uint32_t *state_replica, uint16_t n)
{
    const uint16_t w = (uint16_t)BS_WORDS(n);
    for (uint16_t k = 0; k < w; k++) state_replica[k] = 0;
    for (uint16_t i = 0; i < n; i++)
        if (bytes[i] >= 128) state_replica[i >> 5] |= (1u << (i & 31u));
}

void bs_unpack(const uint32_t *state_replica, uint8_t *bytes, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++)
        bytes[i] = (state_replica[i >> 5] >> (i & 31u)) & 1u ? 255 : 0;
}
