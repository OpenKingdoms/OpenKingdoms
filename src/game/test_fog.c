#include "tak_fog.h"
#include "tak_unit.h"
#include "tak_world.h"
#include "tak_platform.h"

#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static Unit g_test_units[4];
static int g_test_unit_count = 0;
static UnitDef g_test_defs[4];

const Unit *Units_GetActive(int *out_count) {
    if (out_count) *out_count = g_test_unit_count;
    return g_test_units;
}

const UnitDef *Units_GetDef(int idx) {
    if (idx < 0 || idx >= 4) return NULL;
    return &g_test_defs[idx];
}

/* The reveal anchor. It is simulation state on the unit rather than a
 * cache, so fog.c asks the unit layer for it and this fixture owns the
 * unit array. Same rule as the real one: the anchor follows the unit
 * into each new fog cell. */
void Units_FogAnchor(int handle, int sight, int32_t *out_x, int32_t *out_y) {
    if (handle < 0 || handle >= g_test_unit_count) return;
    Unit *u = &g_test_units[handle];
    if (!u->fog_lit || u->fog_sight != (int16_t)sight ||
        u->world_x / TAK_FOG_CELL_PX != u->fog_x / TAK_FOG_CELL_PX ||
        u->world_y / TAK_FOG_CELL_PX != u->fog_y / TAK_FOG_CELL_PX) {
        u->fog_x = u->world_x;
        u->fog_y = u->world_y;
        u->fog_sight = (int16_t)sight;
        u->fog_lit = 1;
    }
    if (out_x) *out_x = u->fog_x;
    if (out_y) *out_y = u->fog_y;
}

/* A terrain the stamp is not supposed to read. The ridge is a band
 * of high ground between two x values, and every query is counted
 * so a test can assert the stamp made none. */
static int g_ridge_x0, g_ridge_x1, g_ridge_h, g_terrain_queries;

int Terrain_SampleHeight(const GameWorld *world,
                         int32_t world_x, int32_t world_y) {
    (void)world;
    (void)world_y;
    g_terrain_queries++;
    if (g_ridge_x1 > g_ridge_x0 &&
        world_x >= g_ridge_x0 && world_x < g_ridge_x1)
        return g_ridge_h;
    return 0;
}

#define ASSERT_EQ_INT(exp, got) do { \
    int _e = (exp); \
    int _g = (got); \
    if (_e != _g) { \
        fprintf(stderr, "ASSERT_EQ_INT failed at %s:%d: expected %d got %d\n", \
                __FILE__, __LINE__, _e, _g); \
        return 1; \
    } \
} while (0)

static int test_map_revealed_initializes_explored(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    w.map_pixels_w = 256;
    w.map_pixels_h = 192;
    w.cfg.line_of_sight = 1;
    w.cfg.map_revealed = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    int total = w.fog_w * w.fog_h;
    for (int i = 0; i < total; i++) {
        ASSERT_EQ_INT(TAK_FOG_EXPLORED, w.fog_state[i]);
    }
    Fog_Update(&w, 1);
    for (int i = 0; i < total; i++) {
        ASSERT_EQ_INT(TAK_FOG_EXPLORED, w.fog_state[i]);
    }
    Fog_Free(&w);
    return 0;
}

/* With Line of Sight off the original still grows the explored map as
 * units move (legacy:167404-167409) and fills only its sight map
 * (legacy:167211-167219). Ground never walked stays unexplored. */
static int test_los_off_still_tracks_explored_ground(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 0;
    w.cfg.map_revealed = 0;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 96;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 64;
    g_test_units[0].world_y = 64;

    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 64, 64));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 384, 384));

    /* The unit walks away and the ground it saw stays explored. */
    g_test_units[0].world_x = 384;
    g_test_units[0].world_y = 384;
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_EXPLORED, Fog_StateAtForPlayer(&w, 1, 64, 64));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 384, 384));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAt(&w, 64, 448));

    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* A simulation guard. What the idle search, the AI and the influence
 * maps read stays "everything is seen" with Line of Sight off, the
 * original's full sight map (legacy:167211-167219), however much of
 * the explored map is still dark. */
