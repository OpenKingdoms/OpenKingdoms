/*
 * test_orders.c -- factory build queues, rally points, Shift queued
 * orders and turned gates, with no game data.
 *
 * The harness is test_build_facing's flat world with synthetic defs: a
 * walking builder, a soldier, a barracks that trains it, a gate and a
 * hall. Nothing here has a script, so a barracks trains on its own
 * centre and every product walks out from there.
 */

#include "test_framework.h"
#include "tak_battle_config.h"
#include "tak_command_exec.h"
#include "tak_command_queue.h"
#include "tak_commands.h"
#include "tak_economy.h"
#include "tak_features.h"
#include "tak_hud.h"
#include "tak_ingame.h"
#include "tak_memory.h"
#include "tak_moveinfo.h"
#include "tak_net_protocol.h"
#include "tak_occupancy.h"
#include "tak_pathing.h"
#include "tak_terrain.h"
#include "tak_tnt.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define OQ_TILES  128
#define OQ_GROUND 64

enum { OQ_BUILDER = 0, OQ_SOLDIER, OQ_ARCHER, OQ_BARRACKS, OQ_HELPER, OQ_GATE,
       OQ_HALL, OQ_DEF_COUNT };

static void oq_fill(UnitDef *d, const char *name, float velocity, int fx, int fz) {
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

static GameWorld *oq_world(void) {
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    cfg.line_of_sight = 0;
    cfg.players[0].kind = TAK_SLOT_HUMAN;
    cfg.players[1].kind = TAK_SLOT_HUMAN;
    if (World_BeginLoad(NULL, &cfg, "synthetic", "aramon") != 0) return NULL;
    GameWorld *w = World_Get();
    if (!w) return NULL;
    w->map_pixels_w = OQ_TILES * 16;
    w->map_pixels_h = OQ_TILES * 16;
    w->viewport_w = 640;
    w->viewport_h = 480;
    w->water_height = 0;
    w->tnt.width_tiles = OQ_TILES;
    w->tnt.height_tiles = OQ_TILES;
    w->tnt.height_w = OQ_TILES + 1;
    w->tnt.height_h = OQ_TILES + 1;
    size_t hn = (size_t)w->tnt.height_w * (size_t)w->tnt.height_h;
    w->tnt.heightmap = (uint8_t *)tak_malloc(hn);
    if (!w->tnt.heightmap) return NULL;
    memset(w->tnt.heightmap, OQ_GROUND, hn);
    memset(&w->moveinfo, 0, sizeof(w->moveinfo));
    w->moveinfo.count = 1;
    strncpy(w->moveinfo.classes[0].name, "TESTSMALL", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[0].footprint_x = 1;
    w->moveinfo.classes[0].footprint_z = 1;
    w->moveinfo.classes[0].max_slope = 30;
    if (!Occ_Ensure(w)) return NULL;
    TAK_PathCacheReset();
    World_MarkLoaded();

    UnitDef defs[OQ_DEF_COUNT];
    oq_fill(&defs[OQ_BUILDER], "TESTBUILD", 1.4f, 1, 1);
    defs[OQ_BUILDER].cap_flags |= UNIT_CAP_BUILDER;
    defs[OQ_BUILDER].worker_time = 60.0f;
    defs[OQ_BUILDER].build_distance = 32;
    oq_fill(&defs[OQ_SOLDIER], "TESTSWORD", 1.4f, 1, 1);
    oq_fill(&defs[OQ_ARCHER], "TESTBOW", 1.4f, 1, 1);
    /* Three hit points a tick on a soldier: one every 100 ticks. */
    oq_fill(&defs[OQ_BARRACKS], "TESTBARRACK", 0.0f, 4, 4);
    defs[OQ_BARRACKS].cap_flags |= UNIT_CAP_BUILDER;
    defs[OQ_BARRACKS].worker_time = 60.0f;
    /* Finishes a soldier twenty times faster than a slow barracks. */
    oq_fill(&defs[OQ_HELPER], "TESTHELP", 1.4f, 1, 1);
    defs[OQ_HELPER].cap_flags |= UNIT_CAP_BUILDER;
    defs[OQ_HELPER].worker_time = 1200.0f;
    defs[OQ_HELPER].build_distance = 64;
    oq_fill(&defs[OQ_GATE], "TESTGATE", 0.0f, 14, 4);
    defs[OQ_GATE].is_gate = 1;
    defs[OQ_GATE].onoffable = 1;
    oq_fill(&defs[OQ_HALL], "TESTHALL", 0.0f, 3, 1);
    if (Units_DebugSetDefs(defs, OQ_DEF_COUNT) != OQ_DEF_COUNT) return NULL;
    if (Units_DebugSetYardmap(OQ_BARRACKS, "oooooooooooooooo") != 0) return NULL;
    /* The shipped gates' yard: walls at each end, the doorway between. */
    if (Units_DebugSetYardmap(OQ_GATE, "ooooccccccoooo ooooccccccoooo "
                                       "ooooccccccoooo ooooccccccoooo") != 0)
        return NULL;
    if (Units_DebugSetYardmap(OQ_HALL, "ooo") != 0) return NULL;
    Economy_AdjustCaps(&w->economy, 1, 1000000, 0.0f);
    Economy_Earn(&w->economy, 1, 1000000);
    Units_SetLocalPlayer(1);
    TAK_CmdQueue_Reset(0);
    return w;
}

static void oq_end(void) {
    Units_SelectSingle(-1);
    HUD_ClearCommandMode();
    Units_ClearInstances();
    Features_FreeAll();
    World_End(NULL);
    TAK_PathCacheReset();
    TAK_CmdQueue_Reset(0);
}

static const Unit *oq_unit(int handle) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    return (handle >= 0 && handle < count) ? &units[handle] : NULL;
}

static void oq_ticks(int n) {
    for (int t = 0; t < n; t++) {
        TAK_CmdQueue_Run();
        Units_TickEngines();
    }
}

/* Finished units of def the player has, frames not counted. */
static int oq_count(int def) {
    int count = 0, n = 0;
    const Unit *units = Units_GetActive(&count);
    for (int i = 0; i < count; i++)
        if (units[i].alive == UNIT_ALIVE_ACTIVE && units[i].def_idx == def &&
            !units[i].under_construction && units[i].player_id == 1) n++;
    return n;
}

/* One command from seat 1 for one unit, as a client sends it. */
static int oq_command(int type, int unit, int32_t x, int32_t y, int target,
                      int def, int arg) {
    TAK_GameCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint8_t)type;
    cmd.seat = 1;
    cmd.target_x = x;
    cmd.target_y = y;
    cmd.target_unit_id = target >= 0 ? Units_GetStableId(target) : 0u;
    cmd.build_type_id = (uint16_t)(def < 0 ? 0 : def);
    cmd.arg = (uint16_t)arg;
    cmd.unit_ids[cmd.unit_count++] = Units_GetStableId(unit);
    return TAK_CommandExec_Apply(&cmd);
}

