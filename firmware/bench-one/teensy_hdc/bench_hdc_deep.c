/* ===========================================================================================
 *  bench_hdc_deep.c -- see bench_hdc_deep.h. The whole cluster, one question, full dimension.
 * ===========================================================================================
 */

#include "bench_hdc_deep.h"

#define HD_NO_CAND 0xFFFFFFFFu

uint32_t hd_hamming_slice(const hd_t a, const hd_t b, uint16_t w0, uint16_t wn)
{
    uint32_t d = 0;
    const uint32_t end = (uint32_t)w0 + wn;
    for (uint32_t i = w0; i < end && i < HD_WORDS; i++)
        d += hd_popcount(a[i] ^ b[i]);
    return d;
}

void hd_deep_begin(hd_deep_t *d, uint16_t query_id, const uint32_t *cand, uint16_t k,
                   uint16_t slices_expected)
{
    if (k > HD_MAX_K) k = HD_MAX_K;
    d->query_id        = query_id;
    d->k               = k;
    d->seen            = 0;
    d->slices_in       = 0;
    d->slices_expected = slices_expected;
    for (uint16_t i = 0; i < k; i++) { d->cand[i] = cand[i]; d->sum[i] = 0; }
}

int hd_deep_add(hd_deep_t *d, uint16_t node, uint16_t query_id,
                const uint16_t *partial, uint16_t k)
{
    if (query_id != d->query_id) return 0;
    if (k != d->k)               return 0;      /* a reply about a different shortlist */
    if (node >= HD_MAX_NODES)    return 0;

    const uint64_t bit = (uint64_t)1 << node;
    /* A sum is NOT idempotent. Everywhere else in this system a duplicate is free; here it
     * would inflate one slice's contribution and make a candidate look further away than it
     * is. So duplicates are refused outright rather than merged. */
    if (d->seen & bit) return 0;
    d->seen |= bit;
    d->slices_in++;

    for (uint16_t i = 0; i < k; i++) d->sum[i] += partial[i];
    return 1;
}

int hd_deep_ready(const hd_deep_t *d)
{
    return (d->slices_expected && d->slices_in >= d->slices_expected) ? 1 : 0;
}

int32_t hd_deep_best(const hd_deep_t *d, uint32_t *dist, uint32_t *margin_q8)
{
    if (!d->k) return -1;

    uint32_t b0 = 0xFFFFFFFFu, b1 = 0xFFFFFFFFu;
    uint16_t bi = 0xFFFFu;
    for (uint16_t i = 0; i < d->k; i++) {
        const uint32_t s = d->sum[i];
        if (s < b0 || (s == b0 && bi != 0xFFFFu && d->cand[i] < d->cand[bi])) {
            b1 = b0; b0 = s; bi = i;
        } else if (s < b1) {
            b1 = s;
        }
    }
    if (bi == 0xFFFFu) return -1;
    if (dist) *dist = b0;

    /* Margin in sigma. The standard deviation of a Hamming distance between unrelated vectors
     * over W bits is sqrt(W)/2, and W here is HD_BITS * slices_in. Reported in 1/256ths so the
     * caller can threshold without floating point. */
    if (margin_q8) {
        const uint32_t width = (uint32_t)HD_BITS * (d->slices_in ? d->slices_in : 1u);
        uint32_t root = 1;
        while (root * root < width) root++;            /* integer sqrt, once per query */
        const uint32_t sigma2 = root;                  /* 2*sigma = sqrt(width)        */
        *margin_q8 = (b1 == 0xFFFFFFFFu || !sigma2)
                     ? 0u
                     : (uint32_t)(((uint64_t)(b1 - b0) * 2u * 256u) / sigma2);
    }
    return (int32_t)d->cand[bi];
}

void hd_deep_score(const hd_mem_t *mem, const hd_t query, uint16_t w0, uint16_t wn,
                   const uint32_t *cand, uint16_t k, uint16_t base, uint16_t *partial_out)
{
    for (uint16_t i = 0; i < k; i++) {
        const uint32_t g = cand[i];
        /* A candidate outside this node's range scores the maximum for this slice rather than
         * zero. Zero would make an id nobody holds look like a perfect match. */
        if (g < base || g >= (uint32_t)base + mem->n) {
            partial_out[i] = (uint16_t)(wn * 32u);
            continue;
        }
        partial_out[i] = (uint16_t)hd_hamming_slice(mem->v[g - base], query, w0, wn);
    }
}


/* -------------------------------------------------------------------------------------------
 * WIRE
 * ----------------------------------------------------------------------------------------- */
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t g32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

uint32_t hd_deep_pack_req(const hd_deep_t *d, uint8_t *out, uint32_t cap)
{
    const uint32_t need = HD_DEEP_REQ_BYTES(d->k);
    if (cap < need) return 0;
    p16(out + 0, d->query_id);
    p16(out + 2, d->k);
    for (uint16_t i = 0; i < d->k; i++) p32(out + 4 + 4u * i, d->cand[i]);
    return need;
}

int hd_deep_unpack_req(uint16_t *qid, uint16_t *k, uint32_t *cand,
                       uint16_t max_k, const uint8_t *in, uint32_t len)
{
    if (len < 4u) return 0;
    const uint16_t kk = g16(in + 2);
    if (kk > max_k || len < HD_DEEP_REQ_BYTES(kk)) return 0;
    *qid = g16(in + 0);
    *k   = kk;
    for (uint16_t i = 0; i < kk; i++) cand[i] = g32(in + 4 + 4u * i);
    return 1;
}

uint32_t hd_deep_pack_rep(uint16_t qid, uint16_t node, const uint16_t *partial, uint16_t k,
                          uint8_t *out, uint32_t cap)
{
    const uint32_t need = HD_DEEP_REP_BYTES(k);
    if (cap < need) return 0;
    p16(out + 0, qid);
    p16(out + 2, node);
    p16(out + 4, k);
    for (uint16_t i = 0; i < k; i++) p16(out + 6 + 2u * i, partial[i]);
    return need;
}

int hd_deep_unpack_rep(uint16_t *qid, uint16_t *node, uint16_t *k, uint16_t *partial,
                       uint16_t max_k, const uint8_t *in, uint32_t len)
{
    if (len < 6u) return 0;
    const uint16_t kk = g16(in + 4);
    if (kk > max_k || len < HD_DEEP_REP_BYTES(kk)) return 0;
    *qid  = g16(in + 0);
    *node = g16(in + 2);
    *k    = kk;
    for (uint16_t i = 0; i < kk; i++) partial[i] = g16(in + 6 + 2u * i);
    return 1;
}
