/*
 * test_build_facing.c -- buildings placed turned by quarter turns, with
 * no game data.
 *
 * The harness is the movement tests' flat world with synthetic defs: a
 * walking builder, a long hall three cells by one, a hall with one
 * blocking corner and a lodestone. Yardmaps come from FBI strings through
 * the same parse the game uses.
 */

#include "test_framework.h"
#include "tak_battle_config.h"
#include "tak_features.h"
#include "tak_command_exec.h"
#include "tak_command_queue.h"
#include "tak_commands.h"
#include "tak_hud.h"
#include "tak_memory.h"
#include "tak_moveinfo.h"
#include "tak_net_protocol.h"
#include "tak_occupancy.h"
#include "tak_pathing.h"
#include "tak_sim_hash.h"
#include "tak_tnt.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define BF_TILES  128
#define BF_GROUND 64

enum { BF_BUILDER = 0, BF_WALKER, BF_HALL, BF_CORNER, BF_LODE, BF_KEEP, BF_TOWER,
       BF_RAISER, BF_DEF_COUNT };

static void bf_fill(UnitDef *d, const char *name, float velocity, int fx, int fz) {
    memset(d, 0, sizeof(*d));
    strncpy(d->unitname, name, sizeof(d->unitname) - 1);
    strncpy(d->display_name, name, sizeof(d->display_name) - 1);
    strncpy(d->movement_class, velocity > 0.0f ? "TESTSMALL" : "",
            sizeof(d->movement_class) - 1);
    d->max_health = 300;
    d->sight_distance = 320;
    d->max_velocity = velocity;
    d->acceleration = velocity / 4.0f;
    d->brake_rate = velocity / 2.0f;
    d->turn_rate = 4000.0f;
    d->max_slope = 30;
    d->bmcode = velocity > 0.0f ? 1 : 0;
    d->cap_flags = velocity > 0.0f ? (UNIT_CAP_MOVE | UNIT_CAP_STOP) : 0;
    d->footprint_x = fx;
    d->footprint_z = fz;
    d->build_cost = 100;
    d->buildtime = 100.0f;
}

