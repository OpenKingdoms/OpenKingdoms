#include "tak_pathing.h"
#include "tak_world.h"
#include "tak_features.h"
#include "tak_terrain.h"
#include "tak_moveinfo.h"

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
    int count = TAK_PathPlan(&world, 32, 32, 9 * 32, 6 * 32, 12, &path);
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
    int blocked = TAK_PathPlan(&world, 32, 32, 7 * 32, 3 * 32, 12, &path);
    EXPECT(blocked == 0);

    MoveClassDef climber;
    memset(&climber, 0, sizeof(climber));
    climber.footprint_x = 1;
    climber.footprint_z = 1;
    climber.max_slope = 80;
    int routed = TAK_PathPlanForMoveClass(&world, 32, 32, 7 * 32, 3 * 32,
                                          &climber, 12, &path);
    EXPECT(routed > 0);
    free(world.tnt.heightmap);
}

static void test_large_map_routes_past_old_expansion_cutoff(void) {
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
    int count = TAK_PathPlan(&world, 32, 32, 90 * 32, 90 * 32, 12, &path);
    EXPECT(count > 0);
    EXPECT(path.x[0] > 32 || path.y[0] > 32);
    free(world.tnt.heightmap);
}

int main(void) {
    test_routes_through_height_gap();
    test_move_class_slope_changes_pathability();
    test_large_map_routes_past_old_expansion_cutoff();
    if (g_failures) {
        fprintf(stderr, "%d pathing tests failed\n", g_failures);
        return 1;
    }
    printf("pathing tests passed\n");
    return 0;
}
