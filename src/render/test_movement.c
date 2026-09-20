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
#include "tak_ingame.h"
#include "tak_command_queue.h"
#include "tak_terrain.h"

#include <SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ── the harness ───────────────────────────────────────────────────── */

#define MV_TILES   192      /* 16 px tiles per side, so a 3072 px map */
#define MV_GROUND  64       /* flat height, clear of the water line */

enum { MV_DEF_WALKER = 0, MV_DEF_KNIGHT, MV_DEF_BUILDER, MV_DEF_HUT,
       MV_DEF_HORSE, MV_DEF_HORSE3, MV_DEF_COUNT };

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
    /* A walking builder and a two tile thing for it to build. */
    mv_fill_def(&defs[MV_DEF_BUILDER], "TESTBUILD", "TESTSMALL", 1.4f, 300);
    defs[MV_DEF_BUILDER].cap_flags |= UNIT_CAP_BUILDER;
    defs[MV_DEF_BUILDER].worker_time = 20.0f;
    defs[MV_DEF_BUILDER].build_distance = 32;
    /* The horseman's speed and turnrate, from data/units/araknigh.fbi.
     * It travels 1.45 px a tick and turns 0.048 radians, so it needs a
     * 30 px radius to come round. The other defs turn eight times
     * tighter than they travel and never leave the line they walk. */
    mv_fill_def(&defs[MV_DEF_HORSE], "TESTHORSE", "TESTSMALL", 2.9f, 400);
    defs[MV_DEF_HORSE].turn_rate = 1000.0f;
    defs[MV_DEF_HORSE].acceleration = 10.0f;
    defs[MV_DEF_HORSE].brake_rate = 10.0f;
    /* The same horseman in the three tile class the knight really
     * carries, from data/gamedata/moveinfo.tdf: ARAKNIGH is
     * GROUND3 and GROUND3 is three tiles square. */
    mv_fill_def(&defs[MV_DEF_HORSE3], "TESTHOR3", "TESTBIG", 2.9f, 400);
    defs[MV_DEF_HORSE3].turn_rate = 1000.0f;
    defs[MV_DEF_HORSE3].acceleration = 10.0f;
    defs[MV_DEF_HORSE3].brake_rate = 10.0f;
    mv_fill_def(&defs[MV_DEF_HUT], "TESTHUT", "", 0.0f, 500);
    defs[MV_DEF_HUT].cap_flags = 0;
    defs[MV_DEF_HUT].footprint_x = 2;
    defs[MV_DEF_HUT].footprint_z = 2;
    defs[MV_DEF_HUT].build_cost = 100;
    defs[MV_DEF_HUT].buildtime = 100.0f;
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

/* Issue #191. A builder boxed in by ground it cannot climb, ordered to
 * build outside the box. The mover's give up ends the build order, the
 * builder stands free again, and the computer player whose builder it
 * is hears which site failed. A human's builder gives up the same way
 * and nobody keeps a record. */