#define OQ_CX (60 * 16)
#define OQ_CY (60 * 16)

static int oq_barracks(void) {
    return Units_Spawn(OQ_BARRACKS, 1, 0, OQ_CX, OQ_CY);
}

/* ── production never stalls ──────────────────────────────────────── */

/* Patrol on a barracks is where its soldiers patrol to (manual section
 * III), and the training goes on. */
TEST(a_barracks_trains_on_after_a_patrol_order) {
    ASSERT_NOT_NULL(oq_world());
    int b = oq_barracks();
    for (int k = 0; k < 3; k++) ASSERT_EQ_INT(0, Units_FactoryEnqueue(b, OQ_SOLDIER));
    oq_ticks(30);
    ASSERT_EQ_INT(1, Units_OrderPatrol(b, OQ_CX + 300, OQ_CY));
    oq_ticks(600);
    ASSERT_EQ_INT(3, oq_count(OQ_SOLDIER));
    oq_end();
}

/* Stop halts what a barracks holds for its soldiers, never its training. */
TEST(a_barracks_trains_on_after_stop) {
    ASSERT_NOT_NULL(oq_world());
    int b = oq_barracks();
    for (int k = 0; k < 3; k++) ASSERT_EQ_INT(0, Units_FactoryEnqueue(b, OQ_SOLDIER));
    oq_ticks(30);
    Units_OrderStop(b);
    oq_ticks(600);
    ASSERT_EQ_INT(3, oq_count(OQ_SOLDIER));
    oq_end();
}

/* A barracks cannot follow anyone, so a guard order leaves it training. */
TEST(a_barracks_trains_on_after_a_guard_order) {
    ASSERT_NOT_NULL(oq_world());
    int b = oq_barracks();
    int friend_ = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX + 200, OQ_CY);
    for (int k = 0; k < 2; k++) ASSERT_EQ_INT(0, Units_FactoryEnqueue(b, OQ_SOLDIER));
    oq_ticks(30);
    Units_OrderGuard(b, friend_);
    oq_ticks(400);
    ASSERT_EQ_INT(3, oq_count(OQ_SOLDIER));
    oq_end();
}

/* At the unit limit a barracks waits with its queue, as the original
 * does, and trains the rest once there is room again. */
TEST(a_barracks_at_the_unit_limit_waits_and_goes_on) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    int b = oq_barracks();
    w->cfg.units_per_side = 3;
    for (int k = 0; k < 3; k++) ASSERT_EQ_INT(0, Units_FactoryEnqueue(b, OQ_SOLDIER));
    oq_ticks(500);
    ASSERT_EQ_INT(2, oq_count(OQ_SOLDIER));
    int first = -1, count = 0;
    const Unit *units = Units_GetActive(&count);
    for (int i = 0; i < count && first < 0; i++)
        if (units[i].def_idx == OQ_SOLDIER && units[i].alive == UNIT_ALIVE_ACTIVE)
            first = i;
    ASSERT(first >= 0);
    Units_SetOwner(first, 2, 1);
    oq_ticks(400);
    ASSERT_EQ_INT(2, oq_count(OQ_SOLDIER));
    ASSERT_EQ_INT(0, Units_FactoryQueueCount(b));
    oq_end();
}

/* ── rally points ─────────────────────────────────────────────────── */

/* A soldier a builder finished still goes to the barracks' rally
 * point, as the original hands every product its factory's orders. */
TEST(a_soldier_another_builder_finished_goes_to_the_rally_point) {
    ASSERT_NOT_NULL(oq_world());
    int b = oq_barracks();
    Units_FactorySetRally(b, OQ_CX + 400, OQ_CY + 200);
    ASSERT_EQ_INT(0, Units_FactoryEnqueue(b, OQ_SOLDIER));
    int frame = oq_unit(b)->build_target;
    ASSERT(frame >= 0);
    int helper = Units_Spawn(OQ_HELPER, 1, 0, OQ_CX + 64, OQ_CY);
    ASSERT_EQ_INT(1, Units_OrderRepair(helper, frame));
    for (int t = 0; t < 200 && oq_unit(frame)->under_construction; t++) oq_ticks(1);
    ASSERT_EQ_INT(0, (int)oq_unit(frame)->under_construction);
    oq_ticks(2);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)oq_unit(frame)->cmd_kind);
    ASSERT_EQ_INT(OQ_CX + 400, oq_unit(frame)->cmd_x);
    ASSERT_EQ_INT(OQ_CY + 200, oq_unit(frame)->cmd_y);
    oq_end();
}

