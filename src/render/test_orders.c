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
#include "tak_ai.h"
#include "tak_battle_config.h"
#include "tak_command_exec.h"
#include "tak_command_queue.h"
#include "tak_commands.h"
#include "tak_economy.h"
#include "tak_features.h"
#include "tak_fog.h"
#include "tak_hud.h"
#include "tak_cob.h"
#include "tak_ingame.h"
#include "tak_memory.h"
#include "tak_moveinfo.h"
#include "tak_net_protocol.h"
#include "tak_occupancy.h"
#include "tak_order_overlay.h"
#include "tak_platform.h"
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
       OQ_HALL, OQ_BOW, OQ_CANNON, OQ_FLYER, OQ_LIMITED, OQ_DEF_COUNT };

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
    /* Armed: a bow that takes flyers and a cannon that cannot. */
    oq_fill(&defs[OQ_BOW], "TESTLONGBOW", 1.4f, 1, 1);
    defs[OQ_BOW].num_weapons = 1;
    defs[OQ_BOW].weapons[0].range = 400;
    defs[OQ_BOW].weapons[0].damage = 10;
    oq_fill(&defs[OQ_CANNON], "TESTCANNON", 1.4f, 1, 1);
    defs[OQ_CANNON].num_weapons = 1;
    defs[OQ_CANNON].weapons[0].range = 400;
    defs[OQ_CANNON].weapons[0].damage = 10;
    defs[OQ_CANNON].weapons[0].no_air_weapon = 1;
    oq_fill(&defs[OQ_FLYER], "TESTFLYER", 2.0f, 1, 1);
    defs[OQ_FLYER].can_fly = 1;
    defs[OQ_FLYER].cruise_alt = 120;
    /* A builder held to its own list, as every builder but a monarch is. */
    oq_fill(&defs[OQ_LIMITED], "TESTMAGE", 1.4f, 1, 1);
    defs[OQ_LIMITED].cap_flags |= UNIT_CAP_BUILDER;
    defs[OQ_LIMITED].worker_time = 60.0f;
    defs[OQ_LIMITED].build_distance = 32;
    defs[OQ_LIMITED].builder_limited = 1;
    /* FBI category lines, for the select keys. */
    strcpy(defs[OQ_BUILDER].category, "TEST BUILDER");
    strcpy(defs[OQ_HELPER].category, "TEST BUILDER");
    strcpy(defs[OQ_SOLDIER].category, "TEST MELEE ATTACK");
    strcpy(defs[OQ_BOW].category, "TEST ATTACK BALLISTIC");
    strcpy(defs[OQ_BARRACKS].category, "TEST FACTORY");
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

/* ── several builders on one frame ────────────────────────────────── */

/* Ticks a hall frame takes with n builders of workertime 60 at it from
 * the start, and the mana it took. */
static int oq_build_hall_with(int n, double *spent) {
    GameWorld *w = oq_world();
    if (!w) return -1;
    int b0 = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 50, OQ_CY);
    int b1 = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX + 50, OQ_CY);
    int frame = Units_BeginBuildingForUnit(b0, OQ_HALL, OQ_CX, OQ_CY);
    if (frame < 0) { oq_end(); return -1; }
    if (n > 1 && Units_OrderRepair(b1, frame) != 1) { oq_end(); return -1; }
    double was = w->economy.players[0].spent_total;
    int t = 0;
    while (t < 2000 && oq_unit(frame)->under_construction) { oq_ticks(1); t++; }
    *spent = w->economy.players[0].spent_total - was;
    oq_end();
    return t;
}

/* Each builder adds its own workertime, so two finish in half the time,
 * and the frame costs its buildcost however many work on it: the drain
 * doubles and the total stays (legacy:39451-39505). */
TEST(two_builders_finish_a_frame_in_half_the_time_for_the_same_mana) {
    double one_spent = 0.0, two_spent = 0.0;
    int one = oq_build_hall_with(1, &one_spent);
    int two = oq_build_hall_with(2, &two_spent);
    printf("(one builder %d ticks %.1f mana, two %d ticks %.1f mana) ",
           one, one_spent, two, two_spent);
    ASSERT(one > 0 && one < 2000);
    ASSERT(two > 0);
    ASSERT(two * 2 >= one - 2 && two * 2 <= one + 2);
    ASSERT(one_spent >= 98.0 && one_spent <= 102.0);
    ASSERT(two_spent >= one_spent - 2.0 && two_spent <= one_spent + 2.0);
}

/* A monarch is not limited to its list, so it helps a frame it could not
 * start, a soldier in a barracks among them (legacy:233569-233572). */
TEST(a_monarch_helps_a_frame_it_could_not_start) {
    ASSERT_NOT_NULL(oq_world());
    const int hall = OQ_HALL;
    Units_DebugSetBuildables(OQ_BUILDER, &hall, 1);
    int b = oq_barracks();
    ASSERT_EQ_INT(0, Units_FactoryEnqueue(b, OQ_SOLDIER));
    int frame = oq_unit(b)->build_target;
    ASSERT(frame >= 0);
    int king = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX + 50, OQ_CY);
    ASSERT_EQ_INT(1, Units_CanHelpBuild(king, frame));
    ASSERT_EQ_INT(1, Units_OrderRepair(king, frame));
    /* The barracks alone takes 100 ticks, with the help about 50. */
    int t = 0;
    while (t < 200 && oq_unit(frame)->under_construction) { oq_ticks(1); t++; }
    printf("(%d ticks) ", t);
    ASSERT(t <= 70);
    oq_end();
}

/* A limited builder helps only the types on its own build list, so a
 * mage builder walks past a barracks soldier (legacy:233569-233572). */
TEST(a_limited_builder_does_not_help_a_frame_off_its_list) {
    ASSERT_NOT_NULL(oq_world());
    const int hall = OQ_HALL;
    Units_DebugSetBuildables(OQ_LIMITED, &hall, 1);
    int b = oq_barracks();
    ASSERT_EQ_INT(0, Units_FactoryEnqueue(b, OQ_SOLDIER));
    int frame = oq_unit(b)->build_target;
    ASSERT(frame >= 0);
    int mage = Units_Spawn(OQ_LIMITED, 1, 0, OQ_CX + 70, OQ_CY);
    ASSERT_EQ_INT(0, Units_CanHelpBuild(mage, frame));
    ASSERT_EQ_INT(0, Units_OrderRepair(mage, frame));
    oq_command(TAK_CMD_REPAIR, mage, OQ_CX, OQ_CY, frame, -1, 0);
    oq_command(TAK_CMD_REPAIR, mage, OQ_CX, OQ_CY, frame, -1, TAK_CMD_ARG_QUEUE);
    oq_ticks(1);
    ASSERT(oq_unit(mage)->cmd_kind != UNIT_CMD_BUILD);
    ASSERT_EQ_INT(0, (int)oq_unit(mage)->leg_count);
    oq_end();
}

/* The same builder helps a type its list holds. */
TEST(a_limited_builder_helps_a_frame_on_its_list) {
    ASSERT_NOT_NULL(oq_world());
    const int hall = OQ_HALL;
    Units_DebugSetBuildables(OQ_LIMITED, &hall, 1);
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 50, OQ_CY);
    int frame = Units_BeginBuildingForUnit(bd, OQ_HALL, OQ_CX, OQ_CY);
    ASSERT(frame >= 0);
    int mage = Units_Spawn(OQ_LIMITED, 1, 0, OQ_CX + 50, OQ_CY);
    ASSERT_EQ_INT(1, Units_OrderRepair(mage, frame));
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(mage)->cmd_kind);
    ASSERT_EQ_INT(frame, (int)oq_unit(mage)->build_target);
    oq_end();
}

/* Only the frame's owner helps build it: the original compares the
 * players, so an ally's frame is no help (legacy:233564). */
TEST(a_builder_does_not_help_an_allys_frame) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    w->allied[1][2] = w->allied[2][1] = 1;
    int theirs = Units_Spawn(OQ_BUILDER, 2, 1, OQ_CX - 50, OQ_CY);
    int frame = Units_BeginBuildingForUnit(theirs, OQ_HALL, OQ_CX, OQ_CY);
    ASSERT(frame >= 0);
    int mine = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX + 50, OQ_CY);
    ASSERT_EQ_INT(0, Units_OrderRepair(mine, frame));
    oq_command(TAK_CMD_REPAIR, mine, OQ_CX, OQ_CY, frame, -1, TAK_CMD_ARG_QUEUE);
    oq_ticks(1);
    ASSERT_EQ_INT(0, (int)oq_unit(mine)->leg_count);
    oq_end();
}

/* The hammer shows over your own frame only when a selected unit could
 * help build it (legacy:186541-186545). */
TEST(the_hammer_shows_only_for_a_helper_that_could_build_it) {
    ASSERT_NOT_NULL(oq_world());
    const int hall = OQ_HALL;
    Units_DebugSetBuildables(OQ_LIMITED, &hall, 1);
    int b = oq_barracks();
    ASSERT_EQ_INT(0, Units_FactoryEnqueue(b, OQ_SOLDIER));
    int frame = oq_unit(b)->build_target;
    ASSERT(frame >= 0);
    int king = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX + 300, OQ_CY);
    int mage = Units_Spawn(OQ_LIMITED, 1, 0, OQ_CX + 300, OQ_CY + 60);
    Units_SelectSingle(king);
    ASSERT_EQ_INT(HUD_CMD_HEAL, InGame_HoverCursorOn(frame, OQ_CX, OQ_CY));
    Units_SelectSingle(mage);
    ASSERT(InGame_HoverCursorOn(frame, OQ_CX, OQ_CY) != HUD_CMD_HEAL);
    oq_end();
}

