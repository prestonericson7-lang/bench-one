/* mesh.c -- see mesh.h. */
#include "mesh.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_D 3.14159265358979323846

static int mesh_alloc(mesh_t *m, uint32_t nv, uint32_t ni)
{
    m->nverts = 0;
    m->nidx = 0;
    m->v = calloc(nv ? nv : 1, sizeof *m->v);
    m->idx = calloc(ni ? ni : 1, sizeof *m->idx);
    if (!m->v || !m->idx) {
        mesh_free(m);
        return -1;
    }
    return 0;
}

void mesh_free(mesh_t *m)
{
    free(m->v);
    free(m->idx);
    m->v = NULL;
    m->idx = NULL;
    m->nverts = m->nidx = 0;
}

static uint32_t addv(mesh_t *m, double px, double py, double pz, double nx, double ny, double nz,
                     unsigned r, unsigned g, unsigned b)
{
    geom_vertex *v = &m->v[m->nverts];
    v->px = (float)px;
    v->py = (float)py;
    v->pz = (float)pz;
    v->nx = (float)nx;
    v->ny = (float)ny;
    v->nz = (float)nz;
    v->r = (uint8_t)r;
    v->g = (uint8_t)g;
    v->b = (uint8_t)b;
    v->a = 255;
    return m->nverts++;
}

static void addt(mesh_t *m, uint32_t a, uint32_t b, uint32_t c)
{
    m->idx[m->nidx++] = (uint16_t)a;
    m->idx[m->nidx++] = (uint16_t)b;
    m->idx[m->nidx++] = (uint16_t)c;
}

static void colour(const uint8_t *rgb, double s, double t, unsigned out[3])
{
    if (rgb) {
        out[0] = rgb[0];
        out[1] = rgb[1];
        out[2] = rgb[2];
        return;
    }
    out[0] = (unsigned)(127.5 + 127.4 * cos(2.0 * PI_D * s));
    out[1] = (unsigned)(127.5 + 127.4 * cos(2.0 * PI_D * (s + t) + 2.1));
    out[2] = (unsigned)(127.5 + 127.4 * cos(2.0 * PI_D * t + 4.2));
}

