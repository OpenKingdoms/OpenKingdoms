/*
 * minimap.c -- in-battle minimap widget (Phase B2 scope).
 *
 * Displays the TNT's pre-rendered overview image (ingame_minimap_bg,
 * 8-bit paletted, ~440×431 in shipped maps) in the top-right of the
 * window. Uses the world's terrain palette to expand the 8-bit indices
 * into RGBA, uploads once per world load, and SDL_RenderCopy-scales
 * to a fixed corner size on every draw.
 *
 * The bigger HUD is Phase D work. Phase D additions, in rough order
 * of value:
 *   - Camera-viewport rectangle overlaid on the minimap (a scaled-down
 *     outline showing the current cam_x/cam_y + viewport_w/h region).
 *   - Per-unit pixel dots, coloured by team/faction: green for the
 *     local player, blue for allies, red for enemies, grey for fog.
 *     Needs the unit list from the simulation, which doesn't exist
 *     yet — bolt on when Phase C's unit manager lands.
 *   - Click-to-move-camera: mapping minimap click coords back to world
 *     coords, setting world->cam_x/cam_y so the player can jump around
 *     the map.
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
                SDL_Rect rc = { x0, y0, x1 - x0, y1 - y0 };
                SDL_RenderFillRect(plat->renderer, &rc);
            }
        }
        SDL_SetRenderDrawBlendMode(plat->renderer, prev_blend);
    }

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