/* The rally takes a patrol too, and Shift adds standing orders the
 * soldiers carry on with. */
TEST(a_soldier_carries_the_barracks_standing_orders) {
    ASSERT_NOT_NULL(oq_world());
    int b = oq_barracks();
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, b, OQ_CX + 200, OQ_CY, -1, -1, 0));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_PATROL, b, OQ_CX + 200, OQ_CY + 200, -1, -1,
                                TAK_CMD_ARG_QUEUE));
    UnitOrderView v[4];
    ASSERT_EQ_INT(2, Units_OrdersOf(b, v, 4));
    ASSERT_EQ_INT(UNIT_LEG_MOVE, v[0].kind);
    ASSERT_EQ_INT(1, v[0].rally);
    ASSERT_EQ_INT(UNIT_LEG_PATROL, v[1].kind);
    ASSERT_EQ_INT(OQ_CY + 200, v[1].y);
    ASSERT_EQ_INT(0, Units_FactoryEnqueue(b, OQ_SOLDIER));
    int s = oq_unit(b)->build_target;
    for (int t = 0; t < 200 && oq_unit(s)->under_construction; t++) oq_ticks(1);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)oq_unit(s)->cmd_kind);
    ASSERT_EQ_INT(1, (int)oq_unit(s)->leg_count);
    ASSERT_EQ_INT(UNIT_LEG_PATROL, oq_unit(s)->legs[0].kind);
    for (int t = 0; t < 900 && oq_unit(s)->cmd_kind != UNIT_CMD_PATROL; t++) oq_ticks(1);
    ASSERT_EQ_INT(UNIT_CMD_PATROL, (int)oq_unit(s)->cmd_kind);
    /* Stop drops them again and leaves the queue be. */
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_STOP, b, 0, 0, -1, -1, 0));
    ASSERT_EQ_INT(0, Units_OrdersOf(b, v, 4));
    oq_end();
}

/* ── the build buttons ────────────────────────────────────────────── */

/* Shift adds five and takes five off, and a click the last queued one
 * first and the one in training last (legacy:181826-181862). */
TEST(shift_adds_five_and_a_right_click_takes_the_last_first) {
    ASSERT_NOT_NULL(oq_world());
    int b = oq_barracks();
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_ENQUEUE, b, 0, 0, -1, OQ_SOLDIER, 5));
    ASSERT_EQ_INT(5, Units_FactoryQueuedCountForDef(b, OQ_SOLDIER));
    ASSERT_EQ_INT(4, Units_FactoryQueueCount(b));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_ENQUEUE, b, 0, 0, -1, OQ_ARCHER, 1));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_ENQUEUE, b, 0, 0, -1, OQ_SOLDIER, 0));
    ASSERT_EQ_INT(6, Units_FactoryQueuedCountForDef(b, OQ_SOLDIER));
    ASSERT_EQ_INT(3, (int)oq_unit(b)->prod_queue_len);
    /* Five off: the one at the back, then four of the run before it. */
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_DEQUEUE, b, 0, 0, -1, OQ_SOLDIER, 5));
    ASSERT_EQ_INT(1, Units_FactoryQueuedCountForDef(b, OQ_SOLDIER));
    ASSERT_EQ_INT(1, Units_FactoryQueuedCountForDef(b, OQ_ARCHER));
    int frame = oq_unit(b)->build_target;
    ASSERT_EQ_INT(OQ_SOLDIER, (int)oq_unit(frame)->def_idx);
    /* The last soldier is the one in training, and the archer starts. */
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_DEQUEUE, b, 0, 0, -1, OQ_SOLDIER, 1));
    ASSERT_EQ_INT(0, Units_FactoryQueuedCountForDef(b, OQ_SOLDIER));
    ASSERT_EQ_INT(OQ_ARCHER, (int)oq_unit(oq_unit(b)->build_target)->def_idx);
    ASSERT_EQ_INT(0, Units_FactoryQueueCount(b));
    oq_end();
}

/* Ctrl trains one def without end, which nothing can go behind, and a
 * click takes every one of it off (manual section IV). */
TEST(ctrl_trains_without_end_until_a_click_cancels_it) {
    ASSERT_NOT_NULL(oq_world());
    int b = oq_barracks();
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_ENQUEUE, b, 0, 0, -1, OQ_ARCHER, 1));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_ENQUEUE, b, 0, 0, -1, OQ_SOLDIER,
                                TAK_FACTORY_ALL));
    ASSERT_EQ_INT(OQ_SOLDIER, Units_FactoryRepeatOf(b));
    char label[8];
    ASSERT_EQ_INT(1, HUD_QueueBadgeText(b, OQ_SOLDIER, label, sizeof label));
    ASSERT_EQ_STR("+++", label);
    ASSERT_EQ_INT(1, HUD_QueueBadgeText(b, OQ_ARCHER, label, sizeof label));
    ASSERT_EQ_STR("1", label);
    ASSERT_EQ_INT(0, oq_command(TAK_CMD_FACTORY_ENQUEUE, b, 0, 0, -1, OQ_ARCHER, 1));
    oq_ticks(620);
    ASSERT_EQ_INT(1, oq_count(OQ_ARCHER));
    ASSERT(oq_count(OQ_SOLDIER) >= 4);
    ASSERT_EQ_INT(OQ_SOLDIER, Units_FactoryRepeatOf(b));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_DEQUEUE, b, 0, 0, -1, OQ_SOLDIER, 1));
    ASSERT_EQ_INT(-1, Units_FactoryRepeatOf(b));
    ASSERT_EQ_INT(0, Units_FactoryQueuedCountForDef(b, OQ_SOLDIER));
    ASSERT_EQ_INT(0, HUD_QueueBadgeText(b, OQ_SOLDIER, label, sizeof label));
    oq_end();
}

