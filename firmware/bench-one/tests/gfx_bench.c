/* ===========================================================================================
 *  gfx_bench.c -- the graphics pipeline, measured in the units GPUs are sold in
 * ===========================================================================================
 *
 *  A Teensy 4.1 runs TGX: a full software 3D renderer with perspective-correct texturing, Phong
 *  lighting and a z-buffer. That is not a demo, it is the same sequence of operations a graphics card
 *  performs, and it settles an argument this project kept having in the abstract. The gap between a
 *  microcontroller and a GPU is how many units run at once, not what those units do.
 *
 *  So this measures the pipeline itself, on whatever board it is run on, in the units the comparison
 *  is normally made in:
 *
 *      vertices/s    a 4x4 matrix times a position, 16 multiply-adds, then a perspective divide
 *      triangles/s   setup: edge functions, bounding box, backface test, gradient setup
 *      pixels/s      the inner loop: barycentric step, depth test, Gouraud interpolate, write
 *
 *  Same source on the desktop, a Luckfox and (with the ARM build) anything else with a libc, so the
 *  numbers are comparable rather than three different benchmarks wearing one name.
 *
 *
 *  WHY THIS BELONGS IN AN AI PROJECT
 *  ----------------------------------
 *  Measured earlier: nine Teensys come to 2.14 G MAC/s of float, which is 6% of one desktop and a
 *  rounding error against a GPU. They are not going to do inference and that verdict has not changed.
 *
 *  What they can do is everything the machine needs that is NOT inference. A rack of boards running a
 *  model wants a front panel: what each node holds, where the current token is in the pipeline, which
 *  board is hot, how full the KV cache is. That is a real-time 3D display of a machine's internals,
 *  it has to be driven by something, and driving it from the boards that are otherwise idle costs the
 *  inference exactly nothing.
 *
 *  This measures whether they are actually up to it, rather than assuming either way.
 *
 *  RUN
 *      gfx_bench [width] [height] [triangles]
 * ===========================================================================================
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
  static double now_s(void)
  {
      LARGE_INTEGER f, t;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&t);
      return (double)t.QuadPart / (double)f.QuadPart;
  }
#else
  #include <time.h>
  static double now_s(void)
  {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
  }
#endif

typedef struct { float x, y, z, w; } vec4;
typedef struct { float m[16]; } mat4;

static uint16_t *fb;      /* RGB565, which is what a small TFT actually takes */
static uint16_t *zb;      /* 16-bit depth, as TGX offers */
static int W, H;

/* ---------------------------------------------------------------------------------------------
 *  vertex stage: 16 multiply-adds and a divide, exactly what a vertex shader does
 * ------------------------------------------------------------------------------------------ */

static void mat4_mul_vec(const mat4 *m, const vec4 *v, vec4 *out)
{
    out->x = m->m[0]*v->x + m->m[4]*v->y + m->m[8]*v->z  + m->m[12]*v->w;
    out->y = m->m[1]*v->x + m->m[5]*v->y + m->m[9]*v->z  + m->m[13]*v->w;
    out->z = m->m[2]*v->x + m->m[6]*v->y + m->m[10]*v->z + m->m[14]*v->w;
    out->w = m->m[3]*v->x + m->m[7]*v->y + m->m[11]*v->z + m->m[15]*v->w;
}

/* ---------------------------------------------------------------------------------------------
 *  raster stage: edge functions, depth test, Gouraud interpolation
 *
 *  Integer edge functions rather than floating point, which is what every real rasteriser does: the
 *  three edges are linear, so they are set up once per triangle and stepped by an addition per pixel.
 *  A float evaluation per pixel would measure a naive renderer, not this workload.
 * ------------------------------------------------------------------------------------------ */

