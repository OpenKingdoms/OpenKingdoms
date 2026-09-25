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
#include "tak_ui.h"
#include "tak_unit.h"
#include <stdio.h>
#include <string.h>

static struct {
    GPU_Texture *tex;
    int          src_w;    /* native source dims, for aspect-preserving draw */
    int          src_h;
    /* The fog over the radar, a pixel per fog cell, drawn scaled. */
    SDL_Texture *fog_tex;
    int          fog_w, fog_h;
    uint8_t     *fog_px;
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
 * sight covers the ground they stand on, or with Line of Sight off
 * while that ground is explored (legacy:208633-208707). That is the
 * rule Units_IsVisibleToLocalPlayer applies. */
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

/* ── The picture, without a renderer ──────────────────────────────── */

static void thumb_put(uint8_t *out, int tw, int th, int x, int y,
                      uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= tw || y >= th) return;
    uint8_t *p = out + ((size_t)y * (size_t)tw + (size_t)x) * 3u;
    p[0] = r; p[1] = g; p[2] = b;
}

int Minimap_RenderThumbnail(uint8_t *out_rgb, int tw, int th) {
    const GameWorld *world = World_Get();
    if (!out_rgb || tw <= 0 || th <= 0) return -1;
    if (!world || !world->tnt.ingame_minimap_bg ||
        world->tnt.minimap_bg_w <= 0 || world->tnt.minimap_bg_h <= 0) {
        return -1;
    }
    SDL_PixelFormat *fmt = UI_RGBAFormat();
    if (!fmt) return -1;

    const int sw = world->tnt.minimap_bg_w;
    const int sh = world->tnt.minimap_bg_h;
    const uint8_t *src = world->tnt.ingame_minimap_bg;

    /* Nearest sample of the overview image. The sidebar radar fits the
     * image to its slot and leaves the margin black; here the panel is
     * the picture, so the whole image is stretched into it. */
    for (int y = 0; y < th; y++) {
        int sy = (int)(((int64_t)y * sh) / th);
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < tw; x++) {
            int sx = (int)(((int64_t)x * sw) / tw);
            if (sx >= sw) sx = sw - 1;
            uint8_t r, g, b;
            SDL_GetRGB(world->terrain_rgba[src[(size_t)sy * (size_t)sw + sx]],
                       fmt, &r, &g, &b);
            thumb_put(out_rgb, tw, th, x, y, r, g, b);
        }
    }

    /* Fog, the same three cases the radar composites: never seen is
     * black, seen and not currently visible is darkened, visible is
     * left alone. The middle case needs Line of Sight on
     * (legacy:208407-208418, legacy:167211-167219). */
    int viewer = Fog_Viewer();
    if (viewer >= 0 &&
        viewer <= TAK_MAX_PLAYERS && world->fog_layers[viewer] &&
        world->map_pixels_w > 0 && world->map_pixels_h > 0) {
        const uint8_t *fog = world->fog_layers[viewer];
        for (int y = 0; y < th; y++) {
            int wy = (int)(((int64_t)y * world->map_pixels_h) / th);
            int fy = wy / world->fog_cell_px;
            if (fy < 0) fy = 0;
            if (fy >= world->fog_h) fy = world->fog_h - 1;
            for (int x = 0; x < tw; x++) {
                int wx = (int)(((int64_t)x * world->map_pixels_w) / tw);
                int fx = wx / world->fog_cell_px;
                if (fx < 0) fx = 0;
                if (fx >= world->fog_w) fx = world->fog_w - 1;
                int st = fog[(size_t)fy * (size_t)world->fog_w + fx];
                if (st == TAK_FOG_VISIBLE) continue;
                if (st == TAK_FOG_EXPLORED && !world->cfg.line_of_sight)
                    continue;
                uint8_t *p = out_rgb + ((size_t)y * (size_t)tw + x) * 3u;
                if (st == TAK_FOG_EXPLORED) {
                    /* The radar's stand in for the shade table, which
                     * is a 120 of 255 black over what is there. */
                    p[0] = (uint8_t)((int)p[0] * 135 / 255);
                    p[1] = (uint8_t)((int)p[1] * 135 / 255);
                    p[2] = (uint8_t)((int)p[2] * 135 / 255);
                } else {
                    p[0] = p[1] = p[2] = 0;
                }
            }
        }
    }

    /* One dot per unit the viewer may see, in its owner's colour. Two
     * pixels at this scale, where the radar draws four. */
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (units && count > 0 && world->map_pixels_w > 0 && world->map_pixels_h > 0) {
        for (int i = 0; i < count; i++) {
            const Unit *u = &units[i];
            if (u->alive != UNIT_ALIVE_ACTIVE) continue;
            if (!Units_IsVisibleToLocalPlayer(u)) continue;
            int dx = (int)(((int64_t)u->world_x * tw) / world->map_pixels_w);
            int dy = (int)(((int64_t)u->world_y * th) / world->map_pixels_h);
            uint32_t rgba = Units_GetTeamColorRGBA(u->team_color_idx);
            uint8_t r = (uint8_t)(rgba & 0xFFu);
            uint8_t g = (uint8_t)((rgba >> 8) & 0xFFu);
            uint8_t b = (uint8_t)((rgba >> 16) & 0xFFu);
            for (int oy = 0; oy < 2; oy++) {
                for (int ox = 0; ox < 2; ox++) {
                    thumb_put(out_rgb, tw, th, dx + ox, dy + oy, r, g, b);
                }
            }
        }
    }
    return 0;
}

