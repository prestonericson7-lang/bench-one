/* gpu_math.c -- see gpu_math.h. Compile with -ffp-contract=off. */
#include "gpu_math.h"
#include <math.h>
#include <string.h>

v3f v3_make(float x, float y, float z) { v3f v; v.x = x; v.y = y; v.z = z; return v; }
v3f v3_add(v3f a, v3f b) { return v3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
v3f v3_sub(v3f a, v3f b) { return v3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
v3f v3_scale(v3f a, float s) { return v3_make(a.x * s, a.y * s, a.z * s); }
float v3_dot(v3f a, v3f b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }
v3f v3_cross(v3f a, v3f b) { return v3_make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float v3_len(v3f a) { return sqrtf(v3_dot(a, a)); }

v3f v3_norm(v3f a)
{
    float l = v3_len(a);
    return l > 0.0f ? v3_scale(a, 1.0f / l) : a;
}

void m4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void m4_copy(float o[16], const float a[16]) { memmove(o, a, 16 * sizeof(float)); }

void m4_mul(float o[16], const float a[16], const float b[16])
{
    float t[16];
    int c, r;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++)
            t[c * 4 + r] = ((a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1])
                            + a[2 * 4 + r] * b[c * 4 + 2]) + a[3 * 4 + r] * b[c * 4 + 3];
    memcpy(o, t, sizeof t);
}

void m4_translate(float m[16], float x, float y, float z)
{
    m4_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

void m4_scale(float m[16], float sx, float sy, float sz)
{
    m4_identity(m);
    m[0] = sx;
    m[5] = sy;
    m[10] = sz;
}

/* identical to mat_rot() in teensy/teensy_gpu/tg_core.c */
void m4_rotate(float m[16], const float axis[3], double angle)
{
    float x = axis[0], y = axis[1], z = axis[2];
    float len = sqrtf(x * x + y * y + z * z);
    float c, s, C;
    double a = fmod(angle, 6.283185307179586);
    memset(m, 0, 16 * sizeof(float));
    m[15] = 1.0f;
    if (!(len > 0.0f)) {
        m[0] = m[5] = m[10] = 1.0f;
        return;
    }
    x /= len;
    y /= len;
    z /= len;
    c = (float)cos(a);
    s = (float)sin(a);
    C = 1.0f - c;
    m[0] = x * x * C + c;     m[4] = x * y * C - z * s; m[8]  = x * z * C + y * s;
    m[1] = y * x * C + z * s; m[5] = y * y * C + c;     m[9]  = y * z * C - x * s;
    m[2] = z * x * C - y * s; m[6] = z * y * C + x * s; m[10] = z * z * C + c;
}

void m4_rot_x(float m[16], float a)
{
    float c = cosf(a), s = sinf(a);
    m4_identity(m);
    m[5] = c;
    m[6] = s;
    m[9] = -s;
    m[10] = c;
}

void m4_rot_y(float m[16], float a)
{
    float c = cosf(a), s = sinf(a);
    m4_identity(m);
    m[0] = c;
    m[2] = -s;
    m[8] = s;
    m[10] = c;
}

void m4_rot_z(float m[16], float a)
{
    float c = cosf(a), s = sinf(a);
    m4_identity(m);
    m[0] = c;
    m[1] = s;
    m[4] = -s;
    m[5] = c;
}

void m4_perspective(float m[16], float fovy, float aspect, float znear, float zfar)
{
    float f = 1.0f / tanf(fovy * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1.0f;
    m[14] = (2.0f * zfar * znear) / (znear - zfar);
}

void m4_look_at(float m[16], v3f eye, v3f center, v3f up)
{
    v3f f = v3_norm(v3_sub(center, eye));
    v3f s = v3_norm(v3_cross(f, up));
    v3f u = v3_cross(s, f);
    m4_identity(m);
    m[0] = s.x;  m[4] = s.y;  m[8] = s.z;
    m[1] = u.x;  m[5] = u.y;  m[9] = u.z;
    m[2] = -f.x; m[6] = -f.y; m[10] = -f.z;
    m[12] = -v3_dot(s, eye);
    m[13] = -v3_dot(u, eye);
    m[14] = v3_dot(f, eye);
}

void m4_xform(const float m[16], float x, float y, float z, float w, float out[4])
{
    int r;
    for (r = 0; r < 4; r++)
        out[r] = ((m[0 * 4 + r] * x + m[1 * 4 + r] * y) + m[2 * 4 + r] * z) + m[3 * 4 + r] * w;
}
