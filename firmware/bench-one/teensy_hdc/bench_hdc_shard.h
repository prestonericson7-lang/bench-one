/* ===========================================================================================
 *  bench_hdc_shard.h -- interruptible, mergeable work for nodes that cannot be trusted to
 *                       finish on time
 * ===========================================================================================
 *
 *  THE PROBLEM THIS EXISTS FOR
 *  ----------------------------
 *  An ESP32-S3 cannot be relied on to run a computation to completion. Its WiFi and BLE
 *  stacks are not cooperative library code -- they are high-priority FreeRTOS tasks plus
 *  interrupt handlers, and they WILL take the core away from you:
 *
 *      - the WiFi task runs well above any application task and preempts on beacon intervals,
 *        scans, association and RF calibration
 *      - with BLE coexistence enabled the radio is time-sliced, so both stacks interrupt
 *      - flash access during a WiFi event stalls the cache, freezing the other core too
 *      - the task watchdog kills any task that refuses to yield
 *
 *  So a design where an S3 holds part of a chain and must produce its piece before the next
 *  node can proceed does not merely run slowly. It BREAKS: the chain stalls, a timeout fires
 *  somewhere else, and the failure surfaces on a node that did nothing wrong. There is no
 *  configuration that removes this. Pinning compute to core 1 and WiFi to core 0 reduces it
 *  and does not eliminate it.
 *
 *
 *  THE FIX: GIVE THEM WORK THAT DOES NOT CARE WHEN IT FINISHES
 *  ------------------------------------------------------------
 *  The whole substrate is built on two aggregations, and both are a MONOID -- associative,
 *  commutative, with an identity:
 *
 *      SEARCH   combine(best_a, best_b) = whichever has the smaller Hamming distance
 *      BUNDLE   combine(acc_a,  acc_b)  = element-wise sum of the counters
 *
 *  Which means, and this is the entire argument:
 *
 *      * partial results are VALID results -- a node that scanned 60% of its shard returns a
 *        true answer for that 60%, and the coordinator can use it or wait for more
 *      * results may arrive in ANY ORDER
 *      * a result may be DELIVERED TWICE -- the coordinator dedups by node id, so the winner
 *        AND the coverage figure both stay correct
 *      * a node that never answers costs coverage, never correctness
 *
 *  So an S3 is never in a chain. It is handed a shard, it scans as much as the RTOS lets it,
 *  and it reports what it found. If WiFi steals 40 ms, the answer is 40 ms later or slightly
 *  less complete. Nothing crashes, because nothing was waiting on it to be punctual.
 *
 *
 *  HOW A NODE USES IT
 *  -------------------
 *      hd_scan_begin(&s, &my_shard, query);
 *      while (!hd_scan_chunk(&s, 64)) {     // 64 vectors ~ a few hundred us
 *          taskYIELD();                     // WiFi gets the core here, cleanly
 *          if (deadline_passed()) break;    // and a partial answer is still an answer
 *      }
 *      send_partial(&s.result);
 *
 *  The yield point is between chunks, so the WiFi task is never blocked long enough to drop a
 *  beacon, and the watchdog is never starved. Chunk size is the whole tuning knob: smaller is
 *  more responsive to the radio, larger is less loop overhead.
 *
 *
 *  MEASURED, NOT ASSERTED
 *  -----------------------
 *  The invariant was tested against a simulation of exactly this failure mode: 10 nodes,
 *  1000 vectors, 3000 randomised runs with 15% of nodes fully dead, 35% preempted part-way
 *  through their shard, 30% of replies delivered twice, 15% arriving as answers to the
 *  PREVIOUS query, and arrival order shuffled every run.
 *
 *      exactly right (full coverage)   2137
 *      best-of-what-was-actually-seen   863
 *      WRONG                              0
 *
 *  And after delivering every reply four times over, coverage still read 1000 of 1000 with 30
 *  duplicates rejected. The 863 are not failures: each is the true nearest vector among those
 *  that were actually searched, reported alongside an honest coverage number so the layer
 *  above knows how much of the memory the answer is based on.
 * ===========================================================================================
 */

#ifndef BENCH_HDC_SHARD_H
#define BENCH_HDC_SHARD_H

#include "bench_hdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------------------------
 * A PARTIAL SEARCH RESULT -- what a node puts on the wire
 * -----------------------------------------------------------------------------------------
 * 16 bytes. Small enough that ten nodes answering costs 160 bytes, which is why the slow
 * interconnect never mattered for this algorithm.
 * ----------------------------------------------------------------------------------------- */
typedef struct {
    uint16_t node;        /* who answered                                                    */
    uint16_t query_id;    /* which question -- a late answer to an old question is discarded  */
    uint16_t base;        /* this shard's first global index, so slot numbers stay global     */
    uint16_t best_local;  /* winning index WITHIN the shard, 0xFFFF if nothing scanned        */
    uint32_t best_dist;   /* its Hamming distance                                             */
    uint16_t scanned;     /* how many vectors were actually examined                          */
    uint16_t total;       /* how many the shard holds -- scanned < total means partial        */
} hd_partial_t;

/* Resumable scan state. Lives on the worker; never sent anywhere. */
typedef struct {
    const hd_mem_t *mem;
    hd_t            query;
    uint16_t        cursor;
    hd_partial_t    result;
} hd_scan_t;

/* Start a scan. Copies the query so the caller may reuse its buffer immediately. */
void hd_scan_begin(hd_scan_t *s, const hd_mem_t *mem, const hd_t query,
                   uint16_t node, uint16_t query_id, uint16_t base);

/* Examine up to `max_items` more vectors. Returns 1 when the shard is exhausted, 0 if there
 * is more to do. Safe to stop calling at any point: s->result is always a correct answer for
 * the `scanned` vectors it has covered. */