/* A frame is never selected, so with nobody to help it the pointer is
 * the ground's and not the select hand (legacy:186553, 186603). */
TEST(a_frame_nobody_selected_can_help_shows_no_select_hand) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 50, OQ_CY);
    int frame = Units_BeginBuildingForUnit(bd, OQ_HALL, OQ_CX, OQ_CY);
    ASSERT(frame >= 0);
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX + 300, OQ_CY);
    Units_SelectSingle(s);
    ASSERT_EQ_INT(HUD_CUR_NORMAL, InGame_HoverCursorOn(frame, OQ_CX, OQ_CY));
    Units_SelectSingle(-1);
    ASSERT_EQ_INT(HUD_CUR_NORMAL, InGame_HoverCursorOn(frame, OQ_CX, OQ_CY));
    oq_end();
}

/* A click on your own frame orders each unit for itself: a builder that
 * can help joins the work and a soldier walks to the spot
 * (legacy:238458-238660). */
TEST(a_click_on_a_frame_sends_the_helpers_and_walks_the_rest) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 50, OQ_CY);
    int frame = Units_BeginBuildingForUnit(bd, OQ_HALL, OQ_CX, OQ_CY);
    ASSERT(frame >= 0);
    int helper = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX + 300, OQ_CY);
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX + 300, OQ_CY + 60);
    Units_SelectSingle(helper);
    Units_SelectAdd(s);
    InGame_WorldClickOn(OQ_CX, OQ_CY, frame, 0);
    oq_ticks(1);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(helper)->cmd_kind);
    ASSERT_EQ_INT(frame, (int)oq_unit(helper)->build_target);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)oq_unit(s)->cmd_kind);
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

/* A row of halls either side of cell (60, 60), leaving a gap there. */
static void oq_row(void) {
    for (int k = 1; k <= 12; k++) {
        Units_Spawn(OQ_HALL, 1, 0, (60 + 3 * k) * 16 + 8, 60 * 16 + 8);
        Units_Spawn(OQ_HALL, 1, 0, (60 - 3 * k) * 16 + 8, 60 * 16 + 8);
    }
    TAK_PathCacheReset();
}

/* A builder sent to close a gap in a row works from its own side of the
 * row, from the south as from the north, rather than walking round the
 * row to whatever open ground lies nearest the site's centre. */
TEST(a_builder_closes_a_gap_in_a_row_from_its_own_side) {
    int32_t sx = 60 * 16 + 8, sy = 60 * 16 + 8;
    int ticks[2] = { -1, -1 };
    for (int side = 0; side < 2; side++) {
        GameWorld *w = oq_world();
        ASSERT_NOT_NULL(w);
        oq_row();
        int32_t by = side == 0 ? sy + 160 : sy - 160;
        int bd = Units_Spawn(OQ_BUILDER, 1, 0, sx, by);
        ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, sx, sy, -1, OQ_HALL, 0));
        for (int t = 0; t < 1800 && ticks[side] < 0; t++) {
            oq_ticks(1);
            const Unit *u = oq_unit(bd);
            if (u->anim_state == UNIT_ANIM_BUILDING &&
                (side == 0 ? u->world_y > sy : u->world_y < sy))
                ticks[side] = t + 1;
        }
        oq_end();
    }
    printf("[from the south %d ticks, from the north %d] ", ticks[0], ticks[1]);
    ASSERT(ticks[0] > 0 && ticks[0] <= 600);
    ASSERT(ticks[1] > 0 && ticks[1] <= 600);
}

/* ── a builder's build buttons ────────────────────────────────────── */

static int oq_legs_of(int unit, int kind, int def) {
    const Unit *u = oq_unit(unit);
    int n = 0;
    for (int k = 0; k < u->leg_count; k++)
        n += u->legs[k].kind == kind && (def < 0 || u->legs[k].def == def);
    return n;
}

/* A builder with halls, a barracks and a move queued behind the hall in
 * hand. */
static int oq_busy_builder(int *frame) {
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 8, -1, OQ_HALL, 0);
    *frame = oq_unit(bd)->build_target;
    const int q = TAK_CMD_ARG_QUEUE;
    oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 72, -1, OQ_HALL, q);
    oq_command(TAK_CMD_BUILD, bd, OQ_CX + 300, OQ_CY + 8, -1, OQ_BARRACKS, q);
    oq_command(TAK_CMD_MOVE, bd, OQ_CX, OQ_CY + 300, -1, -1, q);
    oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 136, -1, OQ_HALL, q | 1);
    oq_command(TAK_CMD_BUILD, bd, OQ_CX + 72, OQ_CY + 72, -1, OQ_HALL, q);
    return bd;
}

/* The dequeue takes a walking builder's build orders of the def from the
 * head, the one in hand first, and every one for the Ctrl count. The
 * builder stops and its frame stays up (legacy:150067-150093,
 * 181838-181866, 180806-180826). */
TEST(a_dequeue_takes_a_builders_build_orders_of_the_def) {
    ASSERT_NOT_NULL(oq_world());
    int frame = -1;
    int bd = oq_busy_builder(&frame);
    ASSERT(frame >= 0);
    ASSERT_EQ_INT(5, (int)oq_unit(bd)->leg_count);
    ASSERT_EQ_INT(3, Units_QueuedBuildCountForDef(bd, OQ_HALL));
    ASSERT_EQ_INT(4, Units_BuildOrderCountForDef(bd, OQ_HALL));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_DEQUEUE, bd, 0, 0, -1, OQ_HALL, 1));
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(-1, (int)oq_unit(bd)->build_target);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)oq_unit(frame)->alive);
    ASSERT_EQ_INT(5, (int)oq_unit(bd)->leg_count);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_DEQUEUE, bd, 0, 0, -1, OQ_HALL, 1));
    ASSERT_EQ_INT(2, Units_QueuedBuildCountForDef(bd, OQ_HALL));
    ASSERT_EQ_INT(OQ_CX + 8, oq_unit(bd)->legs[2].x);
    ASSERT_EQ_INT(OQ_CY + 136, oq_unit(bd)->legs[2].y);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_FACTORY_DEQUEUE, bd, 0, 0, -1, OQ_HALL,
                                TAK_FACTORY_ALL));
    ASSERT_EQ_INT(0, Units_BuildOrderCountForDef(bd, OQ_HALL));
    ASSERT_EQ_INT(2, (int)oq_unit(bd)->leg_count);
    ASSERT_EQ_INT(OQ_BARRACKS, oq_unit(bd)->legs[0].def);
    ASSERT_EQ_INT(UNIT_LEG_MOVE, oq_unit(bd)->legs[1].kind);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)oq_unit(frame)->alive);
    ASSERT_EQ_INT(0, oq_command(TAK_CMD_FACTORY_DEQUEUE, bd, 0, 0, -1, OQ_HALL,
                                TAK_FACTORY_ALL));
    oq_end();
}

/* A right click on a walking builder's button goes out as a command that
 * takes every build order of that kind, the hall in hand with the queued
 * ones, and nothing else. The builder goes on to its next order and the
 * hall's frame stays where it is. */
TEST(a_right_click_on_a_builders_button_drops_that_kind) {
    ASSERT_NOT_NULL(oq_world());
    int frame = -1;
    int bd = oq_busy_builder(&frame);
    Units_SelectSingle(bd);
    SDL_SetModState(KMOD_NONE);
    ASSERT_EQ_INT(1, HUD_BuildButtonRightClick(OQ_HALL));
    ASSERT_EQ_INT(3, Units_QueuedBuildCountForDef(bd, OQ_HALL));
    ASSERT_EQ_INT(frame, (int)oq_unit(bd)->build_target);
    oq_ticks(1);
    ASSERT_EQ_INT(0, Units_BuildOrderCountForDef(bd, OQ_HALL));
    ASSERT_EQ_INT(1, oq_legs_of(bd, UNIT_LEG_MOVE, -1));
    ASSERT(oq_unit(bd)->build_target != frame);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)oq_unit(frame)->alive);
    ASSERT_EQ_INT(1, (int)oq_unit(frame)->under_construction);
    /* Nothing of that kind left, so nothing goes out. */
    ASSERT_EQ_INT(0, HUD_BuildButtonRightClick(OQ_HALL));
    ASSERT_EQ_INT(1, Units_BuildOrderCountForDef(bd, OQ_BARRACKS));
    ASSERT_EQ_INT(1, HUD_BuildButtonRightClick(OQ_BARRACKS));
    oq_ticks(1);
    ASSERT_EQ_INT(0, Units_BuildOrderCountForDef(bd, OQ_BARRACKS));
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)oq_unit(frame)->alive);
    oq_end();
}

/* A builder with only a hall in hand: the right click drops the order and
 * the builder stops. Its frame stays on the map, not taken off and not
 * paid back (legacy:181838-181866, 180806-180826). */
TEST(a_right_click_drops_the_hall_in_hand_and_leaves_its_frame) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 8, -1,
                                OQ_HALL, 0));
    int frame = oq_unit(bd)->build_target;
    ASSERT(frame >= 0);
    ASSERT_EQ_INT(0, (int)oq_unit(bd)->leg_count);
    oq_ticks(30);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    int32_t fx = oq_unit(frame)->world_x, fy = oq_unit(frame)->world_y;
    Units_SelectSingle(bd);
    SDL_SetModState(KMOD_NONE);
    ASSERT_EQ_INT(1, HUD_BuildButtonRightClick(OQ_HALL));
    oq_ticks(1);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(-1, (int)oq_unit(bd)->build_target);
    int32_t hp = oq_unit(frame)->health;
    oq_ticks(30);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)oq_unit(frame)->alive);
    ASSERT_EQ_INT(1, (int)oq_unit(frame)->under_construction);
    ASSERT_EQ_INT(fx, oq_unit(frame)->world_x);
    ASSERT_EQ_INT(fy, oq_unit(frame)->world_y);
    ASSERT(oq_unit(frame)->health <= hp);
    ASSERT_EQ_INT(0, HUD_BuildButtonRightClick(OQ_HALL));
    oq_end();
}

