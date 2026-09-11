#include "tak_pathing.h"
#include "tak_world.h"
#include "tak_features.h"
#include "tak_terrain.h"
#include "tak_moveinfo.h"
#include "tak_occupancy.h"
#include "tak_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const FeatureDef *Features_GetByIndex(int idx) {
    (void)idx;
    return NULL;
}

int Terrain_SampleHeight(const struct GameWorld *world,
                         int32_t world_x, int32_t world_y) {
    if (!world || !world->tnt.heightmap) return 0;
    if (world_x < 0) world_x = 0;
    if (world_y < 0) world_y = 0;
    int tx = (int)(world_x / 16);
    int ty = (int)(world_y / 16);
    if (tx >= world->tnt.height_w) tx = world->tnt.height_w - 1;
    if (ty >= world->tnt.height_h) ty = world->tnt.height_h - 1;
    return (int)world->tnt.heightmap[ty * world->tnt.height_w + tx] - 32;
}

int Terrain_IsWalkable(const struct GameWorld *world,
                       int32_t world_x, int32_t world_y,
                       int max_slope) {
    if (!world || world_x < 0 || world_y < 0 ||
        world_x >= world->map_pixels_w || world_y >= world->map_pixels_h) {
        return 0;
    }
    if (max_slope <= 0) max_slope = 12;
    int h0 = Terrain_SampleHeight(world, world_x, world_y);
    static const int off[4][2] = { {16,0}, {-16,0}, {0,16}, {0,-16} };
    for (int i = 0; i < 4; i++) {
        int32_t sx = world_x + off[i][0];
        int32_t sy = world_y + off[i][1];
        if (sx < 0 || sy < 0 || sx >= world->map_pixels_w ||
            sy >= world->map_pixels_h) {
            continue;
        }
        int dh = Terrain_SampleHeight(world, sx, sy) - h0;
        if (dh < 0) dh = -dh;
        if (dh > max_slope) return 0;
    }
    return 1;
}

static int g_failures = 0;

#define EXPECT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static void test_routes_through_height_gap(void) {
    TAK_PathCacheReset();
    GameWorld world;
    memset(&world, 0, sizeof(world));
    world.map_pixels_w = 10 * 32;
    world.map_pixels_h = 8 * 32;
    world.tnt.height_w = world.map_pixels_w / 16 + 1;
    world.tnt.height_h = world.map_pixels_h / 16 + 1;
    size_t n = (size_t)world.tnt.height_w * (size_t)world.tnt.height_h;
    world.tnt.heightmap = (uint8_t *)calloc(n, 1);
    EXPECT(world.tnt.heightmap != NULL);
    if (!world.tnt.heightmap) return;
    memset(world.tnt.heightmap, 32, n);

    int wall_x = 5 * 32;
    int gap_y0 = 3 * 32;
    int gap_y1 = 4 * 32;
    for (int y = 0; y < world.map_pixels_h; y += 16) {
        if (y >= gap_y0 && y <= gap_y1) continue;
        int tx = wall_x / 16;
        int ty = y / 16;
        world.tnt.heightmap[ty * world.tnt.height_w + tx] = 96;
        world.tnt.heightmap[ty * world.tnt.height_w + tx + 1] = 96;
    }

    TAK_Path path;
    int count = TAK_PathPlan(&world, 32, 32, 9 * 32, 6 * 32, 12, 0, &path);
    EXPECT(count > 0);
    int used_gap = 0;
    for (int i = 0; i < path.count; i++) {
        EXPECT(Terrain_IsWalkable(&world, path.x[i], path.y[i], 12));
        if (path.x[i] >= wall_x - 32 && path.x[i] <= wall_x + 32 &&
            path.y[i] >= gap_y0 && path.y[i] <= gap_y1 + 32) {
            used_gap = 1;
        }
    }
    EXPECT(used_gap);
    free(world.tnt.heightmap);
}

