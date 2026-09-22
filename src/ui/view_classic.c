/*
 * view_classic.c -- the classic renderer behind the view seam.
 *
 * Delegation only: the calls are the ones the battle screen always
 * made, in the same order, and the pointer mapping is the flat one it
 * always used. Nothing the classic view draws changed for this file.
 */

#include "tak_view.h"
#include "tak_world.h"
#include "tak_terrain.h"
#include "tak_fog.h"
#include "tak_unit.h"
#include "tak_click_map.h"

static int classic_init(TAK_Platform *plat) {
    (void)plat;
    return 0;
}

static void classic_shutdown(TAK_Platform *plat) {
    (void)plat;
}

static void classic_render(const GameWorld *world, TAK_Platform *plat,
                           const SDL_Rect *viewport) {
    (void)viewport;
    Terrain_Render(world, plat);
    /* A feature is ground, and the fog covers ground: a dock whose
     * planks reach past the known map ends where the black begins. */
    Units_RenderFeatures(world, plat);
    Fog_RenderOverlay(world, plat);
    Units_Render(world, plat);
}

static int classic_pointer_to_world(const GameWorld *world,
                                    const TAK_Platform *plat, int wx, int wy,
                                    int32_t *out_x, int32_t *out_y) {
    (void)plat;
    if (!world) return 0;
    ClickMap_WindowToWorld(world->cam_x, world->cam_y, wx, wy, out_x, out_y);
    return 1;
}

static int classic_pointer_to_unit(const GameWorld *world,
                                   const TAK_Platform *plat, int wx, int wy) {
    (void)plat;
    if (!world) return -1;
    return Units_PickAt(world->cam_x + wx, world->cam_y + wy, 48);
}

/* Move the camera and keep it on the map. */
static void classic_scroll(GameWorld *world, int32_t dx, int32_t dy) {
    if (!world || (!dx && !dy)) return;
    int32_t new_x = world->cam_x + dx;
    int32_t new_y = world->cam_y + dy;
    int32_t max_x = world->map_pixels_w - world->viewport_w;
    int32_t max_y = world->map_pixels_h - world->viewport_h;
    if (new_x < 0) new_x = 0; else if (new_x > max_x) new_x = max_x;
    if (new_y < 0) new_y = 0; else if (new_y > max_y) new_y = max_y;
    world->cam_x = new_x;
    world->cam_y = new_y;
}

static const TAK_View k_classic = {
    "classic",
    classic_init,
    classic_shutdown,
    classic_render,
    classic_pointer_to_world,
    classic_pointer_to_unit,
    classic_scroll,
};

const TAK_View *View_Classic(void) {
    return &k_classic;
}
