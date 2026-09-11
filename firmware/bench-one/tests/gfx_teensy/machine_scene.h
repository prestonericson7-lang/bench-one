/* ===========================================================================================
 *  machine_scene.h -- the machine's own picture, and the rasteriser that draws it
 * ===========================================================================================
 *
 *  Header only, and deliberately free of stdio, malloc and any host assumption, so the same code
 *  compiles for this desktop, for a Luckfox over the ARM cross compiler, and for a Teensy inside
 *  the Arduino build. That matters more than it sounds: the whole claim is that a frame split
 *  across different boards stitches exactly, and it only means anything if every board is running
 *  the identical rasteriser rather than a port of it.
 *
 *  The caller supplies the buffers and the band:
 *
 *      scene_setup(fb, zbuf, rowpix, W, H, BY0, BY1);
 *      long pixels = scene_frame(frame_index);
 *
 *  fb and zbuf hold (BY1 - BY0) rows, not H. A node that owns a tenth of the picture allocates a
 *  tenth of the memory, which is what lets a board with 8 MB take part at all.
 *
 *  rowpix may be NULL. When it is not, it accumulates pixels written per row across every frame,
 *  which is the only honest basis for deciding where to cut the bands.
 * ======================================================================================== */

#ifndef MACHINE_SCENE_H
#define MACHINE_SCENE_H

#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct { float x, y, z; } vec3;

static uint16_t *fb, *zbuf;
static int W, H;

/* The band of rows this node is responsible for. A node that owns rows [BY0,BY1) allocates
 * exactly that many rows, not the whole frame -- which is the difference between a board with
 * 8 MB being able to help and not. Default 0..H is the whole frame, one node doing all of it. */
static int BY0, BY1;

/* Pixels written per row, accumulated over every frame of the orbit. This is the only honest
 * basis for splitting the frame: rows are not equal, and on this scene most of them are empty. */
static long *rowpix;

/* ---------------------------------------------------------------------------------------------
 *  the machine being drawn, every figure measured on the bench
 * ------------------------------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int   count;
    float mb;          /* memory per board                                     */
    float rate;        /* effective MB/s on 4-bit weights                       */
    int   layers;      /* what the planner gives it                             */
    float r, g, b;     /* colour: warm for weight carriers, cool for the rest    */
} part_t;

static part_t PARTS[] = {
    /*  name         n     MB     MB/s   layers   colour                  */
    { "Zynq PL",     2, 1024.0f, 2290.0f, 16, 1.00f, 0.35f, 0.10f },  /* the engine        */
    { "Lyra Ultra",  2,  400.0f,  170.7f,  2, 0.95f, 0.70f, 0.15f },  /* capacity + spine  */
    { "Pico",        5,   13.0f,   57.5f,  0, 0.15f, 0.55f, 0.95f },  /* graphics          */
    { "Teensy 4.1",  9,   16.0f,   21.4f,  0, 0.20f, 0.80f, 0.70f },  /* real time         */
    { "ESP32-S3",   15,    8.0f,    9.2f,  0, 0.45f, 0.35f, 0.85f },  /* radios, sensors   */
};
#define NPARTS (int)(sizeof(PARTS) / sizeof(PARTS[0]))

/* ---------------------------------------------------------------------------------------------
 *  rasteriser: integer edge functions, stepped one addition per pixel
 * ------------------------------------------------------------------------------------------ */

