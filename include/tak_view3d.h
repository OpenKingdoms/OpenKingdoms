/*
 * tak_view3d.h -- the 3D view: the same world under a free camera.
 *
 * It implements the view seam (tak_view.h) with terrain built from the
 * heightmap, the shipped models posed by the same COB piece state the
 * classic view uses, map features, and water at the map's water line.
 * All of its GL goes through tak_gl3d.h and its camera arithmetic is
 * tak_camera3d.h. It reads the simulation and writes nothing to it.
 */
#ifndef TAK_VIEW3D_H
#define TAK_VIEW3D_H

#include "tak_view.h"
#include "tak_camera3d.h"

struct GameWorld;

const TAK_View *View_3D(void);

/* Whether init succeeded on this platform. */
int  View3D_IsReady(void);

/* What the last 3D frame drew, for tests. */
typedef struct View3DDrawCounts {
    int units, features, projectiles, effects, beams, ghosts;
} View3DDrawCounts;

/* The build preview for this frame: the building stands at the site
 * the pointer picks, in the player's colour, tinted for a site that
 * will or will not take it. Asked once a frame by the HUD; drawn with
 * the next frame and then forgotten. */
void View3D_SetBuildGhost(int def_idx, int color_idx, int32_t world_x,
                          int32_t world_y, int valid);
View3DDrawCounts View3D_DebugDrawCounts(void);

/* Entering from the classic view: put the free camera at the classic
 * angle over the middle of the classic viewport. Leaving: put the
 * classic camera over the same spot. */
void View3D_EnterFrom(const struct GameWorld *world);
void View3D_LeaveTo(struct GameWorld *world);

/* One frame of camera input: the orbit and tilt keys, zoom keys and
 * wheel, the preset key, and a middle button drag. Window pixels. */
void View3D_Input(struct GameWorld *world, const TAK_Platform *plat,
                  const uint8_t *keys, const uint8_t *prev_keys,
                  float frame_dt, int mouse_x, int mouse_y,
                  int middle_down, int wheel_dy);

/* The camera itself, for a preset from the command line or a test. */
Camera3D *View3D_Camera(void);

/* Average milliseconds the last measured window of frames spent in the
 * 3D render, and how many frames it covered. */
double View3D_RenderMsAverage(int *out_frames);

#endif /* TAK_VIEW3D_H */
