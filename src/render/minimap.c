/*
 * minimap.c -- in-battle minimap widget (Phase B2 scope).
 *
 * Displays the TNT's pre-rendered overview image (ingame_minimap_bg,
 * 8-bit paletted, ~440×431 in shipped maps) in the top-right of the
 * window. Uses the world's terrain palette to expand the 8-bit indices
 * into RGBA, uploads once per world load, and SDL_RenderCopy-scales
 * to a fixed corner size on every draw.
 *
 * Over that image go the fog composite, one dot per visible unit in
 * its owner's colour, and the camera-viewport outline, in that order.
 * The original composites the same three layers and puts the view box
 * on top of the blips (legacy:208152-208344).
 *
 * Placement comes from the HUD: the minimap fills the sidebar column
 * above the sidebar panel art, the slot legacy leaves for it. The
 * in-game .gui authors no widget there, the engine owns the radar
 * surface (:207443-208600).
 */

#include "tak_minimap.h"
#include "tak_gpu.h"
#include "tak_world.h"
#include "tak_memory.h"
#include "tak_fog.h"
#include "tak_hud.h"
#include "tak_unit.h"
#include <stdio.h>
#include <string.h>

static struct {
    GPU_Texture *tex;
    int          src_w;    /* native source dims, for aspect-preserving draw */
    int          src_h;
} mm;

int Minimap_Init(TAK_Platform *plat) {
    if (!plat) return -1;
    if (mm.tex) return 0;  /* idempotent re-entry (ESC-to-menu-and-back) */

    const GameWorld *world = World_Get();
    if (!world || !world->tnt.ingame_minimap_bg ||
        world->tnt.minimap_bg_w <= 0 || world->tnt.minimap_bg_h <= 0) {
        fprintf(stderr, "Minimap_Init: no ingame_minimap_bg on loaded world\n");
        return -1;
    }

    int w = world->tnt.minimap_bg_w;
    int h = world->tnt.minimap_bg_h;

    /* Palette-expand: one RGBA lookup per source byte. The image is
     * small (~190K pixels), this is ~1 ms of work, not worth any
     * SIMD. */
    size_t n = (size_t)w * (size_t)h;
    uint32_t *rgba = (uint32_t *)tak_malloc(n * sizeof(uint32_t));
    if (!rgba) return -1;
    const uint8_t *src = world->tnt.ingame_minimap_bg;
    for (size_t i = 0; i < n; i++) {
        rgba[i] = world->terrain_rgba[src[i]];
    }

    mm.tex = GPU_UploadRGBA(plat, rgba, w, h);
    tak_free(rgba);
    if (!mm.tex) {
        fprintf(stderr, "Minimap_Init: GPU_UploadRGBA failed\n");
        return -1;
    }

    /* Minimap is nearly always drawn at a size smaller than its source
     * resolution, so prefer linear filtering over nearest. */
    GPU_SetTextureFilter(mm.tex, 1);

    mm.src_w = w;
    mm.src_h = h;
    fprintf(stderr, "Minimap_Init: %dx%d uploaded\n", w, h);
    return 0;
}

/* The minimap slot is the sidebar column above the sidebar panel art.
 * The HUD reads it off the dialog and returns it in window pixels.
 * Returns 0 when there's no minimap or no slot. */
static int minimap_slot_rect(TAK_Platform *plat, SDL_Rect *out) {
    if (!plat || !mm.tex) return 0;
    return HUD_GetMinimapRect(plat, out);
}

/* Unit blip size. The original loads five blip shapes from the blips
 * anim file and picks between them by unit kind, but only once the
 * expanded map view is up (legacy:208481). The corner radar always
 * takes the first shape (legacy:208480), a 4x4 frame whose hotspot
 * sits one pixel in from its top-left corner. */
#define MINIMAP_DOT_PX      4
#define MINIMAP_DOT_ORIGIN  1

/* Where the map image itself lands: aspect-preserving fit, centred in
 * the slot. Legacy fits the map into the radar surface the same way
 * (:208355-208400). Whatever the fit leaves over stays black. */