static uint32_t g_fog_draws;
uint32_t Minimap_DebugFogDraws(void) { return g_fog_draws; }
int Minimap_DebugMapRect(TAK_Platform *plat, SDL_Rect *out) {
    return plat && out && mm.tex ? minimap_compute_rect(plat, out) : 0;
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
    if (world->fog_layers[Fog_Viewer()] && world->fog_w > 0 && world->fog_h > 0) {
        /* A pixel per cell and one scaled draw, where a fill per cell
         * was thousands of draws a frame on a map nobody has seen. */
        if (!mm.fog_tex || !mm.fog_px || mm.fog_w != world->fog_w || mm.fog_h != world->fog_h) {
            if (mm.fog_tex) SDL_DestroyTexture(mm.fog_tex);
            tak_free(mm.fog_px);
            mm.fog_w = world->fog_w;
            mm.fog_h = world->fog_h;
            mm.fog_px = (uint8_t *)tak_malloc((size_t)mm.fog_w * (size_t)mm.fog_h * 4u);
            mm.fog_tex = SDL_CreateTexture(plat->renderer, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           mm.fog_w, mm.fog_h);
            if (mm.fog_tex) {
                SDL_SetTextureBlendMode(mm.fog_tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(mm.fog_tex, SDL_ScaleModeNearest);
            }
        }
        if (mm.fog_tex && mm.fog_px) {
            const uint8_t *layer = world->fog_layers[Fog_Viewer()];
            int n = mm.fog_w * mm.fog_h;
            for (int i = 0; i < n; i++) {
                int st = layer[i];
                uint8_t a = 0;
                /* Line of Sight off fills the sight map, so explored
                 * ground reads raw (legacy:167211-167219). Explored
                 * darkens what is there, a stand-in for the shade LUT,
                 * and never seen is black. */
                if (st == TAK_FOG_EXPLORED) a = world->cfg.line_of_sight ? 120 : 0;
                else if (st != TAK_FOG_VISIBLE) a = 255;
                uint8_t *p = &mm.fog_px[i * 4];
                p[0] = p[1] = p[2] = 0;
                p[3] = a;
            }
            SDL_UpdateTexture(mm.fog_tex, NULL, mm.fog_px, mm.fog_w * 4);
            /* The cells' edges as the fills had them. */
            SDL_Rect rc = {
                dst.x, dst.y,
                mm.fog_w * world->fog_cell_px * dw / world->map_pixels_w,
                mm.fog_h * world->fog_cell_px * dh / world->map_pixels_h
            };
            /* A fog grid a little past the map's edge stays on the map. */
            SDL_Rect clip_was;
            int clipped = SDL_RenderIsClipEnabled(plat->renderer);
            SDL_RenderGetClipRect(plat->renderer, &clip_was);
            SDL_RenderSetClipRect(plat->renderer, &dst);
            SDL_RenderCopy(plat->renderer, mm.fog_tex, NULL, &rc);
            SDL_RenderSetClipRect(plat->renderer, clipped ? &clip_was : NULL);
            g_fog_draws++;
        }
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
    if (mm.fog_tex) SDL_DestroyTexture(mm.fog_tex);
    mm.fog_tex = NULL;
    tak_free(mm.fog_px);
    mm.fog_px = NULL;
    mm.fog_w = mm.fog_h = 0;
    if (mm.tex) {
        GPU_FreeTexture(plat, mm.tex);
        mm.tex = NULL;
    }
    mm.src_w = 0;
    mm.src_h = 0;
}
