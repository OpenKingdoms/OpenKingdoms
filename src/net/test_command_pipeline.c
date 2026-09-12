/*
 * test_command_pipeline.c: the road from a click to a unit, with no
 * game data anywhere.
 *
 * The harness registers synthetic unit defs and a flat world, the same
 * way the movement tests do, so the executor, the queue and the seat
 * split can all be driven in CI.
 */

#include "test_framework.h"

#include "tak_ai.h"
#include "tak_battle_config.h"
#include "tak_command_emit.h"
#include "tak_command_exec.h"
#include "tak_command_queue.h"
#include "tak_commands.h"
#include "tak_economy.h"
#include "tak_fog.h"
#include "tak_hud.h"
#include "tak_ingame.h"
#include "tak_memory.h"
#include "tak_moveinfo.h"
#include "tak_occupancy.h"
#include "tak_pathing.h"
#include "tak_hpi.h"
#include "test_hpi_builder.h"
#include "tak_sim_rand.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ── the harness ───────────────────────────────────────────────────── */

#define CP_TILES   192      /* 16 px tiles per side, so a 3072 px map */
#define CP_GROUND  64       /* flat height, clear of the water line */

enum { CP_DEF_WALKER = 0, CP_DEF_ARCHER, CP_DEF_BUILDER, CP_DEF_CARRIER,
       CP_DEF_COUNT };

static void cp_fill_def(UnitDef *d, const char *name, const char *mclass,
                        float velocity, int health) {
    memset(d, 0, sizeof(*d));
    strncpy(d->unitname, name, sizeof(d->unitname) - 1);
    strncpy(d->display_name, name, sizeof(d->display_name) - 1);
    strncpy(d->category, "TEST WALKER", sizeof(d->category) - 1);
    strncpy(d->movement_class, mclass, sizeof(d->movement_class) - 1);
    d->max_health = health;
    d->sight_distance = 640;
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

static void cp_add_weapon(UnitDef *d) {
    d->num_weapons = 1;
    strncpy(d->weapons[0].name, "TESTBOW", sizeof(d->weapons[0].name) - 1);
    d->weapons[0].range = 160;
    d->weapons[0].reload_ticks = 60;
    d->weapons[0].damage = 10;
    d->weapons[0].velocity_pps = 400;
}

/* A flat world with an occupancy layer, one move class and three
 * synthetic defs. Seats 1 through 4 are human and each on its own
 * team. Returns NULL if anything could not be built. */
static uint32_t g_cp_seed = 0;

static GameWorld *cp_world(void) {
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    cfg.seed = g_cp_seed;
    cfg.line_of_sight = 0;
    for (int i = 0; i < 4; i++) {
        cfg.players[i].kind = TAK_SLOT_HUMAN;
        cfg.players[i].team = i + 1;
        cfg.players[i].color = i;
    }
    for (int i = 4; i < TAK_MAX_PLAYERS; i++) cfg.players[i].kind = TAK_SLOT_CLOSED;
    if (World_BeginLoad(NULL, &cfg, "synthetic", "aramon") != 0) return NULL;
    GameWorld *w = World_Get();
    if (!w) return NULL;
    w->map_pixels_w = CP_TILES * 16;
    w->map_pixels_h = CP_TILES * 16;
    w->viewport_w = 640;
    w->viewport_h = 480;
    w->water_height = 0;
    w->tnt.width_tiles = CP_TILES;
    w->tnt.height_tiles = CP_TILES;
    w->tnt.height_w = CP_TILES + 1;
    w->tnt.height_h = CP_TILES + 1;
    size_t hn = (size_t)w->tnt.height_w * (size_t)w->tnt.height_h;
    w->tnt.heightmap = (uint8_t *)tak_malloc(hn);
    if (!w->tnt.heightmap) return NULL;
    memset(w->tnt.heightmap, CP_GROUND, hn);

    memset(&w->moveinfo, 0, sizeof(w->moveinfo));
    w->moveinfo.count = 1;
    strncpy(w->moveinfo.classes[0].name, "TESTSMALL", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[0].footprint_x = 1;
    w->moveinfo.classes[0].footprint_z = 1;
    w->moveinfo.classes[0].max_slope = 30;

    if (!Occ_Ensure(w)) return NULL;
    TAK_PathCacheReset();
    World_MarkLoaded();

    UnitDef defs[CP_DEF_COUNT];
    cp_fill_def(&defs[CP_DEF_WALKER], "TESTSWORD", "TESTSMALL", 1.4f, 200);
    cp_fill_def(&defs[CP_DEF_ARCHER], "TESTARCHR", "TESTSMALL", 1.2f, 150);
    cp_add_weapon(&defs[CP_DEF_ARCHER]);
    cp_fill_def(&defs[CP_DEF_BUILDER], "TESTBUILD", "TESTSMALL", 1.0f, 300);
    defs[CP_DEF_BUILDER].cap_flags |= UNIT_CAP_BUILDER | UNIT_CAP_RECLAIM;
    defs[CP_DEF_BUILDER].worker_time = 20.0f;
    cp_fill_def(&defs[CP_DEF_CARRIER], "TESTBOAT", "TESTSMALL", 1.0f, 400);
    defs[CP_DEF_CARRIER].cap_flags |= UNIT_CAP_TRANSPORT;
    defs[CP_DEF_CARRIER].transport_capacity = 4;
    if (Units_DebugSetDefs(defs, CP_DEF_COUNT) != CP_DEF_COUNT) return NULL;

    Units_SetLocalPlayer(1);
    TAK_CmdQueue_Reset(0);
    return w;
}

static void cp_end(void) {
    Units_ClearInstances();
    World_End(NULL);
    TAK_PathCacheReset();
    TAK_CmdQueue_Reset(0);
    TAK_CmdQueue_SetObserver(NULL, NULL);
}

static const Unit *cp_unit(int handle) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    return (handle >= 0 && handle < count) ? &units[handle] : NULL;
}

/* One simulation tick in the order the game runs it: the orders due
 * this tick, then everything that moves. */
static void cp_tick(void) {
    TAK_CmdQueue_Run();
    Units_TickEngines();
}

/* Build one command by hand, the way a turn bundle delivers it. */
static TAK_GameCommand g_cmd;

static void cp_cmd(uint8_t type, uint8_t seat) {
    memset(&g_cmd, 0, sizeof(g_cmd));
    g_cmd.type = type;
    g_cmd.seat = seat;
}

static void cp_cmd_unit(int handle) {
    g_cmd.unit_ids[g_cmd.unit_count++] = Units_GetStableId(handle);
}

/* ── the ownership check ───────────────────────────────────────────── */

/* One command from seat 1 naming a unit of seat 1 and a unit of seat 2.
 * Only seat 1's unit takes the order. The check that makes this true
 * is the only one left in the whole order path. */
TEST(a_command_moves_only_the_units_its_seat_owns) {
    ASSERT_NOT_NULL(cp_world());
    int mine = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
    int theirs = Units_Spawn(CP_DEF_WALKER, 2, 1, 900, 800);
    ASSERT(mine >= 0 && theirs >= 0);

    cp_cmd(TAK_CMD_MOVE, 1);
    g_cmd.target_x = 1600;
    g_cmd.target_y = 1600;
    cp_cmd_unit(mine);
    cp_cmd_unit(theirs);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));

    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)cp_unit(mine)->cmd_kind);
    ASSERT_EQ_INT(1600, cp_unit(mine)->cmd_x);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(theirs)->cmd_kind);

    /* The same bytes stamped with seat 2 move the other unit and not
     * this one, so it really is the stamp that decides. */
    g_cmd.seat = 2;
    g_cmd.target_x = 400;
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)cp_unit(theirs)->cmd_kind);
    ASSERT_EQ_INT(400, cp_unit(theirs)->cmd_x);
    ASSERT_EQ_INT(1600, cp_unit(mine)->cmd_x);
    cp_end();
}

