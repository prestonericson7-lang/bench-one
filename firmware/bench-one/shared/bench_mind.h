/* ===========================================================================================
 *  bench_mind.h -- parallel deliberation: many brains, one question, at the same instant
 * ===========================================================================================
 *
 *  WHAT THIS IS
 *  -------------
 *  Not a faster search. A different SHAPE of thinking.
 *
 *  A single query asks the memory one thing: "what is this?" That is recall, and recall alone
 *  is what a lookup table does. When a question is actually hard, the useful move is to come at
 *  it from several directions at once and see whether the answers agree.
 *
 *  This file runs that structure. One question becomes N transformed questions -- LENSES -- and
 *  every lens is dispatched to the whole cluster simultaneously. Nothing waits for anything.
 *  The lenses are cheap hypervector transforms, so N lenses cost N times the traffic and ONE
 *  round trip of latency, not N.
 *
 *
 *  THE LENSES, AND WHY THESE ONES
 *  -------------------------------
 *  These are the moves that actually found things while this machine was being built. They are
 *  not a taxonomy of cognition; they are the questions that repeatedly turned out to matter.
 *
 *      RECALL       the query as-is. "What is this?"
 *      ANALOGY      unbind a relation. "What is to X as Y is to Z?" -- the one operation a
 *                   vector-symbolic memory can do that a lookup table fundamentally cannot.
 *      PART         the query with known components removed. "What is left once I account for
 *                   what I already recognise?" This is how a residual gets noticed.
 *      CONTEXT      the query bound with what co-occurred. "What usually accompanies this?"
 *      CONTRAST     the query's complement. "What is this NOT?" -- a genuinely different search
 *                   that catches things similarity misses.
 *      PRECEDENT    the query permuted backward in time. "When did I last see this?"
 *      CONSEQUENCE  the query permuted forward. "What followed, the last times?"
 *
 *
 *  THE PART THAT MATTERS MORE THAN THE ANSWER
 *  -------------------------------------------
 *  AGREEMENT BETWEEN LENSES IS THE CONFIDENCE SIGNAL.
 *
 *  When several independent lines of attack land on the same conclusion, that conclusion is
 *  worth acting on. When they scatter, the honest output is "I do not know", and the machine
 *  must be able to say so -- a system that always returns its best guess with the same
 *  confidence is one you cannot ever trust, which is precisely the failure mode that makes a
 *  large language model unreliable.
 *
 *  So mind_conclude() returns three things, and the second two are not decoration:
 *      - the conclusion
 *      - CONSENSUS: how many lenses independently agreed
 *      - DISSENT:   what the disagreeing lenses said instead
 *
 *  A conclusion with 6/7 consensus and a named dissenter is a far more useful object than a
 *  bare answer, and it is the shape of output that lets a human or an outer loop decide
 *  whether to look closer.
 *
 *
 *  WHY THIS IS THE RIGHT USE OF ELEVEN LINUX BOARDS
 *  -------------------------------------------------
 *  One node running seven lenses in sequence takes seven times as long. Eleven nodes running
 *  seven lenses take one round trip, because each lens is an independent query and the cluster
 *  was already built to answer many at once. The hardware makes deliberation free; the only
 *  thing that was missing was asking more than one question.
 * ===========================================================================================
 */

#ifndef BENCH_MIND_H
#define BENCH_MIND_H

#include "bench_hdc.h"
#include "bench_hdc_shard.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIND_RECALL = 0,
    MIND_ANALOGY,
    MIND_PART,
    MIND_CONTEXT,
    MIND_CONTRAST,
    MIND_PRECEDENT,
    MIND_CONSEQUENCE,
    MIND_LENS_COUNT
} mind_lens_t;

const char *mind_lens_name(mind_lens_t l);

/* -------------------------------------------------------------------------------------------
 * THE DELIBERATION
 * ----------------------------------------------------------------------------------------- */
typedef struct {
    /* Relation vectors the lenses transform through. Derived from one seed so every node in
     * the cluster builds an identical set and no vector is ever transmitted. */
    hd_t     rel_analogy;
    hd_t     rel_context;
    hd_t     rel_part;

    /* Result of the last deliberation, one entry per lens. */
    int32_t  answer [MIND_LENS_COUNT];   /* concept id, or -1 if that lens found nothing     */
    uint32_t dist   [MIND_LENS_COUNT];
    uint8_t  replied[MIND_LENS_COUNT];   /* did this lens get a complete answer at all?      */

    uint32_t rng;
    uint32_t match_tol;                  /* a lens further than this counts as "no opinion"  */
} bench_mind_t;

void mind_init(bench_mind_t *m, uint32_t seed);

/* Build the query for one lens. `ctx` may be NULL for lenses that do not use it.
 * This is the whole transform -- pure hypervector algebra, no search, microseconds. */
void mind_lens_query(const bench_mind_t *m, mind_lens_t lens,
                     const hd_t subject, const hd_t ctx, hd_t out);

/* Record what one lens came back with. Call once per lens as replies arrive, in any order. */
void mind_observe(bench_mind_t *m, mind_lens_t lens, int32_t answer, uint32_t dist);

/* -------------------------------------------------------------------------------------------
 * THE CONCLUSION
 * ----------------------------------------------------------------------------------------- */
typedef struct {
    int32_t  conclusion;       /* what the lenses agreed on, or -1                           */
    uint8_t  consensus;        /* how many lenses voted for it                                */
    uint8_t  voted;            /* how many lenses had an opinion at all                       */
    uint8_t  confident;        /* 1 when consensus is strong enough to act on                 */
    int32_t  dissent;          /* the leading alternative, or -1                              */
    uint8_t  dissent_votes;
    uint32_t best_dist;
} mind_conclusion_t;

/* Weigh the lenses. A lens that never replied does not vote -- silence is not agreement, and
 * counting it as such would make a half-dead cluster look unanimous. */
void mind_conclude(const bench_mind_t *m, mind_conclusion_t *out);

/* One line a human can read. Writes at most `cap` bytes, always NUL-terminated. */
int  mind_explain(const bench_mind_t *m, const mind_conclusion_t *c, char *buf, int cap);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_MIND_H */