static long raster_tri(const vec4 *a, const vec4 *b, const vec4 *c,
                       uint16_t ca, uint16_t cb, uint16_t cc)
{
    const int x0 = (int)a->x, y0 = (int)a->y;
    const int x1 = (int)b->x, y1 = (int)b->y;
    const int x2 = (int)c->x, y2 = (int)c->y;

    const int area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area <= 0) return 0;                       /* backface, or degenerate: culled for free */

    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= W) maxx = W - 1;
    if (maxy >= H) maxy = H - 1;
    if (minx > maxx || miny > maxy) return 0;

    const int A01 = y0 - y1, B01 = x1 - x0;
    const int A12 = y1 - y2, B12 = x2 - x1;
    const int A20 = y2 - y0, B20 = x0 - x2;

    int w0_row = (x1 - minx) * (y2 - miny) - (y1 - miny) * (x2 - minx);
    int w1_row = (x2 - minx) * (y0 - miny) - (y2 - miny) * (x0 - minx);
    int w2_row = (x0 - minx) * (y1 - miny) - (y0 - miny) * (x1 - minx);

    const float inv_area = 1.0f / (float)area;
    const uint16_t za = (uint16_t)(a->z * 65535.0f);
    const uint16_t zbv = (uint16_t)(b->z * 65535.0f);
    const uint16_t zc = (uint16_t)(c->z * 65535.0f);

    long drawn = 0;
    for (int y = miny; y <= maxy; y++) {
        int w0 = w0_row, w1 = w1_row, w2 = w2_row;
        uint16_t *frow = fb + (size_t)y * W;
        uint16_t *zrow = zb + (size_t)y * W;
        for (int x = minx; x <= maxx; x++) {
            if ((w0 | w1 | w2) >= 0) {
                /* Barycentric weights, normalised once, then used for depth and colour. */
                const float l0 = (float)w0 * inv_area;
                const float l1 = (float)w1 * inv_area;
                const float l2 = (float)w2 * inv_area;
                const uint16_t z = (uint16_t)(l0 * za + l1 * zbv + l2 * zc);
                if (z < zrow[x]) {
                    zrow[x] = z;
                    /* Gouraud: interpolate the three channels separately, because RGB565 packs them
                     * and blending the packed value would bleed one channel into the next. */
                    const int r = (int)(l0 * (ca >> 11) + l1 * (cb >> 11) + l2 * (cc >> 11));
                    const int g = (int)(l0 * ((ca >> 5) & 63) + l1 * ((cb >> 5) & 63) + l2 * ((cc >> 5) & 63));
                    const int bl = (int)(l0 * (ca & 31) + l1 * (cb & 31) + l2 * (cc & 31));
                    frow[x] = (uint16_t)((r << 11) | (g << 5) | bl);
                    drawn++;
                }
            }
            w0 += A12; w1 += A20; w2 += A01;
        }
        w0_row += B12; w1_row += B20; w2_row += B01;
    }
    return drawn;
}