static long tri(float ax, float ay, float az, float bx, float by, float bz,
                float cx, float cy, float cz, float r, float g, float b)
{
    const int x0 = (int)ax, y0 = (int)ay, x1 = (int)bx, y1 = (int)by, x2 = (int)cx, y2 = (int)cy;
    const int area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area <= 0) return 0;

    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (minx < 0) minx = 0;
    if (miny < BY0) miny = BY0;
    if (maxx >= W) maxx = W - 1;
    if (maxy >= BY1) maxy = BY1 - 1;
    if (minx > maxx || miny > maxy) return 0;

    /* Named eA01/eB01 rather than A01/B01 because Arduino's binary.h defines B01, B12 and B20 as
     * macros for binary literals, and the collision only shows up when this header is compiled for
     * a Teensy: "expected unqualified-id before numeric constant". */
    const int eA01 = y0 - y1, eB01 = x1 - x0;
    const int eA12 = y1 - y2, eB12 = x2 - x1;
    const int eA20 = y2 - y0, eB20 = x0 - x2;
    int w0r = (x1 - minx) * (y2 - miny) - (y1 - miny) * (x2 - minx);
    int w1r = (x2 - minx) * (y0 - miny) - (y2 - miny) * (x0 - minx);
    int w2r = (x0 - minx) * (y1 - miny) - (y0 - miny) * (x1 - minx);

    const float ia = 1.0f / (float)area;
    const uint16_t za = (uint16_t)(az * 65535.0f), zb2 = (uint16_t)(bz * 65535.0f),
                   zc = (uint16_t)(cz * 65535.0f);
    const int ri = (int)(r * 31.0f), gi = (int)(g * 63.0f), bi = (int)(b * 31.0f);
    const uint16_t col = (uint16_t)((ri << 11) | (gi << 5) | bi);

    long n = 0;
    for (int y = miny; y <= maxy; y++) {
        int w0 = w0r, w1 = w1r, w2 = w2r;
        uint16_t *frow = fb + (size_t)(y - BY0) * W, *zrow = zbuf + (size_t)(y - BY0) * W;
        for (int x = minx; x <= maxx; x++) {
            if ((w0 | w1 | w2) >= 0) {
                const uint16_t z = (uint16_t)(((float)w0 * za + (float)w1 * zb2 + (float)w2 * zc) * ia);
                if (z < zrow[x]) { zrow[x] = z; frow[x] = col; n++; rowpix[y]++; }
            }
            w0 += eA12; w1 += eA20; w2 += eA01;
        }
        w0r += eB12; w1r += eB20; w2r += eB01;
    }
    return n;
}

/* Camera: a fixed orbit, so successive frames differ and nothing can be cached between them. */
static float ca, sa;
static void project(vec3 p, float *sx, float *sy, float *sz)
{
    const float x = p.x * ca - p.z * sa;
    const float z = p.x * sa + p.z * ca + 6.0f;
    const float y = p.y;
    const float iw = 1.0f / (z > 0.05f ? z : 0.05f);
    *sx = (x * 1.4f * iw * 0.5f + 0.5f) * (float)W;
    *sy = (0.5f - y * 1.4f * iw * 0.5f) * (float)H;
    *sz = z * 0.08f;
    if (*sz < 0.0f) *sz = 0.0f;
    if (*sz > 1.0f) *sz = 1.0f;
}