/* ── the Shift overlay ────────────────────────────────────────────── */

/* A move in hand, then an attack, a building, a move, two patrol points
 * and a move queued: a line through each with its own marker, a ghost
 * for the building, and the route closing back through where the
 * patrol began. The move after the patrol is never reached. */
TEST(the_shift_overlay_plans_a_mixed_queue) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX, OQ_CY);
    int foe = Units_Spawn(OQ_SOLDIER, 2, 1, OQ_CX + 400, OQ_CY);
    const int q = TAK_CMD_ARG_QUEUE;
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, bd, OQ_CX + 100, OQ_CY, -1, -1, 0));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_ATTACK, bd, OQ_CX + 390, OQ_CY + 10, foe, -1, q));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 72, -1, OQ_HALL,
                                q | 1));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, bd, OQ_CX - 100, OQ_CY, -1, -1, q));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_PATROL, bd, OQ_CX - 100, OQ_CY + 200, -1, -1, q));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_PATROL, bd, OQ_CX + 100, OQ_CY + 200, -1, -1, q));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, bd, OQ_CX, OQ_CY + 500, -1, -1, q));
    OrderStop st[ORDER_OVERLAY_STOPS_MAX];
    ASSERT_EQ_INT(7, OrderOverlay_Plan(bd, st, ORDER_OVERLAY_STOPS_MAX));
    ASSERT_EQ_INT(ORDER_MARK_MOVE, st[0].mark);
    ASSERT_EQ_INT(0, st[0].queued);
    ASSERT_EQ_INT(OQ_CX + 100, st[0].x);
    ASSERT_EQ_INT(ORDER_MARK_ATTACK, st[1].mark);
    ASSERT_EQ_INT(foe, st[1].target);
    ASSERT_EQ_INT(OQ_CX + 400, st[1].x);
    ASSERT_EQ_INT(ORDER_MARK_BUILD, st[2].mark);
    ASSERT_EQ_INT(1, st[2].ghost);
    ASSERT_EQ_INT(OQ_HALL, st[2].def);
    ASSERT_EQ_INT(1, st[2].facing);
    ASSERT_EQ_INT(OQ_CY + 72, st[2].y);
    ASSERT_EQ_INT(ORDER_MARK_MOVE, st[3].mark);
    ASSERT_EQ_INT(1, st[3].queued);
    ASSERT_EQ_INT(ORDER_MARK_PATROL, st[4].mark);
    ASSERT_EQ_INT(ORDER_MARK_PATROL, st[5].mark);
    ASSERT_EQ_INT(OQ_CX + 100, st[5].x);
    ASSERT_EQ_INT(ORDER_MARK_NONE, st[6].mark);
    ASSERT_EQ_INT(OQ_CX - 100, st[6].x);
    ASSERT_EQ_INT(OQ_CY, st[6].y);
    for (int i = 0; i < 7; i++)
        if (i != 2) ASSERT_EQ_INT(0, st[i].ghost);
    oq_end();
}

/* A patrol in hand comes back through the point it started from and
 * round to its first point again. With one point it just goes back. */
TEST(a_patrol_in_hand_closes_through_its_start) {
    ASSERT_NOT_NULL(oq_world());
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    OrderStop st[ORDER_OVERLAY_STOPS_MAX];
    ASSERT_EQ_INT(0, OrderOverlay_Plan(s, st, ORDER_OVERLAY_STOPS_MAX));
    oq_command(TAK_CMD_PATROL, s, OQ_CX + 160, OQ_CY, -1, -1, 0);
    ASSERT_EQ_INT(2, OrderOverlay_Plan(s, st, ORDER_OVERLAY_STOPS_MAX));
    ASSERT_EQ_INT(ORDER_MARK_PATROL, st[1].mark);
    ASSERT_EQ_INT(OQ_CX, st[1].x);
    oq_command(TAK_CMD_PATROL, s, OQ_CX + 160, OQ_CY + 160, -1, -1, TAK_CMD_ARG_QUEUE);
    ASSERT_EQ_INT(4, OrderOverlay_Plan(s, st, ORDER_OVERLAY_STOPS_MAX));
    ASSERT_EQ_INT(OQ_CY + 160, st[1].y);
    ASSERT_EQ_INT(ORDER_MARK_PATROL, st[2].mark);
    ASSERT_EQ_INT(OQ_CX, st[2].x);
    ASSERT_EQ_INT(OQ_CY, st[2].y);
    ASSERT_EQ_INT(ORDER_MARK_NONE, st[3].mark);
    ASSERT_EQ_INT(OQ_CX + 160, st[3].x);
    ASSERT_EQ_INT(OQ_CY, st[3].y);
    oq_end();
}

/* A barracks shows its rally point and the standing orders behind it,
 * a patrol route closing back at the rally. */
TEST(the_shift_overlay_shows_a_barracks_rally) {
    ASSERT_NOT_NULL(oq_world());
    int b = oq_barracks();
    OrderStop st[ORDER_OVERLAY_STOPS_MAX];
    ASSERT_EQ_INT(0, OrderOverlay_Plan(b, st, ORDER_OVERLAY_STOPS_MAX));
    const int q = TAK_CMD_ARG_QUEUE;
    oq_command(TAK_CMD_MOVE, b, OQ_CX + 200, OQ_CY, -1, -1, 0);
    oq_command(TAK_CMD_PATROL, b, OQ_CX + 200, OQ_CY + 200, -1, -1, q);
    oq_command(TAK_CMD_PATROL, b, OQ_CX + 400, OQ_CY + 200, -1, -1, q);
    ASSERT_EQ_INT(4, OrderOverlay_Plan(b, st, ORDER_OVERLAY_STOPS_MAX));
    ASSERT_EQ_INT(ORDER_MARK_RALLY, st[0].mark);
    ASSERT_EQ_INT(OQ_CX + 200, st[0].x);
    ASSERT_EQ_INT(ORDER_MARK_PATROL, st[1].mark);
    ASSERT_EQ_INT(ORDER_MARK_PATROL, st[2].mark);
    ASSERT_EQ_INT(ORDER_MARK_NONE, st[3].mark);
    ASSERT_EQ_INT(OQ_CX + 200, st[3].x);
    ASSERT_EQ_INT(OQ_CY, st[3].y);
    oq_end();
}

/* A target in the fog gives no position away: the attack in hand is
 * left out and a queued one shows where it was ordered. */
TEST(the_shift_overlay_hides_where_an_unseen_target_is) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    int foe = Units_Spawn(OQ_SOLDIER, 2, 1, OQ_CX + 1200, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_ATTACK, s, OQ_CX + 1200, OQ_CY, foe, -1, 0));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_ATTACK, s, OQ_CX + 1190, OQ_CY + 10, foe, -1,
                                TAK_CMD_ARG_QUEUE));
    OrderStop st[ORDER_OVERLAY_STOPS_MAX];
    ASSERT_EQ_INT(2, OrderOverlay_Plan(s, st, ORDER_OVERLAY_STOPS_MAX));
    ASSERT_EQ_INT(foe, st[1].target);
    ASSERT_EQ_INT(OQ_CX + 1200, st[1].x);
    w->cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(w));
    ASSERT_EQ_INT(1, OrderOverlay_Plan(s, st, ORDER_OVERLAY_STOPS_MAX));
    ASSERT_EQ_INT(-1, st[0].target);
    ASSERT_EQ_INT(OQ_CX + 1190, st[0].x);
    ASSERT_EQ_INT(OQ_CY + 10, st[0].y);
    oq_end();
}

/* A target dying in the fog gives its spot away no more than a live
 * one does: the attack in hand on it is left out. */
TEST(the_shift_overlay_hides_a_dying_target_in_the_fog) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    int foe = Units_Spawn(OQ_SOLDIER, 2, 1, OQ_CX + 1200, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_ATTACK, s, OQ_CX + 1200, OQ_CY, foe, -1, 0));
    OrderStop st[ORDER_OVERLAY_STOPS_MAX];
    ASSERT_EQ_INT(1, OrderOverlay_Plan(s, st, ORDER_OVERLAY_STOPS_MAX));
    ((Unit *)oq_unit(foe))->alive = UNIT_ALIVE_DYING;
    ASSERT_EQ_INT(1, OrderOverlay_Plan(s, st, ORDER_OVERLAY_STOPS_MAX));
    ASSERT_EQ_INT(foe, st[0].target);
    w->cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(w));
    ASSERT_EQ_INT(0, OrderOverlay_Plan(s, st, ORDER_OVERLAY_STOPS_MAX));
    ((Unit *)oq_unit(foe))->alive = UNIT_ALIVE_ACTIVE;
    oq_end();
}

