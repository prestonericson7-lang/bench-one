/* ===========================================================================================
 *  bench_wonder.c -- see bench_wonder.h
 * ===========================================================================================
 */

#include "bench_wonder.h"

/* Unrelated vectors sit at HD_BITS/2 with sigma = sqrt(HD_BITS)/2. Six sigma below that is the
 * line between "this means something" and "this is noise", and it is the same threshold the rest
 * of the system uses to decide whether a match is real. */
#define NOISE_FLOOR (HD_BITS / 2)
#define SIX_SIGMA   ((6u * 90u) / 2u)          /* sqrt(8192) is ~90.5 */
#define MEANS_SOMETHING (NOISE_FLOOR - SIX_SIGMA)

/* -------------------------------------------------------------------------------------------
 * SUPERPOSITION
 * ----------------------------------------------------------------------------------------- */

void hd_super_clear(hd_super_t *s)
{
    s->n = 0;
    s->settled = 0;
    hd_zero(s->v);
}

int hd_super_add(hd_super_t *s, uint16_t slot)
{
    if (s->n >= HD_SUPER_MAX) return 0;
    s->member[s->n++] = slot;
    s->settled = 0;
    return 1;
}

void hd_super_settle(hd_super_t *s, const hd_mem_t *m, hd_acc_t *scratch)
{
    hd_acc_clear(scratch);
    for (uint8_t i = 0; i < s->n; i++) {
        if (s->member[i] < m->n) hd_acc_add(scratch, m->v[s->member[i]]);
    }
    hd_acc_result(scratch, s->v);
    s->settled = 1;
}

uint32_t hd_super_dist(const hd_super_t *s, const hd_t v)
{
    return hd_hamming(s->v, v);
}

uint32_t hd_super_far(const hd_super_t *s)
{
    (void)s;
    return MEANS_SOMETHING;
}

int32_t hd_super_collapse(const hd_super_t *s, const hd_mem_t *m, const hd_t probe,
                          uint32_t *dist_out)
{
    /* The collapse is a scan over the members, and that is not a shortcut -- it is what collapse
     * means here. The state holds every candidate at once at no cost; evidence arrives; the member
     * the evidence agrees with is the one that survives. Nothing had to be decided until the probe
     * showed up, which is the entire reason for holding the superposition rather than guessing
     * early and being right sixty percent of the time. */
    int32_t  best = -1;
    uint32_t bd = 0xFFFFFFFFu;

    for (uint8_t i = 0; i < s->n; i++) {
        const uint16_t slot = s->member[i];
        if (slot >= m->n) continue;
        const uint32_t d = hd_hamming(m->v[slot], probe);
        /* Strictly less-than, lowest slot wins a tie -- the same rule as every other comparison in
         * this system. A collapse that broke ties differently would make the answer depend on the
         * order candidates were added, and the order is whatever the network delivered. */
        if (d < bd) { bd = d; best = (int32_t)slot; }
    }
    if (dist_out) *dist_out = bd;
    return best;
}

/* -------------------------------------------------------------------------------------------
 * QUESTIONS
 * ----------------------------------------------------------------------------------------- */

static const char *k_kind[] = { "CONFUSION", "ORPHAN", "SELF" };

const char *wonder_kind_name(wonder_kind_t k)
{
    return (k <= WQ_SELF) ? k_kind[k] : "?";
}

void wonder_init(wonder_t *w, uint32_t seed)
{
    w->n_open = 0;
    w->ticks = 0;
    w->formed = 0;
    w->closed = 0;
    w->displaced = 0;
    for (uint8_t i = 0; i < WONDER_MAX_OPEN; i++) w->open[i].live = 0;

    uint32_t rng = seed ? seed : 1u;
    hd_random(w->self, &rng);
    w->have_self = 1;
}

void wonder_self_fact(const wonder_t *w, const hd_t role, const hd_t value, hd_t out)
{
    /* A fact about the machine is an ordinary role-filler binding, permuted by one step so it sits
     * in its own region. The permutation is what stops "I have ten nodes" from being confusable
     * with "that thing out there has ten nodes" while keeping both answerable by the same scan. */
    hd_t rv, tmp;
    hd_bind(rv, role, value);
    hd_bind(tmp, rv, w->self);
    hd_permute(out, tmp, 1);              /* NOT in place: hd_permute cannot alias */
}

/* Insert a question, displacing the least urgent if full. Urgency is `tension`: LOWER is more
 * urgent, because a confusion between two nearly identical memories is closer to producing a wrong
 * answer than one between two barely similar ones. */
static int wonder_push(wonder_t *w, const hd_t q, wonder_kind_t kind,
                       uint16_t a, uint16_t b, uint32_t tension)
{
    /* Already asking this? Count the repeat rather than filling the list with duplicates. Coming
     * back to the same question is information: it means experience has not resolved it. */
    for (uint8_t i = 0; i < WONDER_MAX_OPEN; i++) {
        if (w->open[i].live && w->open[i].kind == kind &&
            w->open[i].a == a && w->open[i].b == b) {
            w->open[i].asked++;
            return 0;
        }
    }

    int8_t slot = -1;
    for (uint8_t i = 0; i < WONDER_MAX_OPEN; i++) {
        if (!w->open[i].live) { slot = (int8_t)i; break; }
    }
    if (slot < 0) {
        /* Full. Find the least urgent and take its place, but only if this one beats it -- a
         * machine that discards its hardest question every time a mild one turns up never finishes
         * thinking about anything. */
        uint8_t worst = 0;
        for (uint8_t i = 1; i < WONDER_MAX_OPEN; i++)
            if (w->open[i].tension > w->open[worst].tension) worst = i;
        if (w->open[worst].tension <= tension) return 0;
        slot = (int8_t)worst;
        w->displaced++;
    }

    hd_copy(w->open[slot].q, q);
    w->open[slot].kind = kind;
    w->open[slot].a = a;
    w->open[slot].b = b;
    w->open[slot].tension = tension;
    w->open[slot].asked = 1;
    w->open[slot].live = 1;
    if (w->n_open < WONDER_MAX_OPEN) w->n_open++;
    w->formed++;
    return 1;
}

