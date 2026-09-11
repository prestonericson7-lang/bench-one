/* ===========================================================================================
 *  bench_mind.c -- see bench_mind.h
 * ===========================================================================================
 */

#include "bench_mind.h"

static const char *k_lens[MIND_LENS_COUNT] = {
    "recall", "analogy", "part", "context", "contrast", "precedent", "consequence"
};

const char *mind_lens_name(mind_lens_t l)
{
    return (l < MIND_LENS_COUNT) ? k_lens[l] : "?";
}

void mind_init(bench_mind_t *m, uint32_t seed)
{
    m->rng = seed ? seed : 0x4D1AD001u;
    hd_random(m->rel_analogy, &m->rng);
    hd_random(m->rel_context, &m->rng);
    hd_random(m->rel_part,    &m->rng);

    for (int i = 0; i < MIND_LENS_COUNT; i++) {
        m->answer[i]  = -1;
        m->dist[i]    = 0xFFFFFFFFu;
        m->replied[i] = 0;
    }
    /* A lens whose nearest hit is beyond this has no opinion. 40% of the bits: unrelated
     * vectors sit at 50%, and one-shot recall was measured holding to 20% noise, so 40% is
     * comfortably outside real matches and comfortably inside coincidence. */
    m->match_tol = (HD_BITS * 2u) / 5u;
}

void mind_lens_query(const bench_mind_t *m, mind_lens_t lens,
                     const hd_t subject, const hd_t ctx, hd_t out)
{
    switch (lens) {

    case MIND_RECALL:
        hd_copy(out, subject);
        break;

    case MIND_ANALOGY:
        /* Unbind the analogy relation. In a vector-symbolic memory this is the operation that
         * answers "what is to A as B is to C" -- because bind is its own inverse, the relation
         * cancels and what is left is the counterpart. No other memory structure here can do
         * this at all; it is the single strongest argument for the representation. */
        hd_bind(out, subject, m->rel_analogy);
        break;

    case MIND_PART:
        /* Subject with the context's contribution removed: what is left once the parts already
         * accounted for are cancelled. This is how a residual becomes visible instead of being
         * averaged into the whole. */
        if (ctx) hd_bind(out, subject, ctx);
        else     hd_bind(out, subject, m->rel_part);
        break;

    case MIND_CONTEXT:
        if (ctx) {
            hd_t t;
            hd_bind(t, subject, m->rel_context);
            hd_bind(out, t, ctx);
        } else {
            hd_bind(out, subject, m->rel_context);
        }
        break;

    case MIND_CONTRAST:
        /* The complement. A search for what this is NOT is a genuinely independent probe --
         * it fails in different circumstances than a similarity search, which is exactly the
         * property that makes agreement between the two meaningful. */
        for (uint32_t i = 0; i < HD_WORDS; i++) out[i] = ~subject[i];
        break;

    case MIND_PRECEDENT:
        /* Rotate backward: "what came before things like this". */
        hd_permute(out, subject, -1);
        break;

    case MIND_CONSEQUENCE:
        /* Rotate forward: "what followed things like this". */
        hd_permute(out, subject, 1);
        break;

    default:
        hd_copy(out, subject);
        break;
    }
}

void mind_observe(bench_mind_t *m, mind_lens_t lens, int32_t answer, uint32_t dist)
{
    if (lens >= MIND_LENS_COUNT) return;
    m->replied[lens] = 1;
    /* A hit past the tolerance is recorded as "this lens looked and found nothing", which is
     * information. Recording it as a weak vote would let seven lenses that each found garbage
     * outvote one that found the truth. */
    if (answer >= 0 && dist <= m->match_tol) {
        m->answer[lens] = answer;
        m->dist[lens]   = dist;
    } else {
        m->answer[lens] = -1;
        m->dist[lens]   = dist;
    }
}

void mind_conclude(const bench_mind_t *m, mind_conclusion_t *out)
{
    out->conclusion    = -1;
    out->consensus     = 0;
    out->voted         = 0;
    out->confident     = 0;
    out->dissent       = -1;
    out->dissent_votes = 0;
    out->best_dist     = 0xFFFFFFFFu;

    /* Tally. MIND_LENS_COUNT is 7, so an O(n^2) count over distinct answers is 49 comparisons
     * and needs no allocation -- worth far more than cleverness at this size. */
    int32_t  top = -1, second = -1;
    uint8_t  topv = 0, secv = 0;
    uint32_t topd = 0xFFFFFFFFu;

    for (int i = 0; i < MIND_LENS_COUNT; i++) {
        if (!m->replied[i]) continue;
        out->voted++;
        if (m->answer[i] < 0) continue;

        uint8_t  v = 0;
        uint32_t best = 0xFFFFFFFFu;
        for (int j = 0; j < MIND_LENS_COUNT; j++) {
            if (m->replied[j] && m->answer[j] == m->answer[i]) {
                v++;
                if (m->dist[j] < best) best = m->dist[j];
            }
        }
        if (v > topv || (v == topv && best < topd)) {
            if (top != m->answer[i]) { second = top; secv = topv; }
            top = m->answer[i]; topv = v; topd = best;
        } else if (m->answer[i] != top && v > secv) {
            second = m->answer[i]; secv = v;
        }
    }

    out->conclusion    = top;
    out->consensus     = topv;
    out->dissent       = second;
    out->dissent_votes = secv;
    out->best_dist     = topd;

    /* Act only on a real majority of the lenses that ACTUALLY REPLIED, and never on a single
     * voice. One lens agreeing with itself is not corroboration, and on a cluster where nodes
     * drop out it is the most likely thing to happen. */
    out->confident = (topv >= 2u && out->voted && (uint32_t)topv * 2u > (uint32_t)out->voted)
                     ? 1u : 0u;
}

/* Tiny local formatter -- no stdio, so this links on a bare-metal Teensy and a uClibc box
 * alike. Appends `s`, returns the new position. */
static int put(char *b, int cap, int p, const char *s)
{
    while (*s && p < cap - 1) b[p++] = *s++;
    return p;
}
static int putint(char *b, int cap, int p, int32_t v)
{
    char t[12];
    int n = 0;
    if (v < 0) { if (p < cap - 1) b[p++] = '-'; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < 11) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && p < cap - 1) b[p++] = t[--n];
    return p;
}

int mind_explain(const bench_mind_t *m, const mind_conclusion_t *c, char *buf, int cap)
{
    if (cap < 2) return 0;
    int p = 0;

    if (c->conclusion < 0 || !c->voted) {
        p = put(buf, cap, p, "no lens recognised this");
        buf[p] = 0;
        return p;
    }
    p = put(buf, cap, p, c->confident ? "concept " : "UNSURE, best guess ");
    p = putint(buf, cap, p, c->conclusion);
    p = put(buf, cap, p, " (");
    p = putint(buf, cap, p, c->consensus);
    p = put(buf, cap, p, "/");
    p = putint(buf, cap, p, c->voted);
    p = put(buf, cap, p, " lenses agree: ");
    for (int i = 0, first = 1; i < MIND_LENS_COUNT; i++) {
        if (!m->replied[i] || m->answer[i] != c->conclusion) continue;
        if (!first) p = put(buf, cap, p, ",");
        p = put(buf, cap, p, mind_lens_name((mind_lens_t)i));
        first = 0;
    }
    p = put(buf, cap, p, ")");

    if (c->dissent >= 0) {
        p = put(buf, cap, p, "; dissent: ");
        p = putint(buf, cap, p, c->dissent);
        p = put(buf, cap, p, " x");
        p = putint(buf, cap, p, c->dissent_votes);
    }
    buf[p] = 0;
    return p;
}
