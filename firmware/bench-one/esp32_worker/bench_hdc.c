/* ===========================================================================================
 *  bench_hdc.c -- the substrate. See bench_hdc.h for the design argument and the measurements.
 * ===========================================================================================
 *
 *  Portable C99. No allocation, no floating point, no libc. Builds for Cortex-M7 (Teensy),
 *  Xtensa LX7 (ESP32-S3), Cortex-A7 (Luckfox) and a PC from this one file.
 *
 *  Everything in here is XOR, shift, add and popcount, because those are the operations this
 *  machine is fast at and the representation was chosen to need nothing else.
 * ===========================================================================================
 */

#include "bench_hdc.h"

/* -------------------------------------------------------------------------------------------
 * PRIMITIVES
 * ----------------------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------------------------
 * POPCOUNT -- the hottest instruction in the machine
 * -----------------------------------------------------------------------------------------
 * __builtin_popcount looks free and is not. The Cortex-M7 has NO popcount instruction, so GCC
 * emits `bl __popcountsi2` -- a libgcc FUNCTION CALL -- for every 32-bit word. Disassembled
 * and confirmed: the inner loop of hd_hamming was 7 instructions plus a call, on every one of
 * the 256 words of every comparison the machine has ever made.
 *
 * Measured loop-body cost per word, Cortex-M7 -O2:
 *      __builtin_popcount   7 instr + call (~15-20 cyc)  ~= 22-27 cycles
 *      inline SWAR                                 17    instructions
 *      SWAR + USAD8 horizontal add                 16
 *      4 words batched before one USAD8            14.25   <- shipped
 *
 * USAD8 is an ARMv7E-M DSP instruction that sums four byte lanes in one cycle. Four words'
 * nibble counts can be added together before the horizontal step because each byte holds at
 * most 8, and four of them still fit in a byte.
 * ----------------------------------------------------------------------------------------- */

uint32_t hd_popcount(uint32_t x)
{
#if defined(HD_HAVE_USAD8)
    return hd_usad8(hd_pc_nib(x), 0u);
#elif defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_popcount(x);
#else
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (x * 0x01010101u) >> 24;
#endif
}

/* xorshift32. Deterministic across every node, so the whole cluster can build the SAME
 * alphabet from the same seed without ever sending a hypervector over the wire. That is not a
 * detail: it is why a node can join the machine knowing only a 32-bit number. */
