#include "tak_fog.h"
#include "tak_world.h"
#include "tak_unit.h"
#include "tak_terrain.h"
#include "tak_memory.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

/* Source-truth anchors in the legacy reference:
 *   131932  GameSession_SetFogOfWar stores the session LOS toggle.
 *   226956/234312  unit movement calls Minimap_UpdateFog.
 *   240923/240940  game-loop fog checks walk active units.
 *   130167-130436  Fog_DrawOverlay — the legacy overlay algorithm the
 *                  renderer below reproduces (see docs/digs/
 *                  2026-09-04-rendering-visuals.md §Fog).
 * This module keeps the same model: line-of-sight is session-configured,
 * recomputed from live units, and consumed by world + minimap rendering.
 *
 * Legacy fog cells are 32 px — half map-cell resolution, the same grid
 * as the 32px graphic tiles (:130169). */
#define FOG_CELL_PX 32

static int fog_idx(const GameWorld *w, int x, int y) {
    return y * w->fog_w + x;
}

/* Per-unit reveal cache (see Fog_Update). Keyed by unit handle; a
 * reused handle recomputes on the position test. */
#define FOG_CACHE_MAX 2048
typedef struct FogUnitCache {
    int32_t  x, y;
    int32_t *cells;
    int32_t  cap;
    int32_t  n;
    int16_t  sight;
    int8_t   valid;
} FogUnitCache;
static FogUnitCache g_fog_cache[FOG_CACHE_MAX];

static void fog_cache_reset(void) {
    for (int i = 0; i < FOG_CACHE_MAX; i++) {
        if (g_fog_cache[i].cells) tak_free(g_fog_cache[i].cells);
    }
    memset(g_fog_cache, 0, sizeof(g_fog_cache));
}

static uint8_t *fog_layer(GameWorld *world, int player_id) {
    if (!world || player_id < 1 || player_id > TAK_MAX_PLAYERS) return NULL;
    return world->fog_layers[player_id];
}

static const uint8_t *fog_layer_const(const GameWorld *world, int player_id) {
    if (!world || player_id < 1 || player_id > TAK_MAX_PLAYERS) return NULL;
    return world->fog_layers[player_id];
}

/* Per-corner overlay alpha — the legacy three-level scheme
 * (:130295-130346): opaque black over unexplored, 0x78 black over
 * explored-but-not-visible, clear over visible. Off-map reads as
 * unexplored, matching the legacy edge handling. */
static uint8_t fog_corner_alpha(const GameWorld *world,
                                const uint8_t *layer, int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= world->fog_w || cy >= world->fog_h)
        return 0xFF;
    switch (layer[fog_idx(world, cx, cy)]) {
        case TAK_FOG_VISIBLE:  return 0x00;
        case TAK_FOG_EXPLORED: return 0x78;
        default:               return 0xFF;
    }
}

int Fog_Init(GameWorld *world) {
    if (!world || world->map_pixels_w <= 0 || world->map_pixels_h <= 0)
        return -1;
    Fog_Free(world);
    world->fog_cell_px = FOG_CELL_PX;
    world->fog_w = (world->map_pixels_w + FOG_CELL_PX - 1) / FOG_CELL_PX;
    world->fog_h = (world->map_pixels_h + FOG_CELL_PX - 1) / FOG_CELL_PX;
    size_t n = (size_t)world->fog_w * (size_t)world->fog_h;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        world->fog_layers[p] = (uint8_t *)tak_malloc(n);
        if (!world->fog_layers[p]) {
            Fog_Free(world);
            return -1;
        }
        memset(world->fog_layers[p],
               world->cfg.map_revealed ? TAK_FOG_EXPLORED : TAK_FOG_UNEXPLORED,
               n);
    }
    world->fog_state = world->fog_layers[1];
    fog_cache_reset();
    return 0;
}

void Fog_Free(GameWorld *world) {
    if (!world) return;
    fog_cache_reset();
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (world->fog_layers[p]) tak_free(world->fog_layers[p]);
        world->fog_layers[p] = NULL;
    }
    world->fog_state = NULL;
    world->fog_w = world->fog_h = world->fog_cell_px = 0;
}

