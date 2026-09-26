/*
 * gpu_refrast.c -- bit-exact golden model of the PL renderer (SPEC.md section 5).
 *
 * Integer only (no floats). All record arithmetic is uint32 (wraps modulo 2^32), converted to
 * int32 with a well-defined two's-complement conversion (no implementation-defined casts).
 *
 * The frame is produced row by row: for each row the row is cleared and every record is applied
 * to it in list order. Because every pixel's final value depends only on the ordered sequence of
 * operations on that pixel, this is observably identical to the full-frame algorithm of SPEC 5
 * (and to the PL's 16-row strips), and it needs no heap and only a 2.5 KB Z row on the stack.
 *
 * Out-of-spec records (never produced by gpu_setup): a TRI bbox is intersected with the screen
 * (x > 1279 / y > 719 never drawn; xmin > xmax or ymin > ymax draws nothing); SPRITE pixels are
 * clipped at x >= 1280 / y >= 720 exactly as the spec's loop bounds say. A NULL reader reads 0.
 */
#include <stddef.h>
#include <stdint.h>
#include "gpu_refrast.h"

/* two's complement uint32 -> int32 without implementation-defined behaviour */
static int32_t as_s32(uint32_t u)
{
    if (u <= 0x7FFFFFFFu)
        return (int32_t)u;
    return (int32_t)(u - 0x80000000u) - 0x7FFFFFFF - 1;
}

/* zp = z < 0 ? 0 : (z >> 12) > 65535 ? 65535 : z >> 12 */
static uint32_t z_pix(uint32_t zu)
{
    int32_t z = as_s32(zu);
    if (z < 0)
        return 0;
    z >>= 12;                 /* z >= 0: plain division by 4096 */
    return z > 65535 ? 65535u : (uint32_t)z;
}

/* c8 = c < 0 ? 0 : (c >> 16) > 255 ? 255 : c >> 16 */
static uint32_t chan8(uint32_t cu)
{
    int32_t c = as_s32(cu);
    if (c < 0)
        return 0;
    c >>= 16;
    return c > 255 ? 255u : (uint32_t)c;
}

static void tri_row(const uint32_t *r, int y, uint16_t *fbrow, uint16_t *zbrow)
{
    uint32_t w0 = r[0];
    int xmin = (int)(w0 & 0x7FFu);
    int xmax = (int)((w0 >> 11) & 0x7FFu);
    int ymin = (int)(r[1] & 0x7FFu);
    int ymax = (int)((r[1] >> 11) & 0x7FFu);
    int noedge = (w0 & GPU_F_NOEDGE) != 0;
    int ztest = (w0 & GPU_F_ZTEST) != 0;
    int zwrite = (w0 & GPU_F_ZWRITE) != 0;
    uint32_t dy, e0, e1, e2, a0, a1, a2, z, dz, cr, dr, cg, dg, cb, db;
    int x, xend;

    if (y < ymin || y > ymax)
        return;
    xend = xmax < GPU_W - 1 ? xmax : GPU_W - 1;
    if (xmin > xend)
        return;

    /* values at (xmin, y): v0 + dvdy*dy, then + dvdx per pixel (== v0 + dvdx*dx + dvdy*dy mod 2^32) */
    dy = (uint32_t)(y - ymin);
    a0 = r[2];  e0 = r[4]  + r[3]  * dy;
    a1 = r[5];  e1 = r[7]  + r[6]  * dy;
    a2 = r[8];  e2 = r[10] + r[9]  * dy;
    dz = r[12]; z  = r[11] + r[13] * dy;
    dr = r[15]; cr = r[14] + r[16] * dy;
    dg = r[18]; cg = r[17] + r[19] * dy;
    db = r[21]; cb = r[20] + r[22] * dy;

    for (x = xmin; x <= xend; x++) {
        if (noedge || (as_s32(e0) >= 0 && as_s32(e1) >= 0 && as_s32(e2) >= 0)) {
            uint32_t zp = z_pix(z);
            if (!ztest || zp <= zbrow[x]) {
                if (zwrite)
                    zbrow[x] = (uint16_t)zp;
                fbrow[x] = (uint16_t)(((chan8(cr) >> 3) << 11) | ((chan8(cg) >> 2) << 5)
                                      | (chan8(cb) >> 3));
            }
        }
        e0 += a0; e1 += a1; e2 += a2;
        z += dz; cr += dr; cg += dg; cb += db;
    }
}

static void sprite_row(const uint32_t *r, int y, uint16_t *fbrow, gpu_ddr_read16_fn rd, void *user)
{
    int sx = (int)(r[1] & 0x7FFu);
    int sy = (int)((r[1] >> 11) & 0x7FFu);
    int w  = (int)(r[2] & 0x7FFu);
    int h  = (int)((r[2] >> 11) & 0x7FFu);
    int ck = (r[0] & GPU_F_COLORKEY) != 0;
    uint16_t key = (uint16_t)(r[5] & 0xFFFFu);
    uint32_t base;
    int col;

    if (y < sy || y - sy >= h)
        return;
    base = r[3] + (uint32_t)(y - sy) * r[4];
    for (col = 0; col < w && sx + col < GPU_W; col++) {
        uint16_t p = rd ? rd(user, base + (uint32_t)col * 2u) : (uint16_t)0;
        if (ck && p == key)
            continue;
        fbrow[sx + col] = p;
    }
}

void gpu_refrast_frame(const uint32_t *recs, int nrecs, uint16_t clear_color,
                       gpu_ddr_read16_fn rd, void *user, uint16_t *fb)
{
    uint16_t zb[GPU_W];
    int x, y, i;

    if (nrecs < 0 || recs == NULL)
        nrecs = 0;
    for (y = 0; y < GPU_H; y++) {
        uint16_t *row = fb + (size_t)y * GPU_W;
        for (x = 0; x < GPU_W; x++) {
            row[x] = clear_color;
            zb[x] = 0xFFFFu;
        }
        for (i = 0; i < nrecs; i++) {
            const uint32_t *r = recs + (size_t)i * GPU_REC_WORDS;
            uint32_t type = GPU_W0_TYPE(r[0]);
            if (type == GPU_REC_TRI)
                tri_row(r, y, row, zb);
            else if (type == GPU_REC_SPRITE)
                sprite_row(r, y, row, rd, user);
            /* NOP, END, unknown: ignored */
        }
    }
}
