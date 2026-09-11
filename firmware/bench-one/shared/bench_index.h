/* ===========================================================================================
 *  bench_index.h -- splitting what the machine SEARCHES from what it STORES
 * ===========================================================================================
 *
 *  THE STRUCTURAL MISTAKE THIS CORRECTS
 *  -------------------------------------
 *  Everything before this file kept whole 1 KB concepts in RAM and searched them there. That
 *  put the index and the body in the same place, and they have completely different needs:
 *
 *      the INDEX is read on every single query          -> must be in RAM, must be small
 *      the BODY is read for one survivor in a million   -> can live on a $5 memory card
 *
 *  A 512-bit prefix decides what the full 8192-bit vector would have decided. So the index
 *  entry is 72 bytes against the body's 1024, and the same RAM holds fourteen times as many
 *  concepts.
 *
 *      whole vectors in RAM   2,760,000 concepts
 *      index in RAM           36,200,000 concepts, bodies on cards already owned
 *
 *
 *  EVERY NODE IS SELF-CONTAINED, WHICH REMOVES THE SINGLE POINT OF FAILURE
 *  -----------------------------------------------------------------------
 *  A node indexes ONLY the concepts whose bodies are on its own card. Nothing fetches across
 *  the network, and a dead node costs exactly the concepts it held and nothing else.
 *
 *      11x Luckfox   40 MB index ->    582,542 concepts, 569 MB of bodies on a 4 GB card
 *      FPGA-A      1024 MB index -> 14,913,080 concepts, bodies on the 2 TB SSD
 *      FPGA-B      1024 MB index -> 14,913,080 concepts, bodies on the 128 GB card
 *      ------------------------------------------------------------------------------
 *                                  36,234,122 concepts, no shared disk anywhere
 *
 *  Card headroom is 7x on the Luckfoxes and 144x on the SSD, so storage is not the limit --
 *  index RAM is, which is the correct thing to be limited by.
 *
 *
 *  WHY 512 BITS AND NOT 256, MEASURED
 *  -----------------------------------
 *  The index only has to keep the true match among a SMALL set of survivors; each survivor is
 *  a real read from a real card. Survivors after a 6-sigma screen over 582,542 concepts:
 *
 *      prefix   at 25% noise      at 35% noise
 *        256      22,750          everything. useless.
 *        512           1          125,411
 *       1024           1              159
 *
 *  256 bits is unusable at this scale. 1024 is safest but halves capacity. 512 is exact at
 *  clean input and is protected by the valve below -- which is why 512 ships.
 *
 *
 *  THE SAFETY VALVE
 *  -----------------
 *  Above roughly 30% input noise a 512-bit index stops discriminating and the survivor count
 *  explodes. The machine does NOT then fetch 125,411 bodies to guess. It refuses, exactly as
 *  every other part of this system refuses: an answer it cannot stand behind is not an answer.
 *
 *      survivors <= max_survivors   ->  fetch them, compare in full, return the winner
 *      survivors >  max_survivors   ->  HD_INDEX_TOO_NOISY. no disk touched.
 *
 *  The caller can then re-ask with a cleaner query, accept a coarse answer, or escalate to a
 *  node holding a wider index. What it cannot do is get a confident wrong answer.
 * ===========================================================================================
 */

#ifndef BENCH_INDEX_H
#define BENCH_INDEX_H

#include "bench_hdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Index prefix width in bits. 512 measured best for a ~600k-concept node; raise to 1024 on a
 * node whose input is dirty, at half the capacity. Must be a multiple of 32 and <= HD_BITS. */
#ifndef HD_INDEX_BITS
#define HD_INDEX_BITS 512
#endif

#define HD_INDEX_WORDS (HD_INDEX_BITS / 32)
#define HD_INDEX_BYTES (HD_INDEX_BITS / 8)

/* One index entry: 64 bytes of prefix + 8 of body location = 72. */
typedef struct {
    uint32_t prefix[HD_INDEX_WORDS];
    uint64_t body;                 /* byte offset on this node's own card                    */
} hd_entry_t;

/* Pull a body back from local storage. Returns 1 on success. The index never knows what the
 * storage is -- an SSD, a microSD, or an array in a test all look the same here. */
typedef int (*hd_body_fetch_fn)(void *ctx, uint64_t offset, hd_t out);

typedef struct {
    hd_entry_t      *e;
    uint16_t        *label;
    uint32_t         n;
    uint32_t         cap;

    hd_body_fetch_fn fetch;
    void            *ctx;

    /* The valve. 64 is a sane default: 64 KB of reads is ~3 ms on a microSD, and a query that
     * needs more than 64 candidates is one the index cannot honestly resolve. */
    uint16_t         max_survivors;

    /* Counters -- free, and they are how you find out the index is mis-sized in the field. */
    uint32_t         queries;
    uint32_t         refused;
    uint64_t         bodies_read;
    uint64_t         survivors_total;
} hd_index_t;

typedef enum {
    HD_INDEX_OK = 0,
    HD_INDEX_EMPTY,
    HD_INDEX_TOO_NOISY,     /* survivor count exceeded the valve; nothing was read           */
    HD_INDEX_NO_BODY,       /* survivors existed but storage could not return them           */
} hd_index_status_t;

typedef struct {
    hd_index_status_t status;
    int32_t  slot;          /* winning entry, or -1                                          */
    uint16_t label;
    uint32_t dist;          /* FULL 8192-bit distance of the winner                          */
    uint32_t survivors;     /* how many passed the screen                                    */
    uint32_t bodies_read;   /* how many were actually pulled from the card                   */
} hd_index_result_t;

void hd_index_init(hd_index_t *ix, hd_entry_t *storage, uint16_t *labels, uint32_t cap,
                   hd_body_fetch_fn fetch, void *ctx);

/* Store a concept: keeps its prefix, records where the body lives. The caller writes the body
 * to its own storage and passes the offset back. */
int32_t hd_index_add(hd_index_t *ix, const hd_t v, uint16_t label, uint64_t body_offset);

/* Screen, fetch, decide. Reads the index twice (cheap, contiguous, no allocation) and touches
 * storage only for survivors. */
void hd_index_search(hd_index_t *ix, const hd_t q, hd_index_result_t *out);

/* Prefix distance only -- exposed so a caller can screen without committing to a fetch. */
uint32_t hd_index_prefix_dist(const hd_entry_t *e, const hd_t q);

/* Copy a vector's prefix into an entry. */
void hd_index_set_prefix(hd_entry_t *e, const hd_t v);

const char *hd_index_status_name(hd_index_status_t s);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_INDEX_H */
