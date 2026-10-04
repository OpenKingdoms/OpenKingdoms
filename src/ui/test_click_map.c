/*
 * test_click_map.c -- the screen to world mapping, pinned in numbers.
 *
 * No game data and no SDL, so it runs on every platform we ship,
 * the browser under node included. A click at a stated window position
 * has to give a stated canvas pixel and a stated world point, for a
 * range of window sizes including the fractional scales real monitors
 * give, for both axes, and the ground under a pointer has to be the
 * ground that draws there.
 */

#include "test_framework.h"
#include "tak_click_map.h"

#include <stdio.h>
#include <stdlib.h>

#define CAN_W 640
#define CAN_H 480

/* Window sizes players run: the canvas scale is 2x3 on the first, a
 * fraction on the rest, and the last two are not 4:3 or 16:9. */
static const int k_windows[][2] = {
    { 640, 480 }, { 1280, 720 }, { 1366, 768 }, { 1536, 864 },
    { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 1280, 1024 },
    { 1000, 700 }, { 3440, 1440 },
};
#define N_WINDOWS (int)(sizeof(k_windows) / sizeof(k_windows[0]))

TEST(a_window_pixel_lands_on_a_stated_canvas_pixel) {
    /* Hand computed: floor(wx * 640 / win_w), floor(wy * 480 / win_h). */
    static const int cases[][6] = {
        /* win_w, win_h, wx, wy, cx, cy */
        { 1280,  720,  640, 360, 320, 240 },
        { 1536,  864, 1000, 500, 416, 277 },
        { 1366,  768,  683, 384, 320, 240 },
        { 1366,  768, 1365, 767, 639, 479 },
        { 1920, 1080,  100, 100,  33,  44 },
        { 2560, 1440, 2047, 1439, 511, 479 },
        { 1280, 1024,  640, 512, 320, 240 },
        { 1000,  700,  999, 699, 639, 479 },
        {  640,  480,  511, 430, 511, 430 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int cx = -1, cy = -1;
        ASSERT_EQ_INT(1, ClickMap_WindowToCanvas(cases[i][0], cases[i][1],
                                                 CAN_W, CAN_H,
                                                 cases[i][2], cases[i][3],
                                                 &cx, &cy));
        ASSERT_EQ_INT(cases[i][4], cx);
        ASSERT_EQ_INT(cases[i][5], cy);
    }
    /* Off the window is off the canvas. */
    int cx = 0, cy = 0;
    ASSERT_EQ_INT(0, ClickMap_WindowToCanvas(1280, 720, CAN_W, CAN_H, -1, 10, &cx, &cy));
    ASSERT_EQ_INT(0, ClickMap_WindowToCanvas(1280, 720, CAN_W, CAN_H, 1280, 10, &cx, &cy));
    ASSERT_EQ_INT(0, ClickMap_WindowToCanvas(1280, 720, CAN_W, CAN_H, 10, 720, &cx, &cy));
}

/* Every window pixel inside a canvas pixel's window rect maps back to
 * that pixel, and neighbouring rects tile without a gap or an overlap.
 * This is what keeps a sidebar button's hit test on its picture. */
TEST(a_canvas_rect_round_trips_through_the_window) {
    for (int w = 0; w < N_WINDOWS; w++) {
        int win_w = k_windows[w][0], win_h = k_windows[w][1];
        for (int x = 0; x < CAN_W; x += (x < 16 ? 1 : 37)) {
            for (int y = 0; y < CAN_H; y += (y < 16 ? 1 : 29)) {
                int r[4];
                ClickMap_CanvasRectToWindow(win_w, win_h, CAN_W, CAN_H,
                                            x, y, 1, 1, r);
                ASSERT(r[2] >= 1);
                ASSERT(r[3] >= 1);
                for (int wy = r[1]; wy < r[1] + r[3]; wy++) {
                    for (int wx = r[0]; wx < r[0] + r[2]; wx++) {
                        int cx = -1, cy = -1;
                        ASSERT_EQ_INT(1, ClickMap_WindowToCanvas(win_w, win_h,
                                                                 CAN_W, CAN_H,
                                                                 wx, wy, &cx, &cy));
                        ASSERT_EQ_INT(x, cx);
                        ASSERT_EQ_INT(y, cy);
                    }
                }
            }
        }
        /* Seamless: the rect after column x starts where x's ends. */
        for (int x = 0; x < CAN_W - 1; x++) {
            int a[4], b[4];
            ClickMap_CanvasRectToWindow(win_w, win_h, CAN_W, CAN_H, x, 0, 1, 1, a);
            ClickMap_CanvasRectToWindow(win_w, win_h, CAN_W, CAN_H, x + 1, 0, 1, 1, b);
            ASSERT_EQ_INT(a[0] + a[2], b[0]);
        }
        for (int y = 0; y < CAN_H - 1; y++) {
            int a[4], b[4];
            ClickMap_CanvasRectToWindow(win_w, win_h, CAN_W, CAN_H, 0, y, 1, 1, a);
            ClickMap_CanvasRectToWindow(win_w, win_h, CAN_W, CAN_H, 0, y + 1, 1, 1, b);
            ASSERT_EQ_INT(a[1] + a[3], b[1]);
        }
    }
}

/* The sidebar starts at dialog column 512 and the bottom strip at row
 * 431. The first window pixel of each, and only that one, is HUD. */
TEST(the_hud_edge_falls_on_the_same_window_pixel_both_ways) {
    for (int w = 0; w < N_WINDOWS; w++) {
        int win_w = k_windows[w][0], win_h = k_windows[w][1];
        int r[4], cx = -1, cy = -1;
        ClickMap_CanvasRectToWindow(win_w, win_h, CAN_W, CAN_H, 512, 431, 1, 1, r);
        ASSERT_EQ_INT(1, ClickMap_WindowToCanvas(win_w, win_h, CAN_W, CAN_H, r[0], r[1], &cx, &cy));
        ASSERT_EQ_INT(512, cx);
        ASSERT_EQ_INT(431, cy);
        ASSERT_EQ_INT(1, ClickMap_WindowToCanvas(win_w, win_h, CAN_W, CAN_H, r[0] - 1, r[1] - 1, &cx, &cy));
        ASSERT_EQ_INT(511, cx);
        ASSERT_EQ_INT(430, cy);
    }
}

/* Measured on openkingdoms.net on 2026-09-15: a 1600x900 page, the
 * camera at (0, 2590), a click at (160, 90) ordered a move to
 * (160, 2680) and one at (640, 405) to (640, 2995). */
TEST(a_window_pixel_is_the_camera_plus_itself_in_the_world) {
    int32_t x = 0, y = 0;
    ClickMap_WindowToWorld(0, 2590, 160, 90, &x, &y);
    ASSERT_EQ_INT(160, (int)x);
    ASSERT_EQ_INT(2680, (int)y);
    ClickMap_WindowToWorld(0, 2590, 640, 405, &x, &y);
    ASSERT_EQ_INT(640, (int)x);
    ASSERT_EQ_INT(2995, (int)y);
    ClickMap_WindowToWorld(1234, 5678, 1919, 1079, &x, &y);
    ASSERT_EQ_INT(3153, (int)x);
    ASSERT_EQ_INT(6757, (int)y);
}

/* The browser. SDL hands the engine a window pixel by scaling the
 * page's css position by canvas element over css box, and the page
 * keeps the element at the css box, so at any device pixel ratio one
 * css pixel is one world pixel. Were the element scaled by the ratio,
 * as a high DPI window would do, every click would land the ratio too
 * far from the camera corner: that is the error this guards against. */
static int css_to_window(double css, double css_size, double elem_size) {
    return (int)(css * (elem_size / css_size));
}

TEST(a_page_click_is_one_world_pixel_per_css_pixel_at_any_ratio) {
    static const double ratios[] = { 1.0, 1.25, 1.5, 1.75, 2.0 };
    static const int pages[][2] = { { 1280, 720 }, { 1536, 864 }, { 1600, 900 }, { 2560, 1440 } };
    for (size_t r = 0; r < sizeof(ratios) / sizeof(ratios[0]); r++) {
        for (size_t p = 0; p < sizeof(pages) / sizeof(pages[0]); p++) {
            double css_w = pages[p][0], css_h = pages[p][1];
            int wx = css_to_window(0.7 * css_w, css_w, css_w);
            int wy = css_to_window(0.6 * css_h, css_h, css_h);
            int32_t x = 0, y = 0;
            ClickMap_WindowToWorld(100, 2590, wx, wy, &x, &y);
            ASSERT_EQ_INT(100 + (int)(0.7 * css_w), (int)x);
            ASSERT_EQ_INT(2590 + (int)(0.6 * css_h), (int)y);
            /* A ratio scaled element would put it ratio times out. */
            int bx = css_to_window(0.7 * css_w, css_w, css_w * ratios[r]);
            ASSERT_EQ_INT((int)(0.7 * css_w * ratios[r]), bx);
        }
    }
    /* A 1600x1000 page letterboxes the canvas to 1600x900 fifty pixels
     * down. A page click at (160, 140) is canvas (160, 90). */
    int32_t x = 0, y = 0;
    ClickMap_WindowToWorld(0, 2590, css_to_window(160, 1600, 1600),
                           css_to_window(140 - 50, 900, 900), &x, &y);
    ASSERT_EQ_INT(160, (int)x);
    ASSERT_EQ_INT(2680, (int)y);
}

/* Ground fields for the inverse. */
static int flat64(void *ctx, int32_t x, int32_t y) { (void)ctx; (void)x; (void)y; return 64; }
static int slope(void *ctx, int32_t x, int32_t y) {
    (void)ctx; (void)x;
    int h = (int)(y / 8);
    return h > 255 ? 255 : (h < 0 ? 0 : h);
}
static int cliff(void *ctx, int32_t x, int32_t y) { (void)ctx; (void)x; return y >= 1000 ? 200 : 0; }

/* A point of ground draws lifted by half its height at the default
 * tilt, and the inverse finds it again exactly, on both axes. */
TEST(the_ground_under_a_pointer_is_the_ground_that_draws_there) {
    const float tilt = 0.5f;
    ASSERT_EQ_INT(1868, (int)ClickMap_DrawnY(1900, 64.0f, tilt));
    ASSERT_EQ_INT(1900, (int)ClickMap_DrawnY(1900, 0.0f, tilt));
    ASSERT_EQ_INT(1773, (int)ClickMap_DrawnY(1900, 255.0f, tilt));
    int32_t gx = 0, gy = 0;
    /* Flat ground at 64: a pointer at 1868 is the ground at 1900. */
    ClickMap_GroundUnderPoint(1500, 1868, tilt, flat64, NULL, &gx, &gy);
    ASSERT_EQ_INT(1500, (int)gx);
    ASSERT_EQ_INT(1900, (int)gy);
    ClickMap_GroundUnderPoint(777, 1100, tilt, flat64, NULL, &gx, &gy);
    ASSERT_EQ_INT(777, (int)gx);
    ASSERT_EQ_INT(1132, (int)gy);
    /* A cliff: the pointer at the foot of the drawn face lands on the
     * high ground in front, not the low ground behind it. */
    ClickMap_GroundUnderPoint(10, 900, tilt, cliff, NULL, &gx, &gy);
    ASSERT_EQ_INT(1000, (int)gy);
    ClickMap_GroundUnderPoint(10, 800, tilt, cliff, NULL, &gx, &gy);
    ASSERT_EQ_INT(800, (int)gy);
    /* Round trip over a slope, many pointers, both axes carried. */
    int worst = 0;
    for (int32_t sy = 100; sy < 1900; sy += 7) {
        for (int32_t sx = 3; sx < 3000; sx += 997) {
            ClickMap_GroundUnderPoint(sx, sy, tilt, slope, NULL, &gx, &gy);
            ASSERT_EQ_INT((int)sx, (int)gx);
            int32_t back = ClickMap_DrawnY(gy, (float)slope(NULL, gx, gy), tilt);
            int err = (int)(back - sy);
            if (err < 0) err = -err;
            if (err > worst) worst = err;
            ASSERT_EQ_INT(0, err);
            ASSERT(gy >= sy);
        }
    }
    printf("(worst round trip %d px) ", worst);
    /* Another tilt: the lift scales with it. */
    ASSERT_EQ_INT(1884, (int)ClickMap_DrawnY(1900, 64.0f, 0.25f));
    ClickMap_GroundUnderPoint(1500, 1884, 0.25f, flat64, NULL, &gx, &gy);
    ASSERT_EQ_INT(1900, (int)gy);
}

/* A ray from a raised eye meets a box hanging in the air, such as a
 * flyer at its height, on the face toward the eye, and misses it when
 * aimed at the ground under it. */
TEST(a_ray_meets_a_box_in_the_air_on_its_near_face) {
    const float lo[3] = { 90.0f, 140.0f, 190.0f };
    const float hi[3] = { 110.0f, 180.0f, 210.0f };
    const float eye[3] = { 100.0f, 1000.0f, 700.0f };
    float centre[3] = { 100.0f, 160.0f, 200.0f };
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = centre[k] - eye[k];
    float t = -1.0f;
    ASSERT_EQ_INT(1, ClickMap_RayHitsBox(eye, d, lo, hi, &t));
    ASSERT(t > 0.0f && t < 1.0f);
    /* The entry point lies on the box. */
    for (int k = 0; k < 3; k++) {
        float p = eye[k] + d[k] * t;
        ASSERT(p >= lo[k] - 0.01f && p <= hi[k] + 0.01f);
    }
    /* Aimed at the ground under the box, the ray misses it. */
    const float ground[3] = { 100.0f, 0.0f, 200.0f };
    for (int k = 0; k < 3; k++) d[k] = ground[k] - eye[k];
    ASSERT_EQ_INT(0, ClickMap_RayHitsBox(eye, d, lo, hi, &t));
    /* A ray along an axis, and one starting inside. */
    const float side[3] = { 0.0f, 160.0f, 200.0f };
    const float east[3] = { 1.0f, 0.0f, 0.0f };
    ASSERT_EQ_INT(1, ClickMap_RayHitsBox(side, east, lo, hi, &t));
    ASSERT(t > 89.9f && t < 90.1f);
    ASSERT_EQ_INT(1, ClickMap_RayHitsBox(centre, east, lo, hi, &t));
    ASSERT(t == 0.0f);
    const float west[3] = { -1.0f, 0.0f, 0.0f };
    ASSERT_EQ_INT(0, ClickMap_RayHitsBox(side, west, lo, hi, &t));
}

int main(void) {
    TEST_SUITE("Click mapping");
    RUN(a_window_pixel_lands_on_a_stated_canvas_pixel);
    RUN(a_canvas_rect_round_trips_through_the_window);
    RUN(the_hud_edge_falls_on_the_same_window_pixel_both_ways);
    RUN(a_window_pixel_is_the_camera_plus_itself_in_the_world);
    RUN(a_page_click_is_one_world_pixel_per_css_pixel_at_any_ratio);
    RUN(the_ground_under_a_pointer_is_the_ground_that_draws_there);
    RUN(a_ray_meets_a_box_in_the_air_on_its_near_face);
    TEST_REPORT();
}
