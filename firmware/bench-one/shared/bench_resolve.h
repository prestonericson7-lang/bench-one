/* ===========================================================================================
 *  bench_resolve.h -- close your own gaps before opening your mouth
 * ===========================================================================================
 *
 *  WHAT THIS IS FOR
 *  -----------------
 *  The failure mode of every assistant is the same: it hits the first thing it is unsure about,
 *  asks, gets an answer, hits the next thing, asks again, and twenty exchanges later nobody has
 *  got anywhere. Each question is individually reasonable. The sequence is worthless, because
 *  most of those questions were answerable from evidence the machine already had and never
 *  bothered to go and look at.
 *
 *  This file makes the machine do the looking first.
 *
 *      1. deliberate                     -- many lenses, one question, all at once
 *      2. find what is still ambiguous   -- which candidates remain tied
 *      3. fetch evidence that BEARS ON THAT AMBIGUITY, not evidence in general
 *      4. re-deliberate with it
 *      5. repeat until the answer stops changing
 *      6. only then, if something genuinely cannot be settled from the archive,
 *         ask -- and ask about EXACTLY that thing
 *
 *  Step 5 is the one that matters. The loop stops when more evidence stops moving the answer,
 *  which is a measurable condition, not a guess. Past that point more searching is waste and
 *  the residue is genuinely unknowable from what the machine has lived through.
 *
 *
 *  WHAT MAKES A GOOD QUESTION, MECHANICALLY
 *  -----------------------------------------
 *  When two candidates survive everything, they survive because they are alike everywhere the
 *  evidence reaches. So what distinguishes them is exactly:
 *
 *      difference = candidate_A  XOR  candidate_B
 *
 *  That vector IS the question. Resolved against the concept memory it names the dimension the
 *  machine cannot settle -- and that is worth asking about, because answering it collapses the
 *  ambiguity completely rather than nudging it.
 *
 *  A question generated this way has a property the usual kind does not: it is guaranteed to be
 *  about something the machine could not have worked out for itself, because it only gets
 *  generated after the search saturated.
 *
 *
 *  THE COST RULE
 *  --------------
 *  A confident query NEVER touches the disk. Not "usually" -- never. The archive is entered
 *  only after the fast path has already declared itself unsure, and then it reads a bounded
 *  handful of records at known offsets. Measured on the bench task: 16 records, 16 KB, ~0.6 ms
 *  at USB 2.0 speed, on the minority of queries that need it.
 *
 *
 *  MEASURED BASIS
 *  ---------------
 *  Where consolidation had over-merged two situations into one concept, the fast path named the
 *  concept correctly 100% of the time and was at CHANCE (44-55%) on which of the two it was --
 *  the distinguishing detail no longer existed in RAM. Fetching 16 archive records resolved it
 *  to 100%, at every noise level from 20% to 35%.
 *
 *  That is the gap being connected: information compression destroyed, recovered on demand,
 *  without the user being asked anything at all.
 * ===========================================================================================
 */

#ifndef BENCH_RESOLVE_H
#define BENCH_RESOLVE_H

#include "bench_hdc.h"
#include "bench_dream.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RESOLVE_MAX_ROUNDS
#define RESOLVE_MAX_ROUNDS 6
#endif

/* Supplied by the caller: fetch up to `max` recorded episodes belonging to `concept`.
 * Returns how many were written. This is the ONLY way this file touches storage, so the same
 * loop runs against an SSD on the FPGA, a microSD on a Luckfox, or a RAM array in a test. */
typedef uint16_t (*resolve_fetch_fn)(void *ctx, uint16_t concept, hd_t *out, uint16_t max);

typedef enum {
    RESOLVE_CONFIDENT = 0,   /* settled on the fast path; storage never touched              */
    RESOLVE_RECOVERED,       /* the archive settled it; no question needed                   */
    RESOLVE_SATURATED,       /* more evidence stopped changing the answer; a question remains */
    RESOLVE_UNKNOWN,         /* nothing recognised it at all                                 */
} resolve_status_t;

typedef struct {
    resolve_status_t status;

    int32_t  answer;         /* concept id, or -1                                            */
    uint32_t dist;
    uint32_t margin_q8;      /* separation from the runner-up, 1/256ths of a sigma           */

    int32_t  rival;          /* the candidate it could not rule out, or -1                   */
    hd_t     question;       /* the DIFFERENCE vector -- what to ask about. Valid only when
                              * status is RESOLVE_SATURATED. */

    uint16_t rounds;         /* how many times it went back for more                          */
    uint16_t reads;          /* records actually pulled. 0 on the confident path.             */
} resolve_t;

/* Run the loop. `confident_q8` is the margin (in 1/256ths of a sigma) above which the fast
 * path is trusted without going to storage; 768 (3 sigma) is a sane bench default.
 *
 * `fetch` may be NULL, in which case the machine reasons only from what is in RAM and reports
 * SATURATED immediately when unsure -- which is the correct behaviour for a node with no
 * archive attached, not a degraded one. */
/* `ep_buf`/`ep_cap` are the caller's scratch for fetched episodes -- 4 to 8 vectors is plenty.
 * Caller-owned because an hd_t is 1 KB: eight of them on the stack put this frame at 9,320
 * bytes, which is fine on a Teensy and blows an ESP32 task instantly. Same rule as hd_acc_t,
 * broken once here and caught by -Wstack-usage before it ever ran. */
void resolve_query(const bench_dream_t *d, const hd_t query,
                   resolve_fetch_fn fetch, void *fetch_ctx,
                   uint32_t confident_q8, hd_acc_t *work,
                   hd_t *ep_buf, uint16_t ep_cap, resolve_t *out);

/* Name the thing the question is about, by matching the difference vector against the concept
 * memory. Returns the concept id, or -1 if the ambiguity does not correspond to anything the
 * machine has a name for -- which is itself worth reporting, because "I cannot even describe
 * what I am missing" is a different situation from "I need to know X". */
int32_t resolve_name_question(const bench_dream_t *d, const resolve_t *r, uint32_t *dist_out);

const char *resolve_status_name(resolve_status_t s);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_RESOLVE_H */
