/* ===========================================================================================
 *  fractal_train.c -- training with no dataset, and a capacity meter that never runs out
 * ===========================================================================================
 *
 *  A small network learns to draw the Mandelbrot set: input a coordinate, output how deep that
 *  point escapes. Nothing is loaded from disk. The ground truth is an equation, so every node can
 *  make its own labels, for free, forever.
 *
 *
 *  WHY THIS WORKLOAD SUITS THIS MACHINE WHEN THE LANGUAGE MODEL DOES NOT
 *  ----------------------------------------------------------------------
 *  Measured earlier: one transformer layer of a 3B model is 49.2 MB, and a Luckfox has 13 MB and a
 *  Teensy 16. Not one microcontroller in the fleet can hold a single layer, so on that workload the
 *  208 small boards are traffic control and the FPGAs do everything.
 *
 *  This network is about 35,000 parameters. **Every node can hold the whole thing.** That changes
 *  the shape of the problem completely:
 *
 *      language model    pipeline parallel, one slice of layers per node, activations on the wire
 *      this             data parallel, the whole model on every node, gradients on the wire
 *
 *  And the data problem disappears. Storage on this machine is 2.2 TB at 17 MB/s -- capacity without
 *  bandwidth -- so feeding a dataset to two hundred nodes would be the bottleneck before any
 *  arithmetic happened. Here a node generates its own batch from the escape-time formula in a few
 *  dozen multiplies per sample. Zero bytes read, zero bytes sent.
 *
 *
 *  WHY A FRACTAL AND NOT A PICTURE
 *  --------------------------------
 *  The Mandelbrot boundary has structure at every scale, without limit. A network of a given size
 *  fits it down to some level of detail and no further, and zooming past that shows the blur
 *  immediately. So "how far can it zoom before it smears" is a direct, visible measure of how much a
 *  given number of parameters can hold -- and it never saturates, which a fixed dataset always does.
 *
 *  For a machine whose binding constraint is capacity, a capacity meter that cannot be exhausted is
 *  worth more than another benchmark that gets a perfect score and stops telling you anything.
 *
 *
 *  FOURIER FEATURES, WHICH ARE NOT OPTIONAL
 *  -----------------------------------------
 *  Feeding raw (x, y) into an MLP produces a blurry blob no matter how big the network is: a stack of
 *  smooth activations is heavily biased toward low frequencies, and the Mandelbrot boundary is all
 *  high frequency. Mapping the coordinate through sines and cosines at doubling frequencies first
 *  gives the network something high-frequency to combine, and the same network then resolves fine
 *  detail. It is the difference between the thing working and not working, so it is measurable here:
 *  run with --raw to see the blob.
 *
 *  RUN
 *      fractal_train [--hidden N] [--layers N] [--steps N] [--zoom Z] [--raw] [--out file.ppm]
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

#define MAX_LAYERS   8
#define MAX_HIDDEN   512
#define FOURIER_BANDS 8
#define MAX_IN       (4 * FOURIER_BANDS + 2)   /* sin/cos for x and y at each band, plus raw x,y */
#define BATCH        256
#define MAX_ITER     256

/* ---------------------------------------------------------------------------------------------
 *  the ground truth, which costs nothing to make
 * ------------------------------------------------------------------------------------------ */

/* Smooth escape time in [0,1]. The smooth part matters: plain integer iteration counts give the
 * network a staircase to fit, and it spends its capacity on the steps rather than on the shape. */
static float mandel(double cx, double cy)
{
    double zx = 0.0, zy = 0.0;
    int i = 0;
    for (; i < MAX_ITER; i++) {
        const double zx2 = zx * zx, zy2 = zy * zy;
        if (zx2 + zy2 > 4.0) break;
        const double t = zx2 - zy2 + cx;
        zy = 2.0 * zx * zy + cy;
        zx = t;
    }
    if (i >= MAX_ITER) return 1.0f;
    /* Continuous escape: how far past the radius it went on the step it left. */
    const double mag = sqrt(zx * zx + zy * zy);
    const double nu = (double)i + 1.0 - log(log(mag > 1.0 ? mag : 1.0000001) / log(2.0)) / log(2.0);
    double v = nu / (double)MAX_ITER;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return (float)v;
}

/* ---------------------------------------------------------------------------------------------
 *  the network
 * ------------------------------------------------------------------------------------------ */

typedef struct {
    int n_layers;                 /* hidden layers, not counting the output */
    int hidden;
    int n_in;
    int raw_input;                /* skip the Fourier features, to show why they exist */

    /* Weights, biases, and Adam's two moments for each. Flat arrays, one block per layer. */
    float *w[MAX_LAYERS + 1], *b[MAX_LAYERS + 1];
    float *mw[MAX_LAYERS + 1], *vw[MAX_LAYERS + 1];
    float *mb[MAX_LAYERS + 1], *vb[MAX_LAYERS + 1];
    int    rows[MAX_LAYERS + 1], cols[MAX_LAYERS + 1];

    /* Activations and gradients for one sample. */
    float a[MAX_LAYERS + 2][MAX_HIDDEN];
    float d[MAX_LAYERS + 2][MAX_HIDDEN];
    long   n_params;
} net_t;