uint32_t hd_rand(uint32_t *state)
{
    uint32_t x = *state ? *state : 0x1D0C5EEDu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void hd_zero(hd_t out)
{
    for (uint32_t i = 0; i < HD_WORDS; i++) out[i] = 0;
}

void hd_copy(hd_t out, const hd_t a)
{
    for (uint32_t i = 0; i < HD_WORDS; i++) out[i] = a[i];
}

void hd_random(hd_t out, uint32_t *rng)
{
    for (uint32_t i = 0; i < HD_WORDS; i++) out[i] = hd_rand(rng);
}

void hd_bind(hd_t out, const hd_t a, const hd_t b)
{
    for (uint32_t i = 0; i < HD_WORDS; i++) out[i] = a[i] ^ b[i];
}

uint32_t hd_hamming(const hd_t a, const hd_t b)
{
#if defined(HD_HAVE_USAD8)
    /* Four words' nibble counts summed before one horizontal add. Measured 14.25 instructions
     * per word against 22-27 for the __builtin_popcount version. */
    uint32_t d = 0;
    uint32_t i = 0;
    for (; i + 4u <= HD_WORDS; i += 4u) {
        uint32_t s = hd_pc_nib(a[i]     ^ b[i]);
        s         += hd_pc_nib(a[i + 1] ^ b[i + 1]);
        s         += hd_pc_nib(a[i + 2] ^ b[i + 2]);
        s         += hd_pc_nib(a[i + 3] ^ b[i + 3]);
        d += hd_usad8(s, 0u);
    }
    for (; i < HD_WORDS; i++) d += hd_popcount(a[i] ^ b[i]);
    return d;
#else
    uint32_t d = 0;
    for (uint32_t i = 0; i < HD_WORDS; i++) d += hd_popcount(a[i] ^ b[i]);
    return d;
#endif
}

/* Hamming distance that gives up as soon as it cannot beat `limit`.
 *
 * A nearest-neighbour search does not need to know HOW MUCH worse a candidate is, only that it
 * is worse. Checking every 8 words costs one compare and abandons most non-matches early.
 * Measured saving on a 4,000-concept search: 1.6x fewer words touched, with a bit-identical
 * winner. The saving is larger the better the true match is, because a low `limit` lets a
 * hopeless candidate be dropped sooner.
 *
 * Returns the true distance, or any value >= limit if it gave up. */
uint32_t hd_hamming_limit(const hd_t a, const hd_t b, uint32_t limit)
{
    uint32_t d = 0;
    for (uint32_t blk = 0; blk < HD_WORDS; blk += 8u) {
#if defined(HD_HAVE_USAD8)
        uint32_t s = hd_pc_nib(a[blk]     ^ b[blk]);
        s         += hd_pc_nib(a[blk + 1] ^ b[blk + 1]);
        s         += hd_pc_nib(a[blk + 2] ^ b[blk + 2]);
        s         += hd_pc_nib(a[blk + 3] ^ b[blk + 3]);
        d += hd_usad8(s, 0u);
        s  = hd_pc_nib(a[blk + 4] ^ b[blk + 4]);
        s += hd_pc_nib(a[blk + 5] ^ b[blk + 5]);
        s += hd_pc_nib(a[blk + 6] ^ b[blk + 6]);
        s += hd_pc_nib(a[blk + 7] ^ b[blk + 7]);
        d += hd_usad8(s, 0u);
#else
        for (uint32_t i = blk; i < blk + 8u; i++) d += hd_popcount(a[i] ^ b[i]);
#endif
        if (d >= limit) return d;              /* cannot win */
    }
    return d;
}

int32_t hd_sim_q10(const hd_t a, const hd_t b)
{
    /* +1024 identical, 0 orthogonal, -1024 opposite. */
    const int32_t d = (int32_t)hd_hamming(a, b);
    return 1024 - (int32_t)((int64_t)d * 2048 / HD_BITS);
}

/* Rotate the whole HD_BITS-wide vector left by `shift` bits.
 *
 * `out` must not alias `in`. The wrap is across the entire vector, not per word -- a per-word
 * rotation would leave 256 independent little permutations and two different positions could
 * collide, which quietly destroys the order information this exists to provide. */
void hd_permute(hd_t out, const hd_t in, int32_t shift)
{
    int32_t s = shift % (int32_t)HD_BITS;
    if (s < 0) s += (int32_t)HD_BITS;

    const uint32_t ws = (uint32_t)s / 32u;
    const uint32_t bs = (uint32_t)s % 32u;

    if (bs == 0u) {
        for (uint32_t i = 0; i < HD_WORDS; i++)
            out[i] = in[(i + HD_WORDS - ws) % HD_WORDS];
        return;
    }
    for (uint32_t i = 0; i < HD_WORDS; i++) {
        const uint32_t hi = in[(i + HD_WORDS - ws) % HD_WORDS];
        const uint32_t lo = in[(i + 2u * HD_WORDS - ws - 1u) % HD_WORDS];
        out[i] = (hi << bs) | (lo >> (32u - bs));
    }
}


/* -------------------------------------------------------------------------------------------
 * BUNDLING
 * ----------------------------------------------------------------------------------------- */

void hd_acc_clear(hd_acc_t *a)
{
    for (uint32_t i = 0; i < HD_BITS; i++) a->c[i] = 0;
    a->n = 0;
}

void hd_acc_addw(hd_acc_t *a, const hd_t v, int16_t weight)
{
    for (uint32_t w = 0; w < HD_WORDS; w++) {
        uint32_t word = v[w];
        const uint32_t base = w * 32u;
        for (uint32_t b = 0; b < 32u; b++) {
            /* bipolar: a set bit pushes toward 1, a clear bit pushes toward 0. Counting only
             * the set bits would make a bundle drift to all-ones as it grows. */
            const int32_t s = ((word >> b) & 1u) ? (int32_t)weight : -(int32_t)weight;
            int32_t c = (int32_t)a->c[base + b] + s;
            if (c >  32000) c =  32000;
            if (c < -32000) c = -32000;
            a->c[base + b] = (int16_t)c;
        }
    }
    a->n++;
}

void hd_acc_add(hd_acc_t *a, const hd_t v)
{
    hd_acc_addw(a, v, 1);
}

void hd_acc_result(const hd_acc_t *a, hd_t out)
{
    for (uint32_t w = 0; w < HD_WORDS; w++) {
        uint32_t word = 0;
        const uint32_t base = w * 32u;
        for (uint32_t b = 0; b < 32u; b++)
            if (a->c[base + b] > 0) word |= (1u << b);
        out[w] = word;
    }
    /* A bit whose count is exactly zero stays 0. With an even number of members that is a
     * genuine tie; leaving it deterministic keeps the whole cluster reproducible, which is
     * worth more here than the tiny bias it introduces. */
}


/* -------------------------------------------------------------------------------------------
 * ITEM MEMORY
 * ----------------------------------------------------------------------------------------- */

void hd_mem_init(hd_mem_t *m, hd_t *storage, uint16_t *labels, uint16_t cap)
{
    m->v = storage;
    m->label = labels;
    m->n = 0;
    m->cap = cap;
}

int32_t hd_mem_add(hd_mem_t *m, const hd_t v, uint16_t label)
{
    if (m->n >= m->cap) return -1;
    hd_copy(m->v[m->n], v);
    if (m->label) m->label[m->n] = label;
    return (int32_t)(m->n++);
}

uint32_t hd_hamming_prefix(const hd_t a, const hd_t b)
{
    uint32_t d = 0;
#if defined(HD_HAVE_USAD8)
    for (uint32_t i = 0; i + 4u <= HD_PREFIX_WORDS; i += 4u) {
        uint32_t s = hd_pc_nib(a[i]     ^ b[i]);
        s         += hd_pc_nib(a[i + 1] ^ b[i + 1]);
        s         += hd_pc_nib(a[i + 2] ^ b[i + 2]);
        s         += hd_pc_nib(a[i + 3] ^ b[i + 3]);
        d += hd_usad8(s, 0u);
    }
#else
    for (uint32_t i = 0; i < HD_PREFIX_WORDS; i++) d += hd_popcount(a[i] ^ b[i]);
#endif
    return d;
}

int32_t hd_mem_best(const hd_mem_t *m, const hd_t q, uint32_t *dist)
{
    int32_t  best = -1;
    uint32_t bd   = 0xFFFFFFFFu;

    if (m->n == 0u) { if (dist) *dist = bd; return -1; }

    /* PASS 1 -- read 32 bytes of each candidate and find the best prefix. Nothing is stored:
     * a second cheap pass costs less memory traffic than an array of n distances, and on a
     * 64 MB node an allocation that scales with the memory size is not free. */
    uint32_t pmin = 0xFFFFFFFFu;
    for (uint16_t i = 0; i < m->n; i++) {
        const uint32_t p = hd_hamming_prefix(m->v[i], q);
        if (p < pmin) pmin = p;
    }

    /* 6 sigma over the prefix. sigma of a Hamming distance across P bits is sqrt(P)/2, so for
     * 256 bits that is 8, and the gate is pmin + 48. Verified over 2,400 trials to never
     * exclude the true nearest vector; 4 sigma did, at high noise. */
    uint32_t root = 1;
    while (root * root < (uint32_t)(HD_PREFIX_WORDS * 32u)) root++;
    const uint32_t gate = pmin + (6u * root) / 2u;

    /* PASS 2 -- full comparison only for what survived. hd_hamming_limit still abandons a
     * survivor early if it cannot beat the incumbent, so the two optimisations compose. */
    for (uint16_t i = 0; i < m->n; i++) {
        if (hd_hamming_prefix(m->v[i], q) > gate) continue;
        const uint32_t d = hd_hamming_limit(m->v[i], q, bd);
        if (d < bd) { bd = d; best = (int32_t)i; }
    }
    if (dist) *dist = bd;
    return best;
}

uint16_t hd_mem_topk(const hd_mem_t *m, const hd_t q, uint16_t k,
                     int32_t *slots, uint32_t *dists)
{
    uint16_t filled = 0;
    for (uint16_t i = 0; i < m->n; i++) {
        const uint32_t d = hd_hamming(m->v[i], q);
        /* insertion sort into the top-k; k is small (single digits) so this beats any
         * cleverer structure and allocates nothing. */
        uint16_t pos = filled;
        while (pos > 0 && dists[pos - 1] > d) pos--;
        if (pos >= k) continue;
        for (uint16_t j = (filled < k ? filled : (uint16_t)(k - 1)); j > pos; j--) {
            slots[j] = slots[j - 1];
            dists[j] = dists[j - 1];
        }
        slots[pos] = (int32_t)i;
        dists[pos] = d;
        if (filled < k) filled++;
    }
    return filled;
}

void hd_mem_reinforce(hd_mem_t *m, int32_t slot, const hd_t v, uint8_t strength)
{
    if (slot < 0 || slot >= (int32_t)m->n) return;
    /* Move the stored vector a fraction of the way toward the new example by flipping the
     * bits where they disagree, but only `strength` out of every 256 of them. Repetition
     * sharpens a concept toward the centre of its examples instead of replacing it. */
    uint32_t rng = (uint32_t)slot * 2654435761u + 1u;
    for (uint32_t w = 0; w < HD_WORDS; w++) {
        const uint32_t diff = m->v[slot][w] ^ v[w];
        if (!diff) continue;
        uint32_t flip = 0;
        for (uint32_t b = 0; b < 32u; b++)
            if ((diff >> b) & 1u)
                if ((hd_rand(&rng) & 0xFFu) < strength) flip |= (1u << b);
        m->v[slot][w] ^= flip;
    }
}


/* -------------------------------------------------------------------------------------------
 * SEQUENCE MODEL
 * ----------------------------------------------------------------------------------------- */

void hd_seq_init(hd_seq_t *s, hd_mem_t *sym, hd_mem_t *ctx,
                 hd_acc_t *pred, uint16_t cap_pred, uint8_t order)
{
    s->sym = sym;
    s->ctx = ctx;
    s->pred = pred;
    s->npred = 0;
    s->cap_pred = cap_pred;
    s->order = order;
    /* Two contexts count as the same when they differ in under 5% of bits. Exact matching
     * would work for clean symbol streams but fails the moment a context is built from a
     * noisy sensor, which is the case this machine actually lives in. */
    s->match_tol = HD_BITS / 20u;
}

void hd_seq_context(const hd_seq_t *s, const uint16_t *recent, hd_t out)
{
    hd_t tmp;
    hd_zero(out);
    for (uint8_t i = 0; i < s->order; i++) {
        /* recent[] is oldest..newest; the newest symbol is rotated by 1, the one before it by
         * 2, and so on. The rotation amount IS the position, which is what makes 'ab' and
         * 'ba' different vectors. */
        const uint16_t sym_idx = recent[i];
        if (sym_idx >= s->sym->n) continue;
        const int32_t dist = (int32_t)(s->order - i);
        hd_permute(tmp, s->sym->v[sym_idx], dist);
        for (uint32_t w = 0; w < HD_WORDS; w++) out[w] ^= tmp[w];
    }
}

void hd_seq_observe(hd_seq_t *s, const uint16_t *recent, uint16_t next_sym)
{
    if (next_sym >= s->sym->n) return;

    hd_t c;
    hd_seq_context(s, recent, c);

    uint32_t d = 0;
    const int32_t slot = hd_mem_best(s->ctx, c, &d);

    if (slot >= 0 && d <= s->match_tol) {
        /* Seen this context before: bundle the new successor into its existing prediction.
         * Bundling rather than overwriting is what lets one context legitimately predict
         * several different symbols with the right weighting. */
        const uint16_t p = s->ctx->label[slot];
        if (p < s->npred) hd_acc_add(&s->pred[p], s->sym->v[next_sym]);
        return;
    }
    if (s->npred >= s->cap_pred) return;                 /* memory full: stop, do not corrupt */
    if (hd_mem_add(s->ctx, c, s->npred) < 0) return;

    hd_acc_clear(&s->pred[s->npred]);
    hd_acc_add(&s->pred[s->npred], s->sym->v[next_sym]);
    s->npred++;
}

int32_t hd_seq_predict(const hd_seq_t *s, const uint16_t *recent, uint32_t *confidence)
{
    hd_t c, guess;
    hd_seq_context(s, recent, c);

    uint32_t d = 0;
    const int32_t slot = hd_mem_best(s->ctx, c, &d);
    if (slot < 0 || d > s->match_tol) {
        if (confidence) *confidence = 0;
        return -1;                                        /* never seen this context */
    }
    const uint16_t p = s->ctx->label[slot];
    if (p >= s->npred) { if (confidence) *confidence = 0; return -1; }

    hd_acc_result(&s->pred[p], guess);

    uint32_t bd = 0xFFFFFFFFu;
    int32_t  bi = -1;
    for (uint16_t i = 0; i < s->sym->n; i++) {
        const uint32_t dd = hd_hamming(s->sym->v[i], guess);
        if (dd < bd) { bd = dd; bi = (int32_t)i; }
    }
    /* Confidence is how far below "unrelated" the winner sits. HD_BITS/2 is orthogonal, so a
     * winner at HD_BITS/2 means the machine has no opinion and the caller must be able to see
     * that rather than be handed a symbol as if it were an answer. */
    if (confidence)
        *confidence = (bd < HD_BITS / 2u) ? ((HD_BITS / 2u - bd) * 1024u / (HD_BITS / 2u)) : 0u;
    return bi;
}


/* -------------------------------------------------------------------------------------------
 * ENCODING THE WORLD
 * ----------------------------------------------------------------------------------------- */

void hd_encode_level(hd_t out, const hd_t base, uint32_t value, uint32_t max)
{
    if (max == 0u) max = 1u;
    if (value > max) value = max;

    /* Flip a prefix whose length is proportional to the value. Level 0 is `base`, level max is
     * its complement, and two nearby levels differ in proportionally few bits -- so the
     * machine can generalise "warm" across 61 C and 62 C. A separate random vector per level
     * would make those two temperatures as unrelated as a temperature and a link failure. */
    const uint32_t nflip = (uint32_t)(((uint64_t)value * HD_BITS) / max);
    const uint32_t fw    = nflip / 32u;
    const uint32_t fb    = nflip % 32u;

    for (uint32_t w = 0; w < HD_WORDS; w++) {
        if (w < fw)       out[w] = ~base[w];
        else if (w == fw) out[w] = base[w] ^ ((fb == 0u) ? 0u : ((1u << fb) - 1u));
        else              out[w] = base[w];
    }
}

void hd_encode_bits(hd_t out, const hd_t *pos, const uint8_t *bytes, uint16_t nbits,
                    hd_acc_t *scratch)
{
    /* Bundle the position vectors of every set bit. The accumulator comes from the caller
     * because it is 16 KB -- see the header. */
    hd_acc_clear(scratch);
    for (uint16_t i = 0; i < nbits; i++)
        if ((bytes[i >> 3] >> (i & 7u)) & 1u) hd_acc_add(scratch, pos[i]);

    if (scratch->n == 0u) { hd_zero(out); return; }
    hd_acc_result(scratch, out);
}