static void test_move_class_slope_changes_pathability(void) {
    TAK_PathCacheReset();
    GameWorld world;
    memset(&world, 0, sizeof(world));
    world.map_pixels_w = 8 * 32;
    world.map_pixels_h = 5 * 32;
    world.tnt.height_w = world.map_pixels_w / 16 + 1;
    world.tnt.height_h = world.map_pixels_h / 16 + 1;
    size_t n = (size_t)world.tnt.height_w * (size_t)world.tnt.height_h;
    world.tnt.heightmap = (uint8_t *)calloc(n, 1);
    EXPECT(world.tnt.heightmap != NULL);
    if (!world.tnt.heightmap) return;
    memset(world.tnt.heightmap, 32, n);

    int wall_x = 4 * 32;
    for (int y = 0; y < world.map_pixels_h; y += 16) {
        int tx = wall_x / 16;
        int ty = y / 16;
        world.tnt.heightmap[ty * world.tnt.height_w + tx] = 96;
        world.tnt.heightmap[ty * world.tnt.height_w + tx + 1] = 96;
    }

    TAK_Path path;
    int blocked = TAK_PathPlan(&world, 32, 32, 7 * 32, 3 * 32, 12, 0, &path);
    EXPECT(blocked == 0);

    MoveClassDef climber;
    memset(&climber, 0, sizeof(climber));
    climber.footprint_x = 1;
    climber.footprint_z = 1;
    climber.max_slope = 80;
    int routed = TAK_PathPlanForMoveClass(&world, 32, 32, 7 * 32, 3 * 32,
                                          &climber, 12, 0, &path);
    EXPECT(routed > 0);
    free(world.tnt.heightmap);
}

static void test_large_map_routes_past_old_expansion_cutoff(void) {
    TAK_PathCacheReset();
    GameWorld world;
    memset(&world, 0, sizeof(world));
    world.map_pixels_w = 96 * 32;
    world.map_pixels_h = 96 * 32;
    world.tnt.height_w = world.map_pixels_w / 16 + 1;
    world.tnt.height_h = world.map_pixels_h / 16 + 1;
    size_t n = (size_t)world.tnt.height_w * (size_t)world.tnt.height_h;
    world.tnt.heightmap = (uint8_t *)calloc(n, 1);
    EXPECT(world.tnt.heightmap != NULL);
    if (!world.tnt.heightmap) return;
    memset(world.tnt.heightmap, 32, n);

    TAK_Path path;
    int count = TAK_PathPlan(&world, 32, 32, 90 * 32, 90 * 32, 12, 0, &path);
    EXPECT(count > 0);
    EXPECT(path.x[0] > 32 || path.y[0] > 32);
    free(world.tnt.heightmap);
}

/* ── Unit occupancy layer ──────────────────────────────────────────
 * Structures block planning; a gate's cells are the exception, and
 * only for the gate's owner. */

static void occ_stamp(GameWorld *w, int handle, int owner,
                      int tx, int ty, int fx, int fz,
                      const uint8_t *yard, int open, int is_gate) {
    TAK_OccStamp st;
    st.tx0 = tx;  st.ty0 = ty;
    st.fx  = fx;  st.fz  = fz;
    st.yard = yard;
    st.yard_open = open;
    st.is_gate = is_gate;
    st.handle = handle;
    st.owner = owner;
    Occ_ImprintStamp(w, &st, 1, NULL, NULL);
}

/* A flat map with a solid vertical wall of structures at path-cell
 * column `wall_cell`, leaving a gate-sized span open in the middle. */
static int occ_world_init(GameWorld *w, int cells_w, int cells_h) {
    memset(w, 0, sizeof(*w));
    w->map_pixels_w = cells_w * 32;
    w->map_pixels_h = cells_h * 32;
    w->tnt.height_w = w->map_pixels_w / 16 + 1;
    w->tnt.height_h = w->map_pixels_h / 16 + 1;
    size_t n = (size_t)w->tnt.height_w * (size_t)w->tnt.height_h;
    w->tnt.heightmap = (uint8_t *)calloc(n, 1);
    if (!w->tnt.heightmap) return 0;
    memset(w->tnt.heightmap, 32, n);
    return Occ_Ensure(w);
}

static void occ_world_free(GameWorld *w) {
    Occ_Free(w);
    free(w->tnt.heightmap);
    w->tnt.heightmap = NULL;
}

