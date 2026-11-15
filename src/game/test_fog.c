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

/* Seats with a team of their own are enemies unless they share it. */
int Units_PlayersAreEnemies(int a, int b) {
    return a != b;
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

/* Every test unit's model is this tall. A unit's flight_alt lifts its eye. */
#define TEST_TALL 32

int Terrain_SampleHeight(const GameWorld *world, int32_t world_x, int32_t world_y);

void Units_FogEye(int handle, int32_t *out_eye, int32_t *out_tall) {
    if (handle < 0 || handle >= g_test_unit_count) return;
    const Unit *u = &g_test_units[handle];
    *out_eye = Terrain_SampleHeight(NULL, u->world_x, u->world_y) +
               (int32_t)u->flight_alt + TEST_TALL;
    *out_tall = TEST_TALL;
}

/* The terrain: level but for a ridge, a band of high ground between
 * two x values, or a step at y g_split_y. Set before the first Fog_Update. */
static int g_ridge_x0, g_ridge_x1, g_ridge_h;
static int g_split_y, g_north_h, g_south_h;

int Terrain_SampleHeight(const GameWorld *world,
                         int32_t world_x, int32_t world_y) {
    (void)world;
    if (g_split_y > 0) return world_y < g_split_y ? g_north_h : g_south_h;
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
 * a ridge hides nothing behind it. The ridge itself stands over the
 * unit's eye and is dark but in the nine cells (legacy:167413-167423). */
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

    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 256;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 80;
    g_test_units[0].world_y = 272;
    Fog_Update(&w, 1);

    /* The unit's own cell and the ridge it stands against, in the
     * nine, and the ridge two rows down, which is not. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 80, 272));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 112, 272));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 112, 336));
    /* Ground beyond the ridge, 4 and 6 cells out of the 8 the
     * radius buys. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 208, 272));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 272, 272));
    /* 11 cells out, past the radius. */
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 432, 272));

    g_ridge_x0 = g_ridge_x1 = g_ridge_h = 0;
    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* One unit of sight `sight` on a 1024 px map with a band of ground `h`
 * high from x 448 to 576. It stands at x 464, on the band, and flies
 * `alt` high. Returns how many fog cells of the low ground to its left
 * it lights along its own row, or -1 when its own cell is dark. */
static int cells_lit_to_the_left(int sight, int h, int alt) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 1024;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    g_ridge_x0 = 448;
    g_ridge_x1 = 576;
    g_ridge_h = h;
    if (Fog_Init(&w) != 0) return -1;
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = sight;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].world_x = 464;
    g_test_units[0].world_y = 272;
    g_test_units[0].flight_alt = (float)alt;
    Fog_Update(&w, 1);
    int own = Fog_StateAtForPlayer(&w, 1, 464, 272) == TAK_FOG_VISIBLE;
    int lit = 0;
    for (int32_t lx = 432; lx > 0; lx -= 32)
        if (Fog_StateAtForPlayer(&w, 1, lx, 272) == TAK_FOG_VISIBLE) lit++;
    g_ridge_x0 = g_ridge_x1 = g_ridge_h = 0;
    Fog_Free(&w);
    g_test_unit_count = 0;
    return own ? lit : -1;
}

/* Sight grows with the eye's drop to the ground it falls on, counted
 * against the model's span, up to twice sightdistance in whole cells
 * (legacy:167413-167423). The unit's model is 32 high. */
static int test_high_ground_and_a_flyer_see_twice_as_far(void) {
    /* Level with the low ground: sightdistance, 4 cells. */
    ASSERT_EQ_INT(4, cells_lit_to_the_left(128, 0, 0));
    /* Half a model over it: half as far again. */
    ASSERT_EQ_INT(6, cells_lit_to_the_left(128, 16, 0));
    /* A model's height over it and more: twice, and no further. */
    ASSERT_EQ_INT(8, cells_lit_to_the_left(128, 32, 0));
    ASSERT_EQ_INT(8, cells_lit_to_the_left(128, 200, 0));
    /* A flyer over level ground sees as a unit on a height does. */
    ASSERT_EQ_INT(8, cells_lit_to_the_left(128, 0, 200));
    return 0;
}

/* Cap: twice sight in whole cells, so 135 gives 8, not the cell 8 across
 * and 1 down that 270 px reaches. Seen with VERSWORD and TARBEAK. */