static GameWorld *bf_world(void) {
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    cfg.line_of_sight = 0;
    cfg.players[0].kind = TAK_SLOT_HUMAN;
    cfg.players[1].kind = TAK_SLOT_HUMAN;
    if (World_BeginLoad(NULL, &cfg, "synthetic", "aramon") != 0) return NULL;
    GameWorld *w = World_Get();
    if (!w) return NULL;
    w->map_pixels_w = BF_TILES * 16;
    w->map_pixels_h = BF_TILES * 16;
    w->viewport_w = 640;
    w->viewport_h = 480;
    w->water_height = 0;
    w->tnt.width_tiles = BF_TILES;
    w->tnt.height_tiles = BF_TILES;
    w->tnt.height_w = BF_TILES + 1;
    w->tnt.height_h = BF_TILES + 1;
    size_t hn = (size_t)w->tnt.height_w * (size_t)w->tnt.height_h;
    w->tnt.heightmap = (uint8_t *)tak_malloc(hn);
    if (!w->tnt.heightmap) return NULL;
    memset(w->tnt.heightmap, BF_GROUND, hn);
    memset(&w->moveinfo, 0, sizeof(w->moveinfo));
    w->moveinfo.count = 1;
    strncpy(w->moveinfo.classes[0].name, "TESTSMALL", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[0].footprint_x = 1;
    w->moveinfo.classes[0].footprint_z = 1;
    w->moveinfo.classes[0].max_slope = 30;
    if (!Occ_Ensure(w)) return NULL;
    TAK_PathCacheReset();
    World_MarkLoaded();

    UnitDef defs[BF_DEF_COUNT];
    bf_fill(&defs[BF_BUILDER], "TESTBUILD", 1.4f, 1, 1);
    defs[BF_BUILDER].cap_flags |= UNIT_CAP_BUILDER;
    defs[BF_BUILDER].worker_time = 20.0f;
    defs[BF_BUILDER].build_distance = 32;
    bf_fill(&defs[BF_WALKER], "TESTSWORD", 1.4f, 1, 1);
    bf_fill(&defs[BF_HALL], "TESTHALL", 0.0f, 3, 1);
    bf_fill(&defs[BF_CORNER], "TESTCORNR", 0.0f, 3, 2);
    bf_fill(&defs[BF_LODE], "TESTLODE", 0.0f, 2, 2);
    /* Three by five, and its wreck the same shape, which a raiser can
     * bring back as the keep. */
    bf_fill(&defs[BF_KEEP], "TESTKEEP", 0.0f, 3, 5);
    strncpy(defs[BF_KEEP].corpse, "TESTKEEP_dead", sizeof(defs[BF_KEEP].corpse) - 1);
    /* The same size, with a wreck of one by two on its south end. */
    bf_fill(&defs[BF_TOWER], "TESTTOWER", 0.0f, 3, 5);
    strncpy(defs[BF_TOWER].corpse, "TESTRUBBLE", sizeof(defs[BF_TOWER].corpse) - 1);
    defs[BF_TOWER].corpse_adjust_x = 1;
    defs[BF_TOWER].corpse_adjust_z = 3;
    bf_fill(&defs[BF_RAISER], "TESTRAISE", 1.4f, 1, 1);
    defs[BF_RAISER].cap_flags |= UNIT_CAP_RESURRECT;
    defs[BF_RAISER].worker_time = 400.0f;
    defs[BF_RAISER].build_distance = 64;
    FeatureDef ruins[2];
    memset(ruins, 0, sizeof ruins);
    strncpy(ruins[0].name, "TESTKEEP_dead", sizeof(ruins[0].name) - 1);
    ruins[0].footprint_x = 3;
    ruins[0].footprint_z = 5;
    ruins[0].blocking = 1;
    ruins[0].resurrectable = 1;
    strncpy(ruins[1].name, "TESTRUBBLE", sizeof(ruins[1].name) - 1);
    ruins[1].footprint_x = 1;
    ruins[1].footprint_z = 2;
    ruins[1].blocking = 1;
    if (Features_DebugSetDefs(ruins, 2) != 2) return NULL;
    if (Units_DebugSetDefs(defs, BF_DEF_COUNT) != BF_DEF_COUNT) return NULL;
    if (Units_DebugSetYardmap(BF_HALL, "ooo") != 0) return NULL;
    /* Only the north west cell blocks. */
    if (Units_DebugSetYardmap(BF_CORNER, "o.. ...") != 0) return NULL;
    if (Units_DebugSetYardmap(BF_LODE, "SSSS") != 0) return NULL;
    if (Units_DebugSetYardmap(BF_KEEP, "ooooooooooooooo") != 0) return NULL;
    if (Units_DebugSetYardmap(BF_TOWER, "ooooooooooooooo") != 0) return NULL;
    Units_SetLocalPlayer(1);
    TAK_CmdQueue_Reset(0);
    return w;
}

static void bf_end(void) {
    Units_ClearInstances();
    Features_FreeAll();
    World_End(NULL);
    TAK_PathCacheReset();
    TAK_CmdQueue_Reset(0);
}

static const Unit *bf_unit(int handle) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    return (handle >= 0 && handle < count) ? &units[handle] : NULL;
}

/* A cell centre, where an odd sided footprint snaps to itself. */
#define BF_CX (60 * 16 + 8)
#define BF_CY (60 * 16 + 8)

/* ── the footprint and the yardmap ─────────────────────────────────── */

TEST(a_quarter_turn_swaps_the_footprint) {
    ASSERT_NOT_NULL(bf_world());
    int fx = 0, fz = 0;
    for (int f = 0; f < UNIT_FACINGS; f++) {
        Units_DefFootprint(BF_HALL, f, &fx, &fz);
        ASSERT_EQ_INT(f & 1 ? 1 : 3, fx);
        ASSERT_EQ_INT(f & 1 ? 3 : 1, fz);
    }
    bf_end();
}

