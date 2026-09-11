/* ===========================================================================================
 *  bench_hdc_shard.c -- degrade, never fail.
 * ===========================================================================================
 *
 *  THE INVARIANT THIS FILE ENFORCES
 *  ---------------------------------
 *      A node that is slow makes the answer LATER.
 *      A node that is stalled makes the answer LESS COMPLETE.
 *      A node that is dead makes the answer LESS COMPLETE.
 *      No node can make the answer WRONG.
 *
 *  Every function here is written to hold that. Where a shortcut would have broken it, the
 *  longer version is used and the reason is in the comment.
 * ===========================================================================================
 */

#include "bench_hdc_shard.h"

#define HD_NO_SLOT  0xFFFFu
#define HD_FAR      0xFFFFFFFFu


/* -------------------------------------------------------------------------------------------
 * SCAN
 * ----------------------------------------------------------------------------------------- */

void hd_scan_begin(hd_scan_t *s, const hd_mem_t *mem, const hd_t query,
                   uint16_t node, uint16_t query_id, uint16_t base)
{
    s->mem    = mem;
    s->cursor = 0;
    hd_copy(s->query, query);          /* own the query: the caller may reuse its buffer, and
                                        * on an S3 that buffer is often a DMA'd radio frame  */
    s->result.node       = node;
    s->result.query_id   = query_id;
    s->result.base       = base;
    s->result.best_local = HD_NO_SLOT;
    s->result.best_dist  = HD_FAR;
    s->result.scanned    = 0;
    s->result.total      = mem ? mem->n : 0;
}

int hd_scan_chunk(hd_scan_t *s, uint16_t max_items)
{
    if (!s->mem || s->cursor >= s->mem->n) return 1;

    uint16_t done = 0;
    while (s->cursor < s->mem->n && done < max_items) {
        const uint32_t d = hd_hamming(s->mem->v[s->cursor], s->query);

        /* Strict less-than, plus a lowest-index tie-break. The tie-break is not cosmetic:
         * without it two shards holding equally-distant vectors would produce a winner that
         * depends on which reply arrived first, and the merge below would stop being
         * commutative. Then the same query could return different answers on different runs
         * and nothing in the system would look broken. */
        if (d < s->result.best_dist ||
            (d == s->result.best_dist && s->cursor < s->result.best_local)) {
            s->result.best_dist  = d;
            s->result.best_local = s->cursor;
        }
        s->cursor++;
        s->result.scanned++;
        done++;
    }
    return (s->cursor >= s->mem->n) ? 1 : 0;
}

uint8_t hd_scan_coverage(const hd_scan_t *s)
{
    if (!s->result.total) return 255;
    return (uint8_t)(((uint32_t)s->result.scanned * 255u) / s->result.total);
}


/* -------------------------------------------------------------------------------------------
 * MERGE -- the monoid
 * ----------------------------------------------------------------------------------------- */

void hd_partial_none(hd_partial_t *dst, uint16_t query_id)
{
    dst->node       = 0xFFFFu;
    dst->query_id   = query_id;
    dst->base       = 0;
    dst->best_local = HD_NO_SLOT;
    dst->best_dist  = HD_FAR;
    dst->scanned    = 0;
    dst->total      = 0;
}

void hd_partial_merge(hd_partial_t *dst, const hd_partial_t *src)
{
    /* A reply to a question we are no longer asking is the exact hazard this file exists for:
     * an S3 stalled by its radio can deliver an answer two queries late. Dropping it by id is
     * what turns that from a wrong answer into a missing one. */
    if (src->query_id != dst->query_id) return;

    /* Coverage always accumulates, even from a node that found no candidate -- knowing that a
     * region was searched and contained nothing is real information. */
    dst->scanned = (uint16_t)(dst->scanned + src->scanned);
    dst->total   = (uint16_t)(dst->total   + src->total);

    if (src->best_local == HD_NO_SLOT) return;

    const uint32_t sg = (uint32_t)src->base + src->best_local;
    const uint32_t dg = (uint32_t)dst->base + dst->best_local;

    /* Order-independent by construction: strictly better wins, and an exact tie is broken by
     * the lower GLOBAL index. So merging {a,b,c} in any order, with any repeats, gives the
     * same result -- which is what makes a duplicated packet harmless instead of a bug that
     * shows up once a week. */
    if (dst->best_local == HD_NO_SLOT ||
        src->best_dist < dst->best_dist ||
        (src->best_dist == dst->best_dist && sg < dg)) {
        dst->base       = src->base;
        dst->best_local = src->best_local;
        dst->best_dist  = src->best_dist;
    }
}

void hd_merge_begin(hd_merge_t *m, uint16_t query_id)
{
    hd_partial_none(&m->best, query_id);
    m->seen  = 0;
    m->dupes = 0;
    for (int i = 0; i < HD_MAX_NODES; i++) { m->node_scanned[i] = 0; m->node_total[i] = 0; }
}