static int test_los_off_simulation_still_sees_everything(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 0;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 96;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 64;
    g_test_units[0].world_y = 64;
    Fog_Update(&w, 1);
    Fog_Update(&w, 2);
    for (int p = 1; p <= 2; p++) {
        for (int y = 16; y < 512; y += 32) {
            for (int x = 16; x < 512; x += 32) {
                ASSERT_EQ_INT(1, Fog_IsVisibleForPlayer(&w, p, x, y));
            }
        }
    }
    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* One pixel of the overlay drawn over a white frame. */
static int overlay_sample(const GameWorld *w, SDL_Renderer *r,
                          int x, int y, int *out) {
    TAK_Platform plat;
    memset(&plat, 0, sizeof(plat));
    plat.renderer = r;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderClear(r);
    Fog_RenderOverlay(w, &plat);
    SDL_Rect px = { x, y, 1, 1 };
    uint8_t rgba[4] = { 0, 0, 0, 0 };
    if (SDL_RenderReadPixels(r, &px, SDL_PIXELFORMAT_RGBA32, rgba, 4) != 0)
        return -1;
    *out = rgba[0];
    return 0;
}

/* The overlay keeps its black level in both modes and loses the grey
 * one with Line of Sight off. The original gates only the middle level
 * on the option (legacy:130175, legacy:130295-130305). */
static int test_los_off_overlay_has_no_grey_level(void) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 256, 256, 32,
                                                    SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *r = s ? SDL_CreateSoftwareRenderer(s) : NULL;
    if (!r) {
        fprintf(stderr, "overlay test: no software renderer: %s\n",
                SDL_GetError());
        return 1;
    }
    GameWorld w;
    memset(&w, 0, sizeof(w));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.viewport_w = 256;
    w.viewport_h = 256;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    /* The left columns were explored and are out of sight, the rest
     * were never seen. */
    for (int cy = 0; cy < w.fog_h; cy++) {
        for (int cx = 0; cx < w.fog_w; cx++) {
            w.fog_layers[1][cy * w.fog_w + cx] =
                cx < 4 ? TAK_FOG_EXPLORED : TAK_FOG_UNEXPLORED;
        }
    }
    int explored = -1, dark = -1;
    w.cfg.line_of_sight = 0;
    ASSERT_EQ_INT(0, overlay_sample(&w, r, 64, 64, &explored));
    ASSERT_EQ_INT(0, overlay_sample(&w, r, 192, 64, &dark));
    ASSERT_EQ_INT(255, explored);
    ASSERT_EQ_INT(0, dark);

    /* With the option on, explored ground is dimmed as before. */
    w.cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, overlay_sample(&w, r, 64, 64, &explored));
    ASSERT_EQ_INT(0, overlay_sample(&w, r, 192, 64, &dark));
    ASSERT_EQ_INT(1, explored > 100 && explored < 170);
    ASSERT_EQ_INT(0, dark);

    Fog_Free(&w);
    SDL_DestroyRenderer(r);
    SDL_FreeSurface(s);
    return 0;
}

static int test_player_layers_are_independent(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    w.cfg.map_revealed = 0;
    ASSERT_EQ_INT(0, Fog_Init(&w));

    g_test_unit_count = 2;
    g_test_defs[0].sight_distance = 96;
    g_test_defs[1].sight_distance = 96;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 64;
    g_test_units[0].world_y = 64;
    g_test_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[1].player_id = 2;
    g_test_units[1].def_idx = 1;
    g_test_units[1].world_x = 384;
    g_test_units[1].world_y = 384;

    Fog_Update(&w, 1);
    Fog_Update(&w, 2);
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 64, 64));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 384, 384));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 2, 384, 384));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 2, 64, 64));

    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* Two human seats on one team see through each other's units
 * (legacy:167261-167267). */
static int test_allies_share_sight(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    for (int p = 0; p < 3; p++) {
        w.cfg.players[p].kind = TAK_SLOT_HUMAN;
        w.cfg.players[p].team = (p < 2) ? 1 : 2;
    }
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 96;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 2;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 384;
    g_test_units[0].world_y = 384;

    ASSERT_EQ_INT(1, Fog_SharesSight(&w, 1, 2));
    ASSERT_EQ_INT(0, Fog_SharesSight(&w, 1, 3));
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 384, 384));
    Fog_Update(&w, 3);
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 3, 384, 384));

    /* The ally leaves and the ground stays explored, not forgotten. */
    g_test_units[0].world_x = 64;
    g_test_units[0].world_y = 64;
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_EXPLORED, Fog_StateAtForPlayer(&w, 1, 384, 384));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 64, 64));

    /* On different teams nothing is shared. */
    w.cfg.players[1].team = 3;
    ASSERT_EQ_INT(0, Fog_SharesSight(&w, 1, 2));
    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* A computer seat shares with nobody, in either direction
 * (legacy:206336-206348). Teamless seats share with nobody either. */