/* Find the closest pair of memories, examining at most `budget` of them. Two things it stores that
 * it cannot tell apart are a wrong answer waiting for the right query. */
static int wonder_confusion(wonder_t *w, const hd_mem_t *m, uint16_t budget)
{
    if (m->n < 2) return 0;

    uint32_t bd = 0xFFFFFFFFu;
    uint16_t ba = 0, bb = 0;
    const uint16_t scan = (budget < m->n) ? budget : m->n;

    for (uint16_t i = 0; i < scan; i++) {
        for (uint16_t j = (uint16_t)(i + 1); j < m->n; j++) {
            const uint32_t d = hd_hamming(m->v[i], m->v[j]);
            if (d < bd) { bd = d; ba = i; bb = j; }
        }
    }
    /* Only a confusion if they are genuinely close. Two unrelated memories sitting at the noise
     * floor are not a problem, they are the normal case. */
    if (bd >= MEANS_SOMETHING) return 0;

    hd_t q;
    hd_bind(q, m->v[ba], m->v[bb]);       /* the difference IS the question */
    return wonder_push(w, q, WQ_CONFUSION, ba, bb, bd);
}

/* Find the memory that relates to nothing else. Everything else has neighbours; this one does not,
 * which means it was recorded and never integrated. */
static int wonder_orphan(wonder_t *w, const hd_mem_t *m, uint16_t budget)
{
    if (m->n < 3) return 0;

    uint32_t loneliest = 0;
    uint16_t who = 0;
    int found = 0;
    const uint16_t scan = (budget < m->n) ? budget : m->n;

    for (uint16_t i = 0; i < scan; i++) {
        uint32_t nearest = 0xFFFFFFFFu;
        for (uint16_t j = 0; j < m->n; j++) {
            if (j == i) continue;
            const uint32_t d = hd_hamming(m->v[i], m->v[j]);
            if (d < nearest) nearest = d;
        }
        if (nearest > loneliest) { loneliest = nearest; who = i; found = 1; }
    }
    /* Isolated only if even its NEAREST neighbour is out at the noise floor. */
    if (!found || loneliest < MEANS_SOMETHING) return 0;

    /* The question is the thing itself: "what is this like". Having no answer is what makes it
     * worth asking, and tension is inverted here because a lonelier memory is a more urgent
     * question, while for a confusion a closer pair is. */
    const uint32_t tension = (loneliest > NOISE_FLOOR) ? 0u : (NOISE_FLOOR - loneliest);
    return wonder_push(w, m->v[who], WQ_ORPHAN, who, who, tension);
}

int wonder_tick(wonder_t *w, const hd_mem_t *m, uint16_t budget)
{
    w->ticks++;
    /* Alternate, so neither kind starves the other. A machine that only ever chased its worst
     * confusion would never notice the memory sitting on its own in the corner. */
    if (w->ticks & 1u) {
        if (wonder_confusion(w, m, budget)) return 1;
        return wonder_orphan(w, m, budget);
    }
    if (wonder_orphan(w, m, budget)) return 1;
    return wonder_confusion(w, m, budget);
}

uint8_t wonder_offer(wonder_t *w, const hd_mem_t *m, const hd_t experience, uint32_t recognise)
{
    uint8_t answered = 0;

    for (uint8_t i = 0; i < WONDER_MAX_OPEN; i++) {
        wonder_q_t *q = &w->open[i];
        if (!q->live) continue;

        if (q->kind == WQ_CONFUSION) {
            /* Answered when the experience TELLS THE PAIR APART: near one and not the other. An
             * experience that is close to both, or far from both, says nothing about what
             * distinguishes them and leaves the question open. */
            if (q->a >= m->n || q->b >= m->n) continue;
            const uint32_t da = hd_hamming(experience, m->v[q->a]);
            const uint32_t db = hd_hamming(experience, m->v[q->b]);
            const uint32_t gap = (da > db) ? (da - db) : (db - da);
            if (gap > SIX_SIGMA && (da < recognise || db < recognise)) {
                q->live = 0;
                if (w->n_open) w->n_open--;
                w->closed++;
                answered++;
            }
        } else if (q->kind == WQ_ORPHAN) {
            /* Answered when something arrives that the orphan is actually like. It is no longer
             * alone, so it can be integrated with the rest. */
            if (q->a >= m->n) continue;
            if (hd_hamming(experience, m->v[q->a]) < recognise) {
                q->live = 0;
                if (w->n_open) w->n_open--;
                w->closed++;
                answered++;
            }
        } else {
            /* SELF: answered when the experience matches the stored self-fact the question was
             * raised about. */
            if (hd_hamming(experience, q->q) < recognise) {
                q->live = 0;
                if (w->n_open) w->n_open--;
                w->closed++;
                answered++;
            }
        }
    }
    return answered;
}

const wonder_q_t *wonder_top(const wonder_t *w)
{
    const wonder_q_t *best = 0;
    for (uint8_t i = 0; i < WONDER_MAX_OPEN; i++) {
        if (!w->open[i].live) continue;
        if (!best || w->open[i].tension < best->tension) best = &w->open[i];
    }
    return best;
}