/* A seat outside the eight, and a command whose units have died. */
TEST(a_command_with_no_standing_units_does_nothing) {
    ASSERT_NOT_NULL(cp_world());
    int h = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
    ASSERT(h >= 0);
    uint32_t id = Units_GetStableId(h);

    cp_cmd(TAK_CMD_MOVE, 0);
    g_cmd.unit_ids[g_cmd.unit_count++] = id;
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    g_cmd.seat = TAK_MAX_PLAYERS + 1;
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));

    /* A stable id nobody carries. */
    cp_cmd(TAK_CMD_MOVE, 1);
    g_cmd.unit_ids[g_cmd.unit_count++] = id + 9999u;
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));

    /* A type this build cannot run. */
    cp_cmd(TAK_CMD_NONE, 1);
    cp_cmd_unit(h);
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    cp_end();
}

/* Every unit command reaches the unit, so no type is on the wire
 * without an executor branch behind it. */
TEST(the_executor_runs_every_unit_command) {
    ASSERT_NOT_NULL(cp_world());
    int archer = Units_Spawn(CP_DEF_ARCHER, 1, 0, 800, 800);
    int friend_ = Units_Spawn(CP_DEF_WALKER, 1, 0, 840, 800);
    int enemy = Units_Spawn(CP_DEF_WALKER, 2, 1, 900, 800);
    ASSERT(archer >= 0 && friend_ >= 0 && enemy >= 0);

    cp_cmd(TAK_CMD_ATTACK, 1);
    g_cmd.target_unit_id = Units_GetStableId(enemy);
    cp_cmd_unit(archer);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_CMD_ATTACK, (int)cp_unit(archer)->cmd_kind);
    ASSERT_EQ_INT(enemy, (int)cp_unit(archer)->target);

    cp_cmd(TAK_CMD_ATTACK_GROUND, 1);
    g_cmd.target_x = 1200; g_cmd.target_y = 1200;
    cp_cmd_unit(archer);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_CMD_ATTACK_GROUND, (int)cp_unit(archer)->cmd_kind);

    cp_cmd(TAK_CMD_PATROL, 1);
    g_cmd.target_x = 1000; g_cmd.target_y = 900;
    cp_cmd_unit(friend_);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_CMD_PATROL, (int)cp_unit(friend_)->cmd_kind);

    cp_cmd(TAK_CMD_GUARD, 1);
    g_cmd.target_unit_id = Units_GetStableId(friend_);
    cp_cmd_unit(archer);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_CMD_GUARD, (int)cp_unit(archer)->cmd_kind);

    cp_cmd(TAK_CMD_SET_AGGRO, 1);
    g_cmd.arg = UNIT_AGGRO_PASSIVE;
    cp_cmd_unit(archer);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_AGGRO_PASSIVE, (int)cp_unit(archer)->aggro_mode);

    cp_cmd(TAK_CMD_SET_WEAPON, 1);
    g_cmd.arg = 0;
    cp_cmd_unit(archer);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(0, (int)cp_unit(archer)->weapon_slot);

    cp_cmd(TAK_CMD_STOP, 1);
    cp_cmd_unit(archer);
    cp_cmd_unit(friend_);
    ASSERT_EQ_INT(2, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(archer)->cmd_kind);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(friend_)->cmd_kind);

    /* The special weapon with nothing under the click walks there. */
    cp_cmd(TAK_CMD_SPECIAL_WEAPON, 1);
    g_cmd.target_x = 1300; g_cmd.target_y = 700;
    cp_cmd_unit(friend_);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)cp_unit(friend_)->cmd_kind);
    ASSERT_EQ_INT(1300, cp_unit(friend_)->cmd_x);

    /* A heal names its target by id, so an enemy's id is refused. */
    cp_cmd(TAK_CMD_REPAIR, 1);
    g_cmd.target_unit_id = Units_GetStableId(enemy);
    cp_cmd_unit(archer);
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));

    /* Giving units away hands them to the other seat, which is the
     * original's own player action (legacy:155766-155797). */
    cp_cmd(TAK_CMD_GIVE_UNITS, 1);
    g_cmd.arg = 3;
    cp_cmd_unit(friend_);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(3, (int)cp_unit(friend_)->player_id);
    /* And seat 1 can no longer order it. */
    cp_cmd(TAK_CMD_MOVE, 1);
    g_cmd.target_x = 2000; g_cmd.target_y = 2000;
    cp_cmd_unit(friend_);
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    cp_end();
}