TEST(the_yardmap_turns_clockwise_with_the_building) {
    ASSERT_NOT_NULL(bf_world());
    const UnitDef *d = Units_GetDef(BF_CORNER);
    uint8_t yard[TAK_YARD_MAX_CELLS];
    /* The blocking corner goes north west, north east, south east,
     * south west as the hall turns clockwise. */
    static const int at[4][2] = { {0, 0}, {1, 0}, {2, 1}, {0, 2} };
    for (int f = 0; f < UNIT_FACINGS; f++) {
        ASSERT_EQ_INT(6, Units_ExpandYardmapFacing(d, f, yard, TAK_YARD_MAX_CELLS));
        int w = (f & 1) ? 2 : 3;
        for (int i = 0; i < 6; i++) {
            int blocks = (yard[i] & 0x06) != 0;
            ASSERT_EQ_INT(i == at[f][1] * w + at[f][0], blocks);
        }
    }
    /* Four quarter turns come back to where they started. */
    for (int row = 0; row < 2; row++)
        for (int col = 0; col < 3; col++) {
            int c = col, r = row, w = 3, h = 2;
            for (int k = 0; k < 4; k++) {
                int oc, orow;
                /* One turn back from a frame of w by h. */
                Occ_UnturnCell(1, h, w, c, r, &oc, &orow);
                c = oc; r = orow;
                int t = w; w = h; h = t;
            }
            ASSERT_EQ_INT(col, c);
            ASSERT_EQ_INT(row, r);
        }
    bf_end();
}

/* ── placement ─────────────────────────────────────────────────────── */

/* The long hall is three cells east to west and one north to south.
 * A soldier north of the site blocks it only turned a quarter, and one
 * two cells east only unturned. */
TEST(a_long_hall_is_blocked_on_the_side_it_turns_onto) {
    for (int f = 0; f < UNIT_FACINGS; f++) {
        ASSERT_NOT_NULL(bf_world());
        int north = Units_Spawn(BF_WALKER, 2, 1, BF_CX, BF_CY - 16);
        ASSERT(north >= 0);
        ASSERT_EQ_INT((f & 1) ? 0 : 1,
                      Units_IsBuildSiteClearFacing(BF_HALL, BF_CX, BF_CY, f));
        bf_end();

        ASSERT_NOT_NULL(bf_world());
        int east = Units_Spawn(BF_WALKER, 2, 1, BF_CX + 16, BF_CY);
        ASSERT(east >= 0);
        ASSERT_EQ_INT((f & 1) ? 1 : 0,
                      Units_IsBuildSiteClearFacing(BF_HALL, BF_CX, BF_CY, f));
        bf_end();
    }
}

/* A hall started turned holds the cells it turned onto, in the
 * occupancy grid the planner reads, and a second hall is judged against
 * the turned one. */
TEST(a_turned_hall_holds_the_cells_it_stands_on) {
    for (int f = 0; f < UNIT_FACINGS; f++) {
        GameWorld *w = bf_world();
        ASSERT_NOT_NULL(w);
        int b = Units_Spawn(BF_BUILDER, 1, 0, BF_CX - 200, BF_CY);
        ASSERT(b >= 0);
        int hall = Units_BeginBuildingForUnitFacing(b, BF_HALL, BF_CX, BF_CY, f);
        ASSERT(hall >= 0);
        ASSERT_EQ_INT(f, Units_GetFacing(hall));
        ASSERT_EQ_INT(f, (int)bf_unit(hall)->facing);
        int tx = BF_CX / 16, ty = BF_CY / 16;
        int held_ns = Occ_QueryTileStatic(w, tx, ty - 1, 2) != 0 &&
                      Occ_QueryTileStatic(w, tx, ty + 1, 2) != 0;
        int held_ew = Occ_QueryTileStatic(w, tx - 1, ty, 2) != 0 &&
                      Occ_QueryTileStatic(w, tx + 1, ty, 2) != 0;
        ASSERT(Occ_QueryTileStatic(w, tx, ty, 2) != 0);
        ASSERT_EQ_INT((f & 1) ? 1 : 0, held_ns);
        ASSERT_EQ_INT((f & 1) ? 0 : 1, held_ew);
        /* A second hall one cell north, across the turned one's end. */
        ASSERT_EQ_INT((f & 1) ? 0 : 1,
                      Units_IsBuildSiteClearFacing(BF_HALL, BF_CX, BF_CY - 16, 0));
        bf_end();
    }
}