/* The held keys pick the count a build button sends. */
TEST(the_held_keys_choose_how_many_a_build_button_sends) {
    SDL_SetModState(KMOD_NONE);
    ASSERT_EQ_INT(1, (int)HUD_BuildCountArg());
    SDL_SetModState(KMOD_LSHIFT);
    ASSERT_EQ_INT(5, (int)HUD_BuildCountArg());
    SDL_SetModState(KMOD_LCTRL);
    ASSERT_EQ_INT((int)TAK_FACTORY_ALL, (int)HUD_BuildCountArg());
    SDL_SetModState((SDL_Keymod)(KMOD_LCTRL | KMOD_LSHIFT));
    ASSERT_EQ_INT((int)TAK_FACTORY_ALL, (int)HUD_BuildCountArg());
    SDL_SetModState(KMOD_NONE);
}

/* ── a barracks still being built ─────────────────────────────────── */

/* The original never lets a frame take an order. With the remaster's
 * allowance a barracks still going up takes a queue and trains it once
 * it is finished, and every other order is still refused. */
TEST(an_unfinished_barracks_holds_a_queue_only_when_allowed) {
    ASSERT_NOT_NULL(oq_world());
    int builder = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 120, OQ_CY);
    int b = Units_BeginBuildingForUnit(builder, OQ_BARRACKS, OQ_CX, OQ_CY);
    ASSERT(b >= 0);
    ASSERT_EQ_INT(1, (int)oq_unit(b)->under_construction);
    ASSERT_EQ_INT(0, oq_command(TAK_CMD_FACTORY_ENQUEUE, b, 0, 0, -1, OQ_SOLDIER, 1));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_ENQUEUE, b, 0, 0, -1, OQ_SOLDIER,
                                2 | TAK_FACTORY_UNFINISHED));
    ASSERT_EQ_INT(2, Units_FactoryQueueCount(b));
    ASSERT_EQ_INT(0, oq_command(TAK_CMD_MOVE, b, OQ_CX + 300, OQ_CY, -1, -1, 0));
    oq_command(TAK_CMD_RALLY, b, OQ_CX + 300, OQ_CY, -1, -1, 0);
    ASSERT_EQ_INT(0, (int)oq_unit(b)->rally_set);
    ASSERT_EQ_INT(0, oq_command(TAK_CMD_STOP, b, 0, 0, -1, -1, 0));
    for (int t = 0; t < 900 && oq_unit(b)->under_construction; t++) oq_ticks(1);
    ASSERT_EQ_INT(0, (int)oq_unit(b)->under_construction);
    ASSERT_EQ_INT(0, oq_count(OQ_SOLDIER));
    oq_ticks(300);
    ASSERT_EQ_INT(2, oq_count(OQ_SOLDIER));
    oq_end();
}

/* ── Shift queued orders ──────────────────────────────────────────── */

/* A move with Shift waits behind the one in hand, and the read of a
 * unit's orders lists both. */
TEST(a_shift_move_waits_behind_the_move_in_hand) {
    ASSERT_NOT_NULL(oq_world());
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, s, OQ_CX + 160, OQ_CY, -1, -1, 0));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, s, OQ_CX + 160, OQ_CY + 160, -1, -1,
                                TAK_CMD_ARG_QUEUE));
    UnitOrderView v[4];
    ASSERT_EQ_INT(2, Units_OrdersOf(s, v, 4));
    ASSERT_EQ_INT(0, v[0].queued);
    ASSERT_EQ_INT(1, v[1].queued);
    ASSERT_EQ_INT(OQ_CY + 160, v[1].y);
    for (int t = 0; t < 900 && Units_OrdersOf(s, NULL, 0) > 0; t++) oq_ticks(1);
    ASSERT_EQ_INT(0, Units_OrdersOf(s, NULL, 0));
    ASSERT(oq_unit(s)->world_y > OQ_CY + 140);
    /* Without Shift an order replaces the queue. */
    oq_command(TAK_CMD_MOVE, s, OQ_CX, OQ_CY, -1, -1, 0);
    oq_command(TAK_CMD_MOVE, s, OQ_CX + 64, OQ_CY, -1, -1, TAK_CMD_ARG_QUEUE);
    oq_command(TAK_CMD_MOVE, s, OQ_CX + 32, OQ_CY + 32, -1, -1, 0);
    ASSERT_EQ_INT(1, Units_OrdersOf(s, NULL, 0));
    oq_end();
}

/* A builder given two buildings with Shift puts up the second once the
 * first is done, and its frame appears only then. */