int main(int argc, char **argv)
{
    W = (argc > 1) ? atoi(argv[1]) : 320;
    H = (argc > 2) ? atoi(argv[2]) : 240;
    const int NTRI = (argc > 3) ? atoi(argv[3]) : 2000;

    fb = (uint16_t *)malloc((size_t)W * H * 2);
    zb = (uint16_t *)malloc((size_t)W * H * 2);
    if (!fb || !zb) { printf("cannot allocate a %dx%d framebuffer\n", W, H); return 1; }

    printf("\n========================================================================\n");
    printf("  the 3D pipeline, on this board\n");
    printf("========================================================================\n");
    printf("  %dx%d, RGB565 with a 16-bit depth buffer, %.0f KB of buffers\n",
           W, H, (double)W * H * 4 / 1024.0);

    /* A REAL model-view-projection matrix, not an arbitrary one.
     *
     * The first version filled all sixteen entries with nonzero junk so nothing could be optimised
     * away. It also made w vary wildly, so almost every triangle landed off-screen or behind the
     * camera: 14 million triangles a second and 0.05 pixels each. That measures the culling test, not
     * the rasteriser, and a graphics benchmark that never fills a pixel is not one.
     *
     * This is a plain perspective projection with the model pushed back along z: rotation and scale
     * in the top-left, translation in the last column, and w picked up from z the way perspective
     * actually works. */
    mat4 mvp;
    memset(&mvp, 0, sizeof(mvp));
    const float fov = 1.2f, zn = 0.1f, zf = 10.0f;
    mvp.m[0]  = fov;
    mvp.m[5]  = fov;
    mvp.m[10] = (zf + zn) / (zf - zn);
    mvp.m[11] = 1.0f;                      /* w = z, which is what makes it perspective */
    mvp.m[14] = -2.0f * zf * zn / (zf - zn);
    mvp.m[12] = 0.0f; mvp.m[13] = 0.0f;

    /* ---- vertex stage alone ---------------------------------------------------------------- */
    {
        const int NV = 200000;
        vec4 v = { 0.4f, -0.7f, 2.5f, 1.0f }, o;
        volatile float sink = 0.0f;
        const double t0 = now_s();
        for (int i = 0; i < NV; i++) {
            v.x = 0.4f + (float)(i & 255) * 0.001f;
            mat4_mul_vec(&mvp, &v, &o);
            const float iw = 1.0f / (o.w != 0.0f ? o.w : 1.0f);   /* the perspective divide */
            sink += o.x * iw + o.y * iw + o.z * iw;
        }
        const double dt = now_s() - t0;
        if (sink == 1.5f) printf(" ");
        printf("\n  vertex transform   %10.0f vertices/s  (16 MACs and a divide each)\n", NV / dt);
        printf("  which is           %10.3f G MAC/s in the vertex stage alone\n",
               (double)NV * 16 / dt / 1e9);
    }

    /* ---- full pipeline: transform, cull, rasterise ------------------------------------------ */
    uint32_t rs = 7777u;
    const double t0 = now_s();
    long pixels = 0;
    int frames = 0;

    for (frames = 0; frames < 20; frames++) {
        memset(fb, 0, (size_t)W * H * 2);
        memset(zb, 0xFF, (size_t)W * H * 2);
        for (int t = 0; t < NTRI; t++) {
            /* One random point, then two neighbours close to it, so each triangle covers a
             * plausible number of pixels instead of spanning the whole screen or nothing at all.
             * Real geometry is locally connected; random independent vertices are not. */
            vec4 p[3], o[3];
            rs = rs * 1103515245u + 12345u;
            const float bx = (float)((rs >> 16) % 1000) / 1000.0f - 0.5f;
            rs = rs * 1103515245u + 12345u;
            const float by = (float)((rs >> 16) % 1000) / 1000.0f - 0.5f;
            rs = rs * 1103515245u + 12345u;
            const float bz = 1.5f + (float)((rs >> 16) % 1000) / 1000.0f;
            for (int k = 0; k < 3; k++) {
                rs = rs * 1103515245u + 12345u;
                p[k].x = bx + ((float)((rs >> 16) % 200) / 1000.0f - 0.1f);
                rs = rs * 1103515245u + 12345u;
                p[k].y = by + ((float)((rs >> 16) % 200) / 1000.0f - 0.1f);
                p[k].z = bz;
                p[k].w = 1.0f;
                mat4_mul_vec(&mvp, &p[k], &o[k]);
                const float iw = 1.0f / (o[k].w != 0.0f ? o[k].w : 1.0f);
                /* Viewport transform: clip space to pixels. */
                o[k].x = (o[k].x * iw * 0.5f + 0.5f) * (float)W;
                o[k].y = (o[k].y * iw * 0.5f + 0.5f) * (float)H;
                o[k].z = o[k].z * iw * 0.5f + 0.5f;
                if (o[k].z < 0.0f) o[k].z = 0.0f;
                if (o[k].z > 1.0f) o[k].z = 1.0f;
            }
            pixels += raster_tri(&o[0], &o[1], &o[2], 0xF800, 0x07E0, 0x001F);
        }
    }
    const double dt = now_s() - t0;

    printf("\n  full pipeline, %d triangles a frame, %d frames\n", NTRI, frames);
    printf("  %10.1f frames/s\n", frames / dt);
    printf("  %10.0f triangles/s\n", (double)frames * NTRI / dt);
    printf("  %10.0f pixels/s written (depth-tested and Gouraud shaded)\n", (double)pixels / dt);
    printf("  %10.1f pixels per triangle on average\n", (double)pixels / ((double)frames * NTRI));

    printf("\n  A small TFT panel is 320x240 at 30 fps. Anything above that number drives a live\n");
    printf("  display of this machine's own internals -- which node holds which layer, where the\n");
    printf("  token is in the pipeline, what is hot -- on hardware that is doing nothing else.\n\n");

    free(fb);
    free(zb);
    return 0;
}