static int test_the_reach_stops_at_whole_cells(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 1024;
    w.map_pixels_h = 1024;
    w.cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 135;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].world_x = 16 * 32 + 16;
    g_test_units[0].world_y = 16 * 32 + 16;
    g_test_units[0].flight_alt = 200.0f;
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 24 * 32, 16 * 32));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 23 * 32, 19 * 32));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 24 * 32, 17 * 32));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 25 * 32, 16 * 32));
    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* One unit of sight 128 at (240, y) on a 512 px map whose ground steps
 * at y 256 from `north` to `south`. Returns the state of fog cell (7, row). */
static int state_over_a_step(int north, int south, int32_t y, int row) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    g_split_y = 256;
    g_north_h = north;
    g_south_h = south;
    if (Fog_Init(&w) != 0) return -1;
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 128;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].world_x = 240;
    g_test_units[0].world_y = y;
    Fog_Update(&w, 1);
    int st = Fog_StateAtForPlayer(&w, 1, 240, row * 32 + 16);
    g_split_y = g_north_h = g_south_h = 0;
    Fog_Free(&w);
    g_test_unit_count = 0;
    return st;
}

/* A cell's ground is the lowest inside its own flat map cell, as seen
 * in the original. The step between rows 7 and 8 stays on the map's grid. */
static int test_a_cells_ground_is_the_lowest_in_its_map_cell(void) {
    /* Low ground north, a plateau 128 high from y 256: a unit on the low
     * ground sees row 7 and not the plateau's first row. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, state_over_a_step(0, 128, 208, 7));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, state_over_a_step(0, 128, 208, 8));
    /* A plateau north and low ground south: a unit on the low ground
     * sees row 8 and not the plateau's last row. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, state_over_a_step(128, 0, 400, 8));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, state_over_a_step(128, 0, 400, 7));
    return 0;
}

/* Open water counts at its surface, so a ship at sea sees its own sight
 * and no more, as VERHARP was seen to in the original. */
static int test_a_ship_on_open_water_sees_its_sight(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 1024;
    w.map_pixels_h = 512;
    w.water_height = 50;
    w.cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 128;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].world_x = 464;
    g_test_units[0].world_y = 272;
    /* Riding the surface over a sea bed at 0. */
    g_test_units[0].flight_alt = 50.0f;
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 336, 272));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 304, 272));
    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* A fog cell is the map cell under it, drawn 64 px up the picture, as
 * if all ground stood 128 high. Seen in the original: two units on one
 * map row, one on low ground and one on high, light the same row. */
static int test_the_overlay_is_drawn_two_cells_up(void) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 512, 512, 32,
                                                    SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *r = s ? SDL_CreateSoftwareRenderer(s) : NULL;
    if (!r) {
        fprintf(stderr, "overlay lift test: no software renderer: %s\n",
                SDL_GetError());
        return 1;
    }
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.viewport_w = 512;
    w.viewport_h = 512;
    w.cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 96;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].world_x = 7 * 32 + 16;
    g_test_units[0].world_y = 9 * 32 + 16;
    Fog_Update(&w, 1);
    /* Lit rows 6 to 12 are drawn over picture rows 4 to 10. */
    int above = -1, below = -1, strip = -1;
    ASSERT_EQ_INT(0, overlay_sample(&w, r, 7 * 32 + 16, 5 * 32 + 16, &above));
    ASSERT_EQ_INT(0, overlay_sample(&w, r, 7 * 32 + 16, 12 * 32 + 16, &below));
    ASSERT_EQ_INT(255, above);
    ASSERT_EQ_INT(0, below);
    /* No row is drawn over the map's last 64 px, which stay dark however
     * much is lit, as the last two cells are never seen in the original. */
    memset(w.fog_layers[1], TAK_FOG_VISIBLE, (size_t)w.fog_w * w.fog_h);
    ASSERT_EQ_INT(0, overlay_sample(&w, r, 7 * 32 + 16, 500, &strip));
    ASSERT_EQ_INT(0, strip);
    Fog_Free(&w);
    g_test_unit_count = 0;
    SDL_DestroyRenderer(r);
    SDL_FreeSurface(s);
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

/* A rider still sees from where its transport has taken it
 * (legacy:206485-206491). */
