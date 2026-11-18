/*
 * view_classic.c -- the classic renderer behind the view seam.
 *
 * Delegation only: the calls are the ones the battle screen always
 * made, with the fog after the scene as the original draws it, and the
 * pointer mapping is the flat one it always used. The one addition is
 * the Shift order overlay, drawn last.
 */

#include "tak_view.h"
#include "tak_world.h"
#include "tak_terrain.h"
#include "tak_fog.h"
#include "tak_fog_mist.h"
#include "tak_sides.h"
#include "tak_unit.h"
#include "tak_click_map.h"
#include "tak_order_overlay.h"

static int g_mist_on = 1;
static int g_sim_clock;

void View_ClassicSetMist(int on) { g_mist_on = on ? 1 : 0; }
void View_ClassicSetSimClock(int on) { g_sim_clock = on ? 1 : 0; }

/* The software renderer keeps the original's overlay: the mist costs it
 * tens of milliseconds a frame. */
static int classic_mist(const TAK_Platform *plat) {
    SDL_RendererInfo info;
    if (!g_mist_on || SDL_GetRendererInfo(plat->renderer, &info) != 0) return 0;
    return (info.flags & SDL_RENDERER_SOFTWARE) == 0;
}

static void classic_fog(const GameWorld *world, TAK_Platform *plat) {
    if (!classic_mist(plat)) {
        Fog_RenderOverlay(world, plat);
        return;
    }
    /* Render time in play, so it drifts while paused, the sim tick under
     * a capture. The tint is the side of the seat whose fog is shown. */
    uint32_t clock_ms = g_sim_clock ? (uint32_t)((uint64_t)Units_SimTick() * 1000u / 60u)
                                    : SDL_GetTicks();
    int seat = Fog_Viewer() >= 1 ? Fog_Viewer() : Units_LocalPlayer();
    const TakSideInfo *side = Sides_Get(Units_PlayerSide(seat));
    static const uint8_t white[3] = { 255, 255, 255 };
    FogMist_Render(world, plat, clock_ms, side ? side->name : NULL,
                   side ? side->fog_rgb : white);
}

static int classic_init(TAK_Platform *plat) {
    (void)plat;
    return 0;
}

static void classic_shutdown(TAK_Platform *plat) {
    FogMist_Shutdown(plat);
}

static void classic_render(const GameWorld *world, TAK_Platform *plat,
                           const SDL_Rect *viewport) {
    (void)viewport;
    Terrain_Render(world, plat);
    Units_RenderFeatures(world, plat);
    Fog_RenderJamHaze(world, plat);   /* on the ground, under units (D-045) */
    Units_Render(world, plat);
    /* The original lays its fog over the whole scene, shots and flyers
     * too (docs/notes/2026-09-04-rendering-visuals.md, frame order). */
    classic_fog(world, plat);
    /* The selection's orders, while Shift is held. */
    (void)OrderOverlay_Draw(world, plat);
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
    return Units_PickAt(world->cam_x + wx, world->cam_y + wy, 0);
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