/* A one piece model and a Create that returns, for the preview cache. */
static float    g_oq_pos[9] = { 0, 0, 0, 8, 0, 0, 0, 0, 8 };
static float    g_oq_uv[6];
static uint32_t g_oq_col[3] = { 0xff808080u, 0xff808080u, 0xff808080u };
static uint16_t g_oq_idx[3] = { 0, 1, 2 };
static uint16_t g_oq_node[3];
static uint32_t g_oq_seq[1];
static UnitMesh g_oq_mesh;
static uint32_t g_oq_code[] = { 0x10065000u };   /* RETURN */
static uint32_t g_oq_offsets[1];
static char     g_oq_create[] = "Create";
static char     g_oq_base[] = "base";
static char    *g_oq_script_names[1] = { g_oq_create };
static char    *g_oq_piece_names[1] = { g_oq_base };
static CobScript g_oq_script;

static void oq_ghost_models(int on) {
    memset(&g_oq_mesh, 0, sizeof g_oq_mesh);
    g_oq_mesh.positions = g_oq_pos;
    g_oq_mesh.uvs = g_oq_uv;
    g_oq_mesh.colors = g_oq_col;
    g_oq_mesh.indices = g_oq_idx;
    g_oq_mesh.vert_node_idx = g_oq_node;
    g_oq_mesh.tri_seq = g_oq_seq;
    g_oq_mesh.vert_count = 3;
    g_oq_mesh.tri_count = 1;
    g_oq_mesh.node_count = 1;
    g_oq_mesh.nodes[0].parent = -1;
    strncpy(g_oq_mesh.nodes[0].name, "base", sizeof g_oq_mesh.nodes[0].name - 1);
    memset(&g_oq_script, 0, sizeof g_oq_script);
    g_oq_script.version = 6;
    g_oq_script.code = g_oq_code;
    g_oq_script.num_code_words = 1;
    g_oq_script.script_names = g_oq_script_names;
    g_oq_script.script_offsets = g_oq_offsets;
    g_oq_script.num_scripts = 1;
    g_oq_script.num_pieces = 1;
    g_oq_script.piece_names = g_oq_piece_names;
    for (int d = 0; d < OQ_DEF_COUNT; d++) {
        UnitDef *def = (UnitDef *)Units_GetDef(d);
        def->cob_script = on ? &g_oq_script : NULL;
        for (int c = 0; c < 12; c++) def->mesh_per_color[c] = on ? &g_oq_mesh : NULL;
    }
}

/* More queued ghosts in a frame than the preview cache holds, each its
 * own (def, colour): a frame settles no more than its budget and the
 * rest wait. The overlay's own worst case fits the cache, so once
 * every preview is made a frame settles none. */
TEST(queued_ghosts_settle_a_bounded_number_a_frame) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    oq_ghost_models(1);
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, 320, 240, 32,
                                                       SDL_PIXELFORMAT_ARGB8888);
    ASSERT_NOT_NULL(surf);
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(surf);
    ASSERT_NOT_NULL(r);
    TAK_Platform plat;
    memset(&plat, 0, sizeof plat);
    plat.renderer = r;
    w->cam_x = OQ_CX - 160;
    w->cam_y = OQ_CY - 120;
    const int pairs = OQ_DEF_COUNT * 12;
    ASSERT(pairs > UNITS_GHOSTS_QUEUED_MAX + 8);
    for (int frame = 0; frame < 60; frame++) {
        uint32_t before = Units_DebugGhostSettles();
        Units_GhostFrameBegin();
        for (int k = 0; k < pairs; k++)
            Units_RenderBuildGhostFacing(&plat, w, k % OQ_DEF_COUNT, k / OQ_DEF_COUNT,
                                         OQ_CX, OQ_CY, 110, UNITS_GHOST_QUEUED, 0);
        ASSERT(Units_DebugGhostSettles() - before <= UNITS_GHOST_SETTLES_PER_FRAME);
    }
    int settled_last = 1;
    for (int frame = 0; frame < UNITS_GHOSTS_QUEUED_MAX; frame++) {
        uint32_t before = Units_DebugGhostSettles();
        Units_GhostFrameBegin();
        for (int k = 0; k < UNITS_GHOSTS_QUEUED_MAX; k++)
            Units_RenderBuildGhostFacing(&plat, w, k % OQ_DEF_COUNT, k / OQ_DEF_COUNT,
                                         OQ_CX, OQ_CY, 110, UNITS_GHOST_QUEUED, 0);
        settled_last = (int)(Units_DebugGhostSettles() - before);
        ASSERT(settled_last <= UNITS_GHOST_SETTLES_PER_FRAME);
    }
    ASSERT_EQ_INT(0, settled_last);
    /* The placement cursor is never kept waiting. */
    uint32_t before = Units_DebugGhostSettles();
    Units_GhostFrameBegin();
    ASSERT(Units_DefCanTurn(OQ_GATE));
    for (int facing = 1; facing <= 3; facing++)
        Units_RenderBuildGhostFacing(&plat, w, OQ_GATE, 0, OQ_CX, OQ_CY + 64,
                                     110, 1, facing);
    ASSERT(UNITS_GHOST_SETTLES_PER_FRAME < 3);
    ASSERT_EQ_INT(3, (int)(Units_DebugGhostSettles() - before));
    SDL_DestroyRenderer(r);
    SDL_FreeSurface(surf);
    oq_ghost_models(0);
    oq_end();
}

static int oq_lit_pixels(SDL_Surface *surf) {
    int lit = 0;
    for (int y = 0; y < surf->h; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)surf->pixels +
                                                 (size_t)y * (size_t)surf->pitch);
        for (int x = 0; x < surf->w; x++) lit += (row[x] & 0x00ffffffu) != 0;
    }
    return lit;
}

/* Nothing is drawn without Shift, and with it a selected unit's route
 * is. Another player's unit shows nothing even with Shift. */
TEST(the_shift_overlay_draws_only_while_shift_is_held) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    int foe = Units_Spawn(OQ_SOLDIER, 2, 1, OQ_CX - 100, OQ_CY);
    oq_command(TAK_CMD_MOVE, s, OQ_CX + 100, OQ_CY, -1, -1, 0);
    oq_command(TAK_CMD_MOVE, s, OQ_CX + 100, OQ_CY + 100, -1, -1, TAK_CMD_ARG_QUEUE);
    Units_OrderMove(foe, OQ_CX - 200, OQ_CY);
    w->cam_x = OQ_CX - 320;
    w->cam_y = OQ_CY - 240;
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, 640, 480, 32,
                                                       SDL_PIXELFORMAT_ARGB8888);
    ASSERT_NOT_NULL(surf);
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(surf);
    ASSERT_NOT_NULL(r);
    TAK_Platform plat;
    memset(&plat, 0, sizeof plat);
    plat.renderer = r;
    Units_SelectSingle(s);

    OrderOverlay_SetShift(0);
    ASSERT_EQ_INT(0, OrderOverlay_Draw(w, &plat));
    SDL_RenderFlush(r);
    ASSERT_EQ_INT(0, oq_lit_pixels(surf));

    OrderOverlay_SetShift(1);
    ASSERT_EQ_INT(2, OrderOverlay_Draw(w, &plat));
    SDL_RenderFlush(r);
    ASSERT(oq_lit_pixels(surf) > 100);

    SDL_FillRect(surf, NULL, 0);
    Units_SelectSingle(foe);
    ASSERT_EQ_INT(0, OrderOverlay_Draw(w, &plat));
    SDL_RenderFlush(r);
    ASSERT_EQ_INT(0, oq_lit_pixels(surf));

    OrderOverlay_SetShift(0);
    SDL_DestroyRenderer(r);
    SDL_FreeSurface(surf);
    oq_end();
}

/* A flyer in the air, as its takeoff leaves it (test-only mutation).
 * The model has no script, so the next tick sets it down as a hoverer. */
static void oq_lift(int h, int flying) {
    Unit *u = (Unit *)oq_unit(h);
    u->flight_alt = 120.0f;
    u->flying = (uint8_t)flying;
}

/* Where a point draws: lifted up the map by its height (legacy:197689). */
static int32_t oq_drawn_y(GameWorld *w, int32_t x, int32_t y, float up) {
    float h = (float)Terrain_SampleHeight(w, x, y) + up;
    return y - (int32_t)(h * Units_GetTanTilt());
}

/* A flyer is drawn up at its height, and both views pick it there and
 * not on the ground under it. The original lifts the pick box by the
 * unit's own height (legacy:237815-237922). */
TEST(a_flyer_is_picked_where_it_is_drawn_in_both_views) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    int f = Units_Spawn(OQ_FLYER, 2, 1, OQ_CX, OQ_CY);
    ASSERT(f >= 0);
    oq_lift(f, 1);
    ASSERT_EQ_INT(f, Units_PickAt(OQ_CX, oq_drawn_y(w, OQ_CX, OQ_CY, 120.0f), 0));
    ASSERT(Units_PickAt(OQ_CX, oq_drawn_y(w, OQ_CX, OQ_CY, 0.0f), 0) != f);

    /* The 3D view: a ray from an eye up and to the south, at the body
     * and at the ground under it. */
    float g = (float)Terrain_SampleHeight(w, OQ_CX, OQ_CY);
    const float eye[3] = { (float)OQ_CX, g + 900.0f, (float)OQ_CY + 500.0f };
    const float body[3] = { (float)OQ_CX, g + 136.0f, (float)OQ_CY };
    const float under[3] = { (float)OQ_CX, g, (float)OQ_CY };
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = body[k] - eye[k];
    ASSERT_EQ_INT(f, Units_PickRay(eye, d));
    for (int k = 0; k < 3; k++) d[k] = under[k] - eye[k];
    ASSERT(Units_PickRay(eye, d) != f);
    oq_end();
}

/* Over an enemy flyer the cursor, and what a click sends, are each
 * selected unit's own answer (legacy:238791-238838,
 * legacy:186135-186330, legacy:186914-186960). A bow takes a flyer in
 * the air and a noairweapon cannon cannot, so the cannon alone shows
 * the too far cursor and is given nothing, and a builder with no
 * weapon walks to the ground the pointer is over. */