static void test_wall_blocks_route(void) {
    TAK_PathCacheReset();
    GameWorld world;
    if (!occ_world_init(&world, 20, 10)) { EXPECT(0); return; }
    /* One 2x2 always-blocking segment per two occupancy rows, stacked
     * top to bottom at occupancy columns 20-21 (path-cell column 10). */
    static const uint8_t solid[4] = { 0x2f, 0x2f, 0x2f, 0x2f };
    int h = 1;
    for (int ty = 0; ty < world.occ_h; ty += 2)
        occ_stamp(&world, h++, 1, 20, ty, 2, 2, solid, 0, 0);

    TAK_Path path;
    EXPECT(TAK_PathPlan(&world, 32, 32, 19 * 32, 5 * 32, 12, 1, &path) == 0);
    EXPECT(TAK_PathPlan(&world, 32, 32, 19 * 32, 5 * 32, 12, 2, &path) == 0);
    occ_world_free(&world);
}

static void test_gate_span_in_wall(void) {
    TAK_PathCacheReset();
    /* Same wall, but occupancy rows 8-11 are a gate owned by player 1.
     * `c` blocks only while the yard is closed (legacy:163223-163256)
     * and, on a gate def, those cells are tagged as gate cells
     * (legacy:218012-218018). */
    static const uint8_t solid[4]   = { 0x2f, 0x2f, 0x2f, 0x2f };
    static const uint8_t gateway[8] = { 0x2d, 0x2d, 0x2d, 0x2d,
                                        0x2d, 0x2d, 0x2d, 0x2d };
    for (int open = 0; open <= 1; open++) {
        GameWorld world;
        if (!occ_world_init(&world, 20, 10)) { EXPECT(0); return; }
        int h = 1;
        for (int ty = 0; ty < world.occ_h; ty += 2) {
            if (ty >= 8 && ty < 12) continue;
            occ_stamp(&world, h++, 1, 20, ty, 2, 2, solid, 0, 0);
        }
        occ_stamp(&world, h++, 1, 20, 8, 2, 4, gateway, open, 1);

        TAK_Path path;
        int mine  = TAK_PathPlan(&world, 32, 32, 19 * 32, 5 * 32, 12, 1, &path);
        int theirs = TAK_PathPlan(&world, 32, 32, 19 * 32, 5 * 32, 12, 2, &path);
        /* The owner routes through either way. A closed own gate is
         * reclassified passable and opened on arrival
         * (legacy:21986-22032). */
        EXPECT(mine > 0);
        /* An open gate is simply free ground, so anyone may pass; a
         * closed one still blocks the enemy. */
        if (open) EXPECT(theirs > 0);
        else      EXPECT(theirs == 0);
        occ_world_free(&world);
    }
}

static void test_yardmap_parse(void) {
    TAK_PathCacheReset();
    /* Row separators are skipped and the last character repeats to fill
     * (legacy:163259-163262). */
    uint8_t *m = Occ_BuildYardmap("oc co", 0, 2, 2);
    EXPECT(m != NULL);
    if (m) {
        EXPECT(m[0] == 0x2f && m[1] == 0x2d);
        EXPECT(m[2] == 0x2d && m[3] == 0x2f);
        tak_free(m);
    }
    m = Occ_BuildYardmap("o", 0, 2, 2);
    EXPECT(m != NULL);
    if (m) {
        EXPECT(m[0] == 0x2f && m[3] == 0x2f);
        tak_free(m);
    }
    /* Mobile defs never block (legacy:163272-163292). */
    m = Occ_BuildYardmap("oooo", 1, 2, 2);
    EXPECT(m != NULL);
    if (m) {
        EXPECT((m[0] & TAK_OCC_MASK(0)) == 0);
        EXPECT((m[0] & TAK_OCC_MASK(1)) == 0);
        tak_free(m);
    }
    /* Blocking mask: `c` is free open / blocked closed, `o` is both. */
    EXPECT((0x2d & TAK_OCC_MASK(1)) == 0);
    EXPECT((0x2d & TAK_OCC_MASK(0)) != 0);
    EXPECT((0x2f & TAK_OCC_MASK(1)) != 0);
    EXPECT((0x2f & TAK_OCC_MASK(0)) != 0);
}


