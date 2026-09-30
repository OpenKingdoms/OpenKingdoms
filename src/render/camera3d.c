/*
 * camera3d.c -- the free camera's arithmetic, see tak_camera3d.h.
 */

#include "tak_camera3d.h"

#include <math.h>
#include <string.h>

#define C3D_PI 3.14159265358979323846f

#define C3D_MIN_PITCH (12.0f * C3D_PI / 180.0f)
#define C3D_MAX_PITCH (89.0f * C3D_PI / 180.0f)
#define C3D_MIN_DIST  48.0f
#define C3D_MAX_DIST  6000.0f
#define C3D_EYE_FLOOR 20.0f

/* The classic view lifts a point by half its height (legacy:197689),
 * which is the tilt whose tangent is one half. */
#define C3D_CLASSIC_PITCH 1.10714871779f

void Mat4_Identity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void Mat4_Multiply(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int rr = 0; rr < 4; rr++) {
            r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0]
                          + a[1 * 4 + rr] * b[c * 4 + 1]
                          + a[2 * 4 + rr] * b[c * 4 + 2]
                          + a[3 * 4 + rr] * b[c * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

void Mat4_TransformPoint(const float m[16], const float p[3], float out[4]) {
    for (int rr = 0; rr < 4; rr++) {
        out[rr] = m[0 * 4 + rr] * p[0] + m[1 * 4 + rr] * p[1]
                + m[2 * 4 + rr] * p[2] + m[3 * 4 + rr];
    }
}

static void normalize3(float v[3]) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len <= 0.0f) return;
    v[0] /= len; v[1] /= len; v[2] /= len;
}

static void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Right, up and forward of the eye, forward pointing at the target. */
static void camera_basis(const Camera3D *cam, float right[3], float up[3],
                         float fwd[3]) {
    float eye[3];
    Camera3D_Eye(cam, eye);
    fwd[0] = cam->target_x - eye[0];
    fwd[1] = cam->target_y - eye[1];
    fwd[2] = cam->target_z - eye[2];
    normalize3(fwd);
    const float world_up[3] = { 0.0f, 1.0f, 0.0f };
    cross3(fwd, world_up, right);
    normalize3(right);
    cross3(right, fwd, up);
}

void Camera3D_ClassicPreset(Camera3D *cam, float target_x, float target_z,
                            int viewport_w, int viewport_h) {
    if (viewport_w < 1) viewport_w = 1;
    if (viewport_h < 1) viewport_h = 1;
    cam->target_x = target_x;
    cam->target_y = 0.0f;   /* Camera3D_Clamp lifts it onto the terrain */
    cam->target_z = target_z;
    cam->yaw = 0.0f;
    cam->pitch = C3D_CLASSIC_PITCH;
    cam->fov_y = 45.0f * C3D_PI / 180.0f;
    cam->aspect = (float)viewport_w / (float)viewport_h;
    /* The ground across the target fills the viewport's width. */
    cam->dist = (float)viewport_h / (2.0f * tanf(cam->fov_y * 0.5f));
    cam->near_z = 8.0f;
    cam->far_z = 20000.0f;
}

void Camera3D_Eye(const Camera3D *cam, float out[3]) {
    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    out[0] = cam->target_x + cam->dist * sinf(cam->yaw) * cp;
    out[1] = cam->target_y + cam->dist * sp;
    out[2] = cam->target_z + cam->dist * cosf(cam->yaw) * cp;
}

void Camera3D_ViewMatrix(const Camera3D *cam, float out[16]) {
    float right[3], up[3], fwd[3], eye[3];
    camera_basis(cam, right, up, fwd);
    Camera3D_Eye(cam, eye);
    Mat4_Identity(out);
    out[0] = right[0]; out[4] = right[1]; out[8]  = right[2];
    out[1] = up[0];    out[5] = up[1];    out[9]  = up[2];
    out[2] = -fwd[0];  out[6] = -fwd[1];  out[10] = -fwd[2];
    out[12] = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
    out[13] = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
    out[14] =  (fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2]);
}

void Camera3D_ProjMatrix(const Camera3D *cam, float out[16]) {
    float f = 1.0f / tanf(cam->fov_y * 0.5f);
    float n = cam->near_z, fz = cam->far_z;
    memset(out, 0, sizeof(float) * 16);
    out[0]  = f / cam->aspect;
    out[5]  = f;
    out[10] = (fz + n) / (n - fz);
    out[11] = -1.0f;
    out[14] = (2.0f * fz * n) / (n - fz);
}

void Camera3D_ViewProj(const Camera3D *cam, float out[16]) {
    float v[16], p[16];
    Camera3D_ViewMatrix(cam, v);
    Camera3D_ProjMatrix(cam, p);
    Mat4_Multiply(p, v, out);
}

void Camera3D_PointerRay(const Camera3D *cam, int vp_w, int vp_h,
                         float px, float py, float origin[3], float dir[3]) {
    float right[3], up[3], fwd[3];
    camera_basis(cam, right, up, fwd);
    Camera3D_Eye(cam, origin);
    if (vp_w < 1) vp_w = 1;
    if (vp_h < 1) vp_h = 1;
    float nx = (px + 0.5f) / (float)vp_w * 2.0f - 1.0f;
    float ny = 1.0f - (py + 0.5f) / (float)vp_h * 2.0f;
    float t = tanf(cam->fov_y * 0.5f);
    float sx = nx * t * cam->aspect;
    float sy = ny * t;
    for (int i = 0; i < 3; i++) {
        dir[i] = fwd[i] + right[i] * sx + up[i] * sy;
    }
    normalize3(dir);
}