static uint32_t rng_state = 12345u;
static float frand(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)((rng_state >> 8) & 0xFFFFFF) / 16777216.0f;
}

static void encode(const net_t *n, double x, double y, float *out)
{
    if (n->raw_input) { out[0] = (float)x; out[1] = (float)y; return; }
    int k = 0;
    /* Doubling frequencies: each band resolves detail twice as fine as the one before, which is the
     * same way the fractal is built. */
    for (int band = 0; band < FOURIER_BANDS; band++) {
        const double f = (double)(1 << band) * 3.14159265358979;
        out[k++] = (float)sin(f * x);
        out[k++] = (float)cos(f * x);
        out[k++] = (float)sin(f * y);
        out[k++] = (float)cos(f * y);
    }
    out[k++] = (float)x;
    out[k++] = (float)y;
}

static void net_init(net_t *n, int hidden, int layers, int raw)
{
    memset(n, 0, sizeof(*n));
    n->hidden = hidden;
    n->n_layers = layers;
    n->raw_input = raw;
    n->n_in = raw ? 2 : MAX_IN;

    for (int l = 0; l <= layers; l++) {
        const int in = (l == 0) ? n->n_in : hidden;
        const int out = (l == layers) ? 1 : hidden;
        n->rows[l] = out;
        n->cols[l] = in;
        const size_t nw = (size_t)out * in;
        n->w[l]  = (float *)calloc(nw, sizeof(float));
        n->mw[l] = (float *)calloc(nw, sizeof(float));
        n->vw[l] = (float *)calloc(nw, sizeof(float));
        n->b[l]  = (float *)calloc(out, sizeof(float));
        n->mb[l] = (float *)calloc(out, sizeof(float));
        n->vb[l] = (float *)calloc(out, sizeof(float));
        /* He initialisation, because the activation is ReLU: variance 2/fan_in keeps the signal from
         * shrinking to nothing by the last layer. */
        const float s = sqrtf(2.0f / (float)in);
        for (size_t i = 0; i < nw; i++) {
            /* Box-Muller from two uniforms, so the spread is normal rather than flat. */
            const float u1 = frand() + 1e-7f, u2 = frand();
            n->w[l][i] = sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2) * s;
        }
        n->n_params += (long)nw + out;
    }
}

static float net_forward(net_t *n, const float *in)
{
    memcpy(n->a[0], in, (size_t)n->n_in * sizeof(float));
    for (int l = 0; l <= n->n_layers; l++) {
        const int rows = n->rows[l], cols = n->cols[l];
        const float *w = n->w[l];
        for (int r = 0; r < rows; r++) {
            float s = n->b[l][r];
            const float *wr = w + (size_t)r * cols;
            for (int c = 0; c < cols; c++) s += wr[c] * n->a[l][c];
            /* ReLU everywhere but the output, which stays linear so it can reach any value. */
            n->a[l + 1][r] = (l == n->n_layers) ? s : (s > 0.0f ? s : 0.0f);
        }
    }
    return n->a[n->n_layers + 1][0];
}

/* Backpropagation, accumulating into the Adam moments directly. */
static void net_backward(net_t *n, float dloss, float lr, float beta1, float beta2, int t)
{
    n->d[n->n_layers + 1][0] = dloss;

    for (int l = n->n_layers; l >= 0; l--) {
        const int rows = n->rows[l], cols = n->cols[l];
        for (int c = 0; c < cols; c++) n->d[l][c] = 0.0f;

        for (int r = 0; r < rows; r++) {
            const float g = n->d[l + 1][r];
            if (g == 0.0f) continue;
            float *wr = n->w[l] + (size_t)r * cols;
            float *mwr = n->mw[l] + (size_t)r * cols;
            float *vwr = n->vw[l] + (size_t)r * cols;
            for (int c = 0; c < cols; c++) {
                n->d[l][c] += wr[c] * g;
                const float gw = g * n->a[l][c];
                mwr[c] = beta1 * mwr[c] + (1.0f - beta1) * gw;
                vwr[c] = beta2 * vwr[c] + (1.0f - beta2) * gw * gw;
                const float mh = mwr[c] / (1.0f - powf(beta1, (float)t));
                const float vh = vwr[c] / (1.0f - powf(beta2, (float)t));
                wr[c] -= lr * mh / (sqrtf(vh) + 1e-8f);
            }
            n->mb[l][r] = beta1 * n->mb[l][r] + (1.0f - beta1) * g;
            n->vb[l][r] = beta2 * n->vb[l][r] + (1.0f - beta2) * g * g;
            const float mh = n->mb[l][r] / (1.0f - powf(beta1, (float)t));
            const float vh = n->vb[l][r] / (1.0f - powf(beta2, (float)t));
            n->b[l][r] -= lr * mh / (sqrtf(vh) + 1e-8f);
        }
        /* Through the ReLU: a unit that was clamped off passes no gradient back. */
        if (l > 0)
            for (int c = 0; c < cols; c++)
                if (n->a[l][c] <= 0.0f) n->d[l][c] = 0.0f;
    }
}