TEST(only_a_weapon_that_can_take_a_flyer_attacks_it) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    int f = Units_Spawn(OQ_FLYER, 2, 1, OQ_CX, OQ_CY);
    int bow = Units_Spawn(OQ_BOW, 1, 0, OQ_CX - 200, OQ_CY + 100);
    int can = Units_Spawn(OQ_CANNON, 1, 0, OQ_CX - 160, OQ_CY + 100);
    int bld = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 120, OQ_CY + 100);
    ASSERT(f >= 0 && bow >= 0 && can >= 0 && bld >= 0);
    const int32_t y = oq_drawn_y(w, OQ_CX, OQ_CY, 120.0f);

    oq_lift(f, 1);
    Units_SelectSingle(can);
    ASSERT_EQ_INT(HUD_CUR_TOOFAR, InGame_HoverCursorAt(OQ_CX, y));
    ASSERT_EQ_INT(1, Units_OrderMove(can, OQ_CX - 400, OQ_CY + 400));
    InGame_WorldClickOn(OQ_CX, y, Units_PickAt(OQ_CX, y, 0), 0);
    oq_ticks(1);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, oq_unit(can)->cmd_kind);
    ASSERT_EQ_INT(OQ_CX - 400, oq_unit(can)->cmd_x);

    oq_lift(f, 1);
    Units_SelectSingle(bow);
    Units_SelectAdd(can);
    Units_SelectAdd(bld);
    ASSERT_EQ_INT(HUD_CMD_ATTACK, InGame_HoverCursorAt(OQ_CX, y));
    InGame_WorldClickOn(OQ_CX, y, Units_PickAt(OQ_CX, y, 0), 0);
    oq_ticks(1);
    ASSERT_EQ_INT(UNIT_CMD_ATTACK, oq_unit(bow)->cmd_kind);
    ASSERT_EQ_INT(f, oq_unit(bow)->target);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, oq_unit(can)->cmd_kind);
    ASSERT_EQ_INT(OQ_CX - 400, oq_unit(can)->cmd_x);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, oq_unit(bld)->cmd_kind);
    ASSERT(abs(oq_unit(bld)->cmd_x - OQ_CX) <= 16);

    /* An armed attack order sends the bow and not the cannon either. */
    oq_lift(f, 1);
    ASSERT_EQ_INT(1, Units_OrderMove(bow, OQ_CX - 300, OQ_CY + 300));
    Units_SelectSingle(can);
    HUD_SetCommandMode(HUD_CMD_ATTACK);
    ASSERT_EQ_INT(HUD_CUR_TOOFAR,
                  InGame_CommandCursorAt(HUD_CMD_ATTACK, OQ_CX, y));
    Units_SelectAdd(bow);
    ASSERT_EQ_INT(HUD_CMD_ATTACK,
                  InGame_CommandCursorAt(HUD_CMD_ATTACK, OQ_CX, y));
    InGame_WorldClickOn(OQ_CX, y, Units_PickAt(OQ_CX, y, 0), 0);
    oq_ticks(1);
    ASSERT_EQ_INT(UNIT_CMD_ATTACK, oq_unit(bow)->cmd_kind);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, oq_unit(can)->cmd_kind);

    /* On the ground it is a ground target the cannon takes. */
    oq_lift(f, 0);
    Units_SelectSingle(can);
    ASSERT_EQ_INT(HUD_CMD_ATTACK, InGame_HoverCursorAt(OQ_CX, y));

    /* With nothing selected a click on it picks it for a look, and a
     * flyer of your own is selected where it is drawn. */
    oq_lift(f, 1);
    Units_SelectSingle(-1);
    InGame_WorldClickOn(OQ_CX, y, Units_PickAt(OQ_CX, y, 0), 0);
    int n = 0;
    const int *sel = Units_GetSelection(&n);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(f, sel[0]);
    int mine = Units_Spawn(OQ_FLYER, 1, 0, OQ_CX + 300, OQ_CY);
    ASSERT(mine >= 0);
    oq_lift(mine, 1);
    const int32_t my = oq_drawn_y(w, OQ_CX + 300, OQ_CY, 120.0f);
    InGame_WorldClickOn(OQ_CX + 300, my, Units_PickAt(OQ_CX + 300, my, 0), 0);
    sel = Units_GetSelection(&n);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(mine, sel[0]);
    oq_end();
}

/* ── the select keys ───────────────────────────────────────────── */

/* One press of a key with Ctrl or Shift held, then every key up. */
static void oq_chord(int mods, int key) {
    InGame_DebugKeyChord(mods, key);
    InGame_DebugKeyFrame(0, NULL);
}

static int oq_selected(int handle) {
    int n = 0;
    const int *sel = Units_GetSelection(&n);
    for (int i = 0; i < n; i++)
        if (sel[i] == handle) return 1;
    return 0;
}

static int oq_selection_count(void) {
    int n = 0;
    Units_GetSelection(&n);
    return n;
}

/* Ctrl+Z adds every finished unit of yours of a type the selection
 * holds, anywhere on the map, and drops none (Keys.TDF CTRL_Z and
 * CTRLSHIFT_Z, legacy:237389-237448). */
TEST(ctrl_z_selects_every_unit_of_a_type_the_selection_holds) {
    ASSERT_NOT_NULL(oq_world());
    int a = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    int far = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX + 900, OQ_CY + 700);
    int bow = Units_Spawn(OQ_BOW, 1, 0, OQ_CX + 40, OQ_CY);
    int builder = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 40, OQ_CY);
    int foe = Units_Spawn(OQ_SOLDIER, 2, 1, OQ_CX + 80, OQ_CY);
    int hall = Units_Spawn(OQ_HALL, 1, 0, OQ_CX, OQ_CY + 200);
    int frame = Units_BeginBuildingForUnit(builder, OQ_HALL, OQ_CX + 200, OQ_CY + 200);
    ASSERT(a >= 0 && far >= 0 && bow >= 0 && foe >= 0 && hall >= 0 && frame >= 0);
    Units_SelectSingle(a);
    Units_SelectAdd(hall);
    oq_chord(IG_CLICK_CTRL, SDL_SCANCODE_Z);
    ASSERT(oq_selected(a) && oq_selected(far) && oq_selected(hall));
    ASSERT(!oq_selected(bow) && !oq_selected(builder) && !oq_selected(foe));
    ASSERT(!oq_selected(frame));
    ASSERT_EQ_INT(3, oq_selection_count());
    /* Shift leaves it the same command, and the plain Z is no key. */
    Units_SelectSingle(bow);
    oq_chord(IG_CLICK_CTRL | IG_CLICK_SHIFT, SDL_SCANCODE_Z);
    ASSERT_EQ_INT(1, oq_selection_count());
    Units_SelectSingle(a);
    oq_chord(0, SDL_SCANCODE_Z);
    ASSERT_EQ_INT(1, oq_selection_count());
    /* An enemy held up to look at names no type of yours. */
    Units_SelectForInspect(foe);
    oq_chord(IG_CLICK_CTRL, SDL_SCANCODE_Z);
    ASSERT_EQ_INT(1, oq_selection_count());
    ASSERT(oq_selected(foe));
    oq_end();
}

/* Ctrl with a letter picks a category from the FBI's category line in
 * place of the selection, Shift adds it, and Ctrl+A takes every unit
 * of yours (Keys.TDF CTRL_B, CTRLSHIFT_E, CTRL_A). */
TEST(ctrl_letters_select_by_category_and_ctrl_a_takes_all) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    int builder = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 40, OQ_CY);
    int helper = Units_Spawn(OQ_HELPER, 1, 0, OQ_CX - 80, OQ_CY);
    int sword = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    int bow = Units_Spawn(OQ_BOW, 1, 0, OQ_CX + 40, OQ_CY);
    int barracks = Units_Spawn(OQ_BARRACKS, 1, 0, OQ_CX, OQ_CY + 300);
    int foe = Units_Spawn(OQ_BUILDER, 2, 1, OQ_CX + 80, OQ_CY);
    ASSERT(builder >= 0 && helper >= 0 && sword >= 0 && bow >= 0 && barracks >= 0 && foe >= 0);
    int32_t cam_x = w->cam_x, cam_y = w->cam_y;
    Units_SelectSingle(bow);
    oq_chord(IG_CLICK_CTRL, SDL_SCANCODE_B);
    ASSERT_EQ_INT(2, oq_selection_count());
    ASSERT(oq_selected(builder) && oq_selected(helper));
    oq_chord(IG_CLICK_CTRL | IG_CLICK_SHIFT, SDL_SCANCODE_E);
    ASSERT_EQ_INT(3, oq_selection_count());
    ASSERT(oq_selected(sword));
    oq_chord(IG_CLICK_CTRL, SDL_SCANCODE_F);
    ASSERT_EQ_INT(1, oq_selection_count());
    ASSERT(oq_selected(barracks));
    oq_chord(IG_CLICK_CTRL, SDL_SCANCODE_A);
    ASSERT_EQ_INT(5, oq_selection_count());
    ASSERT(!oq_selected(foe));
    /* Ctrl holds W, A, S and D off the camera. */
    oq_chord(IG_CLICK_CTRL, SDL_SCANCODE_W);
    ASSERT_EQ_INT(cam_x, w->cam_x);
    ASSERT_EQ_INT(cam_y, w->cam_y);
    ASSERT_EQ_INT(2, oq_selection_count());
    oq_end();
}