/* The commands about a seat rather than its units. */
TEST(the_executor_runs_the_seat_commands) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    int mine = Units_Spawn(CP_DEF_ARCHER, 1, 0, 800, 800);
    int theirs = Units_Spawn(CP_DEF_WALKER, 2, 1, 900, 800);
    ASSERT(mine >= 0 && theirs >= 0);
    /* Different teams, so they start as enemies. */
    ASSERT_EQ_INT(1, Units_PlayersAreEnemies(1, 2));

    /* One side declaring peace is not enough. */
    cp_cmd(TAK_CMD_ALLIANCE, 1);
    g_cmd.arg = 2 | (1 << 8);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, Units_PlayersAreEnemies(1, 2));
    cp_cmd(TAK_CMD_ALLIANCE, 2);
    g_cmd.arg = 1 | (1 << 8);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(0, Units_PlayersAreEnemies(1, 2));
    /* And an allied unit takes no attack order. */
    cp_cmd(TAK_CMD_ATTACK, 1);
    g_cmd.target_unit_id = Units_GetStableId(theirs);
    cp_cmd_unit(mine);
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));

    cp_cmd(TAK_CMD_SHARE_VISION, 1);
    g_cmd.arg = 2 | (1 << 8);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, (int)w->share_vision[1][2]);
    cp_cmd(TAK_CMD_SHARE_UNITS, 1);
    g_cmd.arg = 2 | (1 << 8);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, (int)w->share_units[1][2]);
    cp_cmd(TAK_CMD_SHARE_MANA, 1);
    g_cmd.arg = 2 | (1 << 8);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, (int)w->share_mana[1][2]);
    /* Nobody allies or shares with themselves. */
    cp_cmd(TAK_CMD_ALLIANCE, 1);
    g_cmd.arg = 1 | (1 << 8);
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));

    /* A mana gift moves the exact amount, in 16.16. */
    Economy_AdjustCaps(&w->economy, 1, 1000, 0.0f);
    /* Room under the recipient's cap, or the gift would be clamped
     * away and the test would prove nothing. */
    Economy_AdjustCaps(&w->economy, 2, 2000, 0.0f);
    Economy_TrySpend(&w->economy, 2, 1000);
    int before_from = Economy_GetMana(&w->economy, 1);
    int before_to = Economy_GetMana(&w->economy, 2);
    cp_cmd(TAK_CMD_MANA_GIFT, 1);
    g_cmd.arg = 2;
    g_cmd.target_x = 250 << 16;
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(before_from - 250, Economy_GetMana(&w->economy, 1));
    ASSERT_EQ_INT(before_to + 250, Economy_GetMana(&w->economy, 2));
    /* More than the seat holds is refused outright. */
    g_cmd.target_x = 100000 << 16;
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));

    /* A power code does nothing in a room that did not allow them. */
    ASSERT_EQ_INT(0, w->cfg.power_codes);
    cp_cmd(TAK_CMD_POWER_CODE, 1);
    g_cmd.arg = 1;
    g_cmd.build_type_id = 100;
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    w->cfg.power_codes = 1;
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));

    /* Resigning takes, once, and the seat issues nothing after it. */
    cp_cmd(TAK_CMD_RESIGN, 1);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, (int)w->resigned[1]);
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    cp_cmd(TAK_CMD_MOVE, 1);
    g_cmd.target_x = 1600; g_cmd.target_y = 1600;
    cp_cmd_unit(mine);
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    cp_end();
}

/* ── the stable id index ───────────────────────────────────────────── */

/* A command names up to 256 units. Finding each one has to cost a
 * handful of probes however many units are on the map, or an eight
 * player battle spends its tick looking units up. */
TEST(a_stable_id_is_found_in_a_few_probes) {
    ASSERT_NOT_NULL(cp_world());
    enum { CP_MANY = 1500 };
    static uint32_t ids[CP_MANY];
    int spawned = 0;
    for (int i = 0; i < CP_MANY; i++) {
        int h = Units_Spawn(CP_DEF_WALKER, (i % 4) + 1, i % 4,
                            64 + (i % 40) * 64, 64 + (i / 40) * 64);
        if (h < 0) break;
        ids[spawned++] = Units_GetStableId(h);
    }
    ASSERT(spawned == CP_MANY);

    int worst = 0;
    for (int i = 0; i < spawned; i++) {
        int h = Units_FindByStableId(ids[i]);
        ASSERT_EQ_INT((int)ids[i], (int)Units_GetStableId(h));
        int probes = Units_DebugStableIdProbes();
        if (probes > worst) worst = probes;
    }
    printf("(worst %d probes over %d units) ", worst, spawned);
    ASSERT(worst <= 8);

    /* An id nobody carries is refused just as cheaply. */
    ASSERT_EQ_INT(-1, Units_FindByStableId(ids[spawned - 1] + 100000u));
    ASSERT(Units_DebugStableIdProbes() <= 8);
    ASSERT_EQ_INT(-1, Units_FindByStableId(0));
    cp_end();
}

/* ── the queue ─────────────────────────────────────────────────────── */

/* An order submitted with a delay reaches its unit on exactly the tick
 * it was scheduled for, and not one before. */
TEST(an_order_waits_for_its_tick) {
    ASSERT_NOT_NULL(cp_world());
    int h = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
    ASSERT(h >= 0);
    TAK_CmdQueue_SetDelay(3);
    cp_cmd(TAK_CMD_MOVE, 0);    /* Submit stamps the seat */
    g_cmd.target_x = 1600; g_cmd.target_y = 800;
    cp_cmd_unit(h);
    ASSERT_EQ_INT(0, TAK_CmdQueue_Submit(1, &g_cmd));
    ASSERT_EQ_INT(1, TAK_CmdQueue_Pending());
    for (int t = 0; t < 3; t++) {
        ASSERT_EQ_INT(t, (int)TAK_CmdQueue_Tick());
        ASSERT_EQ_INT(0, TAK_CmdQueue_Run());
        ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(h)->cmd_kind);
    }
    ASSERT_EQ_INT(1, TAK_CmdQueue_Run());
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)cp_unit(h)->cmd_kind);
    ASSERT_EQ_INT(1, (int)TAK_CmdQueue_LastApplied()->seat);
    ASSERT_EQ_INT(3, (int)TAK_CmdQueue_LastApplied()->tick);
    ASSERT_EQ_INT(0, TAK_CmdQueue_Pending());
    cp_end();
}

static uint8_t g_seen_seat[16];
static uint16_t g_seen_arg[16];
static int g_seen_n;

static void cp_see(const TAK_GameCommand *cmd, void *user) {
    (void)user;
    if (g_seen_n < 16) {
        g_seen_seat[g_seen_n] = cmd->seat;
        g_seen_arg[g_seen_n] = cmd->arg;
        g_seen_n++;
    }
}

/* Inside one tick the order is seat first, then arrival, whatever
 * order the packets came in. Eight machines agree on that. */
