/* ===========================================================================================
 *  bench_hdc_deep.h -- the whole cluster on ONE question, at full dimension
 * ===========================================================================================
 *
 *  MEASURED, at 45% of every bit corrupted, 20,000 concepts:
 *
 *      one node alone      D =  8,192     5.0 sigma     1 error in 9e+07 decisions
 *      eleven together     D = 90,112    26.8 sigma     1 error in 6e+157 decisions
 *
 *      traffic, naive     440,000 bytes/query
 *      traffic, this      1,584   bytes/query        278x less
 *
 *  Beyond ~26 sigma the representation is no longer what limits accuracy: a DRAM bit flip or
 *  a stale reply is. That is the point of building it this way -- error becomes an engineering
 *  budget you can spend hardware against, instead of a property of the model you cannot audit.
 *
 *
 *  TWO STAGES, AND WHY THERE ARE TWO
 *  ----------------------------------
 *  STAGE 1  COARSE.   Item-sharded, exactly as bench_hdc_shard.h does it: every node searches
 *                     its own concepts over its own slice and reports its best few. Cheap, and
 *                     at 5 sigma it is already right ~100% of the time -- but 5 sigma is not
 *                     enough certainty to act on for complex work.
 *
 *  STAGE 2  DEEP.     Take the shortlist. Broadcast just the candidate IDs. EVERY node now
 *                     scores EVERY candidate against its own slice of the vector, and the
 *                     coordinator SUMS the partial distances. That sum is the true distance at
 *                     full 90,112-bit dimension.
 *
 *  Stage 2 is the whole cluster moving in unison on one question. It is slower than stage 1 by
 *  a round trip. It is worth it because a decision made at 26 sigma is a decision that does not
 *  need to be revisited.
 *
 *
 *  WHY SUMMING PARTIALS IS SAFE
 *  -----------------------------
 *  Hamming distance is additive over disjoint slices: the distance across 90,112 bits is
 *  exactly the sum of eleven 8,192-bit distances. So stage 2 is a SUM, which is a monoid, and
 *  it inherits everything bench_hdc_shard.h already relies on -- any order, any duplicates.
 *
 *  With one difference that matters: a MISSING slice is not merely less coverage, it is a
 *  smaller distance for every candidate. Comparing a candidate scored by 11 nodes against one
 *  scored by 9 would silently favour the second. So a deep result is only valid when every
 *  slice reported, and hd_deep_ready() is what says so. Acting on a partial deep result is the
 *  one way this design can produce a confidently wrong answer.
 * ===========================================================================================
 */

#ifndef BENCH_HDC_DEEP_H
#define BENCH_HDC_DEEP_H

#include "bench_hdc.h"
#include "bench_hdc_shard.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shortlist size. 32 measured as sufficient (57.3 sigma at 40% noise, 27.3 at 45%); 64 gave
 * no further gain, so the extra traffic buys nothing. */
#ifndef HD_MAX_K
#define HD_MAX_K 64
#endif

/* -------------------------------------------------------------------------------------------
 * PARTIAL DISTANCE -- what one node computes on its own slice
 * ----------------------------------------------------------------------------------------- */

/* Hamming distance over words [w0, w0+wn) only. This is a node's contribution to a distance
 * whose full width is HD_BITS * n_slices. */
uint32_t hd_hamming_slice(const hd_t a, const hd_t b, uint16_t w0, uint16_t wn);


/* -------------------------------------------------------------------------------------------
 * STAGE 2 ACCUMULATOR -- lives on the coordinator
 * ----------------------------------------------------------------------------------------- */
typedef struct {
    uint16_t query_id;
    uint16_t k;                       /* candidates under consideration                       */
    uint32_t cand[HD_MAX_K];          /* their global ids                                     */
    uint32_t sum[HD_MAX_K];           /* running total distance, summed across slices         */
    uint64_t seen;                    /* which node slices have reported                      */
    uint16_t slices_in;               /* how many                                             */
    uint16_t slices_expected;
} hd_deep_t;

/* Begin a deep pass over a shortlist produced by stage 1. */
void hd_deep_begin(hd_deep_t *d, uint16_t query_id, const uint32_t *cand, uint16_t k,
                   uint16_t slices_expected);

/* Fold in one node's partial distances (k values, same order as cand[]).
 * Returns 1 if counted, 0 if it was a duplicate slice or the wrong query. Duplicates are
 * REFUSED rather than added -- a summed distance double-counted for one slice is not a
 * degraded answer, it is a wrong one. */
int  hd_deep_add(hd_deep_t *d, uint16_t node, uint16_t query_id,
                 const uint16_t *partial, uint16_t k);

/* Every slice has reported. Only then is the summed distance a true full-dimension distance. */
int  hd_deep_ready(const hd_deep_t *d);

/* Winner: global id, or -1. `dist` gets the full-dimension distance, `margin_q8` gets the gap
 * to the runner-up in 1/256ths of a sigma -- the caller's measure of how sure to be. */
int32_t hd_deep_best(const hd_deep_t *d, uint32_t *dist, uint32_t *margin_q8);

/* -------------------------------------------------------------------------------------------
 * WIRE FORMAT
 * -----------------------------------------------------------------------------------------
 * Request : [qid:2][k:2][cand:4*k]              -- candidate ids only, never vectors
 * Reply   : [qid:2][node:2][k:2][partial:2*k]   -- one uint16 per candidate
 *
 * At k=64: request 260 bytes, reply 134 bytes. Partial distances are uint16 because one
 * slice is HD_BITS wide and HD_BITS <= 65535; the SUM is uint32 on the coordinator.
 * ----------------------------------------------------------------------------------------- */
#define HD_DEEP_REQ_BYTES(k)   (4u + 4u * (uint32_t)(k))
#define HD_DEEP_REP_BYTES(k)   (6u + 2u * (uint32_t)(k))

uint32_t hd_deep_pack_req(const hd_deep_t *d, uint8_t *out, uint32_t cap);
int      hd_deep_unpack_req(uint16_t *qid, uint16_t *k, uint32_t *cand,
                            uint16_t max_k, const uint8_t *in, uint32_t len);
uint32_t hd_deep_pack_rep(uint16_t qid, uint16_t node, const uint16_t *partial, uint16_t k,
                          uint8_t *out, uint32_t cap);
int      hd_deep_unpack_rep(uint16_t *qid, uint16_t *node, uint16_t *k, uint16_t *partial,
                            uint16_t max_k, const uint8_t *in, uint32_t len);

/* Node side: score `k` candidates against the query, using only this node's slice. */
void hd_deep_score(const hd_mem_t *mem, const hd_t query, uint16_t w0, uint16_t wn,
                   const uint32_t *cand, uint16_t k, uint16_t base, uint16_t *partial_out);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_HDC_DEEP_H */