TEST(a_shift_build_starts_when_the_one_before_is_done) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 8, -1, OQ_HALL, 0));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 72, -1, OQ_HALL,
                                1 | TAK_CMD_ARG_QUEUE));
    UnitOrderView v[4];
    ASSERT_EQ_INT(2, Units_OrdersOf(bd, v, 4));
    ASSERT_EQ_INT(UNIT_LEG_BUILD, v[1].kind);
    ASSERT_EQ_INT(OQ_HALL, v[1].def);
    ASSERT_EQ_INT(1, v[1].facing);
    ASSERT_EQ_INT(0, oq_count(OQ_HALL));
    int count = 0, frames = 0;
    const Unit *units = Units_GetActive(&count);
    for (int i = 0; i < count; i++) frames += units[i].def_idx == OQ_HALL;
    ASSERT_EQ_INT(1, frames);
    oq_ticks(1200);
    ASSERT_EQ_INT(2, oq_count(OQ_HALL));
    oq_end();
}

/* Patrol points given with Shift make one route, walked round and round
 * through the point it started from. */
TEST(shift_patrol_points_make_one_route) {
    ASSERT_NOT_NULL(oq_world());
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_PATROL, s, OQ_CX + 160, OQ_CY, -1, -1, 0));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_PATROL, s, OQ_CX + 160, OQ_CY + 160, -1, -1,
                                TAK_CMD_ARG_QUEUE));
    UnitOrderView v[4];
    ASSERT_EQ_INT(3, Units_OrdersOf(s, v, 4));
    ASSERT_EQ_INT(OQ_CY + 160, v[1].y);
    ASSERT_EQ_INT(1, v[2].back);
    ASSERT_EQ_INT(OQ_CX, v[2].x);
    /* Each corner in turn: out east, then south, then home. */
    int seen_south = 0, seen_home = 0;
    for (int t = 0; t < 2400 && !seen_home; t++) {
        oq_ticks(1);
        const Unit *u = oq_unit(s);
        if (u->cmd_x == OQ_CX + 160 && u->cmd_y == OQ_CY + 160) seen_south = 1;
        if (seen_south && u->cmd_x == OQ_CX && u->cmd_y == OQ_CY) seen_home = 1;
    }
    ASSERT_EQ_INT(1, seen_home);
    ASSERT_EQ_INT(UNIT_CMD_PATROL, (int)oq_unit(s)->cmd_kind);
    ASSERT_EQ_INT(3, Units_OrdersOf(s, NULL, 0));
    oq_end();
}

/* A click with Shift held queues and keeps the order armed for the next
 * one, as the original does (legacy:243648). */
TEST(a_shift_click_queues_and_keeps_the_order_armed) {
    ASSERT_NOT_NULL(oq_world());
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    Units_SelectSingle(s);
    HUD_SetCommandMode(HUD_CMD_PATROL);
    InGame_WorldClickOn(OQ_CX + 160, OQ_CY, -1, 1);
    ASSERT_EQ_INT(HUD_CMD_PATROL, HUD_GetCommandMode());
    InGame_WorldClickOn(OQ_CX + 160, OQ_CY + 160, -1, 1);
    oq_ticks(1);
    ASSERT_EQ_INT(3, Units_OrdersOf(s, NULL, 0));
    InGame_WorldClickOn(OQ_CX - 160, OQ_CY, -1, 0);
    ASSERT_EQ_INT(HUD_CMD_NONE, HUD_GetCommandMode());
    oq_ticks(1);
    ASSERT_EQ_INT(2, Units_OrdersOf(s, NULL, 0));
    /* A plain ground click with Shift is a queued move. */
    InGame_WorldClickOn(OQ_CX, OQ_CY + 300, -1, 1);
    oq_ticks(1);
    UnitOrderView v[4];
    ASSERT_EQ_INT(3, Units_OrdersOf(s, v, 4));
    oq_end();
}

/* A click the view has already judged open ground is never taken for
 * the unit standing beside it. */
TEST(a_click_on_open_ground_beside_a_unit_moves) {
    ASSERT_NOT_NULL(oq_world());
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    int other = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX + 200, OQ_CY);
    Units_SelectSingle(s);
    InGame_WorldClickOn(OQ_CX + 230, OQ_CY, -1, 0);
    oq_ticks(1);
    int n = 0;
    const int *sel = Units_GetSelection(&n);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(s, sel[0]);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)oq_unit(s)->cmd_kind);
    (void)other;
    oq_end();
}

/* ── gates ────────────────────────────────────────────────────────── */

/* A gate turns like any building, and its doorway turns with it: turned
 * a quarter, the cells that open run north to south. */
TEST(a_gate_turns_and_its_doorway_turns_with_it) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    ASSERT_EQ_INT(1, Units_DefCanTurn(OQ_GATE));
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 200, OQ_CY);
    int32_t x = OQ_CX, y = OQ_CY;
    Units_SnapBuildSiteFacing(OQ_GATE, 1, &x, &y);
    ASSERT_EQ_INT(1, Units_IsBuildSiteClearFacing(OQ_GATE, x, y, 1));
    int g = Units_BeginBuildingForUnitFacing(bd, OQ_GATE, x, y, 1);
    ASSERT(g >= 0);
    ASSERT_EQ_INT(1, Units_GetFacing(g));
    for (int t = 0; t < 3000 && oq_unit(g)->under_construction; t++) oq_ticks(1);
    ASSERT_EQ_INT(0, (int)oq_unit(g)->under_construction);
    oq_ticks(40);
    /* Four wide and fourteen long: the doorway is rows four to nine. */
    int tx0 = Occ_TileOf(x - 2 * 16), ty0 = Occ_TileOf(y - 7 * 16);
    for (int col = 0; col < 4; col++) {
        ASSERT_EQ_INT(0, Occ_IsGateTile(w, tx0 + col, ty0 + 2));
        ASSERT_EQ_INT(1, Occ_IsGateTile(w, tx0 + col, ty0 + 6));
        ASSERT_EQ_INT(0, Occ_IsGateTile(w, tx0 + col, ty0 + 11));
    }
    oq_end();
}

