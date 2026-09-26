/*
 * geom_ref.c -- x86 test helper for test_teensy_sim.py: the plain library calls, with none of the
 * firmware (tg_core.c) in between, so the simulator's output can be checked against them.
 *
 * Reads commands on stdin, writes results on stdout (all little-endian):
 *   u32 op, u32 len, len payload bytes
 *   op 1 MESH    payload = t_mesh_hdr + nverts x geom_vertex + nidx x u16
 *                -> i32 status of geom_mesh_upload
 *   op 2 FRAME   payload = geom_frame_hdr + ndraws x geom_draw
 *                -> i32 status, u32 nrecs, u32 tris_in, tris_out, tris_culled, tris_clipped,
 *                   nrecs x 96-byte records (everything geom_frame emitted, END included)
 *   op 3 RENDER  payload = u32 clear_color, then n x 96-byte records
 *                -> 1280*720 u16 RGB565 pixels from gpu_refrast_frame (sprites read 0)
 *   op 4 RESET   -> i32 0 (geom_reset)
 * EOF on stdin ends the program.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_proto.h"
#include "gpu_setup.h"
#include "gpu_refrast.h"
#include "geom.h"

typedef struct {
    uint32_t *recs;
    uint32_t n, cap;
} recbuf;

static int emit(void *user, const uint32_t rec[GPU_REC_WORDS])
{
    recbuf *b = (recbuf *)user;
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 1024;
        b->recs = (uint32_t *)realloc(b->recs, (size_t)b->cap * GPU_REC_BYTES);
        if (!b->recs)
            exit(3);
    }
    memcpy(b->recs + (size_t)b->n * GPU_REC_WORDS, rec, GPU_REC_BYTES);
    b->n++;
    return 0;
}

static uint16_t rd0(void *user, uint32_t addr)
{
    (void)user;
    (void)addr;
    return 0;
}

static void put(const void *p, size_t n)
{
    if (fwrite(p, 1, n, stdout) != n)
        exit(4);
}

int main(void)
{
    static uint16_t fb[GPU_W * GPU_H];
    uint32_t hdr[2];
    geom_reset();
    while (fread(hdr, 4, 2, stdin) == 2) {
        uint32_t op = hdr[0], len = hdr[1];
        uint8_t *p = (uint8_t *)malloc(len ? len : 1);
        int32_t st;
        if (!p || fread(p, 1, len, stdin) != len)
            return 2;
        if (op == 1) {
            t_mesh_hdr mh;
            if (len < sizeof mh)
                return 2;
            memcpy(&mh, p, sizeof mh);
            if ((uint64_t)sizeof mh + (uint64_t)mh.nverts * 28u + (uint64_t)mh.nidx * 2u != len)
                return 2;
            st = geom_mesh_upload(mh.mesh_id, mh.nverts, mh.nidx,
                                  (const geom_vertex *)(p + sizeof mh),
                                  (const uint16_t *)(p + sizeof mh + (size_t)mh.nverts * 28u));
            put(&st, 4);
        } else if (op == 2) {
            geom_frame_hdr fh;
            geom_stats gs;
            recbuf b = {NULL, 0, 0};
            uint32_t o[6];
            if (len < sizeof fh)
                return 2;
            memcpy(&fh, p, sizeof fh);
            if ((uint64_t)sizeof fh + (uint64_t)fh.ndraws * sizeof(geom_draw) != len)
                return 2;
            memset(&gs, 0, sizeof gs);
            st = geom_frame(&fh, (const geom_draw *)(p + sizeof fh), emit, &b, &gs);
            memcpy(&o[0], &st, 4);
            o[1] = b.n;
            o[2] = gs.tris_in;
            o[3] = gs.tris_out;
            o[4] = gs.tris_culled;
            o[5] = gs.tris_clipped;
            put(o, sizeof o);
            if (b.n)
                put(b.recs, (size_t)b.n * GPU_REC_BYTES);
            free(b.recs);
        } else if (op == 3) {
            uint32_t clear;
            if (len < 4 || (len - 4) % GPU_REC_BYTES)
                return 2;
            memcpy(&clear, p, 4);
            gpu_refrast_frame((const uint32_t *)(const void *)(p + 4), (int)((len - 4) / GPU_REC_BYTES),
                              (uint16_t)clear, rd0, NULL, fb);
            put(fb, sizeof fb);
        } else if (op == 4) {
            geom_reset();
            st = 0;
            put(&st, 4);
        } else {
            return 2;
        }
        free(p);
        fflush(stdout);
    }
    return 0;
}