static int los_clear(const GameWorld *w,
                     int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    int base_a = Terrain_SampleHeight(w, ax, ay) + 18;
    int base_b = Terrain_SampleHeight(w, bx, by) + 6;
    int32_t dx = bx - ax;
    int32_t dy = by - ay;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    steps /= 32;
    if (steps < 1) return 1;
    for (int i = 1; i < steps; i++) {
        float t = (float)i / (float)steps;
        int32_t x = ax + (int32_t)((float)dx * t);
        int32_t y = ay + (int32_t)((float)dy * t);
        int ray_h = base_a + (int)((float)(base_b - base_a) * t);
        if (Terrain_SampleHeight(w, x, y) > ray_h + 8) return 0;
    }
    return 1;
}

void Fog_Update(GameWorld *world, int player_id) {
    uint8_t *layer = fog_layer(world, player_id);
    if (!world || !layer || world->fog_w <= 0 || world->fog_h <= 0)
        return;
    size_t n = (size_t)world->fog_w * (size_t)world->fog_h;
    if (!world->cfg.line_of_sight) {
        memset(layer, TAK_FOG_VISIBLE, n);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (layer[i] == TAK_FOG_VISIBLE)
            layer[i] = TAK_FOG_EXPLORED;
        else if (world->cfg.map_revealed &&
                 layer[i] == TAK_FOG_UNEXPLORED)
            layer[i] = TAK_FOG_EXPLORED;
    }

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->alive != 1 || u->player_id != (uint8_t)player_id) continue;
        /* A nanoframe grants no vision — sight arrives with the
         * finished building, not the moment construction starts. */
        if (u->under_construction) continue;
        const UnitDef *def = Units_GetDef((int)u->def_idx);
        int sight = (def && def->sight_distance > 0) ? def->sight_distance : 256;

        /* Per-unit reveal cache: the LOS raycast scan was 95% of the
         * browser CPU trace, and most late-game units are stationary
         * (buildings, parked armies). Recompute only after the unit
         * moves >= 16px; otherwise re-stamp the cached cell list. */
        FogUnitCache *c = (i < FOG_CACHE_MAX) ? &g_fog_cache[i] : NULL;
        if (c && c->valid && c->sight == (int16_t)sight &&
            labs((long)(u->world_x - c->x)) < 16 &&
            labs((long)(u->world_y - c->y)) < 16) {
            for (int k = 0; k < c->n; k++) layer[c->cells[k]] = TAK_FOG_VISIBLE;
            continue;
        }

        int min_x = (u->world_x - sight) / FOG_CELL_PX;
        int max_x = (u->world_x + sight) / FOG_CELL_PX;
        int min_y = (u->world_y - sight) / FOG_CELL_PX;
        int max_y = (u->world_y + sight) / FOG_CELL_PX;
        if (min_x < 0) min_x = 0;
        if (min_y < 0) min_y = 0;
        if (max_x >= world->fog_w) max_x = world->fog_w - 1;
        if (max_y >= world->fog_h) max_y = world->fog_h - 1;
        int64_t sight2 = (int64_t)sight * sight;
        int cn = 0;
        if (c) {
            int span_x = max_x - min_x + 1;
            int span_y = max_y - min_y + 1;
            int need = span_x * span_y;
            if (need > c->cap) {
                tak_free(c->cells);
                c->cells = (int32_t *)tak_malloc((size_t)need * sizeof(int32_t));
                c->cap = c->cells ? need : 0;
            }
        }
        for (int fy = min_y; fy <= max_y; fy++) {
            for (int fx = min_x; fx <= max_x; fx++) {
                int32_t cx = fx * FOG_CELL_PX + FOG_CELL_PX / 2;
                int32_t cy = fy * FOG_CELL_PX + FOG_CELL_PX / 2;
                int64_t dx = (int64_t)cx - u->world_x;
                int64_t dy = (int64_t)cy - u->world_y;
                if (dx * dx + dy * dy > sight2) continue;
                if (!los_clear(world, u->world_x, u->world_y, cx, cy)) continue;
                int idx = fog_idx(world, fx, fy);
                layer[idx] = TAK_FOG_VISIBLE;
                if (c && c->cells && cn < c->cap) c->cells[cn++] = idx;
            }
        }
        if (c && c->cells) {
            c->valid = 1;
            c->x = u->world_x;
            c->y = u->world_y;
            c->sight = (int16_t)sight;
            c->n = cn;
        }
    }
}

