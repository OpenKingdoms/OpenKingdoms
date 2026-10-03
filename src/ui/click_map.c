/*
 * click_map.c -- the screen to world mapping, see tak_click_map.h.
 *
 * Integer arithmetic throughout: the pointer floors and a rect's edges
 * ceil, so every window pixel inside a canvas pixel's rect maps back to
 * that pixel and rects tile without a seam, on any platform.
 */

#include "tak_click_map.h"

int ClickMap_WindowToCanvas(int win_w, int win_h, int can_w, int can_h,
                            int wx, int wy, int *cx, int *cy) {
    if (win_w <= 0 || win_h <= 0 || can_w <= 0 || can_h <= 0) return 0;
    if (wx < 0 || wy < 0 || wx >= win_w || wy >= win_h) return 0;
    int x = (int)((int64_t)wx * can_w / win_w);
    int y = (int)((int64_t)wy * can_h / win_h);
    if (x >= can_w || y >= can_h) return 0;
    if (cx) *cx = x;
    if (cy) *cy = y;
    return 1;
}

static int ceil_scaled(int v, int win, int can) {
    return (int)(((int64_t)v * win + can - 1) / can);
}

void ClickMap_CanvasRectToWindow(int win_w, int win_h, int can_w, int can_h,
                                 int x, int y, int w, int h, int out[4]) {
    out[0] = x; out[1] = y; out[2] = w; out[3] = h;
    if (can_w <= 0 || can_h <= 0) return;
    out[0] = ceil_scaled(x, win_w, can_w);
    out[1] = ceil_scaled(y, win_h, can_h);
    out[2] = ceil_scaled(x + w, win_w, can_w) - out[0];
    out[3] = ceil_scaled(y + h, win_h, can_h) - out[1];
}

void ClickMap_WindowToWorld(int32_t cam_x, int32_t cam_y, int wx, int wy,
                            int32_t *x, int32_t *y) {
    if (x) *x = cam_x + wx;
    if (y) *y = cam_y + wy;
}

int32_t ClickMap_DrawnY(int32_t world_y, float height, float tan_tilt) {
    return world_y - (int32_t)(height * tan_tilt);
}

void ClickMap_GroundUnderPoint(int32_t flat_x, int32_t flat_y, float tan_tilt,
                               ClickMap_HeightFn height, void *ctx,
                               int32_t *gx, int32_t *gy) {
    if (gx) *gx = flat_x;
    if (gy) *gy = flat_y;
    if (!gy || !height) return;
    int32_t span = (int32_t)(255.0f * tan_tilt) + 2;
    for (int32_t wy = flat_y + span; wy > flat_y; wy--) {
        int32_t sy = ClickMap_DrawnY(wy, (float)height(ctx, flat_x, wy),
                                     tan_tilt);
        if (sy <= flat_y) { *gy = wy; return; }
    }
}

int ClickMap_RayHitsBox(const float origin[3], const float dir[3],
                        const float lo[3], const float hi[3], float *t) {
    float t0 = 0.0f, t1 = 3.0e38f;
    for (int k = 0; k < 3; k++) {
        if (dir[k] == 0.0f) {
            if (origin[k] < lo[k] || origin[k] > hi[k]) return 0;
            continue;
        }
        float a = (lo[k] - origin[k]) / dir[k];
        float b = (hi[k] - origin[k]) / dir[k];
        if (a > b) { float s = a; a = b; b = s; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        if (t0 > t1) return 0;
    }
    if (t) *t = t0;
    return 1;
}