/* Ctrl+Shift with a digit adds the group to the selection, where Ctrl
 * alone files a new one (Keys.TDF CTRLSHIFT_1, RetrieveSquadAdd). */
TEST(ctrl_shift_digit_adds_a_group_to_the_selection) {
    ASSERT_NOT_NULL(oq_world());
    int a = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    int b = Units_Spawn(OQ_BOW, 1, 0, OQ_CX + 40, OQ_CY);
    ASSERT(a >= 0 && b >= 0);
    Units_SelectSingle(a);
    oq_chord(IG_CLICK_CTRL, SDL_SCANCODE_1);
    Units_SelectSingle(b);
    oq_chord(IG_CLICK_CTRL | IG_CLICK_SHIFT, SDL_SCANCODE_1);
    ASSERT_EQ_INT(2, oq_selection_count());
    ASSERT(oq_selected(a) && oq_selected(b));
    oq_chord(0, SDL_SCANCODE_1);
    ASSERT_EQ_INT(1, oq_selection_count());
    ASSERT(oq_selected(a));
    oq_end();
}

/* Ctrl+U takes your units in the view in place of the selection
 * (Keys.TDF CTRL_U, legacy:237503-237545). */
TEST(ctrl_u_selects_your_units_in_the_view) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    w->cam_x = OQ_CX - w->viewport_w / 2;
    w->cam_y = OQ_CY - w->viewport_h / 2;
    int seen = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX, OQ_CY);
    int away = Units_Spawn(OQ_SOLDIER, 1, 0, OQ_CX + 900, OQ_CY + 700);
    int foe = Units_Spawn(OQ_SOLDIER, 2, 1, OQ_CX + 40, OQ_CY);
    ASSERT(seen >= 0 && away >= 0 && foe >= 0);
    Units_SelectSingle(away);
    oq_chord(IG_CLICK_CTRL, SDL_SCANCODE_U);
    ASSERT_EQ_INT(1, oq_selection_count());
    ASSERT(oq_selected(seen));
    oq_end();
}

/* ── a walking builder's summons without end ──────────────────────── */

/* Frames of def under construction at (x, y). */
static int oq_frames_of_at(int def, int32_t x, int32_t y) {
    int count = 0, n = 0;
    const Unit *units = Units_GetActive(&count);
    for (int i = 0; i < count; i++)
        n += units[i].alive == UNIT_ALIVE_ACTIVE && units[i].def_idx == def &&
             units[i].under_construction && units[i].world_x == x &&
             units[i].world_y == y;
    return n;
}

/* Ticks until the frame is done, at most limit. */
static void oq_until_done(int frame, int limit) {
    for (int t = 0; t < limit && oq_unit(frame)->under_construction; t++)
        oq_ticks(1);
}

/* Ctrl on a walking builder's button for a unit, then a click: the unit
 * is summoned there without end, each one stepping off the spot, and the
 * button reads +++ (legacy:150077-150084, 39237, 12272-12315). */
TEST(ctrl_on_a_builders_unit_button_summons_without_end) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    Units_SelectSingle(bd);
    HUD_BeginBuildPlacementRepeat(OQ_SOLDIER, 1);
    ASSERT_EQ_INT(1, HUD_BuildPlacementRepeats());
    InGame_WorldClickOn(OQ_CX + 8, OQ_CY + 8, -1, 0);
    ASSERT_EQ_INT(HUD_CMD_NONE, HUD_GetCommandMode());
    oq_ticks(1);
    int first = oq_unit(bd)->build_target;
    ASSERT(first >= 0);
    int32_t sx = oq_unit(first)->world_x, sy = oq_unit(first)->world_y;
    char label[8];
    ASSERT_EQ_INT(1, HUD_QueueBadgeText(bd, OQ_SOLDIER, label, sizeof label));
    ASSERT_EQ_STR("+++", label);
    oq_until_done(first, 300);
    ASSERT_EQ_INT(0, (int)oq_unit(first)->under_construction);
    /* At least 32 px off: one cell of footprint and one roll. */
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)oq_unit(first)->cmd_kind);
    int32_t dx = oq_unit(first)->cmd_x - sx, dy = oq_unit(first)->cmd_y - sy;
    ASSERT(dx * dx + dy * dy >= 32 * 32);
    ASSERT(dx * dx + dy * dy <= 3 * 128 * 3 * 128);
    for (int t = 0; t < 900 && oq_frames_of_at(OQ_SOLDIER, sx, sy) == 0; t++)
        oq_ticks(1);
    ASSERT_EQ_INT(1, oq_frames_of_at(OQ_SOLDIER, sx, sy));
    oq_ticks(900);
    fprintf(stderr, "  soldiers=%d builder cmd=%d\n", oq_count(OQ_SOLDIER),
            (int)oq_unit(bd)->cmd_kind);
    ASSERT(oq_count(OQ_SOLDIER) >= 4);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(OQ_SOLDIER, Units_FactoryRepeatOf(bd));
    ASSERT_EQ_INT(1, HUD_QueueBadgeText(bd, OQ_SOLDIER, label, sizeof label));
    ASSERT_EQ_STR("+++", label);
    oq_end();
}

/* Ctrl still held at the placing click is no Ctrl-click: the summons
 * replaces what the builder holds, queue and all (legacy:39177-39180). */
TEST(ctrl_through_both_clicks_summons_without_end) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, bd, OQ_CX - 300, OQ_CY, -1, -1, 0));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, bd, OQ_CX - 300, OQ_CY + 200, -1, -1,
                                TAK_CMD_ARG_QUEUE));
    Units_SelectSingle(bd);
    HUD_BeginBuildPlacementRepeat(OQ_SOLDIER, 1);
    InGame_WorldClickOn(OQ_CX + 8, OQ_CY + 8, -1, IG_CLICK_CTRL);
    oq_ticks(1);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(1, (int)oq_unit(bd)->build_endless);
    ASSERT_EQ_INT(0, (int)oq_unit(bd)->leg_count);
    oq_ticks(900);
    fprintf(stderr, "  soldiers=%d builder cmd=%d\n", oq_count(OQ_SOLDIER),
            (int)oq_unit(bd)->cmd_kind);
    ASSERT(oq_count(OQ_SOLDIER) >= 3);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    oq_end();
}

/* With Shift the summons queues and the placement still ends after the
 * one click (legacy:242531-242540). Nothing Shift places goes behind it
 * (legacy:39208-39219). */
TEST(a_shift_click_queues_a_summons_and_nothing_goes_behind_it) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 104, -1,
                                OQ_HALL, 0));
    Units_SelectSingle(bd);
    HUD_BeginBuildPlacementRepeat(OQ_SOLDIER, 1);
    InGame_WorldClickOn(OQ_CX + 8, OQ_CY + 8, -1, IG_CLICK_SHIFT);
    ASSERT_EQ_INT(HUD_CMD_NONE, HUD_GetCommandMode());
    oq_ticks(1);
    ASSERT_EQ_INT(1, oq_legs_of(bd, UNIT_LEG_BUILD, OQ_SOLDIER));
    ASSERT_EQ_INT(1, (int)oq_unit(bd)->legs[0].endless);
    ASSERT_EQ_INT(OQ_SOLDIER, Units_FactoryRepeatOf(bd));
    ASSERT_EQ_INT(0, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 200, OQ_CY + 8, -1,
                                OQ_HALL, TAK_CMD_ARG_QUEUE));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_MOVE, bd, OQ_CX, OQ_CY + 300, -1, -1,
                                TAK_CMD_ARG_QUEUE));
    oq_ticks(1500);
    ASSERT_EQ_INT(1, oq_count(OQ_HALL));
    ASSERT(oq_count(OQ_SOLDIER) >= 2);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(1, (int)oq_unit(bd)->build_endless);
    oq_end();
}

/* A right click on the button ends the summons and leaves the frame in
 * hand standing (legacy:150087-150093, 181838-181866). */
TEST(a_right_click_ends_a_summons_without_end) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 8, -1,
                                OQ_SOLDIER, TAK_CMD_ARG_ENDLESS));
    oq_until_done(oq_unit(bd)->build_target, 300);
    for (int t = 0; t < 600 && oq_unit(bd)->build_target < 0; t++) oq_ticks(1);
    int frame = oq_unit(bd)->build_target;
    ASSERT(frame >= 0);
    Units_SelectSingle(bd);
    SDL_SetModState(KMOD_NONE);
    ASSERT_EQ_INT(1, HUD_BuildButtonRightClick(OQ_SOLDIER));
    oq_ticks(1);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(-1, Units_FactoryRepeatOf(bd));
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)oq_unit(frame)->alive);
    int made = oq_count(OQ_SOLDIER);
    oq_ticks(600);
    ASSERT_EQ_INT(made, oq_count(OQ_SOLDIER));
    char label[8];
    ASSERT_EQ_INT(0, HUD_QueueBadgeText(bd, OQ_SOLDIER, label, sizeof label));
    oq_end();
}

/* A unit on the spot is waited for and the next goes up once it leaves,
 * while a building there ends the order (legacy:12088-12124). */