static int test_a_unit_aboard_still_sees(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 96;
    g_test_units[0].alive = UNIT_ALIVE_TRANSPORTED;
    g_test_units[0].player_id = 1;
    g_test_units[0].carried_by = 3;
    g_test_units[0].world_x = 256;
    g_test_units[0].world_y = 256;
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 256, 256));
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

/* A watcher sees the whole map: every screen query answers visible and
 * the overlay is clear. What each seat sees, which the simulation reads,
 * does not move, and one seat's view comes back when it is asked for. */
static int test_a_watcher_sees_the_whole_map_and_no_seat_changes(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 512;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 1;
    g_test_defs[0].sight_distance = 96;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].world_x = 64;
    g_test_units[0].world_y = 64;
    Fog_Update(&w, 1);
    Fog_SetViewer(1);

    Fog_SetSeeAll(1);
    ASSERT_EQ_INT(0, Fog_Viewer());
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAt(&w, 384, 384));
    ASSERT_EQ_INT(1, Fog_IsVisible(&w, 384, 384));
    ASSERT_EQ_INT(1, Fog_ShowsAt(&w, 384, 384));
    ASSERT_EQ_INT(0, Fog_OverlayAlphaAt(&w, 384, 384));
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(&w, 1, 384, 384));
    ASSERT_EQ_INT(0, Fog_SeatSeesAt(&w, 1, 384, 384));
    ASSERT_EQ_INT(0, Fog_IsVisibleForPlayer(&w, 2, 64, 64));

    Fog_SetSeeAll(0);
    ASSERT_EQ_INT(1, Fog_Viewer());
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAt(&w, 384, 384));
    ASSERT_EQ_INT(0xFF, Fog_OverlayAlphaAt(&w, 384, 384));
    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* sightdistancejam, a tag of our own for mods: an enemy jammer puts
 * the ground round it back under fog for a seat, all but the nine cells
 * round the seat's own unit, and never for its own side. One that can
 * be switched off jams only while it is on. */
static int test_a_sight_jammer_fogs_its_ground_for_the_enemy(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 1024;
    w.map_pixels_h = 512;
    w.cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(&w));
    g_test_unit_count = 2;
    g_test_defs[0].sight_distance = 320;
    g_test_defs[1].sight_distance = 64;
    g_test_defs[1].sight_jam_distance = 128;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[0].def_idx = 0;
    g_test_units[0].world_x = 208;
    g_test_units[0].world_y = 272;
    g_test_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[1].player_id = 2;
    g_test_units[1].def_idx = 1;
    g_test_units[1].world_x = 336;
    g_test_units[1].world_y = 272;

    Fog_Update(&w, 1);
    Fog_Update(&w, 2);
    /* The jammer and the ground round it are fogged for seat 1. */
    ASSERT_EQ_INT(TAK_FOG_EXPLORED, Fog_StateAtForPlayer(&w, 1, 336, 272));
    ASSERT_EQ_INT(TAK_FOG_EXPLORED, Fog_StateAtForPlayer(&w, 1, 432, 272));
    ASSERT_EQ_INT(0, Fog_IsVisibleForPlayer(&w, 1, 336, 272));
    /* Inside the radius, but one cell from seat 1's own unit. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 240, 272));
    /* Past the radius seat 1 still sees. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 496, 272));
    /* Its own side sees round it. */
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 2, 336, 272));

    /* Switched off, it jams nothing. */
    g_test_defs[1].onoffable = 1;
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(&w, 1, 336, 272));
    g_test_units[1].cob_activation = 1;
    Fog_Update(&w, 1);
    ASSERT_EQ_INT(TAK_FOG_EXPLORED, Fog_StateAtForPlayer(&w, 1, 336, 272));

    Fog_Free(&w);
    g_test_unit_count = 0;
    return 0;
}

/* Ours, for mods (D-045): whoever a sight jammer does not jam sees a haze
 * on its ground, the seat it jams sees none. With no jammer, or one
 * switched off, the screen is not touched at all. */
