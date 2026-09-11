/* ===========================================================================================
 *  bench_vision.c -- see bench_vision.h
 * ===========================================================================================
 */

#include "bench_vision.h"

void vision_init(bench_vision_t *v, uint32_t seed)
{
    v->rng = seed ? seed : 0x5EE1A600u;
    for (int i = 0; i < VIS_GRID * VIS_GRID; i++) hd_random(v->cell[i], &v->rng);

    /* Orientation vectors are DERIVED BY ROTATION from the first, not drawn independently.
     * That is deliberate: adjacent orientations must be similar, because an edge at 44 degrees
     * and one at 46 degrees are nearly the same edge and should not land in orthogonal
     * concepts. Independent random vectors per bin would make the encoder brittle at exactly
     * the boundaries where real images sit. */
    hd_random(v->orient[0], &v->rng);
    for (int i = 1; i < VIS_ORIENTS; i++)
        hd_permute(v->orient[i], v->orient[i - 1], (int32_t)(HD_BITS / (VIS_ORIENTS * 4)));

    hd_random(v->mag_base, &v->rng);
}

/* Sobel-ish gradient over the cell, computed on a downsampled sample of the source so the cost
 * does not scale with image size. */
void vision_features(const uint8_t *img, uint16_t w, uint16_t h,
                     uint8_t *orient_out, uint8_t *mag_out)
{
    if (w < 3u || h < 3u) {
        for (int i = 0; i < VIS_GRID * VIS_GRID; i++) { orient_out[i] = 0; mag_out[i] = 0; }
        return;
    }

    for (int gy = 0; gy < VIS_GRID; gy++) {
        for (int gx = 0; gx < VIS_GRID; gx++) {
            const uint32_t x0 = (uint32_t)gx * w / VIS_GRID;
            const uint32_t x1 = (uint32_t)(gx + 1) * w / VIS_GRID;
            const uint32_t y0 = (uint32_t)gy * h / VIS_GRID;
            const uint32_t y1 = (uint32_t)(gy + 1) * h / VIS_GRID;

            int32_t sx = 0, sy = 0;
            uint32_t n = 0;

            /* Step so that a cell is sampled at most ~8x8 times regardless of source size:
             * a 4K frame and a thumbnail cost the same here, which is what lets the same code
             * run on a Teensy and on a Linux box with a real camera. */
            const uint32_t stepx = ((x1 - x0) / 8u) + 1u;
            const uint32_t stepy = ((y1 - y0) / 8u) + 1u;

            for (uint32_t y = y0 + 1u; y + 1u < y1 && y + 1u < h; y += stepy) {
                for (uint32_t x = x0 + 1u; x + 1u < x1 && x + 1u < w; x += stepx) {
                    const int32_t l = img[y * w + (x - 1u)];
                    const int32_t r = img[y * w + (x + 1u)];
                    const int32_t u = img[(y - 1u) * w + x];
                    const int32_t d = img[(y + 1u) * w + x];
                    sx += (r - l);
                    sy += (d - u);
                    n++;
                }
            }
            const int i = gy * VIS_GRID + gx;
            if (!n) { orient_out[i] = 0; mag_out[i] = 0; continue; }

            sx /= (int32_t)n;
            sy /= (int32_t)n;

            const int32_t ax = sx < 0 ? -sx : sx;
            const int32_t ay = sy < 0 ? -sy : sy;
            int32_t mag = ax + ay;                     /* L1: no sqrt, no float */
            if (mag > 255) mag = 255;
            mag_out[i] = (uint8_t)mag;

            /* Unsigned orientation in VIS_ORIENTS bins, by comparison only -- no atan2, no
             * floating point, so this runs identically on every node. */
            uint8_t o;
            if (ax == 0 && ay == 0)      o = 0;
            else if (ay * 5 < ax * 2)    o = 0;        /* ~horizontal   */
            else if (ax * 5 < ay * 2)    o = 2;        /* ~vertical     */
            else if ((sx > 0) == (sy > 0)) o = 1;      /* diagonal      */
            else                         o = 3;        /* anti-diagonal */
            if (VIS_ORIENTS > 4) {
                /* refine into the finer bin using which component dominates */
                const uint8_t fine = (uint8_t)((ax > ay) ? 0u : 1u);
                o = (uint8_t)((o * 2u + fine) % VIS_ORIENTS);
            } else {
                o = (uint8_t)(o % VIS_ORIENTS);
            }
            orient_out[i] = o;
        }
    }
}

void vision_encode_gray(const bench_vision_t *v, const uint8_t *img, uint16_t w, uint16_t h,
                        hd_acc_t *scratch, hd_t out)
{
    uint8_t orient[VIS_GRID * VIS_GRID];
    uint8_t mag[VIS_GRID * VIS_GRID];
    vision_features(img, w, h, orient, mag);

    hd_acc_clear(scratch);

    /* static: three hd_t locals put this frame at 3,240 bytes. Encoding a frame is not
     * reentrant -- one camera, one encoder -- and 3 KB is most of an ESP32 task stack. */
    static hd_t lvl, bound, tmp;
    for (int i = 0; i < VIS_GRID * VIS_GRID; i++) {
        /* A cell with no edge in it contributes NOTHING. Encoding "flat" as a value would fill
         * the vector with agreement between every pair of blank skies, which is the visual
         * equivalent of two silent channels looking like a shared feature. */
        if (mag[i] < 12u) continue;

        const uint8_t q = (uint8_t)((uint32_t)mag[i] * VIS_MAGS / 256u);
        hd_encode_level(lvl, v->mag_base, q, (uint32_t)(VIS_MAGS - 1));

        hd_bind(tmp, v->orient[orient[i]], lvl);      /* this kind of edge, this strong */
        hd_bind(bound, v->cell[i], tmp);              /* ...seen here                    */
        hd_acc_add(scratch, bound);
    }

    if (scratch->n == 0u) { hd_zero(out); return; }   /* a blank frame is not a concept */
    hd_acc_result(scratch, out);
}