static int minimap_compute_rect(TAK_Platform *plat, SDL_Rect *out) {
    SDL_Rect slot;
    if (mm.src_w <= 0 || mm.src_h <= 0) return 0;
    if (!minimap_slot_rect(plat, &slot)) return 0;
    if (slot.w <= 0 || slot.h <= 0) return 0;

    int dw = slot.w;
    int dh = (int)((float)slot.w * (float)mm.src_h / (float)mm.src_w + 0.5f);
    if (dh > slot.h) {
        dh = slot.h;
        dw = (int)((float)slot.h * (float)mm.src_w / (float)mm.src_h + 0.5f);
    }
    out->x = slot.x + (slot.w - dw) / 2;
    out->y = slot.y + (slot.h - dh) / 2;
    out->w = dw;
    out->h = dh;
    return 1;
}

/* Dot rect for one world position, clipped to the drawn map rect.
 * Legacy scales the position by the radar rect over the map size and
 * adds the rect origin (legacy:208475-208479). It also shifts north by
 * half the unit's altitude, which is the world view's isometric skew,
 * baked into world_y here instead. Returns 0 when the dot falls
 * outside the map rect. */
static int minimap_dot_rect(const GameWorld *world, const SDL_Rect *map,
                            int32_t world_x, int32_t world_y,
                            SDL_Rect *out) {
    if (!world || world->map_pixels_w <= 0 || world->map_pixels_h <= 0)
        return 0;
    int mx = map->x + (int)((int64_t)world_x * map->w / world->map_pixels_w);
    int my = map->y + (int)((int64_t)world_y * map->h / world->map_pixels_h);
    SDL_Rect r = { mx - MINIMAP_DOT_ORIGIN, my - MINIMAP_DOT_ORIGIN,
                   MINIMAP_DOT_PX, MINIMAP_DOT_PX };
    if (!SDL_IntersectRect(&r, map, out)) return 0;
    return 1;
}

/* One dot per unit the local player may see, in that unit's owner
 * colour. The original walks the whole unit array on every radar
 * refresh and draws every unit carrying the minimap-visible bit
 * (legacy:208470-208524), colouring the blip from the owner's player
 * record rather than the viewer's (legacy:208505). Own units carry the
 * bit unconditionally, anyone else's only while the local player's
 * sight covers the ground they stand on (legacy:208633-208707), which
 * is the rule Units_IsVisibleToLocalPlayer already applies. */
static void minimap_draw_unit_dots(TAK_Platform *plat, const GameWorld *world,
                                   const SDL_Rect *map) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (!units || count <= 0) return;

    SDL_BlendMode prev;
    SDL_GetRenderDrawBlendMode(plat->renderer, &prev);
    SDL_SetRenderDrawBlendMode(plat->renderer, SDL_BLENDMODE_NONE);
    for (int i = 0; i < count; i++) {
        const Unit *u = &units[i];
        /* Riders in a transport get no blip of their own
         * (legacy:208474), and TRANSPORTED is not ACTIVE here. */
        if (u->alive != UNIT_ALIVE_ACTIVE) continue;
        if (!Units_IsVisibleToLocalPlayer(u)) continue;
        SDL_Rect dot;
        if (!minimap_dot_rect(world, map, u->world_x, u->world_y, &dot))
            continue;
        uint32_t rgba = Units_GetTeamColorRGBA(u->team_color_idx);
        SDL_SetRenderDrawColor(plat->renderer,
                               (uint8_t)(rgba & 0xFFu),
                               (uint8_t)((rgba >> 8) & 0xFFu),
                               (uint8_t)((rgba >> 16) & 0xFFu), 255);
        SDL_RenderFillRect(plat->renderer, &dot);
    }
    SDL_SetRenderDrawBlendMode(plat->renderer, prev);
}

