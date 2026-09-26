/*
 * test_ref.c -- independent reference renderer for zynq/test_sim.py (x86 only, glibc).
 *
 * Reads a scene script and renders it the way SPEC 4/5/7 say the system must: primitives are set
 * up with common/gpu_setup.c, the list follows the collector rules (TRI/SPRITE kept up to 1536,
 * everything else dropped) and gpu_refrast_frame draws it. None of fpgagpud's code is used, so
 * comparing its READBACK with this output checks the daemon's parsing, pool, record pushing and
 * the simulated PL end to end.
 *
 *   test_ref SCRIPT
 * Script lines (numbers decimal or 0x hex; floats as their IEEE-754 bit pattern in hex):
 *   clear C                                        clear colour for the next render
 *   tri x0 y0 z0 r0 g0 b0 x1 .. b1 x2 .. b2 FLAGS CULL   -> gpu_setup_tri
 *   rect X0 Y0 X1 Y1 RGB565 ZBITS FLAGS            -> gpu_setup_rect
 *   upload ID W H SEED ADDR STRIDE                 sprite pixels spx(SEED, i) written at ADDR
 *   sprite ID X Y FLAGS KEY                        -> gpu_setup_sprite with the upload's geometry
 *   rec W0 .. W23                                  raw record
 *   render FILE                                    render the list, write FILE (1280*720 LE u16)
 *                                                  and start a new, empty list
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_proto.h"
#include "gpu_setup.h"
#include "gpu_refrast.h"

static uint8_t *vddr;
static uint32_t list[GPU_LIST_SLOTS * GPU_REC_WORDS];
static int nlist;
static struct { uint32_t addr, stride, w, h; int valid; } spr[256];

/* same generator as test_sim.py: spx() */
static uint16_t spx(uint32_t seed, uint32_t i)
{
    uint32_t x = seed * 0x9E3779B1u + i * 0x85EBCA77u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    if (((x >> 16) & 7u) == 0)
        return 0xF81Fu;
    return (uint16_t)x;
}

static uint16_t rd16(void *user, uint32_t addr)
{
    (void)user;
    if (addr < GPU_DDR_BASE || addr - GPU_DDR_BASE > GPU_DDR_SIZE - 2)
        return 0;
    return (uint16_t)(vddr[addr - GPU_DDR_BASE] | (vddr[addr - GPU_DDR_BASE + 1] << 8));
}

static void add(const uint32_t *r)
{
    uint32_t t = GPU_W0_TYPE(r[0]);
    if (t != GPU_REC_TRI && t != GPU_REC_SPRITE)
        return;
    if (nlist >= GPU_LIST_SLOTS)
        return;                                     /* LIST_OVERFLOW */
    memcpy(list + (size_t)nlist * GPU_REC_WORDS, r, GPU_REC_BYTES);
    nlist++;
}

static uint32_t num(char **save)
{
    char *t = strtok_r(NULL, " \t\r\n", save);
    if (!t) {
        fprintf(stderr, "test_ref: missing number\n");
        exit(2);
    }
    return (uint32_t)strtoul(t, NULL, 0);
}

static float fbits(char **save)
{
    uint32_t u = num(save);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

int main(int argc, char **argv)
{
    FILE *f;
    char line[4096];
    uint16_t clear = 0, *fb;
    if (argc != 2) {
        fprintf(stderr, "usage: test_ref SCRIPT\n");
        return 2;
    }
    f = fopen(argv[1], "r");
    vddr = calloc(1, GPU_DDR_SIZE);
    fb = malloc(GPU_FB_BYTES);
    if (!f || !vddr || !fb) {
        perror("test_ref");
        return 2;
    }
    while (fgets(line, sizeof line, f)) {
        char *save = NULL, *cmd = strtok_r(line, " \t\r\n", &save);
        uint32_t rec[GPU_SETUP_MAX_OUT][GPU_REC_WORDS];
        int i, n;
        if (!cmd || cmd[0] == '#')
            continue;
        if (!strcmp(cmd, "clear")) {
            clear = (uint16_t)num(&save);
        } else if (!strcmp(cmd, "tri")) {
            gpu_vtx v[3];
            uint32_t flags;
            int cull;
            for (i = 0; i < 3; i++) {
                v[i].x = fbits(&save);
                v[i].y = fbits(&save);
                v[i].z = fbits(&save);
                v[i].r = (uint8_t)num(&save);
                v[i].g = (uint8_t)num(&save);
                v[i].b = (uint8_t)num(&save);
                v[i].a = 0;
            }
            flags = num(&save);
            cull = (int)num(&save);
            n = gpu_setup_tri(v, flags & (GPU_F_ZTEST | GPU_F_ZWRITE), cull, rec, GPU_SETUP_MAX_OUT);
            for (i = 0; i < n; i++)
                add(rec[i]);
        } else if (!strcmp(cmd, "rect")) {
            int x0 = (int)num(&save), y0 = (int)num(&save), x1 = (int)num(&save), y1 = (int)num(&save);
            uint16_t c = (uint16_t)num(&save);
            float z = fbits(&save);
            uint32_t flags = num(&save);
            if (gpu_setup_rect(x0, y0, x1, y1, c, z, flags & (GPU_F_ZTEST | GPU_F_ZWRITE), rec[0]) == 1)
                add(rec[0]);
        } else if (!strcmp(cmd, "upload")) {
            uint32_t id = num(&save), w = num(&save), h = num(&save), seed = num(&save);
            uint32_t addr = num(&save), stride = num(&save), x, y;
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    uint32_t a = addr + y * stride + x * 2 - GPU_DDR_BASE;
                    uint16_t p = spx(seed, y * w + x);
                    vddr[a] = (uint8_t)p;
                    vddr[a + 1] = (uint8_t)(p >> 8);
                }
            spr[id & 255].addr = addr;
            spr[id & 255].stride = stride;
            spr[id & 255].w = w;
            spr[id & 255].h = h;
            spr[id & 255].valid = 1;
        } else if (!strcmp(cmd, "sprite")) {
            uint32_t id = num(&save) & 255u;
            int x = (int)num(&save), y = (int)num(&save);
            uint32_t flags = num(&save), key = num(&save);
            if (spr[id].valid && gpu_setup_sprite(x, y, (int)spr[id].w, (int)spr[id].h, spr[id].addr,
                                                  spr[id].stride, (int)(flags & 1u), (uint16_t)key,
                                                  rec[0]) == 1)
                add(rec[0]);
        } else if (!strcmp(cmd, "rec")) {
            for (i = 0; i < GPU_REC_WORDS; i++)
                rec[0][i] = num(&save);
            add(rec[0]);
        } else if (!strcmp(cmd, "render")) {
            char *name = strtok_r(NULL, " \t\r\n", &save);
            FILE *o;
            gpu_refrast_frame(list, nlist, clear, rd16, NULL, fb);
            o = name ? fopen(name, "wb") : NULL;
            if (!o || fwrite(fb, 1, GPU_FB_BYTES, o) != GPU_FB_BYTES) {
                perror("test_ref: write");
                return 2;
            }
            fclose(o);
            printf("%d\n", nlist);
            nlist = 0;
        } else {
            fprintf(stderr, "test_ref: unknown command %s\n", cmd);
            return 2;
        }
    }
    fclose(f);
    return 0;
}
