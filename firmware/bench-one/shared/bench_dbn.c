/* ===========================================================================================
 *  bench_dbn.c -- one node = one layer. See bench_dbn.h for the design argument.
 * ===========================================================================================
 */

#include "bench_dbn.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Local memcpy so this file has no libc dependency -- it has to build for a Teensy, an
 * ESP-IDF app and a uClibc Luckfox binary without three different sets of includes. */
static void bd_copy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

void bench_dbn_init(bench_dbn_t *n, uint8_t layer_index,
                    uint32_t *vis, uint32_t *fantasy, uint32_t *recon,
                    uint32_t *hid, uint32_t *hscratch)
{
    n->vis         = vis;
    n->fantasy     = fantasy;
    n->recon       = recon;
    n->hid         = hid;
    n->hscratch    = hscratch;
    n->state       = BENCH_DBN_IDLE;
    n->layer_index = layer_index;
    n->lr          = 6;
    n->decay_shift = 9;
    n->has_input   = 0;
    n->batches     = 0;
    n->err_last    = 255;
    n->err_ema     = 255u * 256u;
    n->err_best    = 0xFFFFFFFFu;
    n->stall       = 0;
    n->patience    = 400;

    /* Seed the persistent chain from the layer's own RNG. It must NOT start all-zero: an
     * all-zero chain gives a zero negative phase for the first batches, the weights all move
     * the same way, and the layer starts from a state it then has to climb out of. */
    const uint16_t vw = n->rbm.vw;
    for (uint32_t r = 0; r < BS_REPLICAS; r++)
        for (uint16_t k = 0; k < vw; k++)
            n->fantasy[(size_t)r * vw + k] = (uint32_t)(r * 2654435761u) ^ (uint32_t)(k * 40503u + 1u);
}

bench_dbn_result_t bench_dbn_input(bench_dbn_t *n, const void *payload, size_t len)
{
    bench_dbn_hdr_t h;
    if (!payload || len < sizeof(h)) return BENCH_DBN_BAD_HEADER;
    bd_copy(&h, payload, sizeof(h));

    /* Refuse anything that is not shaped exactly like this layer's input. A node quietly
     * training on a mis-shaped batch would look like a convergence problem forever. */
    if (h.magic    != BENCH_DBN_MAGIC)   return BENCH_DBN_BAD_HEADER;
    if (h.replicas != (uint8_t)BS_REPLICAS) return BENCH_DBN_BAD_HEADER;
    if (h.units    != n->rbm.nv)         return BENCH_DBN_BAD_HEADER;
    if (h.words    != n->rbm.vw)         return BENCH_DBN_BAD_HEADER;

    const size_t need = sizeof(h) + (size_t)BS_REPLICAS * h.words * 4u;
    if (len < need) return BENCH_DBN_BAD_HEADER;

    bd_copy(n->vis, (const uint8_t *)payload + sizeof(h),
            (size_t)BS_REPLICAS * h.words * 4u);
    n->has_input = 1;
    if (n->state == BENCH_DBN_IDLE) n->state = BENCH_DBN_TRAINING;
    return BENCH_DBN_OK;
}

bench_dbn_result_t bench_dbn_step(bench_dbn_t *n)
{
    if (!n->has_input) return BENCH_DBN_NO_INPUT;
    n->has_input = 0;

    if (n->state == BENCH_DBN_FROZEN) {
        /* Forward only, and DETERMINISTIC. A frozen layer is a feature extractor: if it
         * sampled, the same input would give the next layer a different vector every time and
         * layer k+1 would be trying to model this layer's sampling noise. */
        bs_mean_hidden(&n->rbm, n->vis, n->hid);
        return BENCH_DBN_EMITTED;
    }

    const uint32_t err = bs_pcd_step(&n->rbm, n->vis, n->fantasy,
                                     n->hid, n->hscratch, n->recon, n->lr);

    /* Weight decay, then refresh the ternary planes. The planes are what the forward pass
     * actually reads, so skipping the rethreshold means the shadow moves and the network
     * never changes -- measured, and it looks exactly like "the model refuses to learn". */
    if (n->rbm.w_shadow && n->decay_shift) {
        const size_t nw = (size_t)n->rbm.nv * n->rbm.nh;
        const uint8_t sh = n->decay_shift;
        for (size_t i = 0; i < nw; i++) n->rbm.w_shadow[i] -= (int16_t)(n->rbm.w_shadow[i] >> sh);
    }
    bs_rethreshold(&n->rbm);

    n->batches++;
    n->err_last = err;
    /* EMA at 1/16, x256 fixed point. Raw per-batch error is noisy because the reconstruction
     * is sampled; deciding convergence on a single batch would freeze layers at random. */
    n->err_ema = n->err_ema - (n->err_ema >> 4) + (err * 256u >> 4);

    if (n->err_ema + 64u < n->err_best) {     /* +64 = a quarter of one error unit of slack  */
        n->err_best = n->err_ema;
        n->stall = 0;
    } else if (n->stall < 0xFFFFu) {
        n->stall++;
    }

    if (n->patience && n->stall >= n->patience) return BENCH_DBN_CONVERGED;
    return BENCH_DBN_OK;
}

size_t bench_dbn_output_bytes(const bench_dbn_t *n)
{
    return sizeof(bench_dbn_hdr_t) + (size_t)BS_REPLICAS * n->rbm.hw * 4u;
}

size_t bench_dbn_output(const bench_dbn_t *n, void *out, size_t cap)
{
    const size_t need = bench_dbn_output_bytes(n);
    if (!out || cap < need) return 0;

    bench_dbn_hdr_t h;
    h.magic    = BENCH_DBN_MAGIC;
    h.layer    = n->layer_index;
    h.replicas = (uint8_t)BS_REPLICAS;
    h.units    = n->rbm.nh;
    h.words    = n->rbm.hw;

    bd_copy(out, &h, sizeof(h));
    bd_copy((uint8_t *)out + sizeof(h), n->hid, (size_t)BS_REPLICAS * n->rbm.hw * 4u);
    return need;
}

void bench_dbn_freeze(bench_dbn_t *n)
{
    /* One last consistent snapshot: threshold the shadow, then match the gain to the
     * sparsity that actually resulted. Freezing without the regain leaves the layer running
     * at a gain computed for a different weight population. */
    bs_rethreshold(&n->rbm);
    bs_regain(&n->rbm);
    n->state = BENCH_DBN_FROZEN;
}

void bench_dbn_dream(bench_dbn_t *n)
{
    const uint16_t vw = n->rbm.vw;
    for (uint32_t r = 0; r < BS_REPLICAS; r++)
        for (uint16_t k = 0; k < vw; k++)
            n->vis[(size_t)r * vw + k] = bs_popcount(r + k) * 2654435761u + (uint32_t)(r * 97u + k);

    bs_generate(&n->rbm, n->vis, n->hscratch, NULL, 0);
}
