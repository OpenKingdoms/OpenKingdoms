/*
 * tak_view.h -- the seam between the battle screen and what draws it.
 *
 * A view draws one frame of a read only world into the play area,
 * turns a pointer into a world point and into a unit, and scrolls its
 * own camera. The battle screen holds one active view and forwards to
 * it. The classic renderer is the first view, by delegation to the
 * calls it always made (src/ui/view_classic.c), and the 3D renderer
 * is the second (src/render/view3d.c).
 */
#ifndef TAK_VIEW_H
#define TAK_VIEW_H

#include "tak_platform.h"
#include <stdint.h>

struct GameWorld;

typedef struct TAK_View {
    const char *name;

    /* Bring the view up on the platform. 0, or -1 when it cannot run
     * here, in which case the screen stays on the view it had. */
    int  (*init)(TAK_Platform *plat);
    void (*shutdown)(TAK_Platform *plat);

    /* Draw the world into the viewport, given in window pixels. */
    void (*render)(const struct GameWorld *world, TAK_Platform *plat,
                   const SDL_Rect *viewport);

    /* The flat world point under a window pixel, in the coordinates the
     * order and pick paths take (the classic reading, which they then
     * lift onto the ground themselves). Returns 0 off the world. */
    int  (*pointer_to_world)(const struct GameWorld *world,
                             const TAK_Platform *plat, int wx, int wy,
                             int32_t *out_x, int32_t *out_y);

    /* The unit under a window pixel, or -1. */
    int  (*pointer_to_unit)(const struct GameWorld *world,
                            const TAK_Platform *plat, int wx, int wy);

    /* Scroll the camera by window pixels right and down. */
    void (*scroll)(struct GameWorld *world, int32_t dx, int32_t dy);
} TAK_View;

const TAK_View *View_Classic(void);
/* The classic view's fog: the mist (D-053) when on and the renderer is
 * not software, else the original's overlay. */
void View_ClassicSetMist(int on);
/* Move the mist by the sim tick instead of render time, for a capture
 * that must draw the same frame every run. */
void View_ClassicSetSimClock(int on);

#endif /* TAK_VIEW_H */