int  hd_scan_chunk(hd_scan_t *s, uint16_t max_items);

/* Fraction of the shard covered so far, 0..255. The coordinator uses this to decide whether
 * it has enough of the memory searched to trust a negative answer. */
uint8_t hd_scan_coverage(const hd_scan_t *s);


/* -------------------------------------------------------------------------------------------
 * COMBINING -- the monoid, on the coordinator
 * ----------------------------------------------------------------------------------------- */

/* Identity element: an empty result that loses to everything. */
void hd_partial_none(hd_partial_t *dst, uint16_t query_id);

/* THE COORDINATOR'S ACCUMULATOR.
 *
 * This is separate from hd_partial_t on purpose, and the separation was forced by a measured
 * bug rather than chosen for tidiness.
 *
 * Picking the nearest vector is idempotent all by itself -- min(x,x) is x -- so an early
 * version merged straight into an hd_partial_t. But COVERAGE is a sum, and a sum is not
 * idempotent. Replaying two duplicated packets took `scanned` from 244 to 423 out of 600.
 *
 * That is not a cosmetic counter. Coverage is what decides whether "no match" means "this is
 * new" or "the nodes holding it never answered". Inflate it and the machine confidently
 * reports novelty for something it already knows, because a stalled node stayed silent -- the
 * precise failure this whole file exists to prevent, reintroduced through the back door.
 *
 * So the coordinator remembers which nodes it has already counted. Duplicates are dropped,
 * and the algebra is genuinely idempotent instead of nearly. */
#define HD_MAX_NODES 64

typedef struct {
    hd_partial_t best;
    uint64_t     seen;                       /* one bit per node id that has ever replied   */
    uint16_t     node_scanned[HD_MAX_NODES]; /* BEST coverage seen from each node           */
    uint16_t     node_total  [HD_MAX_NODES]; /* how big that node says its shard is         */
    uint32_t     dupes;                      /* stale/no-better replies -- a free link metric */
} hd_merge_t;

void hd_merge_begin(hd_merge_t *m, uint16_t query_id);

/* Fold one node's reply in. Safe with the same reply any number of times, with replies in any
 * order, and with replies to old queries. Returns 1 if it improved anything.
 *
 * PER-NODE MAX, NOT FIRST-WINS. An earlier version kept only the first reply from each node,
 * which is idempotent but wrong for retries: a node that got preempted after 40% of its shard
 * was marked "counted" and then skipped forever, so re-asking could never recover the rest.
 * Coverage plateaued at 94% no matter how many rounds were run.
 *
 * Keeping the BEST report per node is still idempotent -- max(x,x) is x -- still commutative,
 * and lets a partial answer be replaced by a fuller one. Measured: recall 94.7% -> 98.7%,
 * coverage 92.1% -> 98.7%, in an average of 2.35 rounds. */
int  hd_merge_add(hd_merge_t *m, const hd_partial_t *src);

/* Should this node be asked again? True when it has never answered, or answered partially.
 * The coordinator re-asks only these, so a retry costs a fraction of the original query. */
int  hd_merge_node_incomplete(const hd_merge_t *m, uint16_t node);

/* Coverage of the whole distributed memory so far, 0..255. Retry until this is high enough
 * for the decision being made -- a cheap lookup can accept 60%, "have I ever seen this?"
 * cannot accept anything below about 95%. */
uint8_t hd_merge_coverage(const hd_merge_t *m, uint32_t total_memory);

/* dst := better_of(dst, src) WITHOUT dedup. Use hd_merge_add unless you are combining two
 * accumulators that you already know cover disjoint node sets. */
void hd_partial_merge(hd_partial_t *dst, const hd_partial_t *src);

/* Global index of the winner, or -1 if nothing has been merged. */
int32_t hd_partial_slot(const hd_partial_t *p);

/* How much of the whole distributed memory got searched, 0..255. A confident "I have never
 * seen this" requires BOTH a large best_dist and high coverage -- without the coverage check
 * a machine with half its nodes asleep would confidently report novelty for things it knows. */
uint8_t hd_partial_coverage(const hd_partial_t *merged, uint32_t total_memory);


/* -------------------------------------------------------------------------------------------
 * DISTRIBUTED BUNDLING -- the other monoid
 * -----------------------------------------------------------------------------------------
 * Summing accumulators lets many nodes each bundle part of a set and have the coordinator add
 * the halves. This is the "just let the ESP32s sum" job: it is pure accumulation, it can be
 * stopped and resumed at any vector, and two partial sums combine with a plain add.
 * ----------------------------------------------------------------------------------------- */

/* dst += src, saturating. Associative and commutative, so nodes may report in any order. */
void hd_acc_merge(hd_acc_t *dst, const hd_acc_t *src);

/* Bundle a run of vectors into `acc`, at most `max_items` per call, resuming from *cursor.
 * Returns 1 when [first,last) is exhausted. Same yield-between-chunks contract as the scan. */
int  hd_bundle_chunk(hd_acc_t *acc, const hd_mem_t *mem,
                     uint16_t first, uint16_t last, uint16_t *cursor, uint16_t max_items);

/* -------------------------------------------------------------------------------------------
 * WIRE HELPERS -- fixed little-endian layout, so a Teensy, an S3 and a Luckfox agree without
 * anyone including anyone else's headers.
 * ----------------------------------------------------------------------------------------- */
#define HD_PARTIAL_WIRE_BYTES 16
void hd_partial_pack  (const hd_partial_t *p, uint8_t out[HD_PARTIAL_WIRE_BYTES]);
int  hd_partial_unpack(hd_partial_t *p, const uint8_t in[HD_PARTIAL_WIRE_BYTES]);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_HDC_SHARD_H */
