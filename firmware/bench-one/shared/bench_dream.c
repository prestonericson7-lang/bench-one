/* ===========================================================================================
 *  bench_dream.c -- see bench_dream.h
 * ===========================================================================================
 */

#include "bench_dream.h"

void dream_init(bench_dream_t *d, hd_mem_t *inst, hd_mem_t *proto,
                uint16_t *support, uint8_t *absorbed, hd_acc_t *work)
{
    d->inst    = inst;
    d->proto   = proto;
    d->support  = support;
    d->absorbed = absorbed;
    d->work     = work;

    d->merge_tol    = (HD_BITS * 45u) / 100u;
    d->familiar_tol = (HD_BITS * 35u) / 100u;

    d->observed = d->stored = d->reinforced = d->merges = 0;
    for (uint16_t i = 0; i < proto->cap; i++) support[i] = 0;
    for (uint16_t i = 0; i < inst->cap;  i++) absorbed[i] = 0;
}

uint8_t dream_surprise(const bench_dream_t *d, const hd_t v)
{
    uint32_t dp = 0xFFFFFFFFu, di = 0xFFFFFFFFu;
    if (d->proto->n) hd_mem_best(d->proto, v, &dp);
    if (d->inst->n)  hd_mem_best(d->inst,  v, &di);

    uint32_t best = (dp < di) ? dp : di;
    if (best == 0xFFFFFFFFu) return 255;          /* an empty mind is maximally surprised */

    /* Scale so that "identical" is 0 and "unrelated" (HD_BITS/2) is 255. Anything past
     * orthogonal is still just 255 -- there is no such thing as more surprising than new. */
    const uint32_t half = HD_BITS / 2u;
    if (best >= half) return 255;
    return (uint8_t)((best * 255u) / half);
}

dream_result_t dream_observe(bench_dream_t *d, const hd_t v, uint16_t label)
{
    d->observed++;

    /* Does a prototype already cover this? Prototypes are checked first because reinforcing a
     * general concept is worth more than storing another near-duplicate instance -- that is the
     * whole difference between a mind that generalises and a log file. */
    if (d->proto->n) {
        uint32_t dist = 0;
        const int32_t slot = hd_mem_best(d->proto, v, &dist);
        if (slot >= 0 && dist <= d->familiar_tol) {
            /* Pull the prototype gently toward this example. Strength falls as support grows:
             * an established concept should not be yanked around by one more instance, but a
             * young one should still be forming. */
            const uint16_t s = d->support[slot];
            uint8_t pull = (uint8_t)(s < 4u ? 64u : (s < 16u ? 24u : 8u));
            hd_mem_reinforce(d->proto, slot, v, pull);
            if (d->support[slot] < 0xFFFFu) d->support[slot]++;
            d->reinforced++;
            return DREAM_REINFORCED;
        }
    }

    /* Failing that, does a stored instance already cover it? */
    if (d->inst->n) {
        uint32_t dist = 0;
        const int32_t slot = hd_mem_best(d->inst, v, &dist);
        if (slot >= 0 && dist <= d->familiar_tol) {
            hd_mem_reinforce(d->inst, slot, v, 48);
            d->reinforced++;
            return DREAM_REINFORCED;
        }
    }

    /* Genuinely new. This is the only path that consumes memory, which is the point of the
     * gate: a machine that stores every observation fills up with the unsurprising. */
    if (hd_mem_add(d->inst, v, label) < 0) return DREAM_FULL;
    d->stored++;
    return DREAM_STORED;
}

uint16_t dream_consolidate(bench_dream_t *d, uint16_t *cursor, uint16_t budget)
{
    if (!d->inst->n) return 0;
    uint16_t formed = 0, looked = 0;

    while (looked < budget && *cursor < d->inst->n) {
        const uint16_t i = (*cursor)++;
        looked++;
        if (d->absorbed[i]) continue;        /* already part of a concept */

        /* Gather every OTHER instance that is really the same thing. */
        hd_acc_clear(d->work);
        hd_acc_add(d->work, d->inst->v[i]);
        uint16_t members = 1;
        uint16_t group[128];
        group[0] = i;

        for (uint16_t j = 0; j < d->inst->n; j++) {
            if (j == i || d->absorbed[j]) continue;
            if (hd_hamming(d->inst->v[i], d->inst->v[j]) <= d->merge_tol) {
                hd_acc_add(d->work, d->inst->v[j]);
                group[members++] = j;
                if (members >= 128u) break;      /* measured bundle ceiling: past ~128 members
                                                  * the parts stop being individually
                                                  * retrievable and the prototype goes vague */
            }
        }

        /* One instance is an anecdote, not a concept. Two could be a coincidence. Three is the
         * smallest number from which the measured prototype actually beat its own instances
         * (+0.030 at k=3, nothing at k=2), so that is where a category is allowed to form. */
        if (members < 3u) continue;

        /* static, not a local: an hd_t is 1 KB and this ran the frame to 1,344 bytes, which is
         * comfortable on a Teensy and not on a 4 KB ESP32 task stack. Consolidation is not
         * reentrant by design -- it is the one thing the machine does alone. */
        static hd_t proto;
        hd_acc_result(d->work, proto);

        /* Does this category already exist? Strengthen rather than duplicate. */
        uint32_t dist = 0;
        const int32_t existing = d->proto->n ? hd_mem_best(d->proto, proto, &dist) : -1;
        if (existing >= 0 && dist <= d->merge_tol) {
            hd_mem_reinforce(d->proto, existing, proto, 96);
            if (d->support[existing] < 0xFFFFu - members)
                d->support[existing] = (uint16_t)(d->support[existing] + members);
        } else {
            const int32_t slot = hd_mem_add(d->proto, proto, d->inst->label[i]);
            if (slot < 0) break;                 /* prototype memory full: stop, keep instances */
            d->support[slot] = members;
            d->merges++;
        }
        /* Every member now belongs to a concept and must not seed another one. */
        for (uint16_t g = 0; g < members; g++) d->absorbed[group[g]] = 1;
        formed++;
    }

    if (*cursor >= d->inst->n) *cursor = 0;      /* wrap: consolidation is never finished, the
                                                  * same way sleep is not a task that completes */
    return formed;
}

void dream_recall(const bench_dream_t *d, const hd_t q, int want_specific, dream_recall_t *out)
{
    out->proto_id      = -1;
    out->proto_dist    = 0xFFFFFFFFu;
    out->proto_support = 0;
    out->inst_id       = -1;
    out->inst_dist     = 0xFFFFFFFFu;
    out->known         = 0;

    const uint32_t recognise = (HD_BITS * 2u) / 5u;   /* 40%: past this is coincidence */

    if (d->proto->n) {
        uint32_t dist = 0;
        const int32_t s = hd_mem_best(d->proto, q, &dist);
        if (s >= 0 && dist <= recognise) {
            out->proto_id      = (int32_t)d->proto->label[s];
            out->proto_dist    = dist;
            out->proto_support = d->support[s];
            out->known         = 1;
        }
    }

    /* The specific instance is only looked for when asked. Knowing WHAT KIND of thing something
     * is answers most questions, costs a search over a handful of prototypes instead of
     * thousands of instances, and is the reason this is fast. */
    if (want_specific && d->inst->n) {
        uint32_t dist = 0;
        const int32_t s = hd_mem_best(d->inst, q, &dist);
        if (s >= 0 && dist <= recognise) {
            out->inst_id   = (int32_t)d->inst->label[s];
            out->inst_dist = dist;
            out->known     = 1;
        }
    }
}