/* A client whose orders mean something new says so in its hello. */
TEST(a_client_that_queues_orders_is_kept_from_an_older_room) {
    ASSERT(TAK_ENGINE_BUILD_ID >= 9);
}

/* ── review follow-ups ────────────────────────────────────────────── */

/* A frame is never selected, as in the original, so a click on one is a
 * click on the ground: with a barracks selected it moves the rally. */
TEST(a_frame_is_never_selected_and_a_click_on_one_is_ground) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 300, OQ_CY);
    int frame = Units_BeginBuildingForUnit(bd, OQ_BARRACKS, OQ_CX, OQ_CY);
    ASSERT(frame >= 0);
    int n = 0;
    Units_SelectSingle(frame);
    Units_GetSelection(&n);
    ASSERT_EQ_INT(0, n);
    Units_SelectAdd(frame);
    Units_GetSelection(&n);
    ASSERT_EQ_INT(0, n);
    ASSERT_EQ_INT(0, Units_IsSelectable(frame));
    int b = Units_Spawn(OQ_BARRACKS, 1, 0, OQ_CX + 400, OQ_CY);
    Units_SelectSingle(b);
    InGame_WorldClickOn(OQ_CX, OQ_CY, frame, 0);
    oq_ticks(1);
    const int *sel = Units_GetSelection(&n);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(b, sel[0]);
    ASSERT_EQ_INT(UNIT_RALLY_MOVE, (int)oq_unit(b)->rally_set);
    oq_end();
}

/* A click beside a soldier, off its box, is a click on the ground: the
 * original picks a unit by its drawn box and nothing near it. */
TEST(a_click_beside_a_soldier_moves_the_rally) {
    ASSERT_NOT_NULL(oq_world());
    int b = oq_barracks();
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX + 200, OQ_CY + 200);
    ASSERT(s >= 0);
    Units_SelectSingle(b);
    /* Where the soldier is drawn, lifted by half the ground's height. */
    int32_t sy = OQ_CY + 200 -
                 (int32_t)((float)Terrain_SampleHeight(World_Get(), OQ_CX + 200, OQ_CY + 200) *
                           Units_GetTanTilt());
    InGame_WorldClick(OQ_CX + 230, sy, 0);
    oq_ticks(1);
    int n = 0;
    const int *sel = Units_GetSelection(&n);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(b, sel[0]);
    ASSERT_EQ_INT(UNIT_RALLY_MOVE, (int)oq_unit(b)->rally_set);
    /* On the soldier itself the click still selects it. */
    InGame_WorldClick(OQ_CX + 200, sy, 0);
    sel = Units_GetSelection(&n);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(s, sel[0]);
    oq_end();
}

/* A barracks trains units. A building on its queue could never start
 * and would hold up everything behind it. */
TEST(a_barracks_refuses_a_building_on_its_queue) {
    ASSERT_NOT_NULL(oq_world());
    int b = oq_barracks();
    ASSERT_EQ_INT(-1, Units_FactoryAdd(b, OQ_HALL, 1, 0));
    ASSERT_EQ_INT(0, Units_FactoryQueuedCountForDef(b, OQ_HALL));
    oq_end();
}

/* A soldier lost on the pad is not trained again: the original counts
 * a run down when its unit is done or its frame is lost
 * (legacy:9293-9303, 9524-9527). */
TEST(a_soldier_lost_on_the_pad_is_not_trained_again) {
    ASSERT_NOT_NULL(oq_world());
    int b = oq_barracks();
    ASSERT_EQ_INT(0, Units_FactoryAdd(b, OQ_SOLDIER, 3, 0));
    oq_ticks(20);
    int frame = oq_unit(b)->build_target;
    ASSERT(frame >= 0);
    Units_SetOwner(frame, 2, 1);
    oq_ticks(600);
    ASSERT_EQ_INT(2, oq_count(OQ_SOLDIER));
    ASSERT_EQ_INT(0, Units_FactoryQueueCount(b));
    oq_end();
}

/* A queued order carries a def only when it is a building, so what a
 * save keeps is what the hash counts. */
TEST(a_queued_move_carries_no_def) {
    ASSERT_NOT_NULL(oq_world());
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    oq_command(TAK_CMD_MOVE, s, OQ_CX + 200, OQ_CY, -1, -1, 0);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, s, OQ_CX, OQ_CY + 200, -1, OQ_HALL,
                                TAK_CMD_ARG_QUEUE | 3));
    ASSERT_EQ_INT(1, (int)oq_unit(s)->leg_count);
    ASSERT_EQ_INT(0, oq_unit(s)->legs[0].def);
    ASSERT_EQ_INT(0, oq_unit(s)->legs[0].facing);
    oq_end();
}

/* Ctrl changes the order in hand and keeps the queue behind it (manual
 * section IV). */
TEST(ctrl_changes_the_order_in_hand_and_keeps_the_queue) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 8, -1, OQ_HALL, 0));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 72, -1, OQ_HALL,
                                TAK_CMD_ARG_QUEUE));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, bd, OQ_CX - 200, OQ_CY, -1, -1,
                                TAK_CMD_ARG_KEEP));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(1, (int)oq_unit(bd)->leg_count);
    ASSERT_EQ_INT(UNIT_LEG_BUILD, oq_unit(bd)->legs[0].kind);
    /* Without Ctrl the queue goes with it. */
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, bd, OQ_CX - 100, OQ_CY, -1, -1, 0));
    ASSERT_EQ_INT(0, (int)oq_unit(bd)->leg_count);
    oq_end();
}

