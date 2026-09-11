/* ===========================================================================================
 *  bench_store.h -- the machine's life, on disk
 * ===========================================================================================
 *
 *  WHAT THIS ADDS
 *  ---------------
 *  Everything above this file lives in RAM and dies with the power. This gives the machine an
 *  autobiography: every experience it has ever had, kept, in order, for as long as the disk
 *  lasts. What it THINKS with stays in RAM. What it HAS LIVED THROUGH goes here.
 *
 *      2 TB / 1 KB per concept  =  2,000,000,000 experiences
 *
 *
 *  THE NUMBER THAT DICTATES THE DESIGN
 *  ------------------------------------
 *  The Zynq PS USB port is USB 2.0 high-speed. A USB-C plug does not change that -- the
 *  controller is 480 Mbit/s and measured mass-storage throughput on Zynq PS USB is about
 *  25 MB/s. So:
 *
 *      full linear scan of 2 TB at 25 MB/s  =  ~22 HOURS
 *
 *  Searching the archive is therefore not a thing that can ever happen. This is not a
 *  limitation to engineer around; it is the reason the architecture is shaped the way it is,
 *  and it makes the dream layer load-bearing rather than a nicety:
 *
 *      RAM holds PROTOTYPES  -- few, general, searched constantly
 *      SSD holds INSTANCES   -- many, specific, read only when a concept is not enough
 *
 *  A query asks the concepts. Only if the caller needs the specific case does anything touch
 *  the disk, and then it reads a handful of records at known offsets, not two terabytes.
 *
 *
 *  APPEND-ONLY, AND WHY
 *  ---------------------
 *  Records are only ever appended. Nothing is rewritten, nothing is moved, nothing is deleted
 *  in place. Three reasons, all of which have bitten real systems:
 *
 *   1. CRASH SAFETY. A torn write can only ever damage the last record. Everything before it
 *      is untouched by construction. A machine intended to run for months and be power-cycled
 *      by a bench supply cannot afford a storage format where a bad moment corrupts history.
 *
 *   2. SPEED. Sequential appends run at full device speed. The one access pattern USB 2.0 mass
 *      storage is actually good at is the one this uses.
 *
 *   3. IT IS A REAL AUTOBIOGRAPHY. Order is preserved for free, so "what did I see before
 *      this?" is answerable, and consolidation can be replayed from the log if the RAM state
 *      is ever lost or you want to re-derive concepts with different parameters.
 *
 *  Forgetting is done by marking, never by erasing. A record that stops being useful gets a
 *  flag; the bytes stay. Disk is the one resource here that is not scarce.
 * ===========================================================================================
 */

#ifndef BENCH_STORE_H
#define BENCH_STORE_H

#include "bench_hdc.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSTORE_MAGIC   0x4C494645u    /* "LIFE" */
#define BSTORE_VERSION 1u

/* One record on disk: a fixed-size header then the vector. Fixed size is deliberate -- it makes
 * the Nth record's offset arithmetic instead of a lookup, which is what lets a 2 TB file be
 * indexed by a few megabytes of RAM. */
typedef struct {
    uint32_t magic;        /* per-record, so a resync after damage is possible               */
    uint32_t seq;          /* monotonic. This is time, as the machine experiences it         */
    uint32_t stamp;        /* caller's clock; 0 if it has none                               */
    uint16_t label;        /* what the machine called it                                     */
    uint8_t  surprise;     /* how unexpected it was when it happened, 0..255                 */
    uint8_t  flags;        /* BSTORE_F_*                                                     */
} bstore_rec_t;

#define BSTORE_F_ABSORBED  0x01u   /* folded into a prototype                                */
#define BSTORE_F_FORGOTTEN 0x02u   /* marked useless; bytes kept, never returned             */

#define BSTORE_REC_BYTES   (sizeof(bstore_rec_t) + HD_BYTES)

typedef struct {
    FILE     *fp;
    uint32_t  next_seq;
    uint64_t  bytes;
    uint32_t  appended;
    uint32_t  read_back;
    int       ok;
} bstore_t;

/* Open or create the life log. Existing files are scanned only for their length -- the whole
 * file is never read, because at 25 MB/s reading 2 TB to start up is not a startup. */
int  bstore_open (bstore_t *s, const char *path);
void bstore_close(bstore_t *s);

/* Append one experience. Returns its sequence number, or 0 on failure.
 * `offset_out` receives the byte offset, which is what an in-RAM index stores. */
uint32_t bstore_append(bstore_t *s, const hd_t v, uint16_t label, uint8_t surprise,
                       uint32_t stamp, uint64_t *offset_out);

/* Read one record back by offset. This is the only read path: by offset, one record, never a
 * scan. Returns 1 on success. */
int  bstore_read(bstore_t *s, uint64_t offset, hd_t v_out, bstore_rec_t *hdr_out);

/* Mark a record without moving it. Used by consolidation to record that an instance now lives
 * inside a concept, and by forgetting. */
int  bstore_mark(bstore_t *s, uint64_t offset, uint8_t flags);

/* Flush to the device. Call after a batch, not after every record -- on USB 2.0 mass storage a
 * per-record fsync costs more than the write. */
void bstore_sync(bstore_t *s);

/* How many experiences fit in `bytes` of disk. */
#define BSTORE_CAPACITY(bytes) ((uint64_t)(bytes) / BSTORE_REC_BYTES)


/* -------------------------------------------------------------------------------------------
 * THE RAM INDEX -- what makes 2 TB usable from 64 MB
 * -----------------------------------------------------------------------------------------
 * Per prototype, the disk offsets of the instances that formed it. That is the entire bridge
 * between the concept in RAM and the cases on disk.
 *
 *     2,500 prototypes x 128 offsets x 8 bytes = 2.5 MB of RAM
 *
 * covering as many instances as the disk can hold. The machine can then answer "what kind of
 * thing is this?" from RAM instantly, and "show me the actual times it happened" with a
 * handful of seeks.
 * ----------------------------------------------------------------------------------------- */
#ifndef BSTORE_FANOUT
#define BSTORE_FANOUT 128
#endif

typedef struct {
    uint64_t *offset;      /* [nproto][BSTORE_FANOUT], caller-owned                          */
    uint16_t *count;       /* [nproto]                                                        */
    uint16_t  nproto;
} bstore_index_t;

void bstore_index_init(bstore_index_t *ix, uint64_t *offsets, uint16_t *counts, uint16_t nproto);

/* Record that this instance belongs to this concept. Silently drops past BSTORE_FANOUT: a
 * concept backed by more than 128 examples does not become more true with the 129th, and the
 * bound is what keeps the index a fixed size. */
void bstore_index_add(bstore_index_t *ix, uint16_t proto, uint64_t offset);

/* Fetch up to `max` real instances behind a concept. This is "show me why you think that". */
uint16_t bstore_index_fetch(const bstore_index_t *ix, bstore_t *s, uint16_t proto,
                            hd_t *out, bstore_rec_t *hdrs, uint16_t max);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_STORE_H */