/* Clearance: a cell is open to a unit only when the largest square
 * footprint that fits there covers the unit's own. A one cell gap in
 * a wall of structures takes a narrow unit and turns a wide one away
 * to the wider opening. */
static void test_wide_unit_avoids_gap_narrow_unit_takes(void) {
    TAK_PathCacheReset();
    GameWorld world;
    if (!occ_world_init(&world, 12, 8)) { EXPECT(0); return; }
    /* Wall at path-cell column 5 (occupancy columns 10-11): solid over
     * occupancy rows 0-5 and 8-9, a one cell gap at rows 6-7 and a
     * three cell opening at rows 10-15. */
    static const uint8_t solid[4] = { 0x2f, 0x2f, 0x2f, 0x2f };
    int h = 1;
    for (int ty = 0; ty < 6; ty += 2) occ_stamp(&world, h++, 2, 10, ty, 2, 2, solid, 0, 0);
    occ_stamp(&world, h++, 2, 10, 8, 2, 2, solid, 0, 0);

    EXPECT(TAK_PathClearanceAt(&world, NULL, 12, 10, 2) == 0);
    EXPECT(TAK_PathClearanceAt(&world, NULL, 12, 10, 6) == 2);
    EXPECT(TAK_PathClearanceAt(&world, NULL, 12, 2, 2) >= 4);

    TAK_Path narrow;
    int n = TAK_PathPlan(&world, 48, 112, 336, 112, 12, 1, &narrow);
    EXPECT(n > 0);
    int narrow_gap = 0;
    for (int i = 0; i < narrow.count; i++) {
        if (narrow.x[i] / 32 == 5 && narrow.y[i] / 32 == 3) narrow_gap = 1;
    }
    EXPECT(narrow_gap);

    MoveClassDef wide;
    memset(&wide, 0, sizeof(wide));
    wide.footprint_x = 4;
    wide.footprint_z = 4;
    wide.max_slope = 30;
    TAK_Path route;
    int w = TAK_PathPlanForMoveClass(&world, 48, 112, 336, 112, &wide, 12, 1,
                                     &route);
    EXPECT(w > 0);
    int wide_gap = 0, wide_opening = 0;
    for (int i = 0; i < route.count; i++) {
        if (route.x[i] / 32 != 5) continue;
        if (route.y[i] / 32 < 5) wide_gap = 1;
        else wide_opening = 1;
    }
    EXPECT(!wide_gap);
    EXPECT(wide_opening);

    /* A parked unit on the gap closes it for everyone but itself. */
    Occ_MoveMobile(&world, 40, 1, 0, 0, 0, 10, 6, 2, 2);
    Occ_SetMobileParked(&world, 40, 10, 6, 2, 2, 1);
    TAK_PathQuery q;
    memset(&q, 0, sizeof(q));
    q.fallback_max_slope = 12;
    q.player_id = 1;
    q.self_plus1 = 7;
    int other = TAK_PathPlanQuery(&world, 48, 112, 336, 112, &q, &narrow);
    int other_gap = 0;
    for (int i = 0; i < narrow.count; i++) {
        if (narrow.x[i] / 32 == 5 && narrow.y[i] / 32 == 3) other_gap = 1;
    }
    EXPECT(other > 0 && !other_gap);
    q.self_plus1 = 41;
    int self = TAK_PathPlanQuery(&world, 48, 112, 336, 112, &q, &narrow);
    int self_gap = 0;
    for (int i = 0; i < narrow.count; i++) {
        if (narrow.x[i] / 32 == 5 && narrow.y[i] / 32 == 3) self_gap = 1;
    }
    EXPECT(self > 0 && self_gap);
    occ_world_free(&world);
}

int main(void) {
    test_routes_through_height_gap();
    test_move_class_slope_changes_pathability();
    test_large_map_routes_past_old_expansion_cutoff();
    test_yardmap_parse();
    test_wall_blocks_route();
    test_gate_span_in_wall();
    test_wide_unit_avoids_gap_narrow_unit_takes();
    if (g_failures) {
        fprintf(stderr, "%d pathing tests failed\n", g_failures);
        return 1;
    }
    printf("pathing tests passed\n");
    return 0;
}
