/* ===========================================================================================
 *  bench_index.c -- see bench_index.h
 * ===========================================================================================
 */

#include "bench_index.h"

static const char *k_status[] = { "OK", "EMPTY", "TOO_NOISY", "NO_BODY" };

const char *hd_index_status_name(hd_index_status_t s)
{
    return (s <= HD_INDEX_NO_BODY) ? k_status[s] : "?";
}

void hd_index_init(hd_index_t *ix, hd_entry_t *storage, uint16_t *labels, uint32_t cap,
                   hd_body_fetch_fn fetch, void *ctx)
{
    ix->e     = storage;
    ix->label = labels;
    ix->n     = 0;
    ix->cap   = cap;
    ix->fetch = fetch;
    ix->ctx   = ctx;
    ix->max_survivors   = 64;
    ix->queries         = 0;
    ix->refused         = 0;
    ix->bodies_read     = 0;
    ix->survivors_total = 0;
}

void hd_index_set_prefix(hd_entry_t *e, const hd_t v)
{
    for (uint32_t i = 0; i < HD_INDEX_WORDS; i++) e->prefix[i] = v[i];
}

int32_t hd_index_add(hd_index_t *ix, const hd_t v, uint16_t label, uint64_t body_offset)
{
    if (ix->n >= ix->cap) return -1;
    hd_index_set_prefix(&ix->e[ix->n], v);
    ix->e[ix->n].body = body_offset;
    if (ix->label) ix->label[ix->n] = label;
    return (int32_t)(ix->n++);
}

uint32_t hd_index_prefix_dist(const hd_entry_t *e, const hd_t q)
{
    uint32_t d = 0;
#if defined(HD_HAVE_USAD8)
    uint32_t i = 0;
    for (; i + 4u <= HD_INDEX_WORDS; i += 4u) {
        uint32_t s = hd_pc_nib(e->prefix[i]     ^ q[i]);
        s         += hd_pc_nib(e->prefix[i + 1] ^ q[i + 1]);
        s         += hd_pc_nib(e->prefix[i + 2] ^ q[i + 2]);
        s         += hd_pc_nib(e->prefix[i + 3] ^ q[i + 3]);
        d += hd_usad8(s, 0u);
    }
    for (; i < HD_INDEX_WORDS; i++) d += hd_popcount(e->prefix[i] ^ q[i]);
#else
    for (uint32_t i = 0; i < HD_INDEX_WORDS; i++) d += hd_popcount(e->prefix[i] ^ q[i]);
#endif
    return d;
}

void hd_index_search(hd_index_t *ix, const hd_t q, hd_index_result_t *out)
{
    out->status      = HD_INDEX_EMPTY;
    out->slot        = -1;
    out->label       = 0;
    out->dist        = 0xFFFFFFFFu;
    out->survivors   = 0;
    out->bodies_read = 0;

    if (!ix->n) return;
    ix->queries++;

    /* PASS 1 -- read only the index, find the best prefix. This is the whole query cost when
     * the answer turns out to be unresolvable, which is exactly when you want it to be cheap. */
    uint32_t pmin = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < ix->n; i++) {
        const uint32_t p = hd_index_prefix_dist(&ix->e[i], q);
        if (p < pmin) pmin = p;
    }

    /* 6 sigma over the prefix; sigma of a Hamming distance across P bits is sqrt(P)/2.
     * Integer sqrt, computed once -- the loops below must not contain a divide. */
    uint32_t root = 1;
    while (root * root < (uint32_t)HD_INDEX_BITS) root++;
    const uint32_t gate = pmin + (6u * root) / 2u;

    /* PASS 2 -- count survivors BEFORE touching storage. Counting first is the point of the
     * valve: a query that would need 125,411 card reads is refused having read nothing. */
    uint32_t surv = 0;
    for (uint32_t i = 0; i < ix->n; i++)
        if (hd_index_prefix_dist(&ix->e[i], q) <= gate) surv++;

    out->survivors = surv;
    ix->survivors_total += surv;

    if (surv > ix->max_survivors) {
        /* The index cannot separate this query from the crowd. Say so. Fetching thousands of
         * bodies to pick among them would be slow AND the answer would be a coin flip, which
         * is the worst of both. */
        ix->refused++;
        out->status = HD_INDEX_TOO_NOISY;
        return;
    }
    if (!ix->fetch) { out->status = HD_INDEX_NO_BODY; return; }

    /* PASS 3 -- pull the survivors' bodies from local storage and compare in full. */
    static hd_t body;                       /* 1 KB: never a stack local */
    uint32_t bd = 0xFFFFFFFFu;
    int32_t  best = -1;

    for (uint32_t i = 0; i < ix->n; i++) {
        if (hd_index_prefix_dist(&ix->e[i], q) > gate) continue;
        if (!ix->fetch(ix->ctx, ix->e[i].body, body)) continue;   /* a bad record is skipped,
                                                                   * never fatal */
        out->bodies_read++;
        ix->bodies_read++;

        const uint32_t d = hd_hamming_limit(body, q, bd);
        if (d < bd) { bd = d; best = (int32_t)i; }
    }

    if (best < 0) { out->status = HD_INDEX_NO_BODY; return; }

    out->status = HD_INDEX_OK;
    out->slot   = best;
    out->dist   = bd;
    out->label  = ix->label ? ix->label[best] : 0u;
}