TEST(a_tick_runs_seat_by_seat_then_by_arrival) {
    ASSERT_NOT_NULL(cp_world());
    static const uint8_t seats[5] = { 3, 1, 2, 1, 3 };
    for (int i = 0; i < 5; i++) {
        cp_cmd(TAK_CMD_RESIGN, seats[i]);
        g_cmd.tick = 0;
        g_cmd.arg = (uint16_t)i;       /* which one this was */
        ASSERT_EQ_INT(0, TAK_CmdQueue_SubmitAt(&g_cmd));
    }
    /* One for a later tick, which this one must not touch. */
    cp_cmd(TAK_CMD_RESIGN, 1);
    g_cmd.tick = 1;
    g_cmd.arg = 99;
    ASSERT_EQ_INT(0, TAK_CmdQueue_SubmitAt(&g_cmd));

    g_seen_n = 0;
    TAK_CmdQueue_SetObserver(cp_see, NULL);
    ASSERT_EQ_INT(5, TAK_CmdQueue_Run());
    static const uint8_t want_seat[5] = { 1, 1, 2, 3, 3 };
    static const uint16_t want_arg[5] = { 1, 3, 2, 0, 4 };
    ASSERT_EQ_INT(5, g_seen_n);
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ_INT(want_seat[i], g_seen_seat[i]);
        ASSERT_EQ_INT(want_arg[i], g_seen_arg[i]);
    }
    ASSERT_EQ_INT(1, TAK_CmdQueue_Run());
    ASSERT_EQ_INT(99, g_seen_arg[5]);
    cp_end();
}

/* A click is a command. The unit it orders does nothing until the tick
 * after, even at zero delay, which is what single player shares with a
 * match: no order is ever applied mid frame. */
TEST(a_click_reaches_the_unit_on_the_next_tick) {
    ASSERT_NOT_NULL(cp_world());
    int mine = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
    int theirs = Units_Spawn(CP_DEF_WALKER, 2, 1, 1600, 1600);
    ASSERT(mine >= 0 && theirs >= 0);

    Units_SelectSingle(mine);
    InGame_WorldClick(1200, 1000, 0);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(mine)->cmd_kind);
    ASSERT_EQ_INT(1, TAK_CmdQueue_Pending());
    TAK_CmdQueue_Run();
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)cp_unit(mine)->cmd_kind);
    ASSERT_EQ_INT(1200, cp_unit(mine)->cmd_x);
    ASSERT_EQ_INT(1000, cp_unit(mine)->cmd_y);

    /* The command carried the local seat, and nobody else's unit moved. */
    ASSERT_EQ_INT(1, (int)TAK_CmdQueue_LastApplied()->seat);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(theirs)->cmd_kind);
    cp_end();
}

/* ── transports ────────────────────────────────────────────────────── */

/* A drag in load mode names the transport first and the riders after
 * it. The ownership check drops a rider of another seat, and the rest
 * are picked up in the order they were named (legacy:238654-238692). */
TEST(a_load_drag_boards_every_rider_it_names) {
    ASSERT_NOT_NULL(cp_world());
    int carrier = Units_Spawn(CP_DEF_CARRIER, 1, 0, 800, 800);
    int r[3];
    for (int i = 0; i < 3; i++) r[i] = Units_Spawn(CP_DEF_WALKER, 1, 0, 700, 760 + i * 40);
    int foe = Units_Spawn(CP_DEF_WALKER, 2, 1, 700, 900);
    ASSERT(carrier >= 0 && r[0] >= 0 && r[1] >= 0 && r[2] >= 0 && foe >= 0);

    cp_cmd(TAK_CMD_LOAD_UNITS, 1);
    cp_cmd_unit(carrier);
    cp_cmd_unit(r[0]);
    cp_cmd_unit(r[1]);
    cp_cmd_unit(foe);
    cp_cmd_unit(r[2]);
    ASSERT_EQ_INT(3, TAK_CommandExec_Apply(&g_cmd));
    int16_t q[8];
    ASSERT_EQ_INT(3, Units_GetLoadQueue(carrier, q, 8));
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_INT(r[i], q[i]);
        ASSERT_EQ_INT(UNIT_CMD_BOARD, (int)cp_unit(r[i])->cmd_kind);
    }
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(foe)->cmd_kind);

    /* Stamped with the other seat, the transport is dropped by the
     * check and nobody has anything to board. */
    g_cmd.seat = 2;
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    cp_end();
}

/* The same through the screen: select the transport, arm Load and drag
 * a box. Nothing moves until the tick. */
TEST(a_drag_on_screen_loads_on_the_next_tick) {
    ASSERT_NOT_NULL(cp_world());
    int carrier = Units_Spawn(CP_DEF_CARRIER, 1, 0, 800, 800);
    int r0 = Units_Spawn(CP_DEF_WALKER, 1, 0, 700, 760);
    int r1 = Units_Spawn(CP_DEF_WALKER, 1, 0, 700, 800);
    int foe = Units_Spawn(CP_DEF_WALKER, 2, 1, 700, 840);
    ASSERT(carrier >= 0 && r0 >= 0 && r1 >= 0 && foe >= 0);
    Units_SelectSingle(carrier);
    HUD_SetCommandMode(HUD_CMD_LOAD);
    InGame_WorldDrag(0, 0, CP_TILES * 16, CP_TILES * 16, 0);
    int16_t q[8];
    ASSERT_EQ_INT(0, Units_GetLoadQueue(carrier, q, 8));
    ASSERT_EQ_INT(HUD_CMD_NONE, HUD_GetCommandMode());
    TAK_CmdQueue_Run();
    ASSERT_EQ_INT(2, Units_GetLoadQueue(carrier, q, 8));
    ASSERT_EQ_INT(r0, q[0]);
    ASSERT_EQ_INT(r1, q[1]);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(foe)->cmd_kind);
    cp_end();
}

/* ── a recorded session replays exactly ────────────────────────────── */

#define CP_REPLAY_TICKS  900
#define CP_REPLAY_N      (CP_REPLAY_TICKS / 60)
#define CP_REPLAY_MAX    16

static TAK_GameCommand g_rec[CP_REPLAY_MAX];
static int g_rec_n;

static void cp_record(const TAK_GameCommand *cmd, void *user) {
    (void)user;
    if (g_rec_n < CP_REPLAY_MAX) g_rec[g_rec_n++] = *cmd;
}

static void cp_replay_spawn(int *out) {
    for (int i = 0; i < 8; i++) {
        out[i] = Units_Spawn(i < 4 ? CP_DEF_WALKER : CP_DEF_ARCHER,
                             i < 4 ? 1 : 2, i < 4 ? 0 : 1,
                             800 + (i % 4) * 48, i < 4 ? 800 : 1400);
        Units_DebugSetAggro(out[i], UNIT_AGGRO_PASSIVE);
    }
}