static int test_a_sight_jammers_haze_shows_only_to_those_it_does_not_jam(void) {
    GameWorld w;
    memset(&w, 0, sizeof(w));
    memset(g_test_units, 0, sizeof(g_test_units));
    memset(g_test_defs, 0, sizeof(g_test_defs));
    w.map_pixels_w = 1024;
    w.map_pixels_h = 512;
    w.viewport_w = 64;
    w.viewport_h = 64;
    w.cam_x = 300;
    w.cam_y = 240;
    w.cfg.line_of_sight = 1;
    g_test_unit_count = 2;
    g_test_defs[1].sight_jam_distance = 128;
    g_test_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[0].player_id = 1;
    g_test_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_test_units[1].player_id = 2;
    g_test_units[1].def_idx = 1;
    g_test_units[1].world_x = 336;
    g_test_units[1].world_y = 272;

    FogHaze hz[4];
    Fog_SetViewer(2);
    ASSERT_EQ_INT(1, Fog_JamHazes(&w, hz, 4));
    ASSERT_EQ_INT(336, hz[0].x);
    ASSERT_EQ_INT(272, hz[0].y);
    ASSERT_EQ_INT(128, hz[0].radius);
    Fog_SetViewer(1);
    ASSERT_EQ_INT(0, Fog_JamHazes(&w, hz, 4));
    Fog_SetSeeAll(1);
    ASSERT_EQ_INT(1, Fog_JamHazes(&w, hz, 4));
    Fog_SetSeeAll(0);

    /* Pixels: the owner's screen takes the haze, the enemy's nothing. */
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, 64, 64, 32,
                                                       SDL_PIXELFORMAT_RGBA32);
    ASSERT_EQ_INT(1, surf != NULL);
    TAK_Platform plat;
    memset(&plat, 0, sizeof(plat));
    plat.renderer = SDL_CreateSoftwareRenderer(surf);
    ASSERT_EQ_INT(1, plat.renderer != NULL);
    const uint32_t *px = (const uint32_t *)surf->pixels;
    SDL_FillRect(surf, NULL, 0);
    Fog_RenderJamHaze(&w, &plat);
    SDL_RenderFlush(plat.renderer);
    ASSERT_EQ_INT(0, (int)px[32 * 64 + 32]);
    Fog_SetViewer(2);
    Fog_RenderJamHaze(&w, &plat);
    SDL_RenderFlush(plat.renderer);
    ASSERT_EQ_INT(1, px[32 * 64 + 32] != 0);

    /* Switched off or under construction it lays nothing. */
    SDL_FillRect(surf, NULL, 0);
    g_test_defs[1].onoffable = 1;
    ASSERT_EQ_INT(0, Fog_JamHazes(&w, hz, 4));
    Fog_RenderJamHaze(&w, &plat);
    SDL_RenderFlush(plat.renderer);
    ASSERT_EQ_INT(0, (int)px[32 * 64 + 32]);
    g_test_units[1].cob_activation = 1;
    ASSERT_EQ_INT(1, Fog_JamHazes(&w, hz, 4));
    g_test_units[1].under_construction = 1;
    ASSERT_EQ_INT(0, Fog_JamHazes(&w, hz, 4));
    g_test_units[1].under_construction = 0;

    /* Sight jamming needs Line of Sight on, and so does its haze. */
    w.cfg.line_of_sight = 0;
    ASSERT_EQ_INT(0, Fog_JamHazes(&w, hz, 4));
    w.cfg.line_of_sight = 1;

    /* A radar jammer has no haze. */
    g_test_defs[1].sight_jam_distance = 0;
    g_test_defs[1].radar_jam_distance = 128;
    ASSERT_EQ_INT(0, Fog_JamHazes(&w, hz, 4));

    SDL_DestroyRenderer(plat.renderer);
    SDL_FreeSurface(surf);
    Fog_SetViewer(1);
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
    failed |= test_high_ground_and_a_flyer_see_twice_as_far();
    failed |= test_the_reach_stops_at_whole_cells();
    failed |= test_a_cells_ground_is_the_lowest_in_its_map_cell();
    failed |= test_a_ship_on_open_water_sees_its_sight();
    failed |= test_the_overlay_is_drawn_two_cells_up();
    failed |= test_the_nine_cells_around_a_unit_are_always_lit();
    failed |= test_a_unit_aboard_still_sees();
    failed |= test_the_reveal_follows_the_unit_across_a_cell_boundary();
    failed |= test_a_watcher_sees_the_whole_map_and_no_seat_changes();
    failed |= test_a_sight_jammer_fogs_its_ground_for_the_enemy();
    failed |= test_a_sight_jammers_haze_shows_only_to_those_it_does_not_jam();
    if (failed) return 1;
    puts("test_fog: ok");
    return 0;
}
