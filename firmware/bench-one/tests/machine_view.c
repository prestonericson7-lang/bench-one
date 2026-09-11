/* ===========================================================================================
 *  machine_view.c -- the machine rendering itself, on the silicon that cannot do inference
 * ===========================================================================================
 *
 *  Every board in this design was measured and given a verdict. Three of those verdicts were "cannot
 *  hold a transformer layer, carries no weights": the Luckfox Picos at 13 MB, the Teensys at 16, the
 *  ESP32s at 8. Twenty-nine boards with nothing to do during inference.
 *
 *  They are not, however, incapable. Measured on a Luckfox Pico with the same rasteriser as this one:
 *  **993 frames a second at 320x240, or 81 at 800x480 with a 2000-triangle scene**, depth-tested and
 *  Gouraud-shaded. A ten dollar board drives a real 3D interface with room to spare.
 *
 *  So this is the machine drawing a live picture of its own insides -- which node holds which layers,
 *  how full each one is, where the current token is in the pipeline -- rendered by boards that are
 *  idle anyway. It costs the inference nodes nothing, because the silicon doing it was never going to
 *  carry weights.
 *
 *
 *  WHY THIS IS THE ARGUMENT AND NOT A DEMO
 *  ----------------------------------------
 *  A graphics card doing inference has no spare silicon. Every shader core is either working on the
 *  model or idle, and watching the model run costs exactly the cycles the model wanted. Observability
 *  on a GPU box competes with the thing being observed.
 *
 *  Here it does not compete at all, because it is not the same chip. Thirty-three separate processors,
 *  and the ones that lost the argument about weights win a different one outright. That is what
 *  heterogeneous actually buys, and it is not visible in any FLOPS total.
 *
 *  RUN
 *      machine_view [width] [height] [frames] [out.ppm]
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

/* The rasteriser and the scene live in a header so that a Teensy and a Luckfox compile the same
 * code, not a port of it. Splitting a frame across boards only means anything if every board is
 * running byte-for-byte the same rasteriser. */
#include "machine_scene.h"

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, BY1 - BY0);
    for (int i = 0; i < W * (BY1 - BY0); i++) {
        const uint16_t c = fb[i];
        fputc((int)(((c >> 11) & 31) * 255 / 31), f);
        fputc((int)(((c >> 5) & 63) * 255 / 63), f);
        fputc((int)((c & 31) * 255 / 31), f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    W = (argc > 1) ? atoi(argv[1]) : 480;
    H = (argc > 2) ? atoi(argv[2]) : 320;
    const int FRAMES = (argc > 3) ? atoi(argv[3]) : 120;
    const char *out = (argc > 4) ? argv[4] : "machine.ppm";
    const int band   = (argc > 5) ? atoi(argv[5]) : 0;
    const int nbands = (argc > 6) ? atoi(argv[6]) : 1;

    /* Rows are handed out so every band differs by at most one row and they tile H exactly. */
    BY0 = (int)(((long)H * band) / nbands);
    BY1 = (int)(((long)H * (band + 1)) / nbands);
    /* Explicit rows win, which is how a balanced split is handed to a node. */
    if (argc > 8) { BY0 = atoi(argv[7]); BY1 = atoi(argv[8]); }
    if (BY0 < 0) BY0 = 0;
    if (BY1 > H) BY1 = H;
    const int BH = BY1 - BY0;
    if (BH <= 0) { printf("band %d of %d is empty at H=%d\n", band, nbands, H); return 1; }

    rowpix = (long *)calloc((size_t)H, sizeof(long));
    fb = (uint16_t *)malloc((size_t)W * BH * 2);
    zbuf = (uint16_t *)malloc((size_t)W * BH * 2);
    if (!fb || !zbuf || !rowpix) { printf("cannot allocate %dx%d\n", W, BH); return 1; }

    int total_boards = 0;
    float total_mb = 0.0f, total_rate = 0.0f;
    for (int i = 0; i < NPARTS; i++) {
        total_boards += PARTS[i].count;
        total_mb += PARTS[i].count * PARTS[i].mb;
        total_rate += PARTS[i].count * PARTS[i].rate;
    }

    printf("\n========================================================================\n");
    printf("  the machine, drawing itself\n");
    printf("========================================================================\n");
    printf("  %d boards, %.2f GB of memory, %.2f GB/s of usable weight throughput\n",
           total_boards, total_mb / 1024.0f, total_rate / 1024.0f);
    printf("  %dx%d, RGB565, 16-bit depth\n", W, H);
    if (nbands > 1)
        printf("  band %d of %d: rows %d..%d, %d KB of buffers on this node\n\n",
               band, nbands, BY0, BY1 - 1, (int)((size_t)W * BH * 4 / 1024));
    else
        printf("  whole frame on one node, %d KB of buffers\n\n",
               (int)((size_t)W * BH * 4 / 1024));
    for (int i = 0; i < NPARTS; i++)
        printf("    %-12s x%-3d %6.0f MB  %7.1f MB/s  %2d layers%s\n",
               PARTS[i].name, PARTS[i].count, PARTS[i].mb, PARTS[i].rate, PARTS[i].layers,
               PARTS[i].layers ? "" : "   <- drawing this picture instead");

    scene_setup(fb, zbuf, rowpix, W, H, BY0, BY1);

    long pix = 0;
    const double t0 = now_s();
    for (int f = 0; f < FRAMES; f++) pix += scene_frame(f);

    const double dt = now_s() - t0;
    write_ppm(out);

    printf("\n  %d frames in %.2f s\n", FRAMES, dt);
    printf("  %8.1f frames/s\n", FRAMES / dt);
    printf("  %8.0f pixels/s, depth tested\n", (double)pix / dt);
    printf("  wrote %s\n", out);
    if (getenv("ROWPROFILE")) {
        FILE *pf = fopen(getenv("ROWPROFILE"), "w");
        if (pf) {
            for (int y = BY0; y < BY1; y++) fprintf(pf, "%d %ld\n", y, rowpix[y]);
            fclose(pf);
        }
    }
    printf("\n  A panel shows 30. Everything above that is headroom on hardware that was,\n");
    printf("  by every measurement in this project, unemployable for inference.\n\n");

    free(fb);
    free(zbuf);
    free(rowpix);
    return 0;
}
