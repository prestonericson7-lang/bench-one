/* ===========================================================================================
 *  bench_store.c -- see bench_store.h
 * ===========================================================================================
 */

#include "bench_store.h"
#include <string.h>

int bstore_open(bstore_t *s, const char *path)
{
    s->fp = NULL; s->next_seq = 1; s->bytes = 0;
    s->appended = 0; s->read_back = 0; s->ok = 0;

    /* "r+b" first so an existing life is continued, never truncated. Opening the machine's own
     * history with "wb" would erase everything it has ever experienced, silently, on a restart
     * -- so that mode is never used anywhere in this file. */
    s->fp = fopen(path, "r+b");
    if (!s->fp) {
        s->fp = fopen(path, "w+b");
        if (!s->fp) return 0;
    }

    if (fseek(s->fp, 0, SEEK_END) != 0) { fclose(s->fp); s->fp = NULL; return 0; }
    const long end = ftell(s->fp);
    if (end < 0) { fclose(s->fp); s->fp = NULL; return 0; }
    s->bytes = (uint64_t)end;

    /* Continue the sequence from the length. The file is NOT scanned: at 25 MB/s, reading a
     * 2 TB log to find out where you were would take 22 hours every boot. Fixed-size records
     * make this arithmetic instead. */
    s->next_seq = (uint32_t)(s->bytes / BSTORE_REC_BYTES) + 1u;

    /* A trailing partial record means the power went out mid-append. Truncating back to the
     * last whole record is the correct repair, and it is only possible because records are
     * fixed size and append-only. */
    const uint64_t ragged = s->bytes % BSTORE_REC_BYTES;
    if (ragged) s->bytes -= ragged;

    s->ok = 1;
    return 1;
}

void bstore_close(bstore_t *s)
{
    if (s->fp) { fflush(s->fp); fclose(s->fp); s->fp = NULL; }
    s->ok = 0;
}

uint32_t bstore_append(bstore_t *s, const hd_t v, uint16_t label, uint8_t surprise,
                       uint32_t stamp, uint64_t *offset_out)
{
    if (!s->ok || !s->fp) return 0;

    bstore_rec_t h;
    h.magic    = BSTORE_MAGIC;
    h.seq      = s->next_seq;
    h.stamp    = stamp;
    h.label    = label;
    h.surprise = surprise;
    h.flags    = 0;

    if (fseek(s->fp, (long)s->bytes, SEEK_SET) != 0) return 0;
    if (fwrite(&h, sizeof(h), 1, s->fp) != 1)        return 0;
    if (fwrite(v, HD_BYTES, 1, s->fp) != 1)          return 0;

    if (offset_out) *offset_out = s->bytes;
    s->bytes += BSTORE_REC_BYTES;
    s->appended++;
    return s->next_seq++;
}

int bstore_read(bstore_t *s, uint64_t offset, hd_t v_out, bstore_rec_t *hdr_out)
{
    if (!s->ok || !s->fp) return 0;
    if (offset + BSTORE_REC_BYTES > s->bytes) return 0;

    bstore_rec_t h;
    if (fseek(s->fp, (long)offset, SEEK_SET) != 0) return 0;
    if (fread(&h, sizeof(h), 1, s->fp) != 1)       return 0;

    /* Per-record magic. An index entry that has gone stale points at the middle of some other
     * record, and without this check that garbage would be returned as a memory -- a false
     * memory being far worse than a missing one. */
    if (h.magic != BSTORE_MAGIC) return 0;
    if (h.flags & BSTORE_F_FORGOTTEN) return 0;

    if (v_out && fread(v_out, HD_BYTES, 1, s->fp) != 1) return 0;
    if (hdr_out) *hdr_out = h;
    s->read_back++;
    return 1;
}

int bstore_mark(bstore_t *s, uint64_t offset, uint8_t flags)
{
    if (!s->ok || !s->fp) return 0;
    if (offset + BSTORE_REC_BYTES > s->bytes) return 0;

    bstore_rec_t h;
    if (fseek(s->fp, (long)offset, SEEK_SET) != 0) return 0;
    if (fread(&h, sizeof(h), 1, s->fp) != 1)       return 0;
    if (h.magic != BSTORE_MAGIC) return 0;

    h.flags |= flags;
    if (fseek(s->fp, (long)offset, SEEK_SET) != 0) return 0;
    if (fwrite(&h, sizeof(h), 1, s->fp) != 1)      return 0;
    return 1;
}

void bstore_sync(bstore_t *s)
{
    if (s->fp) fflush(s->fp);
}


/* -------------------------------------------------------------------------------------------
 * INDEX
 * ----------------------------------------------------------------------------------------- */

void bstore_index_init(bstore_index_t *ix, uint64_t *offsets, uint16_t *counts, uint16_t nproto)
{
    ix->offset = offsets;
    ix->count  = counts;
    ix->nproto = nproto;
    for (uint16_t i = 0; i < nproto; i++) counts[i] = 0;
}

void bstore_index_add(bstore_index_t *ix, uint16_t proto, uint64_t offset)
{
    if (proto >= ix->nproto) return;
    uint16_t *n = &ix->count[proto];
    if (*n >= BSTORE_FANOUT) return;
    ix->offset[(size_t)proto * BSTORE_FANOUT + *n] = offset;
    (*n)++;
}

uint16_t bstore_index_fetch(const bstore_index_t *ix, bstore_t *s, uint16_t proto,
                            hd_t *out, bstore_rec_t *hdrs, uint16_t max)
{
    if (proto >= ix->nproto) return 0;
    const uint16_t have = ix->count[proto];
    uint16_t got = 0;

    for (uint16_t i = 0; i < have && got < max; i++) {
        const uint64_t off = ix->offset[(size_t)proto * BSTORE_FANOUT + i];
        /* A record that fails to read is skipped, not fatal. On a bench machine that gets
         * power-cycled, a couple of unreadable records out of millions is expected and must not
         * take the recall path down with them. */
        if (bstore_read(s, off, out ? out[got] : NULL, hdrs ? &hdrs[got] : NULL)) got++;
    }
    return got;
}
