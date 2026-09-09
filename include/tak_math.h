#ifndef TAK_MATH_H
#define TAK_MATH_H

#include "tak_types.h"
#include <math.h>

/* ── 3D math primitives ────────────────────────────────────────────
 *
 * The game world is 3D (terrain heightmap, per-piece transforms);
 * the screen is 2D with a tilt projection (2.5D look). These types
 * + ops give us a single language for both layers.
 *
 * Conventions:
 *   - Right-handed, +Y up.
 *   - Quaternion (x, y, z, w) with w as the scalar.
 *   - Matrices in row-major float arrays — meshes with future OpenGL
 *     code will need a transpose at upload time.
 *   - Mat4x3 = "pose" matrix for nodes (3x3 rot + 3 trans), stored
 *     as 12 floats: row-major rot[9] then trans[3]. Memory-compatible
 *     with the NodeXform we already use in units.c. */

typedef struct Vec3 { float x, y, z; } Vec3;
typedef struct Vec4 { float x, y, z, w; } Vec4;
typedef struct Quat { float x, y, z, w; } Quat;

typedef struct Mat3   { float m[9];  } Mat3;     /* 3x3 row-major */
typedef struct Mat4x3 { float m[12]; } Mat4x3;   /* 3x3 rot + 3 trans */
typedef struct Mat4   { float m[16]; } Mat4;     /* 4x4 row-major */

/* ── Vec3 ops ─────────────────────────────────────────────────── */
static inline Vec3 vec3_make(float x, float y, float z) {
    Vec3 v = { x, y, z }; return v;
}
static inline Vec3 vec3_add(Vec3 a, Vec3 b) {
    return vec3_make(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline Vec3 vec3_sub(Vec3 a, Vec3 b) {
    return vec3_make(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline Vec3 vec3_scale(Vec3 v, float s) {
    return vec3_make(v.x * s, v.y * s, v.z * s);
}
static inline float vec3_dot(Vec3 a, Vec3 b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}
static inline Vec3 vec3_cross(Vec3 a, Vec3 b) {
    return vec3_make(a.y*b.z - a.z*b.y,
                     a.z*b.x - a.x*b.z,
                     a.x*b.y - a.y*b.x);
}
static inline float vec3_length(Vec3 v) { return sqrtf(vec3_dot(v, v)); }
static inline Vec3 vec3_normalize(Vec3 v) {
    float L = vec3_length(v);
    return (L > 1e-8f) ? vec3_scale(v, 1.0f/L) : vec3_make(0,0,0);
}

/* ── Mat3 ops (row-major) ─────────────────────────────────────── */
static inline Mat3 mat3_identity(void) {
    Mat3 r = {{ 1,0,0, 0,1,0, 0,0,1 }};
    return r;
}
static inline Vec3 mat3_mul_vec3(const Mat3 *m, Vec3 v) {
    return vec3_make(
        m->m[0]*v.x + m->m[1]*v.y + m->m[2]*v.z,
        m->m[3]*v.x + m->m[4]*v.y + m->m[5]*v.z,
        m->m[6]*v.x + m->m[7]*v.y + m->m[8]*v.z);
}
static inline Mat3 mat3_mul(const Mat3 *a, const Mat3 *b) {
    Mat3 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r.m[i*3 + j] = a->m[i*3+0]*b->m[0*3+j]
                         + a->m[i*3+1]*b->m[1*3+j]
                         + a->m[i*3+2]*b->m[2*3+j];
        }
    }
    return r;
}
/* Build a rotation matrix from Euler ZYX angles (TA convention). */
static inline Mat3 mat3_from_euler(float rx, float ry, float rz) {
    float cx = cosf(rx), sx = sinf(rx);
    float cy = cosf(ry), sy = sinf(ry);
    float cz = cosf(rz), sz = sinf(rz);
    Mat3 r;
    r.m[0] = cy*cz;   r.m[1] = sx*sy*cz - cx*sz;   r.m[2] = cx*sy*cz + sx*sz;
    r.m[3] = cy*sz;   r.m[4] = sx*sy*sz + cx*cz;   r.m[5] = cx*sy*sz - sx*cz;
    r.m[6] = -sy;     r.m[7] = sx*cy;              r.m[8] = cx*cy;
    return r;
}

/* ── Mat4x3 (pose: rot + translation) ─────────────────────────── */
static inline Mat4x3 mat4x3_identity(void) {
    Mat4x3 r = {{ 1,0,0, 0,1,0, 0,0,1, 0,0,0 }};
    return r;
}
static inline Mat4x3 mat4x3_make(Mat3 rot, Vec3 trans) {
    Mat4x3 r;
    for (int i = 0; i < 9; i++) r.m[i] = rot.m[i];
    r.m[9]  = trans.x;
    r.m[10] = trans.y;
    r.m[11] = trans.z;
    return r;
}
/* Compose two pose matrices: result = parent * child. */
static inline Mat4x3 mat4x3_mul(const Mat4x3 *p, const Mat4x3 *c) {
    Mat3 pr = {{ p->m[0],p->m[1],p->m[2], p->m[3],p->m[4],p->m[5], p->m[6],p->m[7],p->m[8] }};
    Mat3 cr = {{ c->m[0],c->m[1],c->m[2], c->m[3],c->m[4],c->m[5], c->m[6],c->m[7],c->m[8] }};
    Mat3 nr = mat3_mul(&pr, &cr);
    Vec3 ct = vec3_make(c->m[9], c->m[10], c->m[11]);
    Vec3 nt = vec3_add(mat3_mul_vec3(&pr, ct),
                       vec3_make(p->m[9], p->m[10], p->m[11]));
    return mat4x3_make(nr, nt);
}
static inline Vec3 mat4x3_mul_point(const Mat4x3 *m, Vec3 p) {
    Vec3 r;
    r.x = m->m[0]*p.x + m->m[1]*p.y + m->m[2]*p.z + m->m[9];
    r.y = m->m[3]*p.x + m->m[4]*p.y + m->m[5]*p.z + m->m[10];
    r.z = m->m[6]*p.x + m->m[7]*p.y + m->m[8]*p.z + m->m[11];
    return r;
}

/* ── Quaternion ops ────────────────────────────────────────────
 *
 * For animation interpolation between sim ticks; not used in the
 * fixed-point COB animator (which is integer for determinism). */
static inline Quat quat_identity(void) {
    Quat q = { 0, 0, 0, 1 }; return q;
}
static inline Quat quat_from_axis_angle(Vec3 axis, float angle) {
    float h = angle * 0.5f;
    float s = sinf(h);
    Quat q;
    q.x = axis.x * s;
    q.y = axis.y * s;
    q.z = axis.z * s;
    q.w = cosf(h);
    return q;
}
static inline Quat quat_mul(Quat a, Quat b) {
    Quat r;
    r.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    r.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    r.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    r.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    return r;
}
static inline Mat3 quat_to_mat3(Quat q) {
    float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    Mat3 m;
    m.m[0] = 1 - 2*(yy + zz);  m.m[1] = 2*(xy - wz);      m.m[2] = 2*(xz + wy);
    m.m[3] = 2*(xy + wz);      m.m[4] = 1 - 2*(xx + zz);  m.m[5] = 2*(yz - wx);
    m.m[6] = 2*(xz - wy);      m.m[7] = 2*(yz + wx);      m.m[8] = 1 - 2*(xx + yy);
    return m;
}

#endif /* TAK_MATH_H */
