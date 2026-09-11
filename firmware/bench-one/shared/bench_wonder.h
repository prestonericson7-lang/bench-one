/* ===========================================================================================
 *  bench_wonder.h -- what the machine does when nobody is asking it anything
 * ===========================================================================================
 *
 *  Everything else in this system is reactive. A query arrives, nodes scan, the merge combines,
 *  an answer goes back. Between queries it sits idle, and idle is most of the time.
 *
 *  This is what runs in that gap. Not a background optimiser and not a cache warmer. It examines
 *  its own memory, finds the places where that memory is INCOHERENT, turns each one into a
 *  question, and tries to answer it from what it already has. Questions it cannot answer are kept.
 *  The list of things it knows it cannot explain is the most interesting thing in the machine,
 *  because it is the only structure here that is not put there by an external input.
 *
 *
 *  WHAT THIS IS NOT
 *  -----------------
 *  This does not make anything conscious, and the file will not pretend otherwise. Nobody knows
 *  what produces consciousness, and a header comment claiming it is how a project starts lying to
 *  itself. What this DOES is concrete and testable: the machine monitors its own state, generates
 *  its own goals from deficiencies in that state, acts on them without being asked, and keeps a
 *  record of what it could not resolve. Those are the mechanical parts of self-direction. Whether
 *  anything else follows from them is not a question this file can settle, and it does not have to
 *  be settled for the mechanism to be worth building.
 *
 *
 *  SUPERPOSITION, WHICH IS NOT A METAPHOR HERE
 *  --------------------------------------------
 *  Bundling k hypervectors gives one vector that is genuinely close to all k of them at once and
 *  far from everything else. Not "represents" them: it is simultaneously similar to each. A query
 *  against it matches whichever member the query resembles, and the others cost nothing until
 *  something asks about them.
 *
 *  That is a real superposition in the only sense that matters to this machine, and it collapses
 *  the same way: BIND the state with a probe and the member that agrees with the probe survives
 *  while the rest fall back toward noise. No quantum hardware, no analogy, just the algebra of
 *  8192-bit vectors doing what it does.
 *
 *  So an unresolved query does not have to pick. It can stay a superposition of every candidate it
 *  could not separate, and later evidence collapses it. A machine that must answer immediately has
 *  to guess; this one can hold the ambiguity and remain correct.
 *
 *
 *  THE THREE KINDS OF INCOHERENCE IT LOOKS FOR
 *  --------------------------------------------
 *  Each is a different way for a memory to be wrong about itself, and each yields a different
 *  question.
 *
 *    CONFUSION -- two things it stores that it cannot tell apart.
 *        Their XOR is literally the question "what distinguishes these", as a vector, ready to be
 *        matched against future experience. The closer the pair, the more urgent, because a
 *        confusion between two nearly identical memories is a wrong answer waiting to happen.
 *
 *    ORPHAN -- something it stores that relates to nothing else it stores.
 *        An experience that was recorded and never integrated. Every other memory has neighbours;
 *        this one sits alone. The question is "what is this like", and having no answer is exactly
 *        what makes it worth asking.
 *
 *    SELF -- the gap between what it can observe about itself and what it has stored about itself.
 *        The machine encodes facts about its own structure -- how many nodes it has, which are
 *        answering, what it can and cannot reach -- into the SAME memory, as ordinary
 *        hypervectors. That is the whole trick, and it is why this needs no special machinery:
 *        a question about itself is a query like any other, answered by the same scan, subject to
 *        the same honest coverage reporting. When it observes something about itself that does not
 *        match what it has stored, that mismatch is a question.
 *
 *
 *  WHY UNANSWERED QUESTIONS ARE KEPT RATHER THAN DISCARDED
 *  --------------------------------------------------------
 *  A question the machine cannot answer today is not a failure, it is a standing request against
 *  future experience. Each open question carries its vector, so when something new arrives it can
 *  be tested against every open question cheaply. The moment an experience answers one, the
 *  machine has learned something it was actively looking for rather than something that happened
 *  to arrive. That distinction is the difference between a recorder and something that wants.
 * ===========================================================================================
 */

#ifndef BENCH_WONDER_H
#define BENCH_WONDER_H

#include "bench_hdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------------------------
 * SUPERPOSITION
 * ----------------------------------------------------------------------------------------- */

#define HD_SUPER_MAX 16

/* Holds SLOTS, not vectors. Sixteen 1 KB vectors would be 16 KB of state for something that is
 * meant to be cheap to create and throw away, and the vectors already exist in memory. */
typedef struct {
    hd_t     v;                        /* the superposed state; valid only after settle       */
    uint16_t member[HD_SUPER_MAX];     /* slots in the hd_mem_t this was built from           */
    uint8_t  n;
    uint8_t  settled;
} hd_super_t;