/* Live: a player clicks through the screens and the queue records
 * every command it applies. Replay: a fresh world takes only the
 * recording. The two must hash the same at every sample, which is the
 * single player session and its replay agreeing exactly. */
static int cp_replay_run(uint32_t *out, int live) {
    if (!cp_world()) return 0;
    int h[8];
    cp_replay_spawn(h);
    if (live) {
        g_rec_n = 0;
        TAK_CmdQueue_SetObserver(cp_record, NULL);
    } else {
        for (int i = 0; i < g_rec_n; i++) {
            if (TAK_CmdQueue_SubmitAt(&g_rec[i]) != 0) return 0;
        }
    }
    for (int t = 0; t < CP_REPLAY_TICKS; t++) {
        if (live && t == 10) {
            Units_SelectSingle(h[0]);
            Units_SelectAdd(h[1]);
            Units_SelectAdd(h[2]);
            InGame_WorldClick(1500, 1100, 0);
        }
        if (live && t == 200) {
            Units_SelectSingle(h[3]);
            InGame_WorldClick(600, 1300, 0);
        }
        if (live && t == 400) {
            Units_SelectSingle(h[1]);
            TAK_Cmd_EmitSelection(TAK_CMD_STOP, 0, 0, -1, 0, 0);
            Units_SetLocalPlayer(2);
            Units_SelectSingle(h[5]);
            Units_SelectAdd(h[6]);
            TAK_Cmd_EmitSelection(TAK_CMD_MOVE, 900, 700, -1, 0, 0);
            Units_SetLocalPlayer(1);
        }
        cp_tick();
        if ((t + 1) % 60 == 0) {
            out[t / 60] = Units_DebugStateHash() ^
                          (uint32_t)TAK_AI_DebugStateHash();
        }
    }
    cp_end();
    return 1;
}

TEST(a_recorded_session_replays_to_the_same_hashes) {
    static uint32_t live[CP_REPLAY_N], replay[CP_REPLAY_N];
    ASSERT(cp_replay_run(live, 1));
    ASSERT_EQ_INT(4, g_rec_n);
    ASSERT(cp_replay_run(replay, 0));
    int moved = 0;
    for (int i = 1; i < CP_REPLAY_N; i++) if (live[i] != live[i - 1]) moved = 1;
    ASSERT(moved);      /* a stream that never changes would pass blind */
    for (int i = 0; i < CP_REPLAY_N; i++) {
        ASSERT_EQ_INT((int)live[i], (int)replay[i]);
    }
}

/* ── the local seat stays out of the simulation ────────────────────── */

/* A seat nobody plays pursues. A human's idle army never does, whichever
 * seat this machine plays. Before, every unit not of seat 1 walked at
 * the nearest enemy, which in a match is every other human's army.
 * The pursuit only runs for armed units that may auto-target, so these
 * are defensive archers, spaced past their 160 px reach and inside the
 * 640 px pursuit radius. */
TEST(the_pursuit_never_moves_a_human_army) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    w->cfg.players[2].kind = TAK_SLOT_CLOSED;   /* seat 3: a mission army */
    int mine = Units_Spawn(CP_DEF_ARCHER, 1, 0, 1000, 800);
    int human = Units_Spawn(CP_DEF_ARCHER, 2, 1, 700, 800);
    int army = Units_Spawn(CP_DEF_ARCHER, 3, 2, 1300, 800);
    ASSERT(mine >= 0 && human >= 0 && army >= 0);
    Units_DebugSetAggro(mine, UNIT_AGGRO_DEFENSIVE);
    Units_DebugSetAggro(human, UNIT_AGGRO_DEFENSIVE);
    Units_DebugSetAggro(army, UNIT_AGGRO_DEFENSIVE);
    for (int t = 0; t < 5; t++) cp_tick();
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(mine)->cmd_kind);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(human)->cmd_kind);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)cp_unit(army)->cmd_kind);
    cp_end();
}

/* Seat 1 beaten does not end a battle seats 2 and 3 are still fighting.
 * Seat 1 sees its defeat, and the battle ends for everyone when one side
 * is left. */
TEST(the_battle_goes_on_while_two_sides_still_stand) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    int u1 = Units_Spawn(CP_DEF_WALKER, 1, 0, 400, 400);
    int u2 = Units_Spawn(CP_DEF_WALKER, 2, 1, 2400, 400);
    int u3 = Units_Spawn(CP_DEF_WALKER, 3, 2, 400, 2400);
    ASSERT(u1 >= 0 && u2 >= 0 && u3 >= 0);
    InGame_DebugRunSimTicks(2);
    ASSERT_EQ_INT(0, w->skirmish_game_over);

    Units_EliminatePlayer(1, -1);
    InGame_DebugRunSimTicks(2);
    ASSERT_EQ_INT(0, w->skirmish_game_over);
    ASSERT_EQ_INT(-1, w->skirmish_local_result);

    Units_EliminatePlayer(2, -1);
    InGame_DebugRunSimTicks(2);
    ASSERT_EQ_INT(1, w->skirmish_game_over);
    ASSERT_EQ_INT(Units_PlayerTeamId(3), w->skirmish_winner_team);
    cp_end();
}

/* A unit script reads what PLAY-SOUND returns, so the answer cannot
 * depend on what this machine has selected or can see. */
TEST(a_script_hears_the_same_answer_on_every_machine) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    int theirs = Units_Spawn(CP_DEF_WALKER, 2, 1, 2800, 2800);
    ASSERT(theirs >= 0);
    Units_SelectSingle(-1);
    /* A chatty category, and this machine has not selected the unit. */
    ASSERT_EQ_INT(1, Units_DebugCobPlaySound(theirs, "TESTSND", 0));
    /* A positional one, with the unit out of this seat's sight. */
    w->cfg.line_of_sight = 1;
    ASSERT_EQ_INT(0, Fog_Init(w));
    ASSERT_EQ_INT(1, Units_DebugCobPlaySound(theirs, "TESTSND", 3));
    /* A sound the script never named is still no sound. */
    ASSERT_EQ_INT(0, Units_DebugCobPlaySound(theirs, "", 3));
    cp_end();
}

/* The same click played from seat 2 orders seat 2's army and carries
 * seat 2. Before, the screens asked whether a unit belonged to seat 1,
 * so a player in any other seat could select and order nothing. */
