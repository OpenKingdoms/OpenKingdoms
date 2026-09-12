/*
 * test_movement.c -- movement tests that need no game data.
 *
 * The harness registers synthetic unit defs (no model, no script, no
 * yardmap) and installs a world with a flat heightmap and an occupancy
 * layer, so the mover, the planner and the occupancy layer can be
 * driven in CI where no game files exist. Behaviour that used to be
 * checkable only against a real map is checked here, next to the state
 * hash streams that guard determinism.
 */

#include "test_framework.h"
#include "tak_world.h"
#include "tak_unit.h"
#include "tak_occupancy.h"
#include "tak_pathing.h"
#include "tak_moveinfo.h"
#include "tak_memory.h"
#include "tak_battle_config.h"
#include "tak_ai.h"

#include <SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ── the harness ───────────────────────────────────────────────────── */

#define MV_TILES   192      /* 16 px tiles per side, so a 3072 px map */
#define MV_GROUND  64       /* flat height, clear of the water line */

enum { MV_DEF_WALKER = 0, MV_DEF_KNIGHT, MV_DEF_COUNT };

static void mv_fill_def(UnitDef *d, const char *name, const char *mclass,
                        float velocity, int health) {
    memset(d, 0, sizeof(*d));
    strncpy(d->unitname, name, sizeof(d->unitname) - 1);
    strncpy(d->display_name, name, sizeof(d->display_name) - 1);
    strncpy(d->category, "TEST WALKER", sizeof(d->category) - 1);
    strncpy(d->movement_class, mclass, sizeof(d->movement_class) - 1);
    d->max_health = health;
    d->sight_distance = 320;
    d->max_velocity = velocity;
    d->acceleration = velocity / 4.0f;
    d->brake_rate = velocity / 2.0f;
    d->turn_rate = 4000.0f;
    d->max_slope = 30;
    d->bmcode = 1;          /* mobile: no yardmap, no structure imprint */
    d->cap_flags = UNIT_CAP_MOVE | UNIT_CAP_STOP;
    d->footprint_x = 1;
    d->footprint_z = 1;
}

/* A flat world with an occupancy layer, two move classes and two
 * synthetic defs. Returns NULL if anything could not be built. */