/* Ground the map marks impassable refuses a building whichever way it
 * turns: the long hall's west end lies on a mark only unturned, and the
 * cell north of its centre only turned a quarter. */
static uint16_t g_bf_marks[BF_TILES * BF_TILES];

TEST(a_turned_hall_is_refused_on_ground_the_map_marks) {
    static const int marks[2][2] = { { BF_CX / 16 - 1, BF_CY / 16 },
                                     { BF_CX / 16, BF_CY / 16 - 1 } };
    for (int m = 0; m < 2; m++)
        for (int f = 0; f < UNIT_FACINGS; f++) {
            GameWorld *w = bf_world();
            ASSERT_NOT_NULL(w);
            for (int i = 0; i < BF_TILES * BF_TILES; i++) g_bf_marks[i] = 0xFFFFu;
            g_bf_marks[marks[m][1] * BF_TILES + marks[m][0]] = TNT_CELL_IMPASSABLE;
            w->tnt.feature_layer = g_bf_marks;
            int clear = Units_IsBuildSiteClearFacing(BF_HALL, BF_CX, BF_CY, f);
            w->tnt.feature_layer = NULL;
            /* West end: blocked unturned. North cell: blocked turned. */
            int want_blocked = m == 0 ? !(f & 1) : (f & 1);
            ASSERT_EQ_INT(want_blocked ? 0 : 1, clear);
            bf_end();
        }
}

/* The model turns with the footprint: a quarter turn clockwise from
 * facing south is facing west. */
TEST(a_turned_building_faces_the_way_it_turned) {
    ASSERT_NOT_NULL(bf_world());
    const float pi = 3.14159265358979323846f;
    ASSERT(Units_BuildHeadingFacing(BF_HALL, 0) == Units_BuildHeading(BF_HALL));
    ASSERT(Units_BuildHeadingFacing(BF_HALL, 1) == -pi / 2.0f);
    ASSERT(Units_BuildHeadingFacing(BF_HALL, 2) == 0.0f);
    ASSERT(Units_BuildHeadingFacing(BF_HALL, 3) == pi / 2.0f);
    int b = Units_Spawn(BF_BUILDER, 1, 0, BF_CX - 200, BF_CY);
    int hall = Units_BeginBuildingForUnitFacing(b, BF_HALL, BF_CX, BF_CY, 1);
    ASSERT(hall >= 0);
    ASSERT(bf_unit(hall)->heading == -pi / 2.0f);
    bf_end();
}

/* A lodestone's yard has to cover the pad it stands on, so it never
 * turns, whatever the order asks. */
TEST(a_lodestone_never_turns) {
    ASSERT_NOT_NULL(bf_world());
    ASSERT_EQ_INT(1, Units_DefCanTurn(BF_HALL));
    ASSERT_EQ_INT(0, Units_DefCanTurn(BF_LODE));
    for (int f = 0; f < UNIT_FACINGS; f++) ASSERT_EQ_INT(0, Units_DefFacing(BF_LODE, f));
    HUD_BeginBuildPlacement(BF_LODE);
    ASSERT_EQ_INT(0, HUD_TurnBuild(1));
    ASSERT_EQ_INT(0, HUD_GetBuildFacing());
    HUD_ClearCommandMode();
    bf_end();
}

/* R and ] turn the armed building clockwise, Shift+R and [ back, and
 * a new placement starts unturned. */
