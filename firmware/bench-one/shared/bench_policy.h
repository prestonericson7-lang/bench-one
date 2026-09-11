/* ===========================================================================================
 *  bench_policy.h -- the machine's operating judgement
 * ===========================================================================================
 *
 *  WHAT THIS IS, AND WHAT IT HONESTLY IS NOT
 *  ------------------------------------------
 *  You asked for it to think like I do. Here is the straight version of what transfers and
 *  what does not.
 *
 *  What does NOT transfer: knowledge and reasoning. Those came out of an enormous training
 *  run, and no encoding trick reproduces them on 8,192 bits. Anything claiming otherwise is
 *  selling you something.
 *
 *  What DOES transfer, and is worth more here than either of us assumed: the PROCEDURE. Look
 *  at what actually found the bugs while this system was being built --
 *
 *      a kernel was written and looked correct        -> ran it, found 3 real bugs
 *      error sat exactly at chance                    -> asked what is downstream of the update
 *      it worked at one layer size and not another    -> hunted for a constant that should scale
 *      reconstruction fine, generation dead           -> suspected temperature, annealed, fixed
 *      a function looked cheap                        -> measured its stack, found 16 KB
 *      a merge looked idempotent                      -> tested it, coverage double-counted
 *
 *  None of that is insight. Every line is a STATE -> ACTION rule, and rules are exactly what
 *  this substrate stores one-shot and recalls under noise. That is a policy, it is learnable,
 *  and it is the part that will actually keep your bench running while you sleep.
 *
 *
 *  WHY IT IS NOT AN if/else LADDER
 *  --------------------------------
 *  Because a ladder only fires on conditions someone predicted. This recalls the NEAREST
 *  remembered situation, so a state nobody wrote a branch for still retrieves the closest
 *  precedent -- and because encode_level makes 61 C and 62 C similar vectors, a rule learned
 *  at one temperature already covers its neighbours. And when you correct it, the correction
 *  is one hd_mem_add. No recompile, no reflash, no retraining.
 *
 *  Every rule below is one you can delete, and every rule it learns from you outranks the ones
 *  I seeded, because yours were observed on your hardware and mine were observed on mine.
 *
 *
 *  HOW IT LEARNS
 *  --------------
 *      observe   state + action + outcome
 *      good      reinforce that memory toward this state (it sharpens)
 *      bad       weaken it, and past a threshold the rule is retired
 *
 *  There is no training run. A rule exists the moment it is added and is usable on the next
 *  query, which is the only workable model for a machine that is being physically rebuilt
 *  between runs.
 * ===========================================================================================
 */

#ifndef BENCH_POLICY_H
#define BENCH_POLICY_H

#include "bench_hdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------------------------
 * OBSERVATIONS -- what the machine can notice about itself
 * -----------------------------------------------------------------------------------------
 * These are the roles in a state record. A state is built by binding each role to a level
 * vector for its current value and bundling the lot, so a state is one hypervector regardless
 * of how many facts went into it.
 * ----------------------------------------------------------------------------------------- */
typedef enum {
    POL_F_TEMP = 0,       /* hottest node die temperature, 0..120 C                          */
    POL_F_ERR_TREND,      /* learning error: 0 falling fast .. 128 flat .. 255 rising         */
    POL_F_COVERAGE,       /* fraction of the distributed memory that answered, 0..255         */
    POL_F_LINK_LOSS,      /* frames lost or duplicated per hundred, 0..100                    */
    POL_F_STALL,          /* worst observed node stall, 0..255 (log-ish ms)                   */
    POL_F_MEM_FULL,       /* item memory occupancy, 0..255                                    */
    POL_F_RAIL_SAG,       /* supply droop in tens of mV, 0..255                               */
    POL_F_STALL_COUNT,    /* nodes currently silent, 0..38                                    */
    POL_F_VERIFIED,       /* has this artefact actually been RUN? 0 = no, 255 = yes           */
    POL_F_SCALE_CHANGED,  /* did a size/shape change since it last worked? 0 or 255           */
    POL_F_COUNT
} pol_field_t;