/* A frame taken off by a cancel lifts its cells with it, so the site is
 * free again at once. */
TEST(a_cancelled_frame_frees_its_site) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 8, -1, OQ_HALL, 0));
    int tx = (OQ_CX + 8) / 16, ty = (OQ_CY + 8) / 16;
    ASSERT(Occ_QueryTileStatic(World_Get(), tx, ty, 2) != 0);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_CANCEL, bd, 0, 0, -1, -1, 0));
    ASSERT_EQ_INT(0, Occ_QueryTileStatic(World_Get(), tx, ty, 2));
    ASSERT_EQ_INT(1, Units_IsBuildSiteClear(OQ_HALL, OQ_CX + 8, OQ_CY + 8));
    oq_end();
}

/* A move queued behind patrol points stays behind them, and the route
 * still comes back through the point it started from. */
TEST(a_move_queued_after_a_patrol_keeps_the_route_whole) {
    ASSERT_NOT_NULL(oq_world());
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    oq_command(TAK_CMD_PATROL, s, OQ_CX + 160, OQ_CY, -1, -1, 0);
    oq_command(TAK_CMD_PATROL, s, OQ_CX + 160, OQ_CY + 160, -1, -1, TAK_CMD_ARG_QUEUE);
    oq_command(TAK_CMD_MOVE, s, OQ_CX - 300, OQ_CY, -1, -1, TAK_CMD_ARG_QUEUE);
    int seen_south = 0, seen_home = 0;
    for (int t = 0; t < 2400 && !seen_home; t++) {
        oq_ticks(1);
        const Unit *u = oq_unit(s);
        if (u->cmd_x == OQ_CX + 160 && u->cmd_y == OQ_CY + 160) seen_south = 1;
        if (seen_south && u->cmd_x == OQ_CX && u->cmd_y == OQ_CY) seen_home = 1;
    }
    ASSERT_EQ_INT(1, seen_home);
    const Unit *u = oq_unit(s);
    ASSERT_EQ_INT(UNIT_LEG_MOVE, u->legs[u->leg_count - 1].kind);
    ASSERT_EQ_INT(OQ_CX - 300, u->legs[u->leg_count - 1].x);
    oq_end();
}

/* Shift on your own frame with a builder selected queues the help. */
TEST(a_shift_click_on_a_frame_queues_the_help) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 300, OQ_CY);
    int other = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 200, OQ_CY + 100);
    int frame = Units_BeginBuildingForUnit(other, OQ_HALL, OQ_CX + 8, OQ_CY + 8);
    ASSERT(frame >= 0);
    oq_command(TAK_CMD_MOVE, bd, OQ_CX - 500, OQ_CY, -1, -1, 0);
    Units_SelectSingle(bd);
    InGame_WorldClickOn(OQ_CX + 8, OQ_CY + 8, frame, IG_CLICK_SHIFT);
    oq_ticks(1);
    ASSERT_EQ_INT(1, (int)oq_unit(bd)->leg_count);
    ASSERT_EQ_INT(UNIT_LEG_REPAIR, oq_unit(bd)->legs[0].kind);
    oq_end();
}

/* ── a builder walks to its site as a move does ────────────────────── */

/* A ridge too steep to climb, x cells 60 to 63 and y cells 30 to 90,
 * between x cell 40 and x cell 84. */
static void oq_ridge(GameWorld *w) {
    for (int y = 30; y <= 90; y++)
        for (int x = 60; x <= 63; x++)
            w->tnt.heightmap[y * w->tnt.height_w + x] = 250;
    TAK_PathCacheReset();
}

#define OQ_SITE_X (84 * 16)
#define OQ_SITE_Y (60 * 16)

/* Ticks until h stands within r px of the site, or -1. */
static int oq_ticks_to_site(int h, int r, int max) {
    for (int t = 0; t < max; t++) {
        oq_ticks(1);
        const Unit *u = oq_unit(h);
        int64_t dx = u->world_x - OQ_SITE_X, dy = u->world_y - OQ_SITE_Y;
        if (dx * dx + dy * dy <= (int64_t)r * r) return t + 1;
    }
    return -1;
}

/* Sent to build behind a ridge, a builder takes the route round it and
 * gets there about as soon as a plain move to the same spot. The walker
 * steers by its route, and the builder turns to the site only once it
 * is there. */
TEST(a_builder_goes_round_a_ridge_as_a_move_does) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    oq_ridge(w);
    int mover = Units_Spawn(OQ_BUILDER, 1, 0, 40 * 16, OQ_SITE_Y);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, mover, OQ_SITE_X, OQ_SITE_Y, -1, -1, 0));
    /* The hall's half diagonal and a margin, then the build distance. */
    int reach = 37 + 32;
    int move_ticks = oq_ticks_to_site(mover, reach, 5400);
    oq_end();

    w = oq_world();
    ASSERT_NOT_NULL(w);
    oq_ridge(w);
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, 40 * 16, OQ_SITE_Y);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_SITE_X, OQ_SITE_Y, -1, OQ_HALL, 0));
    int build_ticks = oq_ticks_to_site(bd, reach, 5400);
    int kind = oq_unit(bd)->cmd_kind;
    oq_end();
    printf("[move %d ticks, build %d ticks] ", move_ticks, build_ticks);
    ASSERT(move_ticks > 0);
    ASSERT(build_ticks > 0);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, kind);
    ASSERT(build_ticks <= move_ticks + move_ticks / 4);
}