int Camera3D_RayHitGround(const float origin[3], const float dir[3],
                          float max_dist, Camera3D_HeightFn height,
                          void *ctx, float *out_x, float *out_z) {
    if (!height || max_dist <= 0.0f) return 0;
    /* Two pixel steps find the crossing, a bisection settles it. */
    const float step = 2.0f;
    float prev_t = 0.0f;
    float prev_above = origin[1] - height(ctx, origin[0], origin[2]);
    if (prev_above < 0.0f) {
        if (out_x) *out_x = origin[0];
        if (out_z) *out_z = origin[2];
        return 1;
    }
    for (float t = step; t <= max_dist; t += step) {
        float x = origin[0] + dir[0] * t;
        float y = origin[1] + dir[1] * t;
        float z = origin[2] + dir[2] * t;
        float above = y - height(ctx, x, z);
        if (above <= 0.0f) {
            float lo = prev_t, hi = t;
            for (int i = 0; i < 24; i++) {
                float mid = (lo + hi) * 0.5f;
                float mx = origin[0] + dir[0] * mid;
                float my = origin[1] + dir[1] * mid;
                float mz = origin[2] + dir[2] * mid;
                if (my - height(ctx, mx, mz) <= 0.0f) hi = mid; else lo = mid;
            }
            if (out_x) *out_x = origin[0] + dir[0] * hi;
            if (out_z) *out_z = origin[2] + dir[2] * hi;
            return 1;
        }
        prev_t = t;
        prev_above = above;
    }
    (void)prev_above;
    return 0;
}

void Camera3D_Clamp(Camera3D *cam, float map_w, float map_h,
                    Camera3D_HeightFn height, void *ctx) {
    if (cam->target_x < 0.0f) cam->target_x = 0.0f;
    if (cam->target_z < 0.0f) cam->target_z = 0.0f;
    if (cam->target_x > map_w) cam->target_x = map_w;
    if (cam->target_z > map_h) cam->target_z = map_h;
    if (cam->pitch < C3D_MIN_PITCH) cam->pitch = C3D_MIN_PITCH;
    if (cam->pitch > C3D_MAX_PITCH) cam->pitch = C3D_MAX_PITCH;
    if (cam->dist < C3D_MIN_DIST) cam->dist = C3D_MIN_DIST;
    if (cam->dist > C3D_MAX_DIST) cam->dist = C3D_MAX_DIST;
    while (cam->yaw > C3D_PI)  cam->yaw -= 2.0f * C3D_PI;
    while (cam->yaw < -C3D_PI) cam->yaw += 2.0f * C3D_PI;
    if (height) {
        cam->target_y = height(ctx, cam->target_x, cam->target_z);
        float eye[3];
        Camera3D_Eye(cam, eye);
        float floor_y = height(ctx, eye[0], eye[2]) + C3D_EYE_FLOOR;
        if (eye[1] < floor_y) {
            /* Tilt up over the ground rather than move the target. */
            float s = (floor_y - cam->target_y) / cam->dist;
            if (s > 1.0f) s = 1.0f;
            float p = asinf(s);
            if (p > cam->pitch) cam->pitch = p;
            if (cam->pitch > C3D_MAX_PITCH) cam->pitch = C3D_MAX_PITCH;
        }
    }
}

void Camera3D_FrustumPlanes(const float m[16], float planes[6][4]) {
    /* Row i of a column major matrix is m[i], m[4 + i], m[8 + i], m[12 + i]. */
    for (int k = 0; k < 6; k++) {
        int row = k / 2;
        float sign = (k % 2 == 0) ? 1.0f : -1.0f;
        for (int c = 0; c < 4; c++) {
            planes[k][c] = m[c * 4 + 3] + sign * m[c * 4 + row];
        }
        float len = sqrtf(planes[k][0] * planes[k][0] + planes[k][1] * planes[k][1]
                        + planes[k][2] * planes[k][2]);
        if (len > 0.0f) {
            for (int c = 0; c < 4; c++) planes[k][c] /= len;
        }
    }
}

int Camera3D_SphereInFrustum(const float planes[6][4], const float c[3],
                             float radius) {
    for (int k = 0; k < 6; k++) {
        float d = planes[k][0] * c[0] + planes[k][1] * c[1]
                + planes[k][2] * c[2] + planes[k][3];
        if (d < -radius) return 0;
    }
    return 1;
}

void Camera3D_Pan(Camera3D *cam, float right_px, float forward_px) {
    float sy = sinf(cam->yaw), cy = cosf(cam->yaw);
    /* Right on the ground is east at yaw zero, forward is north. */
    cam->target_x += right_px * cy - forward_px * sy;
    cam->target_z += -right_px * sy - forward_px * cy;
}

void Camera3D_Orbit(Camera3D *cam, float dyaw, float dpitch) {
    cam->yaw += dyaw;
    cam->pitch += dpitch;
}

void Camera3D_Zoom(Camera3D *cam, float factor) {
    if (factor > 0.0f) cam->dist *= factor;
}