TEST(a_builder_that_cannot_reach_its_site_gives_the_build_up) {
    for (int seat_is_ai = 1; seat_is_ai >= 0; seat_is_ai--) {
        GameWorld *w = mv_world();
        ASSERT_NOT_NULL(w);
        if (seat_is_ai) w->cfg.players[0].kind = TAK_SLOT_AI;
        for (int tx = 96; tx <= 112; tx++) {
            w->tnt.heightmap[(size_t)96 * w->tnt.height_w + tx] = 255;
            w->tnt.heightmap[(size_t)108 * w->tnt.height_w + tx] = 255;
        }
        for (int ty = 96; ty <= 108; ty++) {
            w->tnt.heightmap[(size_t)ty * w->tnt.height_w + 96] = 255;
            w->tnt.heightmap[(size_t)ty * w->tnt.height_w + 112] = 255;
        }
        TAK_PathCacheReset();
        int b = Units_Spawn(MV_DEF_BUILDER, 1, 0, 104 * 16, 102 * 16);
        ASSERT(b >= 0);
        Units_DebugSetAggro(b, UNIT_AGGRO_PASSIVE);
        int32_t sx = 130 * 16, sy = 102 * 16;
        int frame = Units_BeginBuildingForUnit(b, MV_DEF_HUT, sx, sy);
        ASSERT(frame >= 0);
        ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)mv_unit(b)->cmd_kind);
        ASSERT_EQ_INT(0, TAK_AI_DebugFailedSites(1));

        int ended = -1;
        for (int t = 0; t < MV_POCKET_TICKS && ended < 0; t++) {
            Units_TickEngines();
            if (mv_unit(b)->cmd_kind == UNIT_CMD_NONE) ended = t + 1;
        }
        const Unit *u = mv_unit(b);
        printf("(%s seat: ended at %d, builder at %d,%d, cmd %d) ",
               seat_is_ai ? "ai" : "human", ended, u->world_x, u->world_y,
               (int)u->cmd_kind);
        ASSERT(ended > 0);
        ASSERT_EQ_INT(-1, (int)u->build_target);
        /* Still in its box: it never found a way through. */
        ASSERT(u->world_x > 96 * 16 && u->world_x < 112 * 16);
        ASSERT_EQ_INT(seat_is_ai ? 1 : 0, TAK_AI_DebugFailedSites(1));
        /* The frame nobody reached goes with the order rather than
         * standing there to decay, and no unit is counted lost. */
        for (int t = 0; t < 900; t++) Units_TickEngines();
        ASSERT(mv_unit(frame)->alive != UNIT_ALIVE_ACTIVE);
        ASSERT_EQ_INT(0, (int)w->stats[1].losses);
        mv_end();
    }
}

/* Issue #99: what a near blocked unit costs in turning. The mover aims
 * 16 px along its route while a block flag is set and 80 px otherwise,
 * which is the original's own pair (legacy:183439-183443). A guard and
 * not evidence, since it passed before the fix on this branch too. */
#define MV_CORRIDOR_LEN 1400

/* Turning over a leg: every tick's heading change, wrapped and summed
 * without its sign, plus how often that change swapped direction. */