/* -------------------------------------------------------------------------------------------
 * ACTIONS -- what it may decide to do
 * -----------------------------------------------------------------------------------------
 * Deliberately small and deliberately boring. Every one is reversible, and the last is always
 * available: a policy that cannot say "I do not know, ask the human" will eventually do
 * something confident and wrong at 3 a.m.
 * ----------------------------------------------------------------------------------------- */
typedef enum {
    POL_A_CONTINUE = 0,        /* nothing is wrong                                           */
    POL_A_RUN_IT,              /* stop reasoning about it and execute it                      */
    POL_A_CHECK_DOWNSTREAM,    /* the update is happening; find what consumes it               */
    POL_A_HUNT_FIXED_CONST,    /* works at one size, not another                              */
    POL_A_ANNEAL,              /* good at reconstructing, bad at generating                    */
    POL_A_MEASURE_RESOURCE,    /* it looks cheap; measure stack/time before believing that     */
    POL_A_DEDUP_BY_SOURCE,     /* an aggregate is double-counting                              */
    POL_A_LOWER_CLOCK,         /* thermal                                                      */
    POL_A_SHED_LOAD,           /* power / rail sag                                             */
    POL_A_WAIT_LONGER,         /* slow is not failed                                           */
    POL_A_REDUCE_CHUNK,        /* stalls are hurting responsiveness                            */
    POL_A_RESHARD,             /* a node is gone for good                                      */
    POL_A_FREEZE_LAYER,        /* it has stopped improving                                     */
    POL_A_CHECKPOINT,          /* save before doing anything risky                             */
    POL_A_ASK_HUMAN,           /* no precedent close enough                                    */
    POL_A_COUNT
} pol_action_t;

const char *pol_action_name(pol_action_t a);
const char *pol_field_name (pol_field_t f);

/* -------------------------------------------------------------------------------------------
 * THE POLICY
 * ----------------------------------------------------------------------------------------- */
typedef struct {
    hd_t      role[POL_F_COUNT];   /* one random vector per observable                        */
    hd_t      base[POL_F_COUNT];   /* level-encoding base per observable                      */
    hd_t      act [POL_A_COUNT];   /* one random vector per action                            */

    hd_mem_t  mem;                 /* remembered situations; label = action                   */
    int8_t   *score;               /* confidence per memory, -128..127                        */

    uint32_t  match_tol;           /* how close a precedent must be to count                  */
    uint32_t  rng;
} bench_policy_t;

/* `storage`, `labels` and `scores` are caller-owned, cap entries each. */
void bench_policy_init(bench_policy_t *p, hd_t *storage, uint16_t *labels,
                       int8_t *scores, uint16_t cap, uint32_t seed);

/* Build a state vector from a full field array (POL_F_COUNT entries, 0..255 each).
 * Use 0xFFFF in a field to mean "not observed" and it is left out of the bundle -- an unknown
 * value must not read as zero, or a node that failed to report its temperature would look ice
 * cold and the policy would happily raise the clock. */
void bench_policy_encode(bench_policy_t *p, const uint16_t *fields, hd_t out, hd_acc_t *scratch);

/* Ask: what did we do last time it looked like this?
 * Writes the distance to the nearest precedent and its confidence. Returns POL_A_ASK_HUMAN
 * when nothing is close enough -- which is the correct answer far more often than it feels. */
pol_action_t bench_policy_decide(const bench_policy_t *p, const hd_t state,
                                 uint32_t *distance, int8_t *confidence);

/* Teach it a rule. Returns the slot or -1 if full. */
int32_t bench_policy_learn(bench_policy_t *p, const hd_t state, pol_action_t action,
                           int8_t initial_score);

/* Tell it how that turned out. `good` reinforces the memory toward this state and raises its
 * score; otherwise the score falls, and a rule that keeps being wrong is retired. */
void bench_policy_feedback(bench_policy_t *p, const hd_t state, pol_action_t taken, int good);

/* Seed the diagnostics that were actually earned building this machine. Every one of these
 * fired on a real bug in this codebase; none is hypothetical. Call once on a fresh policy --
 * and delete any you disagree with, because a rule you have not seen fire is a rule you should
 * not trust. */
void bench_policy_seed_defaults(bench_policy_t *p, hd_acc_t *scratch);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_POLICY_H */