/* One board, as a box. Twelve triangles, each face shaded differently so the form reads. */
static long box(float cx, float cy, float cz, float w, float h, float d, float r, float g, float b)
{
    const float X[2] = { cx - w, cx + w }, Y[2] = { cy - h, cy + h }, Z[2] = { cz - d, cz + d };
    float px[8], py[8], pz[8];
    for (int i = 0; i < 8; i++) {
        vec3 v = { X[i & 1], Y[(i >> 1) & 1], Z[(i >> 2) & 1] };
        project(v, &px[i], &py[i], &pz[i]);
    }
    /* BINNING.
     *
     * A node that owns rows [BY0,BY1) still paid to set up all twenty-four triangles of every box,
     * band or no band. Measured on a Luckfox: 159 us a frame of pure setup, which is a ceiling of
     * 6274 fps no matter how many boards join in. Splitting rows without splitting geometry stops
     * scaling as soon as that term dominates.
     *
     * The eight corners are already projected. Their y-range is the box's y-range, so one compare
     * against the band throws away all twenty-four triangles at once. */
    /* Only worth doing when there is something to reject. On one node owning the whole frame the
     * scan never rejects anything and costs 7% (measured: 345 fps down to 321). */
    if (BY1 - BY0 < H) {
    float ylo = py[0], yhi = py[0];
    for (int i = 1; i < 8; i++) {
        if (py[i] < ylo) ylo = py[i];
        if (py[i] > yhi) yhi = py[i];
    }
    /* Two pixels of margin, because tri() truncates its vertices toward zero: a corner at
     * y = -0.5 is outside the band by this test and lands on row 0 once it is an int. Without
     * the margin the bands stop stitching, which is exactly how this was caught. */
    if (yhi < (float)BY0 - 2.0f || ylo > (float)(BY1 - 1) + 2.0f) return 0;
    }

    static const int F[6][4] = {
        {0,1,3,2}, {4,6,7,5}, {0,4,5,1}, {2,3,7,6}, {0,2,6,4}, {1,5,7,3}
    };
    static const float SHADE[6] = { 1.00f, 0.55f, 0.75f, 0.90f, 0.65f, 0.85f };
    long n = 0;
    for (int f = 0; f < 6; f++) {
        const float s = SHADE[f];
        const int *q = F[f];
        /* Both windings, because the orbit shows every face from both sides and a backface test that
         * culls the one you are looking at draws a hole instead of a box. */
        n += tri(px[q[0]], py[q[0]], pz[q[0]], px[q[1]], py[q[1]], pz[q[1]],
                 px[q[2]], py[q[2]], pz[q[2]], r * s, g * s, b * s);
        n += tri(px[q[0]], py[q[0]], pz[q[0]], px[q[2]], py[q[2]], pz[q[2]],
                 px[q[1]], py[q[1]], pz[q[1]], r * s, g * s, b * s);
        n += tri(px[q[0]], py[q[0]], pz[q[0]], px[q[2]], py[q[2]], pz[q[2]],
                 px[q[3]], py[q[3]], pz[q[3]], r * s, g * s, b * s);
        n += tri(px[q[0]], py[q[0]], pz[q[0]], px[q[3]], py[q[3]], pz[q[3]],
                 px[q[2]], py[q[2]], pz[q[2]], r * s, g * s, b * s);
    }
    return n;
}

/* ---------------------------------------------------------------------------------------------
 *  the two calls a host makes
 * ------------------------------------------------------------------------------------------ */

static void scene_setup(uint16_t *frame, uint16_t *depth, long *profile,
                        int width, int height, int y0, int y1)
{
    fb = frame; zbuf = depth; rowpix = profile;
    W = width; H = height; BY0 = y0; BY1 = y1;
}

/* One frame of the orbit into the band. Returns pixels that passed the depth test. */
static long scene_frame(int f)
{
    long pix = 0;
    const int BH = BY1 - BY0;
    const float ang = (float)f * 0.045f;
    ca = cosf(ang);
    sa = sinf(ang);
    memset(fb, 0, (size_t)W * BH * 2);
    memset(zbuf, 0xFF, (size_t)W * BH * 2);

    /* The token's position in the pipeline, which is what makes it a live view rather than a
     * diagram: it walks the weight-carrying nodes in order, one stage per few frames. */
    const int stage_now = (f / 6) % 18;

    int slot = 0, weight_slot = 0;
    for (int i = 0; i < NPARTS; i++) {
        for (int c = 0; c < PARTS[i].count; c++, slot++) {
            /* Laid out in a grid: a shelf of boards, six to a row. */
            const float gx = ((float)(slot % 6) - 2.5f) * 0.62f;
            const float gy = 1.25f - (float)(slot / 6) * 0.62f;
            /* Height carries memory, on a log scale, or 1 GB would dwarf 8 MB into nothing. */
            const float hgt = 0.05f + 0.16f * logf(1.0f + PARTS[i].mb) / logf(1025.0f) * 2.2f;

            float r = PARTS[i].r, g = PARTS[i].g, b = PARTS[i].b;
            if (PARTS[i].layers > 0) {
                /* A weight carrier lights up as the token passes through it. */
                if (weight_slot == stage_now % 4) { r = 1.0f; g = 1.0f; b = 0.6f; }
                weight_slot++;
            }
            pix += box(gx, gy - hgt * 0.5f, 0.0f, 0.26f, hgt, 0.26f, r, g, b);
        }
    }
    return pix;
}

#endif /* MACHINE_SCENE_H */