static float mv_leg_turning(int h, int32_t gx, int32_t gy, int max_ticks,
                            int *out_ticks, int *out_blocked,
                            int *out_reversals) {
    float prev = mv_unit(h)->heading, total = 0.0f;
    int blocked = 0, sign = 0, reversals = 0, t = 0;
    for (; t < max_ticks; t++) {
        Units_TickEngines();
        const Unit *u = mv_unit(h);
        float d = u->heading - prev;
        while (d >  3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        total += (d < 0.0f ? -d : d);
        prev = u->heading;
        /* A tick that barely moved the heading is neither direction. */
        int sg = (d > 0.002f) ? 1 : (d < -0.002f ? -1 : 0);
        if (sg != 0) {
            if (sign != 0 && sg != sign) reversals++;
            sign = sg;
        }
        if (u->route_flags & (UNIT_ROUTE_BLOCKED | UNIT_ROUTE_BLOCKED_HARD |
                              UNIT_ROUTE_NEAR_BLOCK)) blocked++;
        int64_t dx = (int64_t)u->world_x - gx, dy = (int64_t)u->world_y - gy;
        if (dx * dx + dy * dy <= 48 * 48) { t++; break; }
    }
    if (out_ticks) *out_ticks = t;
    if (out_blocked) *out_blocked = blocked;
    if (out_reversals) *out_reversals = reversals;
    return total;
}

TEST(a_near_blocked_unit_holds_its_line) {
    GameWorld *w = mv_world();
    ASSERT_NOT_NULL(w);
    /* Corners 101 to 104 are the only flat ones, so tiles 101 to 103
     * carry the unit and everything either side of them is a cliff.
     * Three tiles of corridor for a two tile unit is one cell of
     * slack, and the 16 px neighbour probe finds the south wall from
     * anywhere in it. */
    for (int tx = 40; tx <= 180; tx++) {
        for (int ty = 94; ty <= 100; ty++)
            w->tnt.heightmap[(size_t)ty * w->tnt.height_w + tx] = 255;
        for (int ty = 105; ty <= 111; ty++)
            w->tnt.heightmap[(size_t)ty * w->tnt.height_w + tx] = 255;
    }
    TAK_PathCacheReset();
    int32_t sx = 60 * 16, sy = 1648;
    int h = Units_Spawn(MV_DEF_HORSE, 1, 0, sx, sy);
    ASSERT(h >= 0);
    Units_DebugSetAggro(h, UNIT_AGGRO_PASSIVE);
    /* It starts facing south, across the corridor, and is sent along
     * it, so half a pi of the budget is the turn onto the route. */
    ASSERT(mv_unit(h)->heading > 3.0f);
    int32_t gx = sx + MV_CORRIDOR_LEN, gy = sy;
    Units_CommandMoveUnit(h, gx, gy);
    int ticks = 0, blocked = 0, reversals = 0;
    float turned = mv_leg_turning(h, gx, gy, 2400, &ticks, &blocked,
                                  &reversals);
    const Unit *u = mv_unit(h);
    printf("(%.2fpi turned, %d reversals, %d ticks, %d blocked, at %d,%d) ",
           (double)(turned / 3.14159265f), reversals, ticks, blocked,
           u->world_x, u->world_y);
    /* It walked the corridor, and it was near blocked for nearly every
     * tick of it: a leg that sailed through open ground proves nothing
     * about the 16 px aim point. */
    int64_t dx = (int64_t)u->world_x - gx, dy = (int64_t)u->world_y - gy;
    ASSERT(dx * dx + dy * dy <= 48 * 48);
    ASSERT(blocked * 10 > ticks * 9);
    /* One settling swing, not a hunt. Today it is one reversal and
     * 0.86 pi, and both are flat against corridor length. */
    ASSERT(reversals <= 4);
    ASSERT(turned < 1.1f * 3.14159265f);
    mv_end();
}
/* Issue #229. A frame is held by a builder that is closing on it. One
 * that has stopped getting nearer is not coming, so the order ends
 * well before the mover's own give up, the frame comes off the ground
 * and the site is free for somebody else. */
TEST(a_frame_whose_builder_is_not_closing_frees_the_site) {
    GameWorld *w = mv_world();
    ASSERT_NOT_NULL(w);
    for (int tx = 96; tx <= 112; tx++) {
        w->tnt.heightmap[(size_t)96 * w->tnt.height_w + tx] = 255;
        w->tnt.heightmap[(size_t)108 * w->tnt.height_w + tx] = 255;
    }
    for (int ty = 96; ty <= 108; ty++) {
        w->tnt.heightmap[(size_t)ty * w->tnt.height_w + 96] = 255;
        w->tnt.heightmap[(size_t)ty * w->tnt.height_w + 112] = 255;
    }
    TAK_PathCacheReset();
    int b = Units_Spawn(MV_DEF_BUILDER, 1, 0, 104 * 16, 102 * 16);
    ASSERT(b >= 0);
    Units_DebugSetAggro(b, UNIT_AGGRO_PASSIVE);
    int32_t sx = 130 * 16, sy = 102 * 16;
    int frame = Units_BeginBuildingForUnit(b, MV_DEF_HUT, sx, sy);
    ASSERT(frame >= 0);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)mv_unit(b)->cmd_kind);
    ASSERT_EQ_INT(0, Units_IsBuildSiteClear(MV_DEF_HUT, sx, sy));

    /* The mover climbs four rungs of 240 ticks before it gives the
     * order up, and that is what the owner sees as ages. The frame
     * has to go well inside it. */
    int gone = 0;
    for (int t = 0; t < 900 && !gone; t++) {
        Units_TickEngines();
        if (mv_unit(frame)->alive != UNIT_ALIVE_ACTIVE) gone = t + 1;
    }
    printf("(frame gone at %d, builder cmd %d) ",
           gone, (int)mv_unit(b)->cmd_kind);
    ASSERT(gone > 0);
    ASSERT(gone < 900);
    /* The builder is free, the pad is clear again and nothing is
     * counted lost: no work was ever done on the frame. */
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)mv_unit(b)->cmd_kind);
    ASSERT_EQ_INT(-1, (int)mv_unit(b)->build_target);
    ASSERT_EQ_INT(1, Units_IsBuildSiteClear(MV_DEF_HUT, sx, sy));
    ASSERT_EQ_INT(0, (int)w->stats[1].losses);
    mv_end();
}