/* A repair behind the ridge is walked the same way. */
TEST(a_repairer_goes_round_a_ridge_as_a_move_does) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    oq_ridge(w);
    int mover = Units_Spawn(OQ_BUILDER, 1, 0, 40 * 16, OQ_SITE_Y);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, mover, OQ_SITE_X, OQ_SITE_Y, -1, -1, 0));
    int reach = 37 + 32;
    int move_ticks = oq_ticks_to_site(mover, reach, 5400);
    oq_end();

    w = oq_world();
    ASSERT_NOT_NULL(w);
    oq_ridge(w);
    int hall = Units_Spawn(OQ_HALL, 1, 0, OQ_SITE_X, OQ_SITE_Y);
    ASSERT(hall >= 0);
    ((Unit *)oq_unit(hall))->health = 100;
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, 40 * 16, OQ_SITE_Y);
    ASSERT_EQ_INT(1, Units_OrderRepair(bd, hall));
    int repair_ticks = oq_ticks_to_site(bd, reach, 5400);
    int kind = oq_unit(bd)->cmd_kind;
    oq_end();
    printf("[move %d ticks, repair %d ticks] ", move_ticks, repair_ticks);
    ASSERT(repair_ticks > 0);
    ASSERT_EQ_INT(UNIT_CMD_REPAIR, kind);
    ASSERT(repair_ticks <= move_ticks + move_ticks / 4);
}

/* A pocket open to the west only, x cells 20 to 63 and y cells 40 to
 * 80, with the builder in its east end and the site east of it. */
static void oq_pocket(GameWorld *w) {
    for (int y = 40; y <= 80; y++)
        for (int x = 20; x <= 63; x++) {
            int wall = x >= 60 || y <= 43 || y >= 77;
            if (wall) w->tnt.heightmap[y * w->tnt.height_w + x] = 250;
        }
    TAK_PathCacheReset();
}

/* The way out of the pocket leads away from the site for longer than a
 * frame's ten seconds of grace, and the builder walking it still holds
 * its frame: it is closing along its route. */
TEST(a_builder_walking_out_of_a_pocket_keeps_its_frame) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    oq_pocket(w);
    int mover = Units_Spawn(OQ_BUILDER, 1, 0, 55 * 16, OQ_SITE_Y);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, mover, OQ_SITE_X, OQ_SITE_Y, -1, -1, 0));
    int reach = 37 + 32;
    int move_ticks = oq_ticks_to_site(mover, reach, 9000);
    oq_end();

    w = oq_world();
    ASSERT_NOT_NULL(w);
    oq_pocket(w);
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, 55 * 16, OQ_SITE_Y);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_SITE_X, OQ_SITE_Y, -1, OQ_HALL, 0));
    int build_ticks = oq_ticks_to_site(bd, reach, 9000);
    int kind = oq_unit(bd)->cmd_kind;
    oq_end();
    printf("[move %d ticks, build %d ticks] ", move_ticks, build_ticks);
    ASSERT(move_ticks > 600);
    ASSERT(build_ticks > 0);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, kind);
    ASSERT(build_ticks <= move_ticks + move_ticks / 4);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    SDL_Init(0);
    TEST_SUITE("Build queues, rally points and queued orders");
    RUN(a_barracks_trains_on_after_a_patrol_order);
    RUN(a_barracks_trains_on_after_stop);
    RUN(a_barracks_trains_on_after_a_guard_order);
    RUN(a_barracks_at_the_unit_limit_waits_and_goes_on);
    RUN(a_soldier_another_builder_finished_goes_to_the_rally_point);
    RUN(a_soldier_carries_the_barracks_standing_orders);
    RUN(shift_adds_five_and_a_right_click_takes_the_last_first);
    RUN(ctrl_trains_without_end_until_a_click_cancels_it);
    RUN(the_held_keys_choose_how_many_a_build_button_sends);
    RUN(an_unfinished_barracks_holds_a_queue_only_when_allowed);
    RUN(a_shift_move_waits_behind_the_move_in_hand);
    RUN(a_shift_build_starts_when_the_one_before_is_done);
    RUN(shift_patrol_points_make_one_route);
    RUN(a_shift_click_queues_and_keeps_the_order_armed);
    RUN(a_click_on_open_ground_beside_a_unit_moves);
    RUN(a_gate_turns_and_its_doorway_turns_with_it);
    RUN(a_client_that_queues_orders_is_kept_from_an_older_room);
    RUN(a_frame_is_never_selected_and_a_click_on_one_is_ground);
    RUN(a_click_beside_a_soldier_moves_the_rally);
    RUN(a_barracks_refuses_a_building_on_its_queue);
    RUN(a_soldier_lost_on_the_pad_is_not_trained_again);
    RUN(a_queued_move_carries_no_def);
    RUN(ctrl_changes_the_order_in_hand_and_keeps_the_queue);
    RUN(a_cancelled_frame_frees_its_site);
    RUN(a_move_queued_after_a_patrol_keeps_the_route_whole);
    RUN(a_shift_click_on_a_frame_queues_the_help);
    RUN(a_builder_goes_round_a_ridge_as_a_move_does);
    RUN(a_repairer_goes_round_a_ridge_as_a_move_does);
    RUN(a_builder_walking_out_of_a_pocket_keeps_its_frame);
    TEST_REPORT();
}
