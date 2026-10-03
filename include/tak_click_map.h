/*
 * tak_click_map.h -- the one screen to world mapping every click, box,
 * button and ghost shares.
 *
 * Pure arithmetic with no SDL in it, so the browser build runs the same
 * check the desktop does (src/ui/test_click_map.c).
 */
#ifndef TAK_CLICK_MAP_H
#define TAK_CLICK_MAP_H

#include <stdint.h>

/* Window pixel to UI canvas pixel. The canvas is stretched over the
 * whole window, each axis on its own scale. Returns 0 off the canvas. */
int ClickMap_WindowToCanvas(int win_w, int win_h, int can_w, int can_h,
                            int wx, int wy, int *cx, int *cy);

/* Canvas rect to window rect, the inverse of the above. The far edge is
 * scaled rather than the width so neighbouring rects stay seamless.
 * out is x, y, w, h. */
void ClickMap_CanvasRectToWindow(int win_w, int win_h, int can_w, int can_h,
                                 int x, int y, int w, int h, int out[4]);

/* Window pixel to the flat world pixel under a camera. The world draws
 * one world pixel per window pixel. */
void ClickMap_WindowToWorld(int32_t cam_x, int32_t cam_y, int wx, int wy,
                            int32_t *x, int32_t *y);

/* Where a point of the given height draws: lifted up the screen by the
 * height times the view's tilt (legacy:197689). */
int32_t ClickMap_DrawnY(int32_t world_y, float height, float tan_tilt);

/* The ground that draws at a flat point. Walks back from the farthest
 * candidate and takes the first that projects onto the point, the face
 * in front (legacy:212277). height reads the ground at a world point. */
typedef int (*ClickMap_HeightFn)(void *ctx, int32_t x, int32_t y);
void ClickMap_GroundUnderPoint(int32_t flat_x, int32_t flat_y, float tan_tilt,
                               ClickMap_HeightFn height, void *ctx,
                               int32_t *gx, int32_t *gy);

/* Where a ray first meets a box with the given corners: 1 with the
 * distance along dir in *t, 0 for a miss. A ray that starts inside the
 * box meets it at 0. The 3D view picks a unit this way. */
int ClickMap_RayHitsBox(const float origin[3], const float dir[3],
                        const float lo[3], const float hi[3], float *t);

#endif /* TAK_CLICK_MAP_H */