TEST(a_summons_waits_for_a_unit_on_its_spot_and_ends_on_a_building) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 8, -1,
                                OQ_SOLDIER, TAK_CMD_ARG_ENDLESS));
    int first = oq_unit(bd)->build_target;
    int32_t sx = oq_unit(first)->world_x, sy = oq_unit(first)->world_y;
    oq_until_done(first, 300);
    int blocker = Units_Spawn(OQ_ARCHER, 1, 0, sx, sy);
    ASSERT(blocker >= 0);
    oq_ticks(200);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(0, oq_frames_of_at(OQ_SOLDIER, sx, sy));
    ASSERT(oq_unit(bd)->build_tries > 0);
    ASSERT_EQ_INT(1, Units_OrderMove(blocker, sx + 200, sy));
    for (int t = 0; t < 200 && oq_frames_of_at(OQ_SOLDIER, sx, sy) == 0; t++)
        oq_ticks(1);
    ASSERT_EQ_INT(1, oq_frames_of_at(OQ_SOLDIER, sx, sy));
    oq_until_done(oq_unit(bd)->build_target, 300);
    ASSERT(Units_Spawn(OQ_HALL, 1, 0, sx, sy) >= 0);
    oq_ticks(10);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(-1, Units_FactoryRepeatOf(bd));
    oq_end();
}

/* A unit that stays on the spot ends the summons once 30 looks, ten
 * frames apart, have gone by (legacy:12097-12124). */
TEST(a_summons_gives_up_on_a_spot_a_unit_keeps) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 8, -1,
                                OQ_SOLDIER, TAK_CMD_ARG_ENDLESS));
    int first = oq_unit(bd)->build_target;
    int32_t sx = oq_unit(first)->world_x, sy = oq_unit(first)->world_y;
    oq_until_done(first, 300);
    ASSERT(Units_Spawn(OQ_ARCHER, 1, 0, sx, sy) >= 0);
    oq_ticks(30 * 20);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    oq_ticks(2 * 20 + 4);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)oq_unit(bd)->cmd_kind);
    oq_end();
}

/* Ctrl means nothing on a building's button, and a command that asks
 * for a building without end gets one (legacy:150077-150084). */
TEST(ctrl_on_a_buildings_button_places_it_once) {
    ASSERT_NOT_NULL(oq_world());
    ASSERT_EQ_INT(0, Units_DefCanRepeat(OQ_HALL));
    ASSERT_EQ_INT(1, Units_DefCanRepeat(OQ_SOLDIER));
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX - 64, OQ_CY);
    Units_SelectSingle(bd);
    HUD_BeginBuildPlacementRepeat(OQ_HALL, 1);
    ASSERT_EQ_INT(0, HUD_BuildPlacementRepeats());
    InGame_WorldClickOn(OQ_CX + 8, OQ_CY + 8, -1, IG_CLICK_SHIFT);
    ASSERT_EQ_INT(HUD_CMD_PLACE_BUILD, HUD_GetCommandMode());
    HUD_ClearCommandMode();
    oq_ticks(1);
    ASSERT_EQ_INT(0, (int)oq_unit(bd)->build_endless);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, OQ_CX + 8, OQ_CY + 104, -1,
                                OQ_HALL, TAK_CMD_ARG_ENDLESS));
    ASSERT_EQ_INT(0, (int)oq_unit(bd)->build_endless);
    ASSERT_EQ_INT(-1, Units_FactoryRepeatOf(bd));
    oq_end();
}

/* ── summoned units, as Zhon makes its army ───────────────────────── */

/* A def that walks has an open yard (legacy:163272-163292), and the
 * placing test looks for units only under a yard's blocking cells
 * (legacy:218790-218811). A summon goes where a soldier stands and a
 * hall does not. */
TEST(a_summon_is_placed_where_a_soldier_stands) {
    ASSERT_NOT_NULL(oq_world());
    int32_t sx = OQ_CX + 8, sy = OQ_CY + 8;
    ASSERT(Units_Spawn(OQ_SOLDIER, 1, 0, sx, sy) >= 0);
    ASSERT_EQ_INT(0, Units_IsBuildSiteClearFacing(OQ_HALL, sx, sy, 0));
    ASSERT_EQ_INT(1, Units_IsBuildSiteClearFacing(OQ_SOLDIER, sx, sy, 0));
    oq_end();
}

/* The order is taken on the soldier's spot and waits for it to clear
 * (legacy:12088-12115): the frame goes up once the soldier walks off. */
TEST(a_summon_waits_for_the_soldier_on_its_spot) {
    ASSERT_NOT_NULL(oq_world());
    int32_t sx = OQ_CX + 8, sy = OQ_CY + 8;
    int s = Units_Spawn(OQ_SOLDIER, 1, 0, sx, sy);
    int b = Units_Spawn(OQ_BUILDER, 1, 0, sx - 120, sy);
    ASSERT(s >= 0 && b >= 0);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, b, sx, sy, -1, OQ_SOLDIER, 0));
    oq_ticks(60);
    ASSERT_EQ_INT(0, oq_frames_of_at(OQ_SOLDIER, sx, sy));
    ASSERT_EQ_INT(1, Units_OrderMove(s, sx + 200, sy));
    int framed = 0;
    for (int t = 0; t < 300 && !framed; t++) {
        oq_ticks(1);
        framed = oq_frames_of_at(OQ_SOLDIER, sx, sy);
    }
    ASSERT_EQ_INT(1, framed);
    oq_end();
}

/* A Shift summons on the frame in hand is taken, waits for the unit
 * that frame becomes, and goes up once it walks off
 * (legacy:12088-12124). */
TEST(a_shift_summons_on_the_frame_in_hand_waits_for_it) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    int32_t sx = OQ_CX + 8, sy = OQ_CY + 8;
    int32_t py = oq_drawn_y(w, sx, sy, 0.0f);
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, sx - 64, sy);
    ASSERT(bd >= 0);
    SDL_SetModState(KMOD_NONE);
    Units_SelectSingle(bd);
    HUD_BeginBuildPlacement(OQ_SOLDIER);
    InGame_WorldClickOn(sx, py, -1, 0);
    oq_ticks(1);
    int first = oq_unit(bd)->build_target;
    ASSERT(first >= 0);
    ASSERT_EQ_INT(1, oq_frames_of_at(OQ_SOLDIER, sx, sy));
    HUD_BeginBuildPlacement(OQ_SOLDIER);
    InGame_WorldClickOn(sx, py, -1, IG_CLICK_SHIFT);
    HUD_ClearCommandMode();
    oq_ticks(1);
    ASSERT_EQ_INT(1, oq_legs_of(bd, UNIT_LEG_BUILD, OQ_SOLDIER));
    oq_until_done(first, 300);
    oq_ticks(2);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(0, oq_legs_of(bd, UNIT_LEG_BUILD, OQ_SOLDIER));
    oq_ticks(100);
    ASSERT_EQ_INT(0, oq_frames_of_at(OQ_SOLDIER, sx, sy));
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(1, Units_OrderMove(first, sx + 200, sy));
    int framed = 0;
    for (int t = 0; t < 200 && !framed; t++) {
        oq_ticks(1);
        framed = oq_frames_of_at(OQ_SOLDIER, sx, sy);
    }
    ASSERT_EQ_INT(1, framed);
    oq_end();
}

/* A plain summons is not moved off when done (legacy:12555-12559), so a
 * second one on its spot gives up once 30 looks, ten frames apart, have
 * gone by (legacy:12097-12116). */
TEST(a_summons_on_a_spot_its_last_one_keeps_gives_up) {
    ASSERT_NOT_NULL(oq_world());
    int32_t sx = OQ_CX + 8, sy = OQ_CY + 8;
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, sx - 64, sy);
    ASSERT(bd >= 0);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, sx, sy, -1, OQ_SOLDIER, 0));
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, sx, sy, -1, OQ_SOLDIER,
                                TAK_CMD_ARG_QUEUE));
    int first = oq_unit(bd)->build_target;
    ASSERT(first >= 0);
    oq_until_done(first, 300);
    oq_ticks(2);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    oq_ticks(30 * 20);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    oq_ticks(3 * 20);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(0, oq_frames_of_at(OQ_SOLDIER, sx, sy));
    ASSERT_EQ_INT(1, oq_count(OQ_SOLDIER));
    oq_end();
}

/* A building's cells refuse a summons at the cursor, and the order ends
 * at once rather than wait (legacy:12088-12098, 218797). */
TEST(a_summons_on_a_building_is_refused_at_once) {
    ASSERT_NOT_NULL(oq_world());
    int32_t sx = OQ_CX + 8, sy = OQ_CY + 8;
    ASSERT(Units_Spawn(OQ_HALL, 1, 0, sx, sy) >= 0);
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, sx - 120, sy);
    ASSERT(bd >= 0);
    ASSERT_EQ_INT(0, Units_IsBuildSiteClearFacing(OQ_SOLDIER, sx, sy, 0));
    ASSERT_EQ_INT(0, oq_command(TAK_CMD_BUILD, bd, sx, sy, -1, OQ_SOLDIER, 0));
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)oq_unit(bd)->cmd_kind);
    oq_end();
}

/* Where a summons waits on units, no frame goes up while any unit stands
 * on its cells. */
TEST(a_summons_frame_never_goes_up_on_a_unit) {
    ASSERT_NOT_NULL(oq_world());
    int32_t sx = OQ_CX + 8, sy = OQ_CY + 8;
    int s = Units_Spawn(OQ_ARCHER, 1, 0, sx, sy);
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, sx - 120, sy);
    ASSERT(s >= 0 && bd >= 0);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, sx, sy, -1, OQ_SOLDIER, 0));
    for (int t = 0; t < 400; t++) {
        oq_ticks(1);
        ASSERT_EQ_INT(0, oq_frames_of_at(OQ_SOLDIER, sx, sy));
    }
    /* It walked to the site while it waited. */
    int32_t dx = oq_unit(bd)->world_x - sx, dy = oq_unit(bd)->world_y - sy;
    ASSERT(dx * dx + dy * dy < 100 * 100);
    oq_end();
}

/* The looks are counted once the builder is in reach, so one sent from
 * far off still holds the order when it gets there (legacy:12063-12124). */