static int test_an_ai_teammate_grants_no_sight(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    w.cfg.players[1].kind = TAK_SLOT_AI;
    w.cfg.players[1].team = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 96;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 2;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 384;
    g_test_units[0].world_y = 384;

    ASSERT_EQ_INT(0, Fog_SharesSight(&w, 1, 2));
    ASSERT_EQ_INT(0, Fog_SharesSight(&w, 2, 1));
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 384, 384));
    /* The AI still sees through its own units. */
    Fog_Update(&w, 2);
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 2, 384, 384));

    /* Two humans with no team are two sides. */
    w.cfg.players[1].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 0;
    w.cfg.players[1].team = 0;
    ASSERT_EQ_INT(0, Fog_SharesSight(&w, 1, 2));
    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* The original's stamp walks fog cells and consults nothing between
 * the unit's cell and the cell it lights (legacy:167396-167427), so
 * a ridge hides nothing behind it. */
static int test_a_ridge_does_not_hide_the_ground_behind_it(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_ridge_x0 = 96;
    g_ridge_x1 = 128;
    g_ridge_h = 200;
    g_terrain_queries = 0;

    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 256;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 80;
    g_test_units[0].world_y = 272;
    Fog_Update(&w, 1);

    /* The unit's own cell and the ridge it stands against. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 80, 272));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 112, 272));
    /* Ground beyond the ridge, 4 and 6 cells out of the 8 the
     * radius buys. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 208, 272));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 272, 272));
    /* 11 cells out, past the radius. */
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 432, 272));
    /* The stamp asked the terrain nothing at all. */
    ASSERT_EQ_INT(0, g_terrain_queries);

    g_ridge_x0 = g_ridge_x1 = g_ridge_h = 0;
    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* The original re-stamps the moment the unit's fog cell changes
 * (legacy:167450-167454), so the reveal is always centred on the
 * cell the unit stands in, not on the one it last stamped from. */
static int test_the_reveal_follows_the_unit_across_a_cell_boundary(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));

    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 128;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].def_idx = 0;
    /* Cell 8, one pixel short of the boundary into cell 9. */
    g_test_units[0].world_x = 287;
    g_test_units[0].world_y = 272;
    Fog_Update(&w, 1);
    /* Four cells either side is what 128 px of sight buys. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 128, 272));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 416, 272));

    /* One pixel right puts the unit in cell 9. */
    g_test_units[0].world_x = 288;
    Fog_Update(&w, 1);
    /* Ground only the new cell reaches. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 416, 272));
    /* Ground the new cell no longer reaches, so it drops to explored. */
    ASSERT_EQ_INT(TAK_FOG_EXPLORED, Fog_StateAtForPlayer(&w, 1, 128, 272));

    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* The block of nine cells around a unit is lit whatever its radius
 * buys (legacy:167401-167402). A wall's sightdistance is 45
 * (arawall.fbi), short of the 45.25 px to a diagonal neighbour's
 * centre, and the original lights that neighbour anyway. */
static int test_the_nine_cells_around_a_unit_are_always_lit(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));

    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 45;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 80;
    g_test_units[0].world_y = 272;
    Fog_Update(&w, 1);

    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 80, 272));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 112, 272));
    /* The four diagonals of the block. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 48, 240));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 112, 240));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 48, 304));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 112, 304));
    /* Two cells out is outside the block and outside the radius. */
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 144, 272));

    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

int main(void) {
    int failed = 0;
    failed |= test_map_revealed_initializes_explored();
    failed |= test_los_off_still_tracks_explored_ground();
    failed |= test_los_off_simulation_still_sees_everything();
    failed |= test_los_off_overlay_has_no_grey_level();
    failed |= test_player_layers_are_independent();
    failed |= test_allies_share_sight();
    failed |= test_an_ai_teammate_grants_no_sight();
    failed |= test_a_ridge_does_not_hide_the_ground_behind_it();
    failed |= test_the_nine_cells_around_a_unit_are_always_lit();
    failed |= test_the_reveal_follows_the_unit_across_a_cell_boundary();
    if (failed) return 1;
    puts("test_fog: ok");
    return 0;
}