/* Issue #229. The other side of the same rule: a long walk to a site
 * the builder really can reach must not cost it the frame. The walk
 * here runs many times the ten second grace before the builder is in
 * range, and the site has to still be there when it arrives. */
TEST(a_builder_walking_a_long_way_keeps_its_frame) {
    GameWorld *w = mv_world();
    ASSERT_NOT_NULL(w);
    int b = Units_Spawn(MV_DEF_BUILDER, 1, 0, 400, 400);
    ASSERT(b >= 0);
    Units_DebugSetAggro(b, UNIT_AGGRO_PASSIVE);
    int32_t sx = 2600, sy = 2600;
    int frame = Units_BeginBuildingForUnit(b, MV_DEF_HUT, sx, sy);
    ASSERT(frame >= 0);

    int arrived = 0, lost = 0;
    for (int t = 0; t < 6000 && !arrived && !lost; t++) {
        Units_TickEngines();
        if (mv_unit(frame)->alive != UNIT_ALIVE_ACTIVE) lost = t + 1;
        else if (mv_dist2(mv_unit(b), sx, sy) <= 96 * 96) arrived = t + 1;
    }
    printf("(arrived at %d, lost at %d, builder at %d,%d) ",
           arrived, lost, mv_unit(b)->world_x, mv_unit(b)->world_y);
    ASSERT_EQ_INT(0, lost);
    ASSERT(arrived > 600);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)mv_unit(frame)->alive);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)mv_unit(b)->cmd_kind);
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

/* Ticks until the unit has stood still for a while, or gives up. */
static int mv_settle(int h, int max_ticks) {
    int still = 0, t = 0;
    while (t < max_ticks && still < 120) {
        Units_TickEngines();
        t++;
        const Unit *u = mv_unit(h);
        if (!u) return t;
        if (u->velocity == 0 && u->cur_speed_ppt == 0.0f) still++;
        else still = 0;
    }
    return t;
}

/* A move order ends at the point it was given, whichever way the unit
 * had to walk to get there. */
TEST(a_move_order_ends_at_its_point) {
    static const int32_t goals[][2] = {
        { 2300, 1500 }, { 700, 1500 }, { 1500, 2300 }, { 1500, 700 },
        { 2100, 2100 }, { 2317, 1433 }, { 803, 2197 },
    };
    static const int defs[] = { MV_DEF_WALKER, MV_DEF_KNIGHT };
    int64_t worst = 0;
    for (int d = 0; d < 2; d++) {
        for (size_t i = 0; i < sizeof(goals) / sizeof(goals[0]); i++) {
            ASSERT_NOT_NULL(mv_world());
            int h = Units_Spawn(defs[d], 1, 0, 1500, 1500);
            ASSERT(h >= 0);
            Units_CommandMoveUnit(h, goals[i][0], goals[i][1]);
            int t = mv_settle(h, 6000);
            const Unit *u = mv_unit(h);
            ASSERT_NOT_NULL(u);
            int64_t d2 = mv_dist2(u, goals[i][0], goals[i][1]);
            printf("\n    def %d goal (%d,%d) stopped at (%d,%d) off by (%d,%d) d2=%lld after %d ticks cmd=%d",
                   d, goals[i][0], goals[i][1], u->world_x, u->world_y,
                   (int)(u->world_x - goals[i][0]), (int)(u->world_y - goals[i][1]),
                   (long long)d2, t, (int)u->cmd_kind);
            if (d2 > worst) worst = d2;
            mv_end();
        }
    }
    printf("\n    worst d2=%lld ", (long long)worst);
    ASSERT(worst <= 8 * 8);
}