static GameWorld *mv_world(void) {
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    cfg.line_of_sight = 0;
    cfg.players[0].kind = TAK_SLOT_HUMAN;
    cfg.players[1].kind = TAK_SLOT_HUMAN;
    if (World_BeginLoad(NULL, &cfg, "synthetic", "aramon") != 0) return NULL;
    GameWorld *w = World_Get();
    if (!w) return NULL;
    w->map_pixels_w = MV_TILES * 16;
    w->map_pixels_h = MV_TILES * 16;
    w->viewport_w = 640;
    w->viewport_h = 480;
    w->water_height = 0;
    w->tnt.width_tiles = MV_TILES;
    w->tnt.height_tiles = MV_TILES;
    w->tnt.height_w = MV_TILES + 1;
    w->tnt.height_h = MV_TILES + 1;
    size_t hn = (size_t)w->tnt.height_w * (size_t)w->tnt.height_h;
    w->tnt.heightmap = (uint8_t *)tak_malloc(hn);
    if (!w->tnt.heightmap) return NULL;
    memset(w->tnt.heightmap, MV_GROUND, hn);

    /* The footprints of the game's GROUND2 and GROUND3, the classes
     * of the swordsman and the knight the needs-data tests use. The
     * game has no one tile class. A wall of one tile units is thinner
     * than a path cell, so the planner routes straight through it. */
    memset(&w->moveinfo, 0, sizeof(w->moveinfo));
    w->moveinfo.count = 2;
    strncpy(w->moveinfo.classes[0].name, "TESTSMALL", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[0].footprint_x = 2;
    w->moveinfo.classes[0].footprint_z = 2;
    w->moveinfo.classes[0].max_slope = 30;
    strncpy(w->moveinfo.classes[1].name, "TESTBIG", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[1].footprint_x = 3;
    w->moveinfo.classes[1].footprint_z = 3;
    w->moveinfo.classes[1].max_slope = 30;

    if (!Occ_Ensure(w)) return NULL;
    TAK_PathCacheReset();
    World_MarkLoaded();

    UnitDef defs[MV_DEF_COUNT];
    mv_fill_def(&defs[MV_DEF_WALKER], "TESTSWORD", "TESTSMALL", 1.4f, 200);
    mv_fill_def(&defs[MV_DEF_KNIGHT], "TESTKNIGH", "TESTBIG", 2.2f, 400);
    if (Units_DebugSetDefs(defs, MV_DEF_COUNT) != MV_DEF_COUNT) return NULL;
    return w;
}

static void mv_end(void) {
    Units_ClearInstances();
    World_End(NULL);
    TAK_PathCacheReset();
}

static void mv_run(int ticks) {
    for (int i = 0; i < ticks; i++) Units_TickEngines();
}

static int64_t mv_dist2(const Unit *u, int32_t x, int32_t y) {
    int64_t dx = (int64_t)u->world_x - x;
    int64_t dy = (int64_t)u->world_y - y;
    return dx * dx + dy * dy;
}

static const Unit *mv_unit(int handle) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    return (handle >= 0 && handle < count) ? &units[handle] : NULL;
}

/* ── what the harness is for ───────────────────────────────────────── */

/* One walker over open ground. This is the harness itself under test:
 * a unit with no model and no script still spawns, plans and walks. */
TEST(a_walker_crosses_open_ground) {
    ASSERT_NOT_NULL(mv_world());
    int h = Units_Spawn(MV_DEF_WALKER, 1, 0, 800, 800);
    ASSERT(h >= 0);
    Units_CommandMoveUnit(h, 1600, 800);
    int arrived = 0;
    for (int i = 0; i < 3600 && !arrived; i++) {
        Units_TickEngines();
        if (mv_dist2(mv_unit(h), 1600, 800) <= 96 * 96) arrived = 1;
    }
    ASSERT(arrived);
    mv_end();
}

/* Ported from the needs-data suite: a wall of passive friends lies
 * across the straight line and the mover has to go round it. */
TEST(unit_walks_around_a_wall_of_friendly_units) {
    ASSERT_NOT_NULL(mv_world());
    #define MV_WALL_N 13
    int32_t rx = 1600, ry = 1600;
    int wall[MV_WALL_N];
    for (int i = 0; i < MV_WALL_N; i++) {
        wall[i] = Units_Spawn(MV_DEF_WALKER, 1, 0, rx,
                              ry + (i - MV_WALL_N / 2) * 32);
        ASSERT(wall[i] >= 0);
        Units_DebugSetAggro(wall[i], UNIT_AGGRO_PASSIVE);
    }
    int h = Units_Spawn(MV_DEF_KNIGHT, 1, 0, rx - 350, ry);
    ASSERT(h >= 0);
    Units_DebugSetAggro(h, UNIT_AGGRO_PASSIVE);
    int32_t gx = rx + 350, gy = ry;
    Units_CommandMoveUnit(h, gx, gy);
    int arrived = 0, ticks = 0;
    for (int i = 0; i < 3600 && !arrived; i++) {
        Units_TickEngines();
        ticks = i + 1;
        if (mv_dist2(mv_unit(h), gx, gy) <= 96 * 96) arrived = 1;
    }
    printf("(%d ticks) ", ticks);
    ASSERT(arrived);
    /* Nobody was shoved through the wall. */
    for (int i = 0; i < MV_WALL_N; i++) {
        int32_t wy = ry + (i - MV_WALL_N / 2) * 32;
        ASSERT(mv_dist2(mv_unit(wall[i]), rx, wy) <= 16 * 16);
    }
    #undef MV_WALL_N
    mv_end();
}

/* Ported: twenty walkers ordered onto one point end up beside each
 * other, never on one tile, and still gather. */
TEST(units_do_not_stack_on_one_another) {
    ASSERT_NOT_NULL(mv_world());
    #define MV_STACK_N 20
    int h[MV_STACK_N];
    int32_t rx = 1600, ry = 1600;
    for (int i = 0; i < MV_STACK_N; i++) {
        h[i] = Units_Spawn(MV_DEF_WALKER, 1, 0,
                           rx - 480 + (i % 5) * 64, ry - 160 + (i / 5) * 64);
        ASSERT(h[i] >= 0);
        Units_DebugSetAggro(h[i], UNIT_AGGRO_PASSIVE);
    }
    for (int i = 0; i < MV_STACK_N; i++) Units_CommandMoveUnit(h[i], rx, ry);
    mv_run(4200);
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    int shared = 0;
    int64_t worst = 0;
    for (int i = 0; i < MV_STACK_N; i++) {
        ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)units[h[i]].alive);
        int64_t d2 = mv_dist2(&units[h[i]], rx, ry);
        if (d2 > worst) worst = d2;
        /* Compare the stamped footprint, not the tile the centre falls
         * in. A footprint is stamped from its centre minus half its
         * size, so two units can share a centre tile while standing on
         * ground of their own. What must never happen is two units
         * stamped from the same occupancy tile. */
        for (int j = i + 1; j < MV_STACK_N; j++) {
            if (units[h[i]].occ_tx == units[h[j]].occ_tx &&
                units[h[i]].occ_ty == units[h[j]].occ_ty) {
                shared++;
            }
        }
    }
    ASSERT_EQ_INT(0, shared);
    ASSERT(worst <= (int64_t)512 * 512);
    #undef MV_STACK_N
    mv_end();
}

