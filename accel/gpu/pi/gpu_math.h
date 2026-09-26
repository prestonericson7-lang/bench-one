/*
 * gpu_math.h -- small float math for the Pi tools: column-major 4x4 matrices (OpenGL layout,
 * m[c*4 + r]), vectors, projection / view helpers.
 *
 * m4_mul uses the summation order of SPEC section 6 and of the Teensy firmware's mat_mul
 * (((a0*b0 + a1*b1) + a2*b2) + a3*b3); compiled with -ffp-contract=off its results are
 * bit-identical to what the Teensy computes (used to predict autonomous-mode frames exactly).
 * m4_rotate is the firmware's Rodrigues rotation (float, angle reduced modulo 2*pi in double).
 */
#ifndef GPU_MATH_H
#define GPU_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define GM_PI 3.14159265358979323846

typedef struct { float x, y, z; } v3f;

v3f   v3_make(float x, float y, float z);
v3f   v3_add(v3f a, v3f b);
v3f   v3_sub(v3f a, v3f b);
v3f   v3_scale(v3f a, float s);
float v3_dot(v3f a, v3f b);
v3f   v3_cross(v3f a, v3f b);
float v3_len(v3f a);
v3f   v3_norm(v3f a);                  /* zero vector stays zero */

void m4_identity(float m[16]);
void m4_copy(float o[16], const float a[16]);
void m4_mul(float o[16], const float a[16], const float b[16]);      /* o = a * b (o may alias) */
void m4_translate(float m[16], float x, float y, float z);           /* m = T */
void m4_scale(float m[16], float sx, float sy, float sz);            /* m = S */
void m4_rotate(float m[16], const float axis[3], double angle);      /* m = R(axis, angle), firmware-exact */
void m4_rot_x(float m[16], float a);
void m4_rot_y(float m[16], float a);
void m4_rot_z(float m[16], float a);
/* OpenGL-style perspective (clip z in [-w, w]); fovy in radians */
void m4_perspective(float m[16], float fovy, float aspect, float znear, float zfar);
void m4_look_at(float m[16], v3f eye, v3f center, v3f up);
/* out = m * (x, y, z, w) */
void m4_xform(const float m[16], float x, float y, float z, float w, float out[4]);

#ifdef __cplusplus
}
#endif
#endif