/* ---- cube -------------------------------------------------------------------------------- */
static const uint8_t def_face_rgb[6][3] = {
    {255, 40, 40}, {40, 230, 230}, {60, 230, 60}, {230, 60, 230}, {60, 90, 255}, {245, 220, 40}
};
static const int face_n[6][3] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };
/* tangents with u x v = n: corners (-u-v), (+u-v), (+u+v), (-u+v) are CCW seen from outside */
static const int face_u[6][3] = { {0, 1, 0}, {0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {0, 1, 0} };
static const int face_v[6][3] = { {0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 0} };

int mesh_cube(mesh_t *m, float h, const uint8_t (*face_rgb)[3])
{
    static const int su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
    int f, k;
    if (!face_rgb)
        face_rgb = def_face_rgb;
    if (mesh_alloc(m, 24, 36) < 0)
        return -1;
    for (f = 0; f < 6; f++) {
        uint32_t base = m->nverts;
        for (k = 0; k < 4; k++) {
            double p[3];
            int i;
            for (i = 0; i < 3; i++)
                p[i] = (double)h * (face_n[f][i] + su[k] * face_u[f][i] + sv[k] * face_v[f][i]);
            addv(m, p[0], p[1], p[2], face_n[f][0], face_n[f][1], face_n[f][2], face_rgb[f][0], face_rgb[f][1],
                 face_rgb[f][2]);
        }
        addt(m, base, base + 1, base + 2);
        addt(m, base, base + 2, base + 3);
    }
    return 0;
}

/* ---- sphere: p(th,ph) = r * (sin th cos ph, cos th, sin th sin ph). With a=(i,j), b=(i,j+1),
 * d=(i+1,j), c=(i+1,j+1) (i along th from +Y down, j along ph), (a,b,d) and (b,c,d) are CCW seen
 * from outside (dp/dph x dp/dth points outwards). ---- */
int mesh_sphere(mesh_t *m, float r, int stacks, int slices, const uint8_t *rgb)
{
    int i, j;
    if (stacks < 2 || slices < 3 || (uint32_t)((stacks + 1) * (slices + 1)) > 65535u)
        return -1;
    if (mesh_alloc(m, (uint32_t)((stacks + 1) * (slices + 1)), (uint32_t)(stacks * slices * 6)) < 0)
        return -1;
    for (i = 0; i <= stacks; i++)
        for (j = 0; j <= slices; j++) {
            double th = PI_D * i / stacks, ph = 2.0 * PI_D * j / slices;
            double nx = sin(th) * cos(ph), ny = cos(th), nz = sin(th) * sin(ph);
            unsigned c[3];
            colour(rgb, (double)j / slices, (double)i / stacks, c);
            addv(m, nx * r, ny * r, nz * r, nx, ny, nz, c[0], c[1], c[2]);
        }
    for (i = 0; i < stacks; i++)
        for (j = 0; j < slices; j++) {
            uint32_t a = (uint32_t)(i * (slices + 1) + j), b = a + 1, d = a + (uint32_t)slices + 1, c = d + 1;
            if (i != 0)
                addt(m, a, b, d);
            if (i != stacks - 1)
                addt(m, b, c, d);
        }
    return 0;
}

/* ---- torus: p(u,v) = ((R + r cos v) cos u, r sin v, (R + r cos v) sin u),
 * n = (cos v cos u, sin v, cos v sin u). (a,b,d), (b,c,d) with a=(i,j), b=(i,j+1), d=(i+1,j):
 * dp/dv x dp/du points along +n, so these are CCW seen from outside. ---- */
int mesh_torus(mesh_t *m, float R, float r, int nu, int nv, const uint8_t *rgb)
{
    int i, j;
    if (nu < 3 || nv < 3 || (uint32_t)((nu + 1) * (nv + 1)) > 65535u)
        return -1;
    if (mesh_alloc(m, (uint32_t)((nu + 1) * (nv + 1)), (uint32_t)(nu * nv * 6)) < 0)
        return -1;
    for (i = 0; i <= nu; i++)
        for (j = 0; j <= nv; j++) {
            double u = 2.0 * PI_D * i / nu, v = 2.0 * PI_D * j / nv;
            double nx = cos(v) * cos(u), ny = sin(v), nz = cos(v) * sin(u);
            double rr = (double)R + (double)r * cos(v);
            unsigned c[3];
            colour(rgb, (double)i / nu, (double)j / nv, c);
            addv(m, rr * cos(u), (double)r * sin(v), rr * sin(u), nx, ny, nz, c[0], c[1], c[2]);
        }
    for (i = 0; i < nu; i++)
        for (j = 0; j < nv; j++) {
            uint32_t a = (uint32_t)(i * (nv + 1) + j), b = a + 1, d = a + (uint32_t)nv + 1, c = d + 1;
            addt(m, a, b, d);
            addt(m, b, c, d);
        }
    return 0;
}

/* ---- verification ------------------------------------------------------------------------- */
int mesh_check(const mesh_t *m, int kind, float R, char *why, int whylen)
{
    uint32_t t, i;
    for (i = 0; i < m->nverts; i++) {
        const geom_vertex *v = &m->v[i];
        double nl = sqrt((double)v->nx * v->nx + (double)v->ny * v->ny + (double)v->nz * v->nz);
        double cx = 0, cz = 0, d;
        if (fabs(nl - 1.0) > 1e-5) {
            snprintf(why, (size_t)whylen, "vertex %u: normal length %.6f", i, nl);
            return -1;
        }
        if (kind == 1) {                /* nearest point of the ring (circle of radius R in y=0) */
            double l = sqrt((double)v->px * v->px + (double)v->pz * v->pz);
            if (l > 0) {
                cx = v->px / l * R;
                cz = v->pz / l * R;
            }
        }
        d = (v->px - cx) * v->nx + (double)v->py * v->ny + (v->pz - cz) * v->nz;
        if (!(d > 0)) {
            snprintf(why, (size_t)whylen, "vertex %u: normal points inwards (dot %.4g)", i, d);
            return -1;
        }
    }
    if (m->nidx % 3) {
        snprintf(why, (size_t)whylen, "%u indices (not a multiple of 3)", m->nidx);
        return -1;
    }
    for (t = 0; t < m->nidx; t += 3) {
        const geom_vertex *a, *b, *c;
        double e1[3], e2[3], fn[3], ns[3], area, dot;
        if (m->idx[t] >= m->nverts || m->idx[t + 1] >= m->nverts || m->idx[t + 2] >= m->nverts) {
            snprintf(why, (size_t)whylen, "triangle %u: index out of range", t / 3);
            return -1;
        }
        a = &m->v[m->idx[t]];
        b = &m->v[m->idx[t + 1]];
        c = &m->v[m->idx[t + 2]];
        e1[0] = (double)b->px - a->px; e1[1] = (double)b->py - a->py; e1[2] = (double)b->pz - a->pz;
        e2[0] = (double)c->px - a->px; e2[1] = (double)c->py - a->py; e2[2] = (double)c->pz - a->pz;
        fn[0] = e1[1] * e2[2] - e1[2] * e2[1];
        fn[1] = e1[2] * e2[0] - e1[0] * e2[2];
        fn[2] = e1[0] * e2[1] - e1[1] * e2[0];
        area = sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
        ns[0] = (double)a->nx + b->nx + c->nx;
        ns[1] = (double)a->ny + b->ny + c->ny;
        ns[2] = (double)a->nz + b->nz + c->nz;
        dot = fn[0] * ns[0] + fn[1] * ns[1] + fn[2] * ns[2];
        if (!(area > 1e-9)) {
            snprintf(why, (size_t)whylen, "triangle %u is degenerate", t / 3);
            return -1;
        }
        if (!(dot > 0)) {
            snprintf(why, (size_t)whylen, "triangle %u is clockwise seen from outside", t / 3);
            return -1;
        }
    }
    return 0;
}