TEST(the_armed_building_turns_both_ways_and_starts_unturned) {
    ASSERT_NOT_NULL(bf_world());
    HUD_BeginBuildPlacement(BF_HALL);
    ASSERT_EQ_INT(0, HUD_GetBuildFacing());
    ASSERT_EQ_INT(1, HUD_TurnBuild(1));
    ASSERT_EQ_INT(1, HUD_GetBuildFacing());
    ASSERT_EQ_INT(1, HUD_TurnBuild(-1));
    ASSERT_EQ_INT(1, HUD_TurnBuild(-1));
    ASSERT_EQ_INT(3, HUD_GetBuildFacing());
    HUD_BeginBuildPlacement(BF_CORNER);
    ASSERT_EQ_INT(0, HUD_GetBuildFacing());
    HUD_ClearCommandMode();
    ASSERT_EQ_INT(0, HUD_TurnBuild(1));
    bf_end();
}

/* A turned keep's wreck lies on the cells the keep stood on, turned the
 * same way and centred where the model draws. */
TEST(a_turned_keeps_wreck_lies_where_it_stood) {
    for (int f = 0; f < UNIT_FACINGS; f++) {
        GameWorld *w = bf_world();
        ASSERT_NOT_NULL(w);
        int b = Units_Spawn(BF_BUILDER, 1, 0, BF_CX - 300, BF_CY);
        int keep = Units_BeginBuildingForUnitFacing(b, BF_KEEP, BF_CX, BF_CY, f);
        ASSERT(keep >= 0);
        int inst = Units_DebugLeaveCorpse(keep);
        ASSERT(inst >= 0);
        int fx = 0, fz = 0;
        Features_InstanceFootprint(w, inst, &fx, &fz);
        ASSERT_EQ_INT((f & 1) ? 5 : 3, fx);
        ASSERT_EQ_INT((f & 1) ? 3 : 5, fz);
        int32_t cx = 0, cy = 0;
        ASSERT_EQ_INT(0, Features_InstanceCentre(w, inst, &cx, &cy));
        ASSERT_EQ_INT(BF_CX, cx);
        ASSERT_EQ_INT(BF_CY, cy);
        ASSERT_EQ_INT(BF_CX - fx * 8, (int)w->features[inst].tile_x * 16);
        ASSERT_EQ_INT(BF_CY - fz * 8, (int)w->features[inst].tile_z * 16);
        bf_end();
    }
}

/* A wreck smaller than the tower, set one cell in and three down, lies
 * on the tower's south end unturned. Turned clockwise with the tower its
 * centre goes south, west, north, east of the tower's, which a turn the
 * wrong way round would put on the other side. */
TEST(a_small_wreck_turns_clockwise_with_its_tower) {
    static const int32_t want[4][2] = { { 0, 24 }, { -24, 0 }, { 0, -24 }, { 24, 0 } };
    for (int f = 0; f < UNIT_FACINGS; f++) {
        GameWorld *w = bf_world();
        ASSERT_NOT_NULL(w);
        int b = Units_Spawn(BF_BUILDER, 1, 0, BF_CX - 300, BF_CY);
        int tower = Units_BeginBuildingForUnitFacing(b, BF_TOWER, BF_CX, BF_CY, f);
        ASSERT(tower >= 0);
        int inst = Units_DebugLeaveCorpse(tower);
        ASSERT(inst >= 0);
        int fx = 0, fz = 0;
        Features_InstanceFootprint(w, inst, &fx, &fz);
        ASSERT_EQ_INT((f & 1) ? 2 : 1, fx);
        ASSERT_EQ_INT((f & 1) ? 1 : 2, fz);
        int32_t cx = 0, cy = 0;
        ASSERT_EQ_INT(0, Features_InstanceCentre(w, inst, &cx, &cy));
        ASSERT_EQ_INT(BF_CX + want[f][0], cx);
        ASSERT_EQ_INT(BF_CY + want[f][1], cy);
        bf_end();
    }
}