TEST(a_summons_from_far_off_counts_its_looks_from_reach) {
    ASSERT_NOT_NULL(oq_world());
    int32_t sx = OQ_CX + 8, sy = OQ_CY + 8;
    int s = Units_Spawn(OQ_ARCHER, 1, 0, sx, sy);
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, sx + 1000, sy + 900);
    ASSERT(s >= 0 && bd >= 0);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, sx, sy, -1, OQ_SOLDIER, 0));
    oq_ticks(32 * 20 + 40);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    ASSERT_EQ_INT(0, oq_frames_of_at(OQ_SOLDIER, sx, sy));
    ASSERT_EQ_INT(1, Units_OrderMove(s, sx + 200, sy));
    int framed = 0;
    for (int t = 0; t < 200 && !framed; t++) {
        oq_ticks(1);
        framed = oq_frames_of_at(OQ_SOLDIER, sx, sy);
    }
    ASSERT_EQ_INT(1, framed);
    oq_end();
}

/* A builder walled in short of its held summons gains no ground, so its
 * looks count and the order ends as one it cannot reach does
 * (legacy:12063-12070). */
TEST(a_summons_its_builder_cannot_reach_gives_up) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    for (int y = 56; y <= 64; y++)
        for (int x = 36; x <= 44; x++)
            if (x == 36 || x == 44 || y == 56 || y == 64)
                w->tnt.heightmap[y * w->tnt.height_w + x] = 250;
    TAK_PathCacheReset();
    int32_t sx = OQ_CX + 8, sy = OQ_CY + 8;
    int s = Units_Spawn(OQ_ARCHER, 1, 0, sx, sy);
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, 40 * 16 + 8, 60 * 16 + 8);
    ASSERT(s >= 0 && bd >= 0);
    ASSERT_EQ_INT(1, oq_command(TAK_CMD_BUILD, bd, sx, sy, -1, OQ_SOLDIER, 0));
    oq_ticks(60);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, (int)oq_unit(bd)->cmd_kind);
    int ended = 0;
    for (int t = 0; t < 1500 && !ended; t++) {
        oq_ticks(1);
        ended = oq_unit(bd)->cmd_kind == UNIT_CMD_NONE;
    }
    ASSERT_EQ_INT(1, ended);
    ASSERT_EQ_INT(0, oq_frames_of_at(OQ_SOLDIER, sx, sy));
    oq_end();
}

static int     g_oq_ghost_valid = -1;
static void oq_ghost(int def, int color, int32_t x, int32_t y, int valid,
                     int facing) {
    (void)def; (void)color; (void)x; (void)y; (void)facing;
    g_oq_ghost_valid = valid;
}

/* The ghost is green for a summons over a soldier and red for a hall
 * there (legacy:218797-218811). */
TEST(the_ghost_of_a_summons_is_green_over_a_soldier) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    int32_t sx = OQ_CX + 8, sy = OQ_CY + 8;
    int32_t py = oq_drawn_y(w, sx, sy, 0.0f);
    ASSERT(Units_Spawn(OQ_ARCHER, 1, 0, sx, sy) >= 0);
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, sx - 120, sy);
    ASSERT(bd >= 0);
    Units_SelectSingle(bd);
    TAK_Platform plat;
    memset(&plat, 0, sizeof plat);
    plat.renderer = (SDL_Renderer *)&plat;   /* only the hook draws */
    HUD_BuildGhostFn was = HUD_GetBuildGhostHook();
    HUD_SetBuildGhostHook(oq_ghost);
    HUD_BeginBuildPlacement(OQ_SOLDIER);
    HUD_DrawCommandCursor(&plat, 0, 0, sx, py);
    int summons = g_oq_ghost_valid;
    HUD_BeginBuildPlacement(OQ_HALL);
    HUD_DrawCommandCursor(&plat, 0, 0, sx, py);
    int hall = g_oq_ghost_valid;
    HUD_SetBuildGhostHook(was);
    ASSERT_EQ_INT(1, summons);
    ASSERT_EQ_INT(0, hall);
    oq_end();
}

/* A def with a move class takes that class's cells, so a 3x3 walker
 * snaps as 3x3 although its own footprint is unset
 * (legacy:163193-163195). */
TEST(a_summons_snaps_to_its_move_class_footprint) {
    GameWorld *w = oq_world();
    ASSERT_NOT_NULL(w);
    w->moveinfo.count = 2;
    strncpy(w->moveinfo.classes[1].name, "TESTBIG", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[1].footprint_x = 3;
    w->moveinfo.classes[1].footprint_z = 3;
    w->moveinfo.classes[1].max_slope = 30;
    UnitDef *d = (UnitDef *)Units_GetDef(OQ_ARCHER);
    ASSERT_NOT_NULL(d);
    strncpy(d->movement_class, "TESTBIG", sizeof(d->movement_class) - 1);
    d->footprint_x = 0;
    d->footprint_z = 0;
    int32_t x = OQ_CX + 5, y = OQ_CY + 5;
    Units_SnapBuildSite(OQ_ARCHER, &x, &y);
    /* The 3x3 cell block from 59 to 61, centred at 59 * 16 + 24. */
    ASSERT_EQ_INT(59 * 16 + 24, x);
    ASSERT_EQ_INT(59 * 16 + 24, y);
    oq_end();
}

/* The computer's site search still keeps off its own army, so it never
 * sets a summons to wait on it. */
TEST(the_computers_summons_site_is_one_no_unit_stands_on) {
    ASSERT_NOT_NULL(oq_world());
    int bd = Units_Spawn(OQ_BUILDER, 1, 0, OQ_CX, OQ_CY);
    ASSERT(bd >= 0);
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    ASSERT_EQ_INT(1, TAK_AI_DebugFindSite(bd, OQ_SOLDIER, &x0, &y0));
    ASSERT(Units_Spawn(OQ_ARCHER, 1, 0, x0, y0) >= 0);
    ASSERT_EQ_INT(1, TAK_AI_DebugFindSite(bd, OQ_SOLDIER, &x1, &y1));
    ASSERT(x1 != x0 || y1 != y0);
    oq_end();
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
    RUN(two_builders_finish_a_frame_in_half_the_time_for_the_same_mana);
    RUN(a_monarch_helps_a_frame_it_could_not_start);
    RUN(a_limited_builder_does_not_help_a_frame_off_its_list);
    RUN(a_limited_builder_helps_a_frame_on_its_list);
    RUN(a_builder_does_not_help_an_allys_frame);
    RUN(the_hammer_shows_only_for_a_helper_that_could_build_it);
    RUN(a_frame_nobody_selected_can_help_shows_no_select_hand);
    RUN(a_click_on_a_frame_sends_the_helpers_and_walks_the_rest);
    RUN(a_builder_goes_round_a_ridge_as_a_move_does);
    RUN(a_repairer_goes_round_a_ridge_as_a_move_does);
    RUN(a_builder_walking_out_of_a_pocket_keeps_its_frame);
    RUN(a_builder_closes_a_gap_in_a_row_from_its_own_side);
    RUN(a_dequeue_takes_a_builders_build_orders_of_the_def);
    RUN(a_right_click_on_a_builders_button_drops_that_kind);
    RUN(a_right_click_drops_the_hall_in_hand_and_leaves_its_frame);
    RUN(the_shift_overlay_plans_a_mixed_queue);
    RUN(a_patrol_in_hand_closes_through_its_start);
    RUN(the_shift_overlay_shows_a_barracks_rally);
    RUN(the_shift_overlay_hides_where_an_unseen_target_is);
    RUN(the_shift_overlay_draws_only_while_shift_is_held);
    RUN(the_shift_overlay_hides_a_dying_target_in_the_fog);
    RUN(queued_ghosts_settle_a_bounded_number_a_frame);
    RUN(a_flyer_is_picked_where_it_is_drawn_in_both_views);
    RUN(only_a_weapon_that_can_take_a_flyer_attacks_it);
    RUN(ctrl_z_selects_every_unit_of_a_type_the_selection_holds);
    RUN(ctrl_letters_select_by_category_and_ctrl_a_takes_all);
    RUN(ctrl_shift_digit_adds_a_group_to_the_selection);
    RUN(ctrl_u_selects_your_units_in_the_view);
    RUN(ctrl_on_a_builders_unit_button_summons_without_end);
    RUN(ctrl_through_both_clicks_summons_without_end);
    RUN(a_shift_click_queues_a_summons_and_nothing_goes_behind_it);
    RUN(a_right_click_ends_a_summons_without_end);
    RUN(a_summons_waits_for_a_unit_on_its_spot_and_ends_on_a_building);
    RUN(a_summons_gives_up_on_a_spot_a_unit_keeps);
    RUN(ctrl_on_a_buildings_button_places_it_once);
    RUN(a_summon_is_placed_where_a_soldier_stands);
    RUN(a_summon_waits_for_the_soldier_on_its_spot);
    RUN(a_shift_summons_on_the_frame_in_hand_waits_for_it);
    RUN(a_summons_on_a_spot_its_last_one_keeps_gives_up);
    RUN(a_summons_on_a_building_is_refused_at_once);
    RUN(a_summons_frame_never_goes_up_on_a_unit);
    RUN(a_summons_from_far_off_counts_its_looks_from_reach);
    RUN(a_summons_its_builder_cannot_reach_gives_up);
    RUN(the_ghost_of_a_summons_is_green_over_a_soldier);
    RUN(a_summons_snaps_to_its_move_class_footprint);
    RUN(the_computers_summons_site_is_one_no_unit_stands_on);
    TEST_REPORT();
}
