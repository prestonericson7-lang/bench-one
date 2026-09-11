/* ===========================================================================================
 *  bench_policy.c -- see bench_policy.h for what this is and what it deliberately is not.
 * ===========================================================================================
 */

#include "bench_policy.h"

#define POL_UNKNOWN 0xFFFFu

static const char *k_action_name[POL_A_COUNT] = {
    "CONTINUE", "RUN_IT", "CHECK_DOWNSTREAM", "HUNT_FIXED_CONST", "ANNEAL",
    "MEASURE_RESOURCE", "DEDUP_BY_SOURCE", "LOWER_CLOCK", "SHED_LOAD", "WAIT_LONGER",
    "REDUCE_CHUNK", "RESHARD", "FREEZE_LAYER", "CHECKPOINT", "ASK_HUMAN",
};
static const char *k_field_name[POL_F_COUNT] = {
    "temp", "err_trend", "coverage", "link_loss", "stall",
    "mem_full", "rail_sag", "stall_count", "verified", "scale_changed",
};

const char *pol_action_name(pol_action_t a)
{
    return (a < POL_A_COUNT) ? k_action_name[a] : "?";
}
const char *pol_field_name(pol_field_t f)
{
    return (f < POL_F_COUNT) ? k_field_name[f] : "?";
}


void bench_policy_init(bench_policy_t *p, hd_t *storage, uint16_t *labels,
                       int8_t *scores, uint16_t cap, uint32_t seed)
{
    p->rng = seed ? seed : 0xC1A0DE01u;

    /* Roles, level bases and actions are all derived from the seed, so every node in the
     * cluster builds an IDENTICAL vocabulary from one 32-bit number. Two nodes can then compare
     * states they each encoded locally without ever exchanging a hypervector. */
    for (int i = 0; i < POL_F_COUNT; i++) hd_random(p->role[i], &p->rng);
    for (int i = 0; i < POL_F_COUNT; i++) hd_random(p->base[i], &p->rng);
    for (int i = 0; i < POL_A_COUNT; i++) hd_random(p->act[i],  &p->rng);

    hd_mem_init(&p->mem, storage, labels, cap);
    p->score = scores;

    /* A precedent must be within ~18% of the bits to count. Tuned against the measured noise
     * floor: unrelated vectors sit at 50% and one-shot recall held at 100% through 20% noise,
     * so 18% accepts genuine near-misses while staying well clear of coincidence. */
    p->match_tol = (HD_BITS * 18u) / 100u;
}

void bench_policy_encode(bench_policy_t *p, const uint16_t *fields, hd_t out, hd_acc_t *scratch)
{
    hd_t lvl, bound;
    hd_acc_clear(scratch);

    for (int f = 0; f < POL_F_COUNT; f++) {
        /* An unobserved field is OMITTED, not encoded as zero. A node that failed to report
         * its temperature must not read as ice-cold, or the policy raises the clock on a board
         * that is actually cooking. */
        if (fields[f] == POL_UNKNOWN) continue;

        uint32_t v = fields[f];
        if (v > 255u) v = 255u;

        hd_encode_level(lvl, p->base[f], v, 255u);
        hd_bind(bound, p->role[f], lvl);     /* role-filler binding: "temp IS 61"             */
        hd_acc_add(scratch, bound);
    }
    if (scratch->n == 0u) { hd_zero(out); return; }
    hd_acc_result(scratch, out);
}

pol_action_t bench_policy_decide(const bench_policy_t *p, const hd_t state,
                                 uint32_t *distance, int8_t *confidence)
{
    uint32_t d = 0;
    const int32_t slot = hd_mem_best(&p->mem, state, &d);

    if (distance) *distance = d;

    if (slot < 0 || d > p->match_tol) {
        if (confidence) *confidence = 0;
        return POL_A_ASK_HUMAN;          /* no precedent. Saying so is the right answer. */
    }
    const int8_t sc = p->score ? p->score[slot] : 0;
    if (confidence) *confidence = sc;

    /* A rule that has been contradicted more than it has been confirmed is not applied, even
     * when it is the nearest match. Retired rules are left in memory rather than deleted so
     * they can recover if the situation that killed them turns out to have been the anomaly. */
    if (sc < 0) return POL_A_ASK_HUMAN;

    return (pol_action_t)p->mem.label[slot];
}

