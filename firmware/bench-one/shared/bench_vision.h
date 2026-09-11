/* ===========================================================================================
 *  bench_vision.h -- turning what a camera sees into something the machine can think about
 * ===========================================================================================
 *
 *  THE JOB
 *  --------
 *  Everything above this file reasons about hypervectors. Nothing so far produces one from an
 *  image. This is that bridge, and it is the piece that decides whether the whole machine is
 *  looking at anything real.
 *
 *
 *  WHY NOT RAW PIXELS
 *  -------------------
 *  Binding a random vector per pixel position to that pixel's brightness is the obvious
 *  encoding and it is close to useless: it makes two photographs of the same subject, shifted
 *  by three pixels or taken in different light, almost orthogonal. The machine would need to be
 *  shown every possible framing of a thing to recognise it, which is not recognition.
 *
 *  So the encoding is built from things that survive the camera moving:
 *
 *      GRADIENT ORIENTATION   which way an edge runs. Survives brightness and contrast
 *                             changes entirely -- an edge is an edge in sun or shade.
 *      LOCAL CONTRAST         how strong that edge is, quantised coarsely.
 *      COARSE POSITION        which region of the frame, on a small grid, so that "dark mass
 *                             low-left" is different from "dark mass top-right" without being
 *                             so precise that a small shift changes everything.
 *
 *  That is the same family of features as HOG, chosen for the same reason, but bound and
 *  bundled instead of concatenated -- so the result is one 8,192-bit vector that lives in the
 *  same space as every other concept the machine holds, and can be compared against a smell,
 *  a sound or a sentence with the identical Hamming distance.
 *
 *
 *  WHAT THIS HONESTLY CANNOT DO
 *  -----------------------------
 *  It will distinguish things with different SHAPE and different edge structure. It will not
 *  reliably tell a German Shepherd from a Belgian Malinois, because those differ in coat
 *  colour distribution and subtle proportion, and a hand-designed gradient histogram throws
 *  most of that away. A convolutional network learns features for exactly this reason.
 *
 *  What this machine has instead is the ability to be TOLD, once, that this is a Malinois, and
 *  to never confuse the two again in that context. Which is a different and in some ways more
 *  honest kind of competence -- it never invents a breed it was not taught.
 * ===========================================================================================
 */

#ifndef BENCH_VISION_H
#define BENCH_VISION_H

#include "bench_hdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Analysis grid. 8x8 cells over the frame: coarse enough that a small shift does not change
 * which cell an edge falls in, fine enough to keep the layout of a subject. */
#ifndef VIS_GRID
#define VIS_GRID 8
#endif

/* Gradient orientation bins. 8 = 45 degrees each, unsigned (0 and 180 are the same edge). */
#ifndef VIS_ORIENTS
#define VIS_ORIENTS 8
#endif

/* Contrast levels per cell. */
#ifndef VIS_MAGS
#define VIS_MAGS 4
#endif

/* The vocabulary: one random vector per (cell, orientation), plus level bases for magnitude.
 * Derived from a seed, so every node in the cluster builds an identical one and no vision
 * vocabulary is ever transmitted. */
typedef struct {
    hd_t cell[VIS_GRID * VIS_GRID];
    hd_t orient[VIS_ORIENTS];
    hd_t mag_base;
    uint32_t rng;
} bench_vision_t;

void vision_init(bench_vision_t *v, uint32_t seed);

/* Encode an 8-bit grayscale image. `w` and `h` may be anything; the image is sampled onto the
 * VIS_GRID x VIS_GRID analysis grid, so a 640x480 frame and a 64x48 thumbnail of the same scene
 * produce similar vectors.
 *
 * `scratch` is a caller-owned accumulator -- 16 KB, and never a stack local. */
void vision_encode_gray(const bench_vision_t *v, const uint8_t *img, uint16_t w, uint16_t h,
                        hd_acc_t *scratch, hd_t out);

/* Per-cell dominant orientation and magnitude, if the caller wants to see what the machine
 * actually extracted. `orient_out` and `mag_out` each hold VIS_GRID*VIS_GRID entries. */
void vision_features(const uint8_t *img, uint16_t w, uint16_t h,
                     uint8_t *orient_out, uint8_t *mag_out);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_VISION_H */