int hd_merge_add(hd_merge_t *m, const hd_partial_t *src)
{
    if (src->query_id != m->best.query_id) { m->dupes++; return 0; }

    /* A node id we cannot track is refused rather than counted. An untracked node would be
     * double-counted on every retry, and coverage is what decides whether "not found" means
     * "new" or "nobody looked". */
    if (src->node >= HD_MAX_NODES) return 0;

    const uint16_t n = src->node;
    int improved = 0;

    /* Coverage: keep the best report from this node and adjust the running total by the
     * DIFFERENCE. Adding raw would double-count a retry; keeping the first would make retries
     * pointless. */
    if (src->scanned > m->node_scanned[n]) {
        m->best.scanned = (uint16_t)(m->best.scanned + (src->scanned - m->node_scanned[n]));
        m->node_scanned[n] = src->scanned;
        improved = 1;
    } else if (m->seen & ((uint64_t)1 << n)) {
        m->dupes++;
    }
    if (src->total > m->node_total[n]) {
        m->best.total = (uint16_t)(m->best.total + (src->total - m->node_total[n]));
        m->node_total[n] = src->total;
    }
    m->seen |= ((uint64_t)1 << n);

    /* The winner is a plain min with a global-index tie-break, which is idempotent on its own,
     * so it is safe to fold every reply through it regardless of coverage. */
    if (src->best_local != 0xFFFFu) {
        const uint32_t sg = (uint32_t)src->base + src->best_local;
        const uint32_t dg = (uint32_t)m->best.base + m->best.best_local;
        if (m->best.best_local == 0xFFFFu ||
            src->best_dist < m->best.best_dist ||
            (src->best_dist == m->best.best_dist && sg < dg)) {
            m->best.base       = src->base;
            m->best.best_local = src->best_local;
            m->best.best_dist  = src->best_dist;
            improved = 1;
        }
    }
    return improved;
}

int hd_merge_node_incomplete(const hd_merge_t *m, uint16_t node)
{
    if (node >= HD_MAX_NODES) return 0;
    if (!(m->seen & ((uint64_t)1 << node))) return 1;          /* never answered */
    return (m->node_scanned[node] < m->node_total[node]) ? 1 : 0;
}

uint8_t hd_merge_coverage(const hd_merge_t *m, uint32_t total_memory)
{
    return hd_partial_coverage(&m->best, total_memory);
}

int32_t hd_partial_slot(const hd_partial_t *p)
{
    if (p->best_local == HD_NO_SLOT) return -1;
    return (int32_t)p->base + (int32_t)p->best_local;
}

uint8_t hd_partial_coverage(const hd_partial_t *merged, uint32_t total_memory)
{
    if (!total_memory) return 255;
    uint32_t c = ((uint32_t)merged->scanned * 255u) / total_memory;
    return (uint8_t)(c > 255u ? 255u : c);
}


/* -------------------------------------------------------------------------------------------
 * DISTRIBUTED BUNDLING
 * ----------------------------------------------------------------------------------------- */

void hd_acc_merge(hd_acc_t *dst, const hd_acc_t *src)
{
    for (uint32_t i = 0; i < HD_BITS; i++) {
        int32_t v = (int32_t)dst->c[i] + (int32_t)src->c[i];
        if (v >  32000) v =  32000;
        if (v < -32000) v = -32000;
        dst->c[i] = (int16_t)v;
    }
    dst->n += src->n;
}

int hd_bundle_chunk(hd_acc_t *acc, const hd_mem_t *mem,
                    uint16_t first, uint16_t last, uint16_t *cursor, uint16_t max_items)
{
    if (!mem) return 1;
    if (last > mem->n) last = mem->n;
    if (*cursor < first) *cursor = first;

    uint16_t done = 0;
    while (*cursor < last && done < max_items) {
        hd_acc_add(acc, mem->v[*cursor]);
        (*cursor)++;
        done++;
    }
    return (*cursor >= last) ? 1 : 0;
}


/* -------------------------------------------------------------------------------------------
 * WIRE FORMAT -- explicit little-endian, no struct packing, no endianness assumption
 * -----------------------------------------------------------------------------------------
 * Written byte by byte on purpose. A packed struct memcpy'd onto the wire works right up until
 * one node is compiled with different alignment rules, and then it fails as garbage distances
 * rather than as a build error.
 * ----------------------------------------------------------------------------------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void hd_partial_pack(const hd_partial_t *p, uint8_t out[HD_PARTIAL_WIRE_BYTES])
{
    put16(out + 0,  p->node);
    put16(out + 2,  p->query_id);
    put16(out + 4,  p->base);
    put16(out + 6,  p->best_local);
    put32(out + 8,  p->best_dist);
    put16(out + 12, p->scanned);
    put16(out + 14, p->total);
}

int hd_partial_unpack(hd_partial_t *p, const uint8_t in[HD_PARTIAL_WIRE_BYTES])
{
    p->node       = get16(in + 0);
    p->query_id   = get16(in + 2);
    p->base       = get16(in + 4);
    p->best_local = get16(in + 6);
    p->best_dist  = get32(in + 8);
    p->scanned    = get16(in + 12);
    p->total      = get16(in + 14);

    /* Refuse impossible shapes rather than merging them. A corrupted frame that claims to have
     * scanned more than it holds would otherwise inflate the coverage figure, and coverage is
     * what the layer above uses to decide whether "not found" can be trusted. */
    if (p->best_local != HD_NO_SLOT && p->best_dist > HD_BITS) return 0;
    if (p->scanned > p->total) return 0;
    return 1;
}