int32_t bench_policy_learn(bench_policy_t *p, const hd_t state, pol_action_t action,
                           int8_t initial_score)
{
    const int32_t slot = hd_mem_add(&p->mem, state, (uint16_t)action);
    if (slot >= 0 && p->score) p->score[slot] = initial_score;
    return slot;
}

void bench_policy_feedback(bench_policy_t *p, const hd_t state, pol_action_t taken, int good)
{
    uint32_t d = 0;
    const int32_t slot = hd_mem_best(&p->mem, state, &d);
    if (slot < 0 || d > p->match_tol) {
        /* No precedent matched, so this is new knowledge rather than a correction. Only record
         * it if it WORKED -- learning a rule from a failure would teach the machine to repeat
         * the thing that just went wrong. */
        if (good) bench_policy_learn(p, state, taken, 1);
        return;
    }
    if (!p->score) return;

    if (good && (pol_action_t)p->mem.label[slot] == taken) {
        if (p->score[slot] < 100) p->score[slot] = (int8_t)(p->score[slot] + 8);
        /* Pull the remembered situation gently toward this one. Repetition sharpens a rule
         * toward the centre of the cases it actually fires on, instead of leaving it pinned to
         * whichever example happened to arrive first. */
        hd_mem_reinforce(&p->mem, slot, state, 32);
    } else {
        if (p->score[slot] > -100) p->score[slot] = (int8_t)(p->score[slot] - 12);
        /* Down faster than up on purpose: a rule that fires wrongly on a bench costs a night,
         * and one that is merely under-confident costs a question. */
    }
}


/* ===========================================================================================
 * THE SEEDED RULES
 * -------------------------------------------------------------------------------------------
 * Every one of these fired on a real defect while this machine was being built. The comment on
 * each is the incident, not an illustration.
 *
 * They are seeded at score 1: barely trusted. Yours will outrank them the moment you confirm
 * one, because yours were observed on your hardware.
 * =========================================================================================*/

static void seed(bench_policy_t *p, hd_acc_t *sc, pol_action_t a,
                 const uint16_t *fields)
{
    hd_t s;
    bench_policy_encode(p, fields, s, sc);
    bench_policy_learn(p, s, a, 1);
}