/* Where a unit is drawn: lifted by half the ground's height. */
static int32_t mv_drawn_y(const Unit *u) {
    return u->world_y - (int32_t)((float)Terrain_SampleHeight(
                            World_Get(), u->world_x, u->world_y) *
                        Units_GetTanTilt());
}

/* A click on the ground sends the unit to the spot under the pointer,
 * so it stops where the pointer was, not half the ground's height
 * above it. The harness ground sits at 64, a 32 px lift. */
TEST(a_click_sends_the_unit_to_the_ground_under_the_pointer) {
    static const int32_t clicks[][2] = {
        { 1500, 1900 }, { 1500, 1100 }, { 1900, 1500 }, { 1180, 1820 },
    };
    ASSERT_EQ_INT(32, (int)(MV_GROUND * Units_GetTanTilt()));
    for (size_t i = 0; i < sizeof(clicks) / sizeof(clicks[0]); i++) {
        GameWorld *w = mv_world();
        ASSERT_NOT_NULL(w);
        w->cam_x = 0;
        w->cam_y = 0;
        Units_SetLocalPlayer(1);
        int h = Units_Spawn(MV_DEF_WALKER, 1, 0, 1500, 1500);
        ASSERT(h >= 0);
        Units_SelectSingle(h);
        InGame_WorldClick(clicks[i][0], clicks[i][1], 0);
        TAK_CmdQueue_Run();
        mv_settle(h, 6000);
        const Unit *u = mv_unit(h);
        ASSERT_NOT_NULL(u);
        int32_t dx = u->world_x - clicks[i][0];
        int32_t dy = mv_drawn_y(u) - clicks[i][1];
        printf("\n    click (%d,%d) drawn at (%d,%d) off by (%d,%d)",
               clicks[i][0], clicks[i][1], u->world_x, mv_drawn_y(u),
               (int)dx, (int)dy);
        ASSERT(dx * dx + dy * dy <= 10 * 10);
        mv_end();
    }
    printf("\n    ");
}

/* A marquee takes the units drawn inside it. The box is on the screen,
 * so a unit counts by where it is drawn, lifted, not by its flat
 * position half the ground's height below. */
TEST(a_marquee_takes_the_units_it_is_drawn_over) {
    GameWorld *w = mv_world();
    ASSERT_NOT_NULL(w);
    Units_SetLocalPlayer(1);
    int h = Units_Spawn(MV_DEF_WALKER, 1, 0, 1500, 1500);
    ASSERT(h >= 0);
    const Unit *u = mv_unit(h);
    ASSERT_NOT_NULL(u);
    ASSERT_EQ_INT(1468, (int)mv_drawn_y(u));
    int n = 0;
    /* Over the drawn unit, clear of its flat position. */
    InGame_WorldDrag(1450, 1440, 1550, 1490, 0);
    Units_GetSelection(&n);
    ASSERT_EQ_INT(1, n);
    /* Over the flat position, clear of the drawn unit. */
    InGame_WorldDrag(1450, 1495, 1550, 1540, 0);
    Units_GetSelection(&n);
    ASSERT_EQ_INT(0, n);
    mv_end();
}

/* Issue #99: a unit as wide as the corridor it walks. Our ground test
 * asks only about the pixel the unit steers by, where the original
 * sweeps the whole footprint (legacy:219098-219170) over the cell it
 * rounds onto (legacy:184165-184170), so the count printed here is a
 * standing deviation and the bound on it is loose on purpose. */

/* The tiles a footprint covers, anchored the way the original does. */
static int mv_fp_tile(int32_t pos, int fp) {
    return Occ_TileOf(pos - (fp - 1) * 8);
}

static int mv_footprint_in_wall(const GameWorld *w, const Unit *u, int fp,
                                int slope) {
    int tx0 = mv_fp_tile(u->world_x, fp);
    int ty0 = mv_fp_tile(u->world_y, fp);
    for (int row = 0; row < fp; row++) {
        for (int col = 0; col < fp; col++) {
            int32_t x = (int32_t)(tx0 + col) * 16 + 8;
            int32_t y = (int32_t)(ty0 + row) * 16 + 8;
            if (!Terrain_IsWalkable(w, x, y, slope)) return 1;
        }
    }
    return 0;
}