int Fog_StateAtForPlayer(const GameWorld *world, int player_id,
                         int32_t world_x, int32_t world_y) {
    const uint8_t *layer = fog_layer_const(world, player_id);
    if (!world || !layer || !world->cfg.line_of_sight)
        return TAK_FOG_VISIBLE;
    int fx = world_x / FOG_CELL_PX;
    int fy = world_y / FOG_CELL_PX;
    if (fx < 0 || fy < 0 || fx >= world->fog_w || fy >= world->fog_h)
        return TAK_FOG_UNEXPLORED;
    return layer[fog_idx(world, fx, fy)];
}

int Fog_IsVisibleForPlayer(const GameWorld *world, int player_id,
                           int32_t world_x, int32_t world_y) {
    return Fog_StateAtForPlayer(world, player_id, world_x, world_y) == TAK_FOG_VISIBLE;
}

int Fog_StateAt(const GameWorld *world, int32_t world_x, int32_t world_y) {
    return Fog_StateAtForPlayer(world, 1, world_x, world_y);
}

int Fog_IsVisible(const GameWorld *world, int32_t world_x, int32_t world_y) {
    return Fog_IsVisibleForPlayer(world, 1, world_x, world_y);
}

/* Legacy-exact fog overlay (legacy:130167-130436):
 * the quad lattice is shifted by half a fog cell so each quad's four
 * corners land on the CENTRES of the four surrounding fog cells. Each
 * corner samples its cell into one of three alpha levels (0 / 0x78 /
 * 0xFF, pure black). Quads whose corners are all visible draw
 * nothing; uniform quads are flat fills; mixed quads interpolate the
 * corner alphas across the quad (legacy Gouraud, SDL_RenderGeometry
 * here) — that interpolation is the smooth fog edge, with no per-cell
 * seams. */
void Fog_RenderOverlay(const GameWorld *world, TAK_Platform *plat) {
    const uint8_t *layer = fog_layer_const(world, 1);
    if (!world || !plat || !plat->renderer || !layer ||
        !world->cfg.line_of_sight) return;
    SDL_Renderer *r = plat->renderer;
    SDL_BlendMode prev;
    SDL_GetRenderDrawBlendMode(r, &prev);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    const int cell = FOG_CELL_PX;
    const int half = cell / 2;
    /* Cell-index range whose half-shifted quads can touch the
     * viewport (extra ±1 covers truncation toward zero). */
    int c0x = (world->cam_x - half) / cell - 1;
    int c0y = (world->cam_y - half) / cell - 1;
    int c1x = (world->cam_x + world->viewport_w + half) / cell + 1;
    int c1y = (world->cam_y + world->viewport_h + half) / cell + 1;

    for (int cy = c0y; cy <= c1y; cy++) {
        for (int cx = c0x; cx <= c1x; cx++) {
            uint8_t a00 = fog_corner_alpha(world, layer, cx,     cy);
            uint8_t a10 = fog_corner_alpha(world, layer, cx + 1, cy);
            uint8_t a11 = fog_corner_alpha(world, layer, cx + 1, cy + 1);
            uint8_t a01 = fog_corner_alpha(world, layer, cx,     cy + 1);
            if ((a00 | a10 | a11 | a01) == 0) continue;   /* all clear */

            int sx = cx * cell + half - world->cam_x;
            int sy = cy * cell + half - world->cam_y;
            if (a00 == a10 && a10 == a11 && a11 == a01) {
                SDL_SetRenderDrawColor(r, 0, 0, 0, a00);
                SDL_Rect rc = { sx, sy, cell, cell };
                SDL_RenderFillRect(r, &rc);
            } else {
                SDL_Vertex v[4];
                const float fx0 = (float)sx, fy0 = (float)sy;
                const float fx1 = (float)(sx + cell), fy1 = (float)(sy + cell);
                v[0].position.x = fx0; v[0].position.y = fy0;
                v[1].position.x = fx1; v[1].position.y = fy0;
                v[2].position.x = fx1; v[2].position.y = fy1;
                v[3].position.x = fx0; v[3].position.y = fy1;
                for (int k = 0; k < 4; k++) {
                    v[k].color.r = 0; v[k].color.g = 0; v[k].color.b = 0;
                    v[k].tex_coord.x = 0.0f; v[k].tex_coord.y = 0.0f;
                }
                v[0].color.a = a00;
                v[1].color.a = a10;
                v[2].color.a = a11;
                v[3].color.a = a01;
                static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
                SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
            }
        }
    }
    SDL_SetRenderDrawBlendMode(r, prev);
}
