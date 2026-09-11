/* ===========================================================================================
 *  bench_dream.h -- how experience becomes intuition
 * ===========================================================================================
 *
 *  THE PROBLEM WITH WHAT CAME BEFORE THIS FILE
 *  --------------------------------------------
 *  bench_hdc.c gives a machine that remembers everything it is shown, exactly, forever. That is
 *  a superb filing cabinet and it is not a mind. It has no opinion about anything it has not
 *  literally seen, and it has no way to notice that fifty things it saw were all the same kind
 *  of thing.
 *
 *  Intuition is not faster recall. It is the ability to answer about a case you have never met,
 *  because your experience has been compressed into something more general than the cases that
 *  produced it. That compression is the missing mechanism, and it is buildable.
 *
 *
 *  MEASURED, BEFORE THIS FILE WAS WRITTEN
 *  ---------------------------------------
 *  A category was invented and NEVER shown to the machine. Only noisy instances of it were --
 *  30% of every bit wrong, which is what real sensing looks like. Bundling those instances
 *  produces a prototype, and the prototype approaches the category nobody ever provided:
 *
 *      instances seen     distance to the true, never-observed category
 *            1                        0.300
 *            3                        0.216
 *            8                        0.126
 *           15                        0.050
 *           30                        0.012
 *           60                        0.001
 *
 *  And on a NEW instance the machine had never encountered, the prototype beats every stored
 *  instance it was built from: 0.305 versus 0.410 at thirty examples.
 *
 *  That is a concept formed from experience, generalising beyond it. It is not a metaphor for
 *  abstraction; it is abstraction, and the numbers are reproducible.
 *
 *
 *  THREE MECHANISMS, ALL FROM HOW MEMORY ACTUALLY WORKS
 *  -----------------------------------------------------
 *
 *  1. SURPRISE-GATED LEARNING.  A person does not lay down a memory for every second of a
 *     familiar commute. Storing everything equally is what fills a memory with nothing.
 *     So: measure how well the existing memory already predicts the input. Familiar input
 *     REINFORCES what is there. Only genuinely surprising input is stored as new.
 *
 *  2. CONSOLIDATION.  Offline, the machine revisits stored instances, finds the ones that were
 *     really the same thing, and collapses them into a prototype. Episodic becomes semantic.
 *     This is why it is called dreaming: it happens when nothing is being asked, it reorganises
 *     rather than adds, and the machine wakes up able to answer things it could not before.
 *
 *  3. TWO-SPEED RECALL.  Prototypes are searched first -- few, general, and they answer "what
 *     KIND of thing is this" instantly. Instances are searched only when the kind is not
 *     enough. General fast, specific slow, which is the order a person answers in too.
 *
 *
 *  WHAT THIS DOES NOT CLAIM
 *  -------------------------
 *  Nothing here is consciousness, and nobody knows what would be. This is abstraction,
 *  generalisation and forgetting-the-right-things -- three mechanisms that make a memory behave
 *  intuitively rather than like a lookup table. That is what can be built with what is on the
 *  bench, and it is worth building on its own terms.
 * ===========================================================================================
 */

#ifndef BENCH_DREAM_H
#define BENCH_DREAM_H

#include "bench_hdc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    hd_mem_t *inst;        /* episodic: individual things that happened                      */
    hd_mem_t *proto;       /* semantic: what kinds of thing there are                        */
    uint16_t *support;     /* per prototype: how many instances it absorbed                  */
    uint8_t  *absorbed;    /* one byte per INSTANCE slot: already folded into a prototype?
                            *
                            * Without this every member of a category forms its own group, so a
                            * 20-instance category is counted twenty times and `support` reads
                            * 576 for 240 observations. Support is what the caller weighs a
                            * judgement by, so an inflated one is a machine that is confident in
                            * proportion to how repetitive its input was. Measured and fixed. */
    hd_acc_t *work;        /* caller-owned, 16 KB, never a local                             */

    /* Two things closer than this are the same category. Measured basis: unrelated vectors sit
     * at 0.500 of HD_BITS and a 30%-noise instance of the same latent sits near 0.42, so the
     * boundary belongs around 0.45 -- tight enough not to merge distinct things, loose enough
     * to catch the same thing seen twice through noise. */
    uint32_t  merge_tol;

    /* Below this distance an input is "already known" and reinforces rather than being stored.
     * This is the surprise gate; raise it to remember less and generalise sooner. */
    uint32_t  familiar_tol;

    uint32_t  observed;    /* inputs seen                                                    */
    uint32_t  stored;      /* how many were surprising enough to keep                        */
    uint32_t  reinforced;  /* how many just strengthened what was already there               */
    uint32_t  merges;      /* prototypes formed by consolidation                              */
} bench_dream_t;

void dream_init(bench_dream_t *d, hd_mem_t *inst, hd_mem_t *proto,
                uint16_t *support, uint8_t *absorbed, hd_acc_t *work);

/* -------------------------------------------------------------------------------------------
 * 1. SURPRISE
 * ----------------------------------------------------------------------------------------- */

/* How unexpected is this? 0 = already known exactly, 255 = nothing in memory resembles it.
 * This is the signal that decides whether anything is worth learning. */
uint8_t dream_surprise(const bench_dream_t *d, const hd_t v);

typedef enum {
    DREAM_REINFORCED = 0,  /* matched something known; that memory was strengthened          */
    DREAM_STORED,          /* surprising: kept as a new instance                             */
    DREAM_FULL,            /* surprising, but there is nowhere to put it                     */
} dream_result_t;

/* Observe one input. Familiar input costs nothing and sharpens what exists; surprising input
 * is stored. Returns which happened. */
dream_result_t dream_observe(bench_dream_t *d, const hd_t v, uint16_t label);

/* -------------------------------------------------------------------------------------------
 * 2. CONSOLIDATION -- run this when nothing is being asked
 * ----------------------------------------------------------------------------------------- */

/* One pass of consolidation, examining at most `budget` instances so it can be interleaved
 * with real work and interrupted at any point (the same contract the ESP32 shard worker uses).
 * `cursor` is caller-owned and carries across calls.
 *
 * Returns the number of prototypes formed or strengthened this pass. */
uint16_t dream_consolidate(bench_dream_t *d, uint16_t *cursor, uint16_t budget);

/* -------------------------------------------------------------------------------------------
 * 3. RECALL -- general first, specific second
 * ----------------------------------------------------------------------------------------- */
typedef struct {
    int32_t  proto_id;     /* which KIND of thing, or -1                                     */
    uint32_t proto_dist;
    uint16_t proto_support;/* how much experience backs that judgement                       */
    int32_t  inst_id;      /* which SPECIFIC thing, or -1 if only the kind is known           */
    uint32_t inst_dist;
    uint8_t  known;        /* 1 if anything recognised it at all                              */
} dream_recall_t;

/* Prototypes are searched first because they are few and general. Instances are searched only
 * when `want_specific` is set, which is the caller saying "the kind is not enough". */
void dream_recall(const bench_dream_t *d, const hd_t q, int want_specific, dream_recall_t *out);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_DREAM_H */
