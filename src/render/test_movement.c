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

    memset(&w->moveinfo, 0, sizeof(w->moveinfo));
    w->moveinfo.count = 2;
    strncpy(w->moveinfo.classes[0].name, "TESTSMALL", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[0].footprint_x = 1;
    w->moveinfo.classes[0].footprint_z = 1;
    w->moveinfo.classes[0].max_slope = 30;
    strncpy(w->moveinfo.classes[1].name, "TESTBIG", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[1].footprint_x = 2;
    w->moveinfo.classes[1].footprint_z = 2;
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
         * in: a one tile footprint is stamped from centre minus half a
         * tile, so two units can share a centre tile while standing on
         * ground of their own. What must never happen is two units
         * holding the same occupancy tile. */
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
    TEST_SUITE("State hash");
    RUN(a_repeated_run_hashes_the_same);
    RUN(a_cold_planner_hashes_the_same);
    TEST_REPORT();
}