/* A keep raised from its wreck stands turned the way it fell. */
TEST(a_raised_keep_stands_the_way_it_fell) {
    GameWorld *w = bf_world();
    ASSERT_NOT_NULL(w);
    int b = Units_Spawn(BF_BUILDER, 1, 0, BF_CX - 300, BF_CY);
    int keep = Units_BeginBuildingForUnitFacing(b, BF_KEEP, BF_CX, BF_CY, 3);
    ASSERT(keep >= 0);
    int inst = Units_DebugLeaveCorpse(keep);
    ASSERT(inst >= 0);
    ASSERT_EQ_INT(3, (int)w->features[inst].facing);
    for (int t = 0; t < 10; t++) Units_TickEngines();
    int r = Units_Spawn(BF_RAISER, 1, 0, BF_CX - 80, BF_CY);
    ASSERT(r >= 0);
    Units_DebugSetAggro(r, UNIT_AGGRO_PASSIVE);
    ASSERT_EQ_INT(1, Units_OrderResurrectFeature(r, BF_CX, BF_CY));
    int raised = -1;
    for (int t = 0; t < 60 * 60 && raised < 0; t++) {
        Units_TickEngines();
        int count = 0;
        const Unit *units = Units_GetActive(&count);
        for (int i = 0; i < count; i++)
            if (units[i].alive == UNIT_ALIVE_ACTIVE && units[i].def_idx == BF_KEEP) raised = i;
    }
    ASSERT(raised >= 0);
    ASSERT_EQ_INT(3, Units_GetFacing(raised));
    bf_end();
}

/* A turned building taken over keeps its facing and its turned cells. */
TEST(a_captured_building_keeps_its_facing) {
    GameWorld *w = bf_world();
    ASSERT_NOT_NULL(w);
    int b = Units_Spawn(BF_BUILDER, 1, 0, BF_CX - 200, BF_CY);
    int hall = Units_BeginBuildingForUnitFacing(b, BF_HALL, BF_CX, BF_CY, 1);
    ASSERT(hall >= 0);
    int taken = Units_Capture(hall, 2);
    ASSERT(taken >= 0);
    ASSERT_EQ_INT(1, Units_GetFacing(taken));
    int tx = BF_CX / 16, ty = BF_CY / 16;
    ASSERT(Occ_QueryTileStatic(w, tx, ty - 1, 1) != 0);
    ASSERT_EQ_INT(0, Occ_QueryTileStatic(w, tx - 1, ty, 1));
    bf_end();
}

/* Only buildings turn: a walking product ignores any facing asked. */
TEST(nothing_that_walks_turns) {
    ASSERT_NOT_NULL(bf_world());
    ASSERT_EQ_INT(0, Units_DefCanTurn(BF_WALKER));
    ASSERT_EQ_INT(0, Units_DefFacing(BF_WALKER, 1));
    int b = Units_Spawn(BF_BUILDER, 1, 0, BF_CX - 200, BF_CY);
    int made = Units_BeginBuildingForUnitFacing(b, BF_WALKER, BF_CX, BF_CY, 1);
    ASSERT(made >= 0);
    ASSERT_EQ_INT(0, Units_GetFacing(made));
    bf_end();
}

/* The classic view has no camera to turn, so its ghost never turns:
 * the keys do nothing there and turning off puts a turned ghost back
 * to 0. The 3D view turns it. */
TEST(the_classic_view_never_turns_the_ghost) {
    ASSERT_NOT_NULL(bf_world());
    HUD_BeginBuildPlacement(BF_HALL);
    HUD_SetBuildTurning(0);
    ASSERT_EQ_INT(0, HUD_TurnBuild(1));
    HUD_SetBuildFacing(2);
    ASSERT_EQ_INT(0, HUD_GetBuildFacing());
    HUD_SetBuildTurning(1);
    ASSERT_EQ_INT(1, HUD_TurnBuild(1));
    ASSERT_EQ_INT(1, HUD_GetBuildFacing());
    HUD_SetBuildTurning(0);
    ASSERT_EQ_INT(0, HUD_GetBuildFacing());
    HUD_SetBuildTurning(1);
    ASSERT_EQ_INT(0, HUD_GetBuildFacing());
    HUD_ClearCommandMode();
    bf_end();
}