TEST(a_wide_unit_walks_a_corridor_its_own_width) {
    GameWorld *w = mv_world();
    ASSERT_NOT_NULL(w);
    /* Corner rows 102 to 105 are the only flat ones, so tile rows 102
     * to 104 carry the unit and everything either side is a cliff.
     * Three tiles for a three tile unit, aligned so the route search
     * plans a straight run down the middle of them. */
    for (int tx = 0; tx < w->tnt.height_w; tx++) {
        for (int ty = 0; ty < w->tnt.height_h; ty++) {
            w->tnt.heightmap[(size_t)ty * w->tnt.height_w + tx] =
                (ty >= 102 && ty <= 105) ? MV_GROUND : 255;
        }
    }
    TAK_PathCacheReset();
    int32_t sx = 60 * 16, sy = 1648;
    int h = Units_Spawn(MV_DEF_HORSE3, 1, 0, sx, sy);
    ASSERT(h >= 0);
    Units_DebugSetAggro(h, UNIT_AGGRO_PASSIVE);
    /* It starts on the line, with its footprint clear of both walls,
     * and facing across the corridor. */
    ASSERT_EQ_INT(0, mv_footprint_in_wall(w, mv_unit(h), 3, 30));
    ASSERT(mv_unit(h)->heading > 3.0f);
    int32_t gx = sx + 700, gy = sy;
    Units_CommandMoveUnit(h, gx, gy);
    float prev = mv_unit(h)->heading, turned = 0.0f;
    int in_wall = 0, worst = 0, ticks = 0, arrived = 0;
    for (; ticks < 1600; ticks++) {
        Units_TickEngines();
        const Unit *u = mv_unit(h);
        float d = u->heading - prev;
        while (d >  3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        turned += (d < 0.0f ? -d : d);
        prev = u->heading;
        if (mv_footprint_in_wall(w, u, 3, 30)) in_wall++;
        int off = (int)(u->world_y - sy);
        if (off < 0) off = -off;
        if (off > worst) worst = off;
        int64_t dx = (int64_t)u->world_x - gx, dy = (int64_t)u->world_y - gy;
        if (dx * dx + dy * dy <= 48 * 48) { arrived = 1; ticks++; break; }
    }
    printf("(%d ticks in the wall, %d px off the line, %.2fpi turned, "
           "%d ticks) ", in_wall, worst, (double)(turned / 3.14159265f),
           ticks);
    ASSERT(arrived);
    /* Today it is 38 ticks of 466. The bound catches a unit that lives
     * in the wall rather than clipping it in passing. */
    ASSERT(in_wall * 4 < ticks);
    /* Turning over the leg, which is what issue #99 measures. Today it
     * is 1.03 pi and the turn onto the corridor is most of it. */
    ASSERT(turned < 1.6f * 3.14159265f);
    mv_end();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("Movement without game data");
    RUN(a_move_order_ends_at_its_point);
    RUN(a_click_sends_the_unit_to_the_ground_under_the_pointer);
    RUN(a_marquee_takes_the_units_it_is_drawn_over);
    RUN(a_walker_crosses_open_ground);
    RUN(unit_walks_around_a_wall_of_friendly_units);
    RUN(units_do_not_stack_on_one_another);
    RUN(a_unit_that_covers_ground_without_closing_on_its_goal_gives_up);
    RUN(a_builder_that_cannot_reach_its_site_gives_the_build_up);
    RUN(a_near_blocked_unit_holds_its_line);
    RUN(a_wide_unit_walks_a_corridor_its_own_width);
    RUN(a_frame_whose_builder_is_not_closing_frees_the_site);
    RUN(a_builder_walking_a_long_way_keeps_its_frame);
    TEST_SUITE("State hash");
    RUN(a_repeated_run_hashes_the_same);
    RUN(a_cold_planner_hashes_the_same);
    TEST_REPORT();
}
