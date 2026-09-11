/* ===========================================================================================
 *  bench_resolve.c -- see bench_resolve.h
 * ===========================================================================================
 */

#include "bench_resolve.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

static const char *k_status[] = {
    "CONFIDENT", "RECOVERED", "SATURATED", "UNKNOWN"
};

const char *resolve_status_name(resolve_status_t s)
{
    return (s <= RESOLVE_UNKNOWN) ? k_status[s] : "?";
}

/* Two nearest concepts, with their distances. The runner-up is not a curiosity: the gap
 * between first and second IS the confidence, and the identity of the second is what any
 * question would have to be about. */
static void top_two(const hd_mem_t *m, const hd_t q,
                    int32_t *i0, uint32_t *d0, int32_t *i1, uint32_t *d1)
{
    *i0 = *i1 = -1;
    *d0 = *d1 = 0xFFFFFFFFu;
    for (uint16_t i = 0; i < m->n; i++) {
        const uint32_t d = hd_hamming(m->v[i], q);
        if (d < *d0)      { *d1 = *d0; *i1 = *i0; *d0 = d; *i0 = (int32_t)i; }
        else if (d < *d1) { *d1 = d;   *i1 = (int32_t)i; }
    }
}

static uint32_t margin_q8(uint32_t d0, uint32_t d1)
{
    if (d1 == 0xFFFFFFFFu || d1 <= d0) return 0;
    /* sigma of a Hamming distance between unrelated vectors over W bits is sqrt(W)/2. */
    uint32_t root = 1;
    while (root * root < (uint32_t)HD_BITS) root++;
    return (uint32_t)(((uint64_t)(d1 - d0) * 2u * 256u) / (root ? root : 1u));
}

void resolve_query(const bench_dream_t *d, const hd_t query,
                   resolve_fetch_fn fetch, void *fetch_ctx,
                   uint32_t confident_q8, hd_acc_t *work,
                   hd_t *ep_buf, uint16_t ep_cap, resolve_t *out)
{
    out->status    = RESOLVE_UNKNOWN;
    out->answer    = -1;
    out->rival     = -1;
    out->dist      = 0xFFFFFFFFu;
    out->margin_q8 = 0;
    out->rounds    = 0;
    out->reads     = 0;

    if (!d->proto->n) return;

    /* ---- FAST PATH. RAM only. This is the path almost every query takes and it must not be
     * allowed to become slower to make the rare path faster. ---- */
    int32_t  i0, i1;
    uint32_t d0, d1;
    top_two(d->proto, query, &i0, &d0, &i1, &d1);

    const uint32_t recognise = (HD_BITS * 2u) / 5u;
    if (i0 < 0 || d0 > recognise) return;                 /* genuinely never seen */

    out->answer    = (int32_t)d->proto->label[i0];
    out->dist      = d0;
    out->margin_q8 = margin_q8(d0, d1);
    if (i1 >= 0) out->rival = (int32_t)d->proto->label[i1];

    if (out->margin_q8 >= confident_q8) {
        out->status = RESOLVE_CONFIDENT;
        return;                                            /* storage never touched */
    }

    /* No archive attached. Reporting SATURATED immediately is correct: the machine has done
     * everything it can and the remaining ambiguity is real. It is not a degraded mode. */
    if (!fetch) {
        out->status = RESOLVE_SATURATED;
        goto build_question;
    }

    /* ---- SLOW PATH. Go and look, but only at evidence bearing on THIS ambiguity. ---- */
    if (!ep_buf || !ep_cap) { out->status = RESOLVE_SATURATED; goto build_question; }
    {
        /* static, not a local: see the header. Consolidation and resolution are the two places
         * a full-width vector is needed as working space, and neither is reentrant. */
        static hd_t enriched;
        hd_copy(enriched, query);
        if (ep_cap > 8u) ep_cap = 8u;

        int32_t last_answer = out->answer;
        uint16_t stable = 0;

        for (uint16_t round = 0; round < RESOLVE_MAX_ROUNDS; round++) {
            out->rounds++;

            /* Fetch episodes of the two candidates that are actually in contention. Fetching
             * evidence "in general" would be a scan, and a scan of an archive this size is
             * hours -- the whole design rests on only ever fetching what discriminates. */
            uint16_t got = 0;
            uint32_t best = 0xFFFFFFFFu;
            int32_t  best_concept = -1;

            const int32_t cands[2] = { i0, i1 };
            for (int c = 0; c < 2; c++) {
                if (cands[c] < 0) continue;
                const uint16_t n = fetch(fetch_ctx, (uint16_t)cands[c], ep_buf, ep_cap);
                out->reads = (uint16_t)(out->reads + n);
                got = (uint16_t)(got + n);
                for (uint16_t k = 0; k < n; k++) {
                    const uint32_t h = hd_hamming(ep_buf[k], enriched);
                    if (h < best) { best = h; best_concept = (int32_t)d->proto->label[cands[c]]; }
                }
            }
            if (!got) break;                               /* nothing recorded; stop */

            if (best_concept >= 0) {
                out->answer = best_concept;
                out->dist   = best;
            }

            /* SATURATION: the answer stopped moving. Two rounds unchanged means further
             * evidence is not going to settle it, and continuing is pure cost. This is the
             * condition that separates "I have not looked hard enough" from "this genuinely
             * cannot be known from what I have lived through". */
            if (out->answer == last_answer) {
                if (++stable >= 2u) break;
            } else {
                stable = 0;
                last_answer = out->answer;
            }

            /* Re-deliberate: the retrieved evidence becomes part of the query, weighted so it
             * informs the next round without drowning what was actually asked. */
            hd_acc_clear(work);
            hd_acc_addw(work, query, 2);
            for (uint16_t k = 0; k < got && k < ep_cap; k++) hd_acc_addw(work, ep_buf[k], 1);
            hd_acc_result(work, enriched);

            top_two(d->proto, enriched, &i0, &d0, &i1, &d1);
            out->margin_q8 = margin_q8(d0, d1);
            if (out->margin_q8 >= confident_q8) {
                out->status = RESOLVE_RECOVERED;
                return;                                    /* settled it without asking */
            }
        }
    }

    out->status = RESOLVE_SATURATED;

build_question:
    /* The residue. Two candidates survived everything the archive could say, so they are alike
     * everywhere the evidence reaches and their DIFFERENCE is precisely what is missing.
     * That vector is the question, and it is worth asking because it could not have been
     * worked out -- the search saturated before it was generated. */
    if (out->rival >= 0 && i1 >= 0 && i0 >= 0) {
        for (uint32_t w = 0; w < HD_WORDS; w++)
            out->question[w] = d->proto->v[i0][w] ^ d->proto->v[i1][w];
    } else {
        hd_zero(out->question);
    }
}

int32_t resolve_name_question(const bench_dream_t *d, const resolve_t *r, uint32_t *dist_out)
{
    if (r->status != RESOLVE_SATURATED || !d->proto->n) return -1;

    uint32_t dist = 0;
    const int32_t slot = hd_mem_best(d->proto, r->question, &dist);
    if (dist_out) *dist_out = dist;

    /* If the difference resembles nothing the machine has a name for, say so rather than
     * returning the nearest thing. "I cannot describe what I am missing" is a different and
     * more honest report than a confidently mislabelled question. */
    if (slot < 0 || dist > (HD_BITS * 2u) / 5u) return -1;
    return (int32_t)d->proto->label[slot];
}