/* ── the order ─────────────────────────────────────────────────────── */

/* The facing rides in the build order's arg, survives the wire, and the
 * tick that runs the order stands the hall turned. */
static uint32_t bf_build_by_order(int facing, int *out_facing) {
    if (!bf_world()) return 0;
    int b = Units_Spawn(BF_BUILDER, 1, 0, BF_CX - 200, BF_CY);
    TAK_GameCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = TAK_CMD_BUILD;
    cmd.seat = 1;
    cmd.target_x = BF_CX;
    cmd.target_y = BF_CY;
    cmd.build_type_id = BF_HALL;
    cmd.arg = (uint16_t)facing;
    cmd.unit_ids[cmd.unit_count++] = Units_GetStableId(b);
    uint8_t wire[64];
    size_t len = 0;
    TAK_GameCommand back;
    size_t used = 0;
    if (TAK_CommandSerialize(&cmd, wire, sizeof(wire), &len) != 0 ||
        TAK_CommandDeserialize(&back, wire, len, &used) != 0) {
        bf_end();
        return 0;
    }
    back.seat = 1;
    TAK_CommandExec_Apply(&back);
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    *out_facing = -1;
    for (int i = 0; i < count; i++)
        if (units[i].alive == UNIT_ALIVE_ACTIVE && units[i].def_idx == BF_HALL)
            *out_facing = units[i].facing;
    for (int t = 0; t < 30; t++) Units_TickEngines();
    uint32_t h = TAK_SimHash();
    bf_end();
    return h;
}

TEST(a_build_order_carries_its_facing_to_the_tick) {
    int got = -1;
    uint32_t turned = bf_build_by_order(1, &got);
    ASSERT_EQ_INT(1, got);
    uint32_t again = bf_build_by_order(1, &got);
    ASSERT_EQ_INT(1, got);
    uint32_t plain = bf_build_by_order(0, &got);
    ASSERT_EQ_INT(0, got);
    /* The same order plays the same everywhere, and the facing is in
     * the hash, so two machines that disagree on it desync visibly. */
    ASSERT(turned != 0 && plain != 0);
    ASSERT_EQ_INT((int)turned, (int)again);
    ASSERT(turned != plain);
}

/* A client whose orders mean something new says so in its hello, and a
 * room of older clients refuses it in the lobby. */
TEST(a_client_that_turns_buildings_is_kept_from_an_older_room) {
    ASSERT(TAK_ENGINE_BUILD_ID >= 2);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("Buildings placed turned");
    RUN(a_quarter_turn_swaps_the_footprint);
    RUN(the_yardmap_turns_clockwise_with_the_building);
    RUN(a_long_hall_is_blocked_on_the_side_it_turns_onto);
    RUN(a_turned_hall_holds_the_cells_it_stands_on);
    RUN(a_turned_hall_is_refused_on_ground_the_map_marks);
    RUN(a_turned_building_faces_the_way_it_turned);
    RUN(a_lodestone_never_turns);
    RUN(the_armed_building_turns_both_ways_and_starts_unturned);
    RUN(a_turned_keeps_wreck_lies_where_it_stood);
    RUN(a_small_wreck_turns_clockwise_with_its_tower);
    RUN(a_raised_keep_stands_the_way_it_fell);
    RUN(a_captured_building_keeps_its_facing);
    RUN(nothing_that_walks_turns);
    RUN(the_classic_view_never_turns_the_ghost);
    RUN(a_build_order_carries_its_facing_to_the_tick);
    RUN(a_client_that_turns_buildings_is_kept_from_an_older_room);
    TEST_REPORT();
}