int Minimap_DebugDotRect(TAK_Platform *plat, int32_t world_x, int32_t world_y,
                         SDL_Rect *out) {
    SDL_Rect map;
    const GameWorld *world = World_Get();
    if (!out || !world) return 0;
    if (!minimap_compute_rect(plat, &map)) return 0;
    return minimap_dot_rect(world, &map, world_x, world_y, out);
}

void Minimap_Draw(TAK_Platform *plat) {
    if (!plat || !mm.tex) return;
    SDL_Rect slot, dst;
    if (!minimap_slot_rect(plat, &slot)) return;
    if (!minimap_compute_rect(plat, &dst)) return;
    int dw = dst.w;
    int dh = dst.h;

    /* The slot is opaque black under the map image: unmapped ground and
     * the aspect-fit margin read the same as legacy's cleared radar
     * surface (:208407-208418). */
    SDL_BlendMode prev_blend;
    SDL_GetRenderDrawBlendMode(plat->renderer, &prev_blend);
    SDL_SetRenderDrawBlendMode(plat->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(plat->renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(plat->renderer, &slot);
    SDL_SetRenderDrawBlendMode(plat->renderer, prev_blend);

    GPU_DrawToWindow(plat, mm.tex, NULL, &dst);

    const GameWorld *world = World_Get();
    if (!world) {
        fprintf(stderr, "Minimap_Draw: no GameWorld available\n");
        return;
    }

    /* Per-cell composite, exactly legacy's three cases (:208407-208418):
     * never seen writes 0 (black), seen but not currently visible takes
     * the map byte through the fog shade table, visible takes it raw. */
    if (world->fog_state && world->cfg.line_of_sight) {
        SDL_GetRenderDrawBlendMode(plat->renderer, &prev_blend);
        for (int fy = 0; fy < world->fog_h; fy++) {
            for (int fx = 0; fx < world->fog_w; fx++) {
                int st = world->fog_state[fy * world->fog_w + fx];
                if (st == TAK_FOG_VISIBLE) continue;
                if (st == TAK_FOG_EXPLORED) {
                    /* Stand-in for the shade LUT: darken what's there. */
                    SDL_SetRenderDrawBlendMode(plat->renderer,
                                                SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(plat->renderer, 0, 0, 0, 120);
                } else {
                    SDL_SetRenderDrawBlendMode(plat->renderer,
                                                SDL_BLENDMODE_NONE);
                    SDL_SetRenderDrawColor(plat->renderer, 0, 0, 0, 255);
                }
                int x0 = dst.x + fx * world->fog_cell_px * dw / world->map_pixels_w;
                int y0 = dst.y + fy * world->fog_cell_px * dh / world->map_pixels_h;
                int x1 = dst.x + (fx + 1) * world->fog_cell_px * dw / world->map_pixels_w;
                int y1 = dst.y + (fy + 1) * world->fog_cell_px * dh / world->map_pixels_h;
                /* Cells share edges: the next cell starts at x1, y1. */
                SDL_Rect rc = { x0, y0, x1 - x0, y1 - y0 };
                SDL_RenderFillRect(plat->renderer, &rc);
            }
        }
        SDL_SetRenderDrawBlendMode(plat->renderer, prev_blend);
    }

    minimap_draw_unit_dots(plat, world, &dst);

    /* Camera-viewport indicator: clipped to the minimap rect so a
     * very small viewport doesn't visually detach into a stray box,
     * and drawn in a soft yellow that reads as "your view here"
     * rather than a UI artefact. Matches the legacy game's behaviour. */
    int cx = dst.x + (world->cam_x * dw / world->map_pixels_w);
    int cy = dst.y + (world->cam_y * dh / world->map_pixels_h);
    int cw = world->viewport_w * dw / world->map_pixels_w;
    int ch = world->viewport_h * dh / world->map_pixels_h;
    /* Minimum 8×8 so the indicator stays visible on huge maps. */
    if (cw < 8) cw = 8;
    if (ch < 8) ch = 8;
    /* Clip to minimap bounds. */
    if (cx < dst.x) { cw -= (dst.x - cx); cx = dst.x; }
    if (cy < dst.y) { ch -= (dst.y - cy); cy = dst.y; }
    if (cx + cw > dst.x + dw) cw = dst.x + dw - cx;
    if (cy + ch > dst.y + dh) ch = dst.y + dh - cy;
    SDL_Rect camera_rect = { cx, cy, cw, ch };

    SDL_SetRenderDrawColor(plat->renderer, 230, 220, 120, 200);
    SDL_RenderDrawRect(plat->renderer, &camera_rect);
}

int Minimap_HandleInput(TAK_Platform *plat,
                         int win_mouse_x, int win_mouse_y,
                         int left_button_down,
                         int32_t *out_cam_x, int32_t *out_cam_y) {
    if (!plat || !out_cam_x || !out_cam_y) return 0;
    if (!mm.tex) return 0;

    SDL_Rect dst;
    if (!minimap_compute_rect(plat, &dst)) return 0;

    /* Hit-test: must be left-button-held AND over the minimap rect.
     * Supporting both point-and-click and drag falls out naturally —
     * as long as the button is held inside the rect, we keep jumping
     * the camera each frame. Release = stop. */
    if (!left_button_down) return 0;
    if (win_mouse_x < dst.x || win_mouse_x >= dst.x + dst.w) return 0;
    if (win_mouse_y < dst.y || win_mouse_y >= dst.y + dst.h) return 0;

    const GameWorld *world = World_Get();
    if (!world || !world->loaded) return 0;

    /* ---------- TODO (user): fill this in ----------
     *
     * Given:
     *   win_mouse_x, win_mouse_y   window-pixel mouse coords
     *   dst                         minimap's window rect (x, y, w, h)
     *   world->map_pixels_w/h       full map size in world pixels
     *   world->viewport_w/h         current viewport size
     *
     * Compute:
     *   1. Mouse offset inside the minimap rect:
     *        rel_x = win_mouse_x - dst.x
     *        rel_y = win_mouse_y - dst.y
     *
     *   2. Inverse scale to world coords (what the mouse is pointing
     *      AT on the full map — integer math, multiply first to
     *      avoid truncating to zero, same trick as Minimap_Draw):
     *        target_wx = rel_x * map_pixels_w / dst.w
     *        target_wy = rel_y * map_pixels_h / dst.h
     *
     *   3. Turn that point into a camera top-left (what cam_x/cam_y
     *      actually hold) by subtracting half the viewport so the
     *      clicked point ends up centered on screen:
     *        cam_x = target_wx - viewport_w / 2
     *        cam_y = target_wy - viewport_h / 2
     *
     *   4. Clamp to valid camera range [0, map_pixels_{w,h} - viewport_{w,h}].
     *      Match the clamp already used in ingame.c so behaviour is
     *      consistent with WASD scroll.
     *
     * Write the results to *out_cam_x and *out_cam_y. Leave the
     * return 1 below intact — it signals "I consumed the click,
     * skip edge-scroll this frame".
     * ------------------------------------------------- */

    int rel_x = win_mouse_x - dst.x;
    int rel_y = win_mouse_y - dst.y;

    int target_world_x = rel_x * world->map_pixels_w / dst.w;
    int target_world_y = rel_y * world->map_pixels_h / dst.h;

    int cam_x = target_world_x - world->viewport_w / 2;
    int cam_y = target_world_y - world->viewport_h / 2;

    if (cam_x < 0) cam_x = 0;
    if (cam_y < 0) cam_y = 0;
    if (cam_x > world->map_pixels_w - world->viewport_w) cam_x = world->map_pixels_w - world->viewport_w;
    if (cam_y > world->map_pixels_h - world->viewport_h) cam_y = world->map_pixels_h - world->viewport_h;

    *out_cam_x = cam_x;
    *out_cam_y = cam_y;

    return 1;
}

void Minimap_Shutdown(TAK_Platform *plat) {
    if (mm.tex) {
        GPU_FreeTexture(plat, mm.tex);
        mm.tex = NULL;
    }
    mm.src_w = 0;
    mm.src_h = 0;
}