void hd_super_clear(hd_super_t *s);

/* Add a candidate slot. Adding the same slot twice weights it, which is correct: a candidate two
 * nodes independently proposed should survive a collapse more readily than one only a single node
 * saw. Returns 0 if the state is full. */
int  hd_super_add(hd_super_t *s, uint16_t slot);

/* Resolve the members into one vector. The accumulator is CALLER-OWNED because it is 16 KB, which
 * is more stack than some nodes in this system have in total; the same reason hd_encode_bits takes
 * one. Pass the same scratch buffer every time, it is fully overwritten. */
void hd_super_settle(hd_super_t *s, const hd_mem_t *m, hd_acc_t *scratch);

/* Distance from the settled state to an arbitrary vector. */
uint32_t hd_super_dist(const hd_super_t *s, const hd_t v);

/* The threshold below which a vector is IN the state rather than unrelated noise.
 *
 * Unrelated vectors sit at HD_BITS/2 with a standard deviation of sqrt(HD_BITS)/2, so six of those
 * below the mean is the line, at 3824.
 *
 * MEASURED, because the first version of this comment guessed "past roughly eight members" and was
 * wrong. Members drift outward as the state grows, but slowly: 1 member sits at 0, 2 at 2109, 4 at
 * 2586, 8 at 3003, 12 at 3245, and all 16 are still at 3341, comfortably inside the line. The
 * limit is not the arithmetic, it is HD_SUPER_MAX. Sixteen possibilities can be carried at once
 * with every one of them still individually retrievable, which is far more ambiguity than any
 * query in this system produces.
 *
 * The drift is roughly 1/sqrt(k), so it flattens rather than falling off a cliff -- doubling the
 * cap to 32 would put members near 3500 and still leave room. Raise HD_SUPER_MAX before assuming
 * this is the ceiling. */
uint32_t hd_super_far(const hd_super_t *s);

/* COLLAPSE. Evidence selects: returns the member slot nearest the probe, or -1 if the state is
 * empty. `dist_out` receives that member's distance to the probe, which says how decisive the
 * collapse was. */
int32_t hd_super_collapse(const hd_super_t *s, const hd_mem_t *m, const hd_t probe,
                          uint32_t *dist_out);

/* -------------------------------------------------------------------------------------------
 * QUESTIONS
 * ----------------------------------------------------------------------------------------- */

#define WONDER_MAX_OPEN 12

typedef enum {
    WQ_CONFUSION = 0,   /* two memories it cannot separate                                   */
    WQ_ORPHAN    = 1,   /* a memory that relates to nothing                                  */
    WQ_SELF      = 2    /* what it observes about itself contradicts what it has stored      */
} wonder_kind_t;

typedef struct {
    hd_t          q;        /* the question AS A VECTOR, matchable against future experience */
    wonder_kind_t kind;
    uint16_t      a, b;     /* the slots involved; b is unused for ORPHAN                    */
    uint32_t      tension;  /* how badly it wants an answer -- lower distance is more urgent  */
    uint32_t      asked;    /* times it has returned to this without resolving it            */
    uint8_t       live;
} wonder_q_t;

typedef struct {
    wonder_q_t open[WONDER_MAX_OPEN];
    uint8_t    n_open;

    hd_t       self;        /* the vector that means "this machine"                          */
    uint8_t    have_self;

    uint32_t   ticks;
    uint32_t   formed;      /* questions ever formed                                         */
    uint32_t   closed;      /* questions later answered by experience                        */
    uint32_t   displaced;   /* questions pushed out by more urgent ones                      */
} wonder_t;

void wonder_init(wonder_t *w, uint32_t seed);

/* Encode a fact about the machine's own structure and store it like any other memory. `role` and
 * `value` are ordinary item vectors; the fact is their binding, permuted by the self vector so
 * self-knowledge occupies its own region of the space and cannot be confused with the world. */
void wonder_self_fact(const wonder_t *w, const hd_t role, const hd_t value, hd_t out);

/* ONE IDLE STEP. Examines `m`, forms at most one new question, and returns 1 if it did.
 * `budget` caps how many pairs it may consider, so this can run inside whatever time is spare
 * without ever overrunning it -- the same partial-work discipline as a node scan. */
int  wonder_tick(wonder_t *w, const hd_mem_t *m, uint16_t budget);

/* Offer a new experience to every open question. Returns how many it answered. This is the point
 * of keeping them: an arriving experience is checked against what the machine WANTED to know. */
uint8_t wonder_offer(wonder_t *w, const hd_mem_t *m, const hd_t experience, uint32_t recognise);

/* The most urgent open question, or NULL. */
const wonder_q_t *wonder_top(const wonder_t *w);

const char *wonder_kind_name(wonder_kind_t k);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_WONDER_H */