TEST(a_click_from_seat_two_orders_seat_twos_army) {
    ASSERT_NOT_NULL(cp_world());
    int mine = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
    int theirs = Units_Spawn(CP_DEF_WALKER, 2, 1, 1600, 1600);
    ASSERT(mine >= 0 && theirs >= 0);
    Units_SetLocalPlayer(2);
    Units_SelectSingle(theirs);
    ASSERT_EQ_INT(1, Units_SelectionOwnedCount());
    InGame_WorldClick(2000, 1400, 0);
    TAK_CmdQueue_Run();
    ASSERT_EQ_INT(2, (int)TAK_CmdQueue_LastApplied()->seat);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)cp_unit(theirs)->cmd_kind);
    ASSERT_EQ_INT(2000, cp_unit(theirs)->cmd_x);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(mine)->cmd_kind);
    Units_SetLocalPlayer(1);
    cp_end();
}

#define CP_SEAT_TICKS  1500
#define CP_SEAT_N      (CP_SEAT_TICKS / 60)

/* The same battle, played from the machine of seat `local`. Each machine
 * selects its own units, but the commands are shared and stamped as a
 * turn bundle stamps them, so every sample must agree. */
static int cp_seat_run(uint32_t *out, int local) {
    GameWorld *w = cp_world();
    if (!w) return 0;
    w->cfg.players[2].kind = TAK_SLOT_CLOSED;   /* seat 3: a mission army */
    int h[9];
    for (int i = 0; i < 9; i++) {
        int seat = i / 3 + 1;
        int32_t x = seat == 3 ? 1300 : 700 + (i % 3) * 48;
        int32_t y = seat == 2 ? 1300 : 700 + (i % 3) * 48;
        h[i] = Units_Spawn(i % 3 == 0 ? CP_DEF_ARCHER : CP_DEF_WALKER,
                           seat, seat - 1, x, y);
        if (h[i] < 0) return 0;
    }
    Units_SetLocalPlayer(local);
    /* What this machine has selected is its own business. */
    Units_SelectSingle(h[(local - 1) * 3]);
    Units_SelectAdd(h[(local - 1) * 3 + 1]);

    static const struct {
        uint32_t tick; uint8_t seat, type; int unit; int32_t x, y;
    } bundle[] = {
        {  30, 1, TAK_CMD_MOVE,   0, 1400, 1000 },
        {  30, 2, TAK_CMD_MOVE,   3, 1000, 1200 },
        { 300, 1, TAK_CMD_PATROL, 1, 1200,  700 },
        { 600, 2, TAK_CMD_RESIGN, -1,   0,    0 },
        { 900, 1, TAK_CMD_RESIGN, -1,   0,    0 },
    };
    for (size_t i = 0; i < sizeof(bundle) / sizeof(bundle[0]); i++) {
        cp_cmd(bundle[i].type, bundle[i].seat);
        g_cmd.tick = bundle[i].tick;
        g_cmd.target_x = bundle[i].x;
        g_cmd.target_y = bundle[i].y;
        if (bundle[i].unit >= 0) cp_cmd_unit(h[bundle[i].unit]);
        if (TAK_CmdQueue_SubmitAt(&g_cmd) != 0) return 0;
    }
    for (int t = 0; t < CP_SEAT_TICKS; t++) {
        InGame_DebugRunSimTicks(1);
        if ((t + 1) % 60 == 0) {
            uint32_t v = Units_DebugStateHash() ^
                         (uint32_t)TAK_AI_DebugStateHash();
            v ^= (uint32_t)w->skirmish_game_over * 0x9e3779b1u;
            v ^= (uint32_t)w->skirmish_winner_team * 0x85ebca6bu;
            v ^= (uint32_t)w->skirmish_end_tick;
            out[t / 60] = v;
        }
    }
    int over = w->skirmish_game_over;
    Units_SetLocalPlayer(1);
    cp_end();
    return over ? 1 : 0;    /* the scenario has to reach its end */
}

/* The plan's seat invariance check: the local seat never reaches the
 * simulation, so seat 1 and seat 3 see the same battle tick for tick. */
TEST(the_same_battle_from_seat_one_and_seat_three_agrees) {
    static uint32_t a[CP_SEAT_N], b[CP_SEAT_N];
    ASSERT(cp_seat_run(a, 1));
    ASSERT(cp_seat_run(b, 3));
    int moved = 0;
    for (int i = 1; i < CP_SEAT_N; i++) if (a[i] != a[i - 1]) moved = 1;
    ASSERT(moved);
    for (int i = 0; i < CP_SEAT_N; i++) ASSERT_EQ_INT((int)a[i], (int)b[i]);
}

/* ── the session seed ──────────────────────────────────────────────── */

static int cp_seeded_draws(uint32_t seed, uint32_t *out, int n,
                           unsigned int *ai_hash) {
    g_cp_seed = seed;
    GameWorld *w = cp_world();
    g_cp_seed = 0;
    if (!w) return 0;
    *ai_hash = TAK_AI_DebugStateHash();
    for (int i = 0; i < n; i++) out[i] = World_Rand(1000);
    cp_end();
    return 1;
}

/* The seed in the start message decides every draw: two machines given
 * the same one agree on the script rolls and the AI, and another seed
 * plays a different battle. */
TEST(the_session_seed_decides_every_draw) {
    uint32_t a[8], b[8], c[8];
    unsigned int ha = 0, hb = 0, hc = 0;
    ASSERT(cp_seeded_draws(1234u, a, 8, &ha));
    ASSERT(cp_seeded_draws(1234u, b, 8, &hb));
    ASSERT(cp_seeded_draws(99u, c, 8, &hc));
    int differs = 0;
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ_INT((int)a[i], (int)b[i]);
        if (a[i] != c[i]) differs = 1;
    }
    ASSERT(differs);
    ASSERT_EQ_INT((int)ha, (int)hb);
    ASSERT(ha != hc);
}

/* ── the def order ─────────────────────────────────────────────────── */

#ifdef _WIN32
#include <direct.h>
#define cp_mkdir(p) _mkdir(p)
#define cp_rmdir(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define cp_mkdir(p) mkdir(p, 0755)
#define cp_rmdir(p) rmdir(p)
#endif

#define CP_ORDER_DIR  "cp_order_tmp"
#define CP_LOOSE_DIR  CP_ORDER_DIR "/loose"
#define CP_MENU_DIR   CP_LOOSE_DIR "/canbuild/midbuild"

/* File names that disagree with the unitnames, so the order the files
 * are listed in and the order of the names differ. */