/* ── Issue #60: covering ground is not making progress ─────────────
 *
 * A pocket that opens west, walled north, south and east, with the
 * goal far to the east behind a wall too long to see round. The only
 * route out runs west, out of the mouth and the long way about, so
 * the way the unit must walk and the way it wants to face point in
 * opposite directions. A friend shuffles in the mouth: it never
 * stands still long enough to become a planning obstacle, so the
 * search keeps routing through it and the mover keeps refusing the
 * step, and the unit paces the length of the pocket for ever.
 *
 * It covers thousands of pixels of ground and closes none of it on
 * the goal. That is the case the stall ladder is for, and while the
 * ladder measured displacement from a reference point that moved
 * with the unit, each length of the pocket reset it and the last
 * rung, the one that ends an order nothing can serve, was never
 * reached. Measured on the unfixed mover: after 3000 ticks the order
 * was still live, the unit had paced 2023 px, was no closer to the
 * goal than the 345 px it started at, and the ladder had reset three
 * times and never climbed past its first rung of four.
 */
#define MV_POCKET_TICKS 3000

TEST(a_unit_that_covers_ground_without_closing_on_its_goal_gives_up) {
    GameWorld *w = mv_world();
    ASSERT_NOT_NULL(w);
    /* The pocket: four tiles of it, so the unit stands legally inside
     * and the walls really hold it. */
    for (int tx = 96; tx <= 110; tx++) {
        w->tnt.heightmap[(size_t)99 * w->tnt.height_w + tx] = 255;
        w->tnt.heightmap[(size_t)104 * w->tnt.height_w + tx] = 255;
    }
    for (int ty = 60; ty <= 140; ty++) {
        w->tnt.heightmap[(size_t)ty * w->tnt.height_w + 110] = 255;
    }
    TAK_PathCacheReset();
    int32_t cy = 102 * 16, gx = 130 * 16 + 8, gy = cy;
    int h = Units_Spawn(MV_DEF_WALKER, 1, 0, 108 * 16, cy);
    ASSERT(h >= 0);
    Units_DebugSetAggro(h, UNIT_AGGRO_PASSIVE);
    int b = Units_Spawn(MV_DEF_WALKER, 1, 0, 98 * 16, cy);
    ASSERT(b >= 0);
    Units_DebugSetAggro(b, UNIT_AGGRO_PASSIVE);
    Units_CommandMoveUnit(h, gx, gy);

    int32_t lx = mv_unit(h)->world_x, ly = mv_unit(h)->world_y;
    int64_t best2 = mv_dist2(mv_unit(h), gx, gy);
    long travelled = 0;
    int ended = -1, max_esc = 0, resets = 0, last_esc = 0;
    for (int t = 0; t < MV_POCKET_TICKS && ended < 0; t++) {
        /* The friend keeps its feet moving in the mouth. Two points
         * a tile apart, re-ordered often enough that it never parks. */
        if ((t % 90) == 0) {
            Units_CommandMoveUnit(b, ((t / 90) & 1) ? 97 * 16 : 99 * 16, cy);
        }
        Units_TickEngines();
        const Unit *u = mv_unit(h);
        int e = (int)u->stall_esc;
        if (e > max_esc) max_esc = e;
        if (e < last_esc) resets++;
        last_esc = e;
        int32_t dx = u->world_x - lx, dy = u->world_y - ly;
        travelled += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        lx = u->world_x; ly = u->world_y;
        int64_t d2 = mv_dist2(u, gx, gy);
        if (d2 < best2) best2 = d2;
        if (u->cmd_kind == UNIT_CMD_NONE) ended = t + 1;
    }
    int best = 0;
    while ((int64_t)(best + 1) * (best + 1) <= best2) best++;
    printf("(ended at %d, travelled %ld px, closest %d px, max rung %d, "
           "%d resets) ", ended, travelled, best, max_esc, resets);
    /* It really was pacing, not standing still: the stationary case
     * was already served and proves nothing here. */
    ASSERT(travelled > 1000);
    /* And none of that pacing was progress. The goal is 345 px away
     * through a wall and it never got near it. */
    ASSERT(best > 300);
    /* So the ladder climbed all four rungs and ended the order. The
     * two resets it took on the way are the mover really getting
     * further along its route than it ever had, which is progress and
     * is meant to start the ladder over. There can only ever be a
     * bounded number of those, because the best way left to walk only
     * ever falls. */
    ASSERT_EQ_INT(4, max_esc);
    ASSERT(resets <= 4);
    ASSERT(ended > 0);
    mv_end();
}