/* ---------------------------------------------------------------------------------------------
 *  rendering, so the result can be looked at rather than described
 * ------------------------------------------------------------------------------------------ */

static void render(net_t *n, const char *path, int W, int H,
                   double cx, double cy, double span, int truth)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);

    float in[MAX_IN];
    for (int py = 0; py < H; py++) {
        for (int px = 0; px < W; px++) {
            const double x = cx + span * ((double)px / W - 0.5);
            const double y = cy + span * ((double)py / H - 0.5) * ((double)H / W);
            float v;
            if (truth) {
                v = mandel(x, y);
            } else {
                /* The network sees the same normalised coordinate it trained on. */
                encode(n, x, y, in);
                v = net_forward(n, in);
            }
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            const unsigned char r = (unsigned char)(255.0f * v);
            const unsigned char g = (unsigned char)(255.0f * v * v);
            const unsigned char b = (unsigned char)(255.0f * sqrtf(v));
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    int hidden = 128, layers = 3, steps = 20000, raw = 0;
    double zoom = 1.0;
    const char *out = "fractal.ppm";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hidden") && i + 1 < argc) hidden = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--layers") && i + 1 < argc) layers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) zoom = atof(argv[++i]);
        else if (!strcmp(argv[i], "--raw")) raw = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    }
    if (hidden > MAX_HIDDEN) hidden = MAX_HIDDEN;
    if (layers > MAX_LAYERS) layers = MAX_LAYERS;

    /* The window being learned. Zoom shrinks it around a point on the boundary, where the detail is. */
    const double cx = -0.743643887037151, cy = 0.13182590420533;
    const double span = 3.0 / zoom;

    net_t net;
    net_init(&net, hidden, layers, raw);

    printf("\n========================================================================\n");
    printf("  learning the Mandelbrot set with no dataset\n");
    printf("========================================================================\n");
    printf("  %d hidden layers of %d, input %s\n", layers, hidden,
           raw ? "RAW (x,y) -- expect a blur" : "Fourier features at 8 bands");
    printf("  %ld parameters, %.2f MB at float32\n", net.n_params, net.n_params * 4.0 / 1048576.0);
    printf("  window %.3g wide at zoom %.0fx\n", span, zoom);
    printf("  every label is computed, never loaded: 0 bytes of dataset\n\n");

    float in[MAX_IN];
    double t0 = now_s();
    double run = 0.0;
    long samples = 0;

    for (int step = 1; step <= steps; step++) {
        double loss = 0.0;
        for (int b = 0; b < BATCH; b++) {
            const double x = cx + span * ((double)frand() - 0.5);
            const double y = cy + span * ((double)frand() - 0.5);
            const float target = mandel(x, y);
            encode(&net, x, y, in);
            const float pred = net_forward(&net, in);
            const float err = pred - target;
            loss += (double)err * err;
            /* Mean squared error, so the gradient at the output is 2*err/BATCH. */
            net_backward(&net, 2.0f * err / BATCH, 0.002f, 0.9f, 0.999f, step);
            samples++;
        }
        run = 0.98 * run + 0.02 * (loss / BATCH);
        if (step % 2000 == 0 || step == 1) {
            printf("  step %6d   loss %.6f   %.0f samples/s\n",
                   step, run, samples / (now_s() - t0));
            fflush(stdout);
        }
    }

    const double secs = now_s() - t0;
    printf("\n  trained %ld samples in %.1f s, %.0f a second\n", samples, secs, samples / secs);

    render(&net, out, 512, 384, cx, cy, span, 0);
    char tpath[512];
    snprintf(tpath, sizeof(tpath), "%s.truth.ppm", out);
    render(&net, tpath, 512, 384, cx, cy, span, 1);
    printf("  wrote %s and %s -- open both and compare\n", out, tpath);

    /* The capacity question, asked directly: how well does it hold up when asked for detail it was
     * never shown? A fixed dataset cannot ask this, because there is nothing past the last example. */
    double held = 0.0;
    const int CHECK = 4096;
    for (int i = 0; i < CHECK; i++) {
        const double x = cx + span * ((double)frand() - 0.5);
        const double y = cy + span * ((double)frand() - 0.5);
        encode(&net, x, y, in);
        const float e = net_forward(&net, in) - mandel(x, y);
        held += (double)e * e;
    }
    printf("  held-out error %.6f over %d fresh points\n", held / CHECK, CHECK);
    printf("\n  Raise --zoom and the same network has more detail to fit in the same parameters.\n");
    printf("  The point where the error stops falling is this network's capacity, measured rather\n");
    printf("  than guessed, and the target never runs out of detail to demand.\n\n");
    return 0;
}