static const char CP_FBI_ZED[] =
    "[UNITINFO]\n{\n\tUnitName=ZED;\n\tName=Zed;\n}\n";
static const char CP_FBI_ALPHA[] =
    "[UNITINFO]\n{\n\tUnitName=ALPHA;\n\tName=Alpha;\n}\n";
static const char CP_FBI_BUILD[] =
    "[UNITINFO]\n{\n\tUnitName=MIDBUILD;\n\tName=Builder;\n"
    "\tBuilder=1;\n\tWorkerTime=10;\n}\n";
static const char CP_MENU_TDF[] = "[MENU]\n{\n\tPriority=1;\n}\n";

static void cp_order_clean(void) {
    VFS_Shutdown();
    remove(CP_ORDER_DIR "/a.hpi");
    remove(CP_ORDER_DIR "/b.hpi");
    remove(CP_MENU_DIR "/alpha.tdf");
    remove(CP_MENU_DIR "/zed.tdf");
    cp_rmdir(CP_MENU_DIR);
    cp_rmdir(CP_LOOSE_DIR "/canbuild");
    cp_rmdir(CP_LOOSE_DIR);
    cp_rmdir(CP_ORDER_DIR);
}

static void cp_order_begin(void) {
    cp_order_clean();
    cp_mkdir(CP_ORDER_DIR);
    cp_mkdir(CP_LOOSE_DIR);
    cp_mkdir(CP_LOOSE_DIR "/canbuild");
    cp_mkdir(CP_MENU_DIR);
}

static void cp_order_end(void) {
    cp_order_clean();
    Units_FreeDefs();
}

/* One archive holding all three defs, listed a, m, z. */
static int cp_order_one_archive(void) {
    TestHPIEntry e[] = {
        { "units/a.fbi", CP_FBI_ZED, 100, 0 },
        { "units/m.fbi", CP_FBI_BUILD, 100, 0 },
        { "units/z.fbi", CP_FBI_ALPHA, 100, 0 },
    };
    VFS_Shutdown();
    remove(CP_ORDER_DIR "/b.hpi");
    return test_write_hpi(CP_ORDER_DIR "/a.hpi", e, 3);
}

/* The same defs split over two archives. The VFS lists the archive
 * mounted last first, so z comes before a. */
static int cp_order_two_archives(void) {
    TestHPIEntry a[] = {
        { "units/a.fbi", CP_FBI_ZED, 100, 0 },
        { "units/m.fbi", CP_FBI_BUILD, 100, 0 },
    };
    TestHPIEntry b[] = { { "units/z.fbi", CP_FBI_ALPHA, 100, 0 } };
    VFS_Shutdown();
    return test_write_hpi(CP_ORDER_DIR "/a.hpi", a, 2) |
           test_write_hpi(CP_ORDER_DIR "/b.hpi", b, 1);
}

/* The builder's menu: one loose entry, or none. */
static void cp_order_menu(const char *entry) {
    remove(CP_MENU_DIR "/alpha.tdf");
    remove(CP_MENU_DIR "/zed.tdf");
    if (!entry) return;
    char path[128];
    snprintf(path, sizeof(path), CP_MENU_DIR "/%s.tdf", entry);
    FILE *f = fopen(path, "wb");
    if (f) { fputs(CP_MENU_TDF, f); fclose(f); }
}

static int cp_order_load(void) {
    VFS_Shutdown();
    if (VFS_Init(CP_ORDER_DIR, CP_LOOSE_DIR) != 0) return -1;
    return Units_LoadDefs();
}

static int cp_menu_of(const char *builder, int *out, int max_out) {
    return Units_GetBuildables(Units_FindDefByName(builder), out, max_out);
}

/* A def's index is what a command and the AI name it by, so every
 * machine has to agree on it. It follows the unitname, not the order
 * the archives and the loose tree list the files in. */
TEST(the_def_order_does_not_depend_on_the_archives) {
    cp_order_begin();
    ASSERT_EQ_INT(0, cp_order_one_archive());
    ASSERT_EQ_INT(3, cp_order_load());
    int alpha = Units_FindDefByName("ALPHA");
    int mid = Units_FindDefByName("MIDBUILD");
    int zed = Units_FindDefByName("ZED");
    ASSERT_EQ_INT(0, cp_order_two_archives());
    ASSERT_EQ_INT(3, cp_order_load());
    ASSERT_EQ_INT(alpha, Units_FindDefByName("ALPHA"));
    ASSERT_EQ_INT(mid, Units_FindDefByName("MIDBUILD"));
    ASSERT_EQ_INT(zed, Units_FindDefByName("ZED"));
    ASSERT_EQ_INT(0, alpha);
    ASSERT_EQ_INT(1, mid);
    ASSERT_EQ_INT(2, zed);
    cp_order_end();
}

/* A new load reads its own build menus. They sat in a static cache no
 * load cleared, so a builder kept the menu of the first battle that
 * asked, def indices and all. */
TEST(a_new_load_reads_its_own_build_menus) {
    int menu[8];
    cp_order_begin();
    ASSERT_EQ_INT(0, cp_order_one_archive());
    cp_order_menu("alpha");
    ASSERT_EQ_INT(3, cp_order_load());
    ASSERT_EQ_INT(1, cp_menu_of("MIDBUILD", menu, 8));
    ASSERT_EQ_INT(Units_FindDefByName("ALPHA"), menu[0]);

    cp_order_menu("zed");
    ASSERT_EQ_INT(3, cp_order_load());
    ASSERT_EQ_INT(1, cp_menu_of("MIDBUILD", menu, 8));
    ASSERT_EQ_INT(Units_FindDefByName("ZED"), menu[0]);
    cp_order_end();
}

/* Every menu is read when the match loads, so no tick reads a file. A
 * menu read at load outlives its file. */
TEST(every_build_menu_is_read_when_the_match_loads) {
    int menu[8];
    cp_order_begin();
    ASSERT_EQ_INT(0, cp_order_one_archive());
    cp_order_menu("alpha");
    ASSERT_EQ_INT(3, cp_order_load());
    ASSERT_EQ_INT(1, Units_LoadAllBuildables());
    cp_order_menu(NULL);
    ASSERT_EQ_INT(1, cp_menu_of("MIDBUILD", menu, 8));
    ASSERT_EQ_INT(Units_FindDefByName("ALPHA"), menu[0]);
    cp_order_end();
}

/* The handshake compares one hash of the def order and every menu. Two
 * layouts of the same content agree, and a different menu does not. */