/* ── state hash streams ────────────────────────────────────────────── */

#define MV_HASH_TICKS  1800
#define MV_HASH_N      (MV_HASH_TICKS / 60)
/* How often the cold run drops the planner's caches. Small enough that
 * this scenario really does drop them, and the test checks that. */
#define MV_COLD_EVERY  17

/* Twelve units of two classes crossing the same ground: movement,
 * crowding and a replan or two. Sampled every 60 ticks. */
static int mv_hash_run(uint32_t *out, int reset_every, int *out_plans) {
    if (!mv_world()) return 0;
    TAK_PathDebugCounters before, after;
    TAK_PathDebugGetCounters(&before);
    TAK_PathDebugResetEvery(reset_every);
    int32_t rx = 1600, ry = 1600;
    for (int i = 0; i < 12; i++) {
        int h = Units_Spawn(i & 1 ? MV_DEF_KNIGHT : MV_DEF_WALKER, 1, 0,
                            rx - 400 + (i % 4) * 48, ry - 200 + (i / 4) * 48);
        if (h < 0) return 0;
        Units_DebugSetAggro(h, UNIT_AGGRO_PASSIVE);
        Units_CommandMoveUnit(h, rx + 400, ry + 200);
    }
    for (int s = 0; s < MV_HASH_N; s++) {
        /* Halfway through, send everyone back the way they came. A
         * second order is a second search per unit, so the cold run
         * really does plan against dropped caches. */
        if (s == MV_HASH_N / 2) {
            int count = 0;
            Units_GetActive(&count);
            for (int h = 0; h < count; h++) {
                Units_CommandMoveUnit(h, rx - 400, ry - 200);
            }
        }
        mv_run(60);
        out[s] = Units_DebugStateHash() ^ (uint32_t)TAK_AI_DebugStateHash();
    }
    TAK_PathDebugResetEvery(0);
    TAK_PathDebugGetCounters(&after);
    if (out_plans) *out_plans = (int)(after.plans - before.plans);
    mv_end();
    return 1;
}

/* The same battle twice in one process gives the same stream. */
TEST(a_repeated_run_hashes_the_same) {
    static uint32_t a[MV_HASH_N], b[MV_HASH_N];
    ASSERT(mv_hash_run(a, 0, NULL));
    ASSERT(mv_hash_run(b, 0, NULL));
    int moved = 0;
    for (int i = 1; i < MV_HASH_N; i++) if (a[i] != a[i - 1]) moved = 1;
    ASSERT(moved);      /* a stream that never changes would pass blind */
    for (int i = 0; i < MV_HASH_N; i++) ASSERT_EQ_INT((int)a[i], (int)b[i]);
}

/* Dropping the planner's caches part way through must not change the
 * battle: cache warmth is not allowed to reach the simulation. */
TEST(a_cold_planner_hashes_the_same) {
    static uint32_t warm[MV_HASH_N], cold[MV_HASH_N];
    int warm_plans = 0, cold_plans = 0;
    ASSERT(mv_hash_run(warm, 0, &warm_plans));
    ASSERT(mv_hash_run(cold, MV_COLD_EVERY, &cold_plans));
    printf("(%d searches) ", cold_plans);
    /* The caches have to have been dropped at least once, or this
     * proves nothing. */
    ASSERT(cold_plans > MV_COLD_EVERY);
    ASSERT_EQ_INT(warm_plans, cold_plans);
    for (int i = 0; i < MV_HASH_N; i++) {
        ASSERT_EQ_INT((int)warm[i], (int)cold[i]);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("Movement without game data");
    RUN(a_walker_crosses_open_ground);
    RUN(unit_walks_around_a_wall_of_friendly_units);
    RUN(units_do_not_stack_on_one_another);
    RUN(a_unit_that_covers_ground_without_closing_on_its_goal_gives_up);
    TEST_SUITE("State hash");
    RUN(a_repeated_run_hashes_the_same);
    RUN(a_cold_planner_hashes_the_same);
    TEST_REPORT();
}