void bench_policy_seed_defaults(bench_policy_t *p, hd_acc_t *scratch)
{
    uint16_t f[POL_F_COUNT];
    #define CLEAR() do { for (int i = 0; i < POL_F_COUNT; i++) f[i] = POL_UNKNOWN; } while (0)

    /* "It compiles and it looks right."  The bit-sliced kernel looked correct and contained
     * three separate bugs. Running it found all three inside an hour. Nothing is believed
     * until it has executed. */
    CLEAR(); f[POL_F_VERIFIED] = 0;
    seed(p, scratch, POL_A_RUN_IT, f);

    /* Error pinned at exactly chance while the weights were visibly changing. The shadow was
     * updating and the ternary planes it computes from never were: the update was real and
     * nothing downstream consumed it. */
    CLEAR(); f[POL_F_ERR_TREND] = 128; f[POL_F_VERIFIED] = 255;
    seed(p, scratch, POL_A_CHECK_DOWNSTREAM, f);

    /* Worked at 784 inputs, dead at 36. A fixed sigmoid slope against a fan-in-dependent
     * activation spread. When behaviour changes with size, look for a constant that should
     * have scaled. */
    CLEAR(); f[POL_F_SCALE_CHANGED] = 255; f[POL_F_ERR_TREND] = 128;
    seed(p, scratch, POL_A_HUNT_FIXED_CONST, f);

    /* Reconstruction fine, generation producing nothing. The free-running chain sat at exactly
     * maximum entropy -- a temperature problem, and annealing took valid samples from 0.8% to
     * 16.4% with the same weights. */
    CLEAR(); f[POL_F_ERR_TREND] = 40; f[POL_F_COVERAGE] = 0;
    seed(p, scratch, POL_A_ANNEAL, f);

    /* A helper that "obviously" cost nothing used 16,432 bytes of stack and would have
     * overflowed an ESP32 task instantly. Cheap-looking is not cheap; measure it. */
    CLEAR(); f[POL_F_MEM_FULL] = 255; f[POL_F_VERIFIED] = 0;
    seed(p, scratch, POL_A_MEASURE_RESOURCE, f);

    /* Coverage read 423 of 600 when 244 had truly been searched: duplicate replies were being
     * summed. Any aggregate fed by a lossy link needs to know who it already counted. */
    CLEAR(); f[POL_F_LINK_LOSS] = 30; f[POL_F_COVERAGE] = 200;
    seed(p, scratch, POL_A_DEDUP_BY_SOURCE, f);

    /* Slow is not failed. An S3 losing its core to WiFi is the designed-for case, and the
     * answer is to wait and accept lower coverage -- never to declare the node dead. */
    CLEAR(); f[POL_F_STALL] = 200; f[POL_F_STALL_COUNT] = 1; f[POL_F_COVERAGE] = 180;
    seed(p, scratch, POL_A_WAIT_LONGER, f);

    /* Stalls long enough to hurt responsiveness: shorten the chunk so the radio is served
     * sooner. Costs a little loop overhead, buys latency back. */
    CLEAR(); f[POL_F_STALL] = 255; f[POL_F_COVERAGE] = 90;
    seed(p, scratch, POL_A_REDUCE_CHUNK, f);

    /* Several nodes silent for a long time: this is no longer a stall, it is a topology
     * change, and the shard map has to be rebuilt or coverage never recovers. */
    CLEAR(); f[POL_F_STALL_COUNT] = 5; f[POL_F_COVERAGE] = 60;
    seed(p, scratch, POL_A_RESHARD, f);

    /* 90 C is where the RT1062's own panic handler resets the chip, and Paul Stoffregen's
     * guidance is to stay under 95 C for the part to last. Act well before either. */
    CLEAR(); f[POL_F_TEMP] = 180;                    /* ~85 C on the 0..120 scale */
    seed(p, scratch, POL_A_LOWER_CLOCK, f);

    /* Rail sagging under load. Fifteen radios keying together is 7.5 A of transient, and a
     * brown-out presents as firmware bugs everywhere except where it started. */
    CLEAR(); f[POL_F_RAIL_SAG] = 200;
    seed(p, scratch, POL_A_SHED_LOAD, f);

    /* Learning has plateaued: stop, freeze, hand the features downstream. Continuing to train
     * a converged layer only lets the persistent chain wander. */
    CLEAR(); f[POL_F_ERR_TREND] = 126; f[POL_F_VERIFIED] = 255; f[POL_F_COVERAGE] = 255;
    seed(p, scratch, POL_A_FREEZE_LAYER, f);

    /* Memory nearly full and everything else healthy: checkpoint before it matters. A 30-day
     * training run that dies at hour 700 with no checkpoint has produced nothing. */
    CLEAR(); f[POL_F_MEM_FULL] = 230; f[POL_F_TEMP] = 100; f[POL_F_ERR_TREND] = 60;
    seed(p, scratch, POL_A_CHECKPOINT, f);

    /* Everything nominal. Worth storing explicitly so that "nothing is wrong" is a positive
     * match rather than the absence of one. */
    CLEAR();
    f[POL_F_TEMP] = 90; f[POL_F_ERR_TREND] = 60; f[POL_F_COVERAGE] = 250;
    f[POL_F_LINK_LOSS] = 2; f[POL_F_STALL] = 40; f[POL_F_STALL_COUNT] = 0;
    f[POL_F_RAIL_SAG] = 20; f[POL_F_MEM_FULL] = 80; f[POL_F_VERIFIED] = 255;
    f[POL_F_SCALE_CHANGED] = 0;
    seed(p, scratch, POL_A_CONTINUE, f);

    #undef CLEAR
}