TEST(the_content_hash_covers_the_order_and_the_menus) {
    cp_order_begin();
    cp_order_menu("alpha");
    ASSERT_EQ_INT(0, cp_order_one_archive());
    ASSERT_EQ_INT(3, cp_order_load());
    Units_LoadAllBuildables();
    uint64_t one = Units_ContentHash();
    ASSERT_EQ_INT(0, cp_order_two_archives());
    ASSERT_EQ_INT(3, cp_order_load());
    Units_LoadAllBuildables();
    uint64_t two = Units_ContentHash();
    cp_order_menu("zed");
    ASSERT_EQ_INT(3, cp_order_load());
    Units_LoadAllBuildables();
    uint64_t other = Units_ContentHash();
    ASSERT(one == two);
    ASSERT(one != other);
    cp_order_end();
}

/* ── unit slots ──────────────────────────────────────────────────────── */

/* A dead unit's slot takes the next unit. Slots were handed out once
 * each, so a battle stopped making units once TAK_MAX_UNITS had ever
 * lived. */
TEST(a_dead_units_slot_takes_the_next_unit) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    int a = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
    int b = Units_Spawn(CP_DEF_WALKER, 1, 0, 900, 800);
    ASSERT(a >= 0 && b >= 0);
    uint32_t old_id = cp_unit(a)->stable_id;
    ASSERT_EQ_INT(0, Units_DebugRemove(a));
    int c = Units_Spawn(CP_DEF_WALKER, 2, 1, 1200, 800);
    ASSERT_EQ_INT(a, c);
    ASSERT(cp_unit(c)->stable_id != old_id);
    ASSERT_EQ_INT(-1, Units_FindByStableId(old_id));
    ASSERT_EQ_INT(c, Units_FindByStableId(cp_unit(c)->stable_id));
    ASSERT_EQ_INT(2, (int)cp_unit(c)->player_id);
    cp_end();
}

TEST(a_long_battle_never_runs_out_of_slots) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    for (int i = 0; i < 2500; i++) {
        int h = Units_Spawn(CP_DEF_WALKER, 1 + (i & 1), i & 1, 800, 800);
        ASSERT(h >= 0);
        ASSERT_EQ_INT(0, Units_DebugRemove(h));
    }
    cp_end();
}

/* The unit that takes a slot is nobody's target, selection or control
 * group member just because the dead one was. */
TEST(a_reused_slot_forgets_the_unit_that_had_it) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    int archer = Units_Spawn(CP_DEF_ARCHER, 1, 0, 800, 800);
    int victim = Units_Spawn(CP_DEF_WALKER, 2, 1, 1400, 800);
    ASSERT(archer >= 0 && victim >= 0);
    Units_CommandAttackUnit(archer, victim);
    ASSERT_EQ_INT(victim, (int)cp_unit(archer)->target);
    Units_SetLocalPlayer(2);
    Units_SelectSingle(victim);
    Units_AssignControlGroup(3);
    Units_SetLocalPlayer(1);

    ASSERT_EQ_INT(0, Units_DebugRemove(victim));
    int heir = Units_Spawn(CP_DEF_WALKER, 2, 1, 2000, 800);
    ASSERT_EQ_INT(victim, heir);
    ASSERT(cp_unit(archer)->target != heir);
    int n = -1;
    Units_GetSelection(&n);
    ASSERT_EQ_INT(0, n);
    Units_SetLocalPlayer(2);
    int recalled = Units_RecallControlGroup(3);
    Units_SelectSingle(-1);
    Units_SetLocalPlayer(1);
    ASSERT_EQ_INT(0, recalled);
    cp_end();
}

/* The lobby's units-per-player limit holds in the battle. A seat at its
 * limit makes no more units until one of its own dies, and the other
 * seats are not held back. */
TEST(a_seat_stops_at_its_unit_limit) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    w->cfg.units_per_side = 3;
    int h[3];
    for (int i = 0; i < 3; i++) {
        h[i] = Units_Spawn(CP_DEF_WALKER, 1, 0, 800 + i * 60, 800);
        ASSERT(h[i] >= 0);
    }
    ASSERT_EQ_INT(-1, Units_Spawn(CP_DEF_WALKER, 1, 0, 1000, 900));
    ASSERT(Units_Spawn(CP_DEF_WALKER, 2, 1, 1400, 900) >= 0);
    ASSERT_EQ_INT(0, Units_DebugRemove(h[1]));
    ASSERT(Units_Spawn(CP_DEF_WALKER, 1, 0, 1000, 900) >= 0);
    cp_end();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("The one ownership check");
    RUN(a_command_moves_only_the_units_its_seat_owns);
    RUN(a_command_with_no_standing_units_does_nothing);
    RUN(the_executor_runs_every_unit_command);
    RUN(the_executor_runs_the_seat_commands);
    TEST_SUITE("Stable id index");
    RUN(a_stable_id_is_found_in_a_few_probes);
    TEST_SUITE("The queue");
    RUN(an_order_waits_for_its_tick);
    RUN(a_tick_runs_seat_by_seat_then_by_arrival);
    RUN(a_click_reaches_the_unit_on_the_next_tick);
    TEST_SUITE("Transports");
    RUN(a_load_drag_boards_every_rider_it_names);
    RUN(a_drag_on_screen_loads_on_the_next_tick);
    TEST_SUITE("Replay");
    RUN(a_recorded_session_replays_to_the_same_hashes);
    TEST_SUITE("The local seat");
    RUN(a_click_from_seat_two_orders_seat_twos_army);
    RUN(the_pursuit_never_moves_a_human_army);
    RUN(the_battle_goes_on_while_two_sides_still_stand);
    RUN(a_script_hears_the_same_answer_on_every_machine);
    RUN(the_same_battle_from_seat_one_and_seat_three_agrees);
    TEST_SUITE("The session seed");
    RUN(the_session_seed_decides_every_draw);
    TEST_SUITE("Unit slots");
    RUN(a_dead_units_slot_takes_the_next_unit);
    RUN(a_long_battle_never_runs_out_of_slots);
    RUN(a_reused_slot_forgets_the_unit_that_had_it);
    RUN(a_seat_stops_at_its_unit_limit);
    TEST_SUITE("The def order");
    RUN(the_def_order_does_not_depend_on_the_archives);
    RUN(a_new_load_reads_its_own_build_menus);
    RUN(every_build_menu_is_read_when_the_match_loads);
    RUN(the_content_hash_covers_the_order_and_the_menus);
    TEST_REPORT();
}
