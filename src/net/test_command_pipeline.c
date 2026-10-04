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
#include "tak_console_cmd.h"
#include "tak_economy.h"
#include "tak_fog.h"
#include "tak_hud.h"
#include "tak_ingame.h"
#include "tak_memory.h"
#include "tak_mission.h"
#include "tak_moveinfo.h"
#include "tak_occupancy.h"
#include "tak_pathing.h"
#include "tak_net_protocol.h"
#include "tak_net_client.h"
#include "tak_net_match.h"
#include "tak_net_relay.h"
#include "tak_bytes.h"
#include "tak_paths.h"
#include "tak_replay_session.h"
#include "tak_savelist.h"
#include "tak_hpi.h"
#include "test_hpi_builder.h"
#include "tak_sim_rand.h"
#include "tak_sim_hash.h"
#include "tak_unit.h"
#include "tak_view_shake.h"
#include "tak_world.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ── the harness ───────────────────────────────────────────────────── */

#define CP_TILES   192      /* 16 px tiles per side, so a 3072 px map */
#define CP_GROUND  64       /* flat height, clear of the water line */

enum { CP_DEF_WALKER = 0, CP_DEF_ARCHER, CP_DEF_BUILDER, CP_DEF_CARRIER,
       CP_DEF_HARPY, CP_DEF_GUARDED, CP_DEF_MONARCH, CP_DEF_MINDMAGE,
       CP_DEF_POOLMAGE, CP_DEF_SQUAD2, CP_DEF_SQUAD3, CP_DEF_DART, CP_DEF_COUNT };

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

/* The Harpy's weapon block in miniature: subtype mindcontrol at 300
 * mana a shot, drawn from the unit's own reserve of 1000. */
static void cp_add_mind_control(UnitDef *d) {
    d->num_weapons = 1;
    UnitWeapon *w = &d->weapons[0];
    strncpy(w->name, "TESTMIND", sizeof(w->name) - 1);
    strncpy(w->type, "line of sight", sizeof(w->type) - 1);
    strncpy(w->subtype, "mindcontrol", sizeof(w->subtype) - 1);
    w->mind_control = 1;
    w->los_kind = 3;
    w->range = 300;
    w->reload_ticks = 90;
    w->damage = 1;
    w->velocity_pps = 169;
    w->mana_per_shot = 300;
    d->max_mana = 1000;
    d->mana_recharge_per_sec = 10.0f;
    d->cap_flags |= UNIT_CAP_ATTACK | UNIT_CAP_CAPTURE;
}

/* A caster with no reserve of its own: both spells come out of the
 * seat's pool. Every shipped caster carries maxmana, so the pool path
 * has no real unit to borrow. */
static void cp_add_pool_spells(UnitDef *d) {
    d->num_weapons = 2;
    UnitWeapon *cheap = &d->weapons[0];
    strncpy(cheap->name, "TESTCHEAP", sizeof(cheap->name) - 1);
    cheap->range = 300;
    cheap->reload_ticks = 60;
    cheap->damage = 10;
    cheap->velocity_pps = 400;
    cheap->mana_per_shot = 100;
    UnitWeapon *dear = &d->weapons[1];
    strncpy(dear->name, "TESTDEAR", sizeof(dear->name) - 1);
    dear->range = 300;
    dear->reload_ticks = 60;
    dear->damage = 40;
    dear->velocity_pps = 400;
    dear->mana_per_shot = 400;
    d->max_mana = 0;
    d->cap_flags |= UNIT_CAP_ATTACK | UNIT_CAP_W_SWITCH;
}

/* A flat world with an occupancy layer, one move class and three
 * synthetic defs. Seats 1 through 4 are human and each on its own
 * team. Returns NULL if anything could not be built. */
static uint32_t g_cp_seed = 0;
/* Set, the seats come from a match's START_GAME the way the battle room
 * builds them: a human's, a computer's or nobody's. */
static const TAK_MsgStartGame *g_cp_start;

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
    if (g_cp_start) {
        cfg.seed = g_cp_start->seed;
        for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
            uint8_t k = g_cp_start->slot[i].kind;
            cfg.players[i].kind = k == TAK_NSLOT_HUMAN ? TAK_SLOT_HUMAN
                                : k == TAK_NSLOT_COMPUTER ? TAK_SLOT_AI : TAK_SLOT_CLOSED;
            cfg.players[i].team = i + 1;
            cfg.players[i].color = i;
        }
    }
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
    w->moveinfo.count = 3;
    strncpy(w->moveinfo.classes[0].name, "TESTSMALL", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[0].footprint_x = 1;
    w->moveinfo.classes[0].footprint_z = 1;
    w->moveinfo.classes[0].max_slope = 30;
    /* The footprints real infantry and cavalry have. */
    strncpy(w->moveinfo.classes[1].name, "TESTTWO", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[1].footprint_x = 2;
    w->moveinfo.classes[1].footprint_z = 2;
    w->moveinfo.classes[1].max_slope = 30;
    strncpy(w->moveinfo.classes[2].name, "TESTTHREE", TAK_MOVEINFO_NAME_MAX - 1);
    w->moveinfo.classes[2].footprint_x = 3;
    w->moveinfo.classes[2].footprint_z = 3;
    w->moveinfo.classes[2].max_slope = 30;

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
    cp_fill_def(&defs[CP_DEF_CARRIER], "TESTBOAT", "TESTTWO", 1.0f, 400);
    defs[CP_DEF_CARRIER].footprint_x = defs[CP_DEF_CARRIER].footprint_z = 2;
    defs[CP_DEF_CARRIER].cap_flags |= UNIT_CAP_TRANSPORT;
    defs[CP_DEF_CARRIER].transport_capacity = 4;
    cp_fill_def(&defs[CP_DEF_HARPY], "TESTHARPY", "TESTSMALL", 3.5f, 900);
    cp_add_mind_control(&defs[CP_DEF_HARPY]);
    cp_fill_def(&defs[CP_DEF_GUARDED], "TESTGUARD", "TESTSMALL", 1.4f, 200);
    defs[CP_DEF_GUARDED].cant_be_captured = 1;
    cp_fill_def(&defs[CP_DEF_MONARCH], "TESTKING", "TESTSMALL", 1.4f, 500);
    defs[CP_DEF_MONARCH].commander = 1;
    /* The same shot with a splash that does not fall off. */
    cp_fill_def(&defs[CP_DEF_MINDMAGE], "TESTMAGE", "TESTSMALL", 1.2f, 300);
    cp_add_mind_control(&defs[CP_DEF_MINDMAGE]);
    defs[CP_DEF_MINDMAGE].weapons[0].area_of_effect = 120;
    defs[CP_DEF_MINDMAGE].weapons[0].edge_effectiveness = 1.0f;
    cp_fill_def(&defs[CP_DEF_POOLMAGE], "TESTPOOLM", "TESTSMALL", 1.2f, 300);
    /* Faster than any shipped unit: 80 px a step, more than the grid's
     * old fixed slack. */
    cp_fill_def(&defs[CP_DEF_DART], "TESTDART", "TESTSMALL", 160.0f, 100);
    defs[CP_DEF_DART].acceleration = 160.0f;
    cp_fill_def(&defs[CP_DEF_SQUAD2], "TESTFOOT", "TESTTWO", 1.4f, 200);
    defs[CP_DEF_SQUAD2].footprint_x = defs[CP_DEF_SQUAD2].footprint_z = 2;
    cp_fill_def(&defs[CP_DEF_SQUAD3], "TESTHORSE", "TESTTHREE", 1.4f, 300);
    defs[CP_DEF_SQUAD3].footprint_x = defs[CP_DEF_SQUAD3].footprint_z = 3;
    cp_add_pool_spells(&defs[CP_DEF_POOLMAGE]);
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
    /* More than the seat holds sends what it holds, as far as the
     * receiver has room, and the rest stays (legacy:206055-206087). */
    int from = Economy_GetMana(&w->economy, 1), to = Economy_GetMana(&w->economy, 2);
    int room = Economy_GetMaxMana(&w->economy, 2) - to;
    int moves = from < room ? from : room;
    g_cmd.target_x = 10000 << 16;   /* 16.16, so it has to fit in 32 bits */
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(from - moves, Economy_GetMana(&w->economy, 1));
    ASSERT_EQ_INT(to + moves, Economy_GetMana(&w->economy, 2));

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

/* ── the console's + commands ──────────────────────────────────────── */

static void cp_code(uint8_t seat, unsigned code, unsigned param) {
    cp_cmd(TAK_CMD_POWER_CODE, seat);
    g_cmd.arg = (uint16_t)(code | (param << 8));
}

/* The first word names the command whatever its case, the rest are
 * its arguments, and developer tooling is not in the table. */
TEST(a_typed_line_names_its_command) {
    ConsoleCmdLine l;
    ASSERT_EQ_INT(CONSOLE_CMD_NOW_I_SEE, ConsoleCmd_Parse("NOWISEE", &l));
    ASSERT_EQ_INT(1, l.count);
    ASSERT_EQ_INT(CONSOLE_CMD_ATM, ConsoleCmd_Parse("atm", &l));
    ASSERT_EQ_INT(CONSOLE_CMD_LOS, ConsoleCmd_Parse("  LOS   off ", &l));
    ASSERT_EQ_INT(2, l.count);
    ASSERT_EQ_STR("off", l.tok[1]);
    ASSERT_EQ_INT(CONSOLE_CMD_GIVE_MANA, ConsoleCmd_Parse("GiveMana 1 250", &l));
    ASSERT_EQ_INT(3, l.count);
    ASSERT_EQ_INT(1, ConsoleCmd_Int(&l, 1, 0));
    ASSERT(ConsoleCmd_Float(&l, 2, 0.0f) == 250.0f);
    ASSERT_EQ_INT(7, ConsoleCmd_Int(&l, 5, 7));
    ASSERT_EQ_INT(CONSOLE_CMD_SHARE_MANA_LIMIT, ConsoleCmd_Parse("sharemanalimit 0.25", &l));
    ASSERT_EQ_INT(CONSOLE_CMD_UNKNOWN, ConsoleCmd_Parse("Profile", &l));
    ASSERT_EQ_INT(CONSOLE_CMD_UNKNOWN, ConsoleCmd_Parse("", &l));
    ASSERT_EQ_INT(CONSOLE_CMD_UNKNOWN, ConsoleCmd_Parse("NowISeeMore", &l));
    ASSERT_EQ_INT(1, ConsoleCmd_IsPowerCode(CONSOLE_CMD_KILL));
    ASSERT_EQ_INT(0, ConsoleCmd_IsPowerCode(CONSOLE_CMD_CLOCK));
    ASSERT_EQ_STR("NowISee", ConsoleCmd_Name(CONSOLE_CMD_NOW_I_SEE));
    char clock[48];
    ConsoleCmd_ClockText(clock, sizeof(clock), 60u * 3725u);
    ASSERT_EQ_STR("Game Time : 01:02:05", clock);
}

/* A room without power codes sends none and runs none, even from a
 * client that sends one anyway. The sharing settings are open to all. */
TEST(a_power_code_is_refused_unless_the_room_allows_it) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    ASSERT_EQ_INT(0, w->cfg.power_codes);
    ASSERT_EQ_INT(CONSOLE_RAN_REFUSED, ConsoleCmd_Run("DoubleShot"));
    ASSERT_EQ_INT(CONSOLE_RAN_REFUSED, ConsoleCmd_Run("atm"));
    ASSERT_EQ_INT(CONSOLE_RAN_REFUSED, ConsoleCmd_Run("Gods"));
    /* ShootAll is in the open table, so it is not in yet, not refused. */
    ASSERT_EQ_INT(0, ConsoleCmd_IsPowerCode(CONSOLE_CMD_SHOOT_ALL));
    ASSERT_EQ_INT(CONSOLE_RAN_NOT_IN_YET, ConsoleCmd_Run("ShootAll"));
    ASSERT_EQ_INT(0, TAK_CmdQueue_Pending());
    for (unsigned code = TAK_CODE_ATM; code < TAK_CODE_SHARE_LIMIT; code++) {
        cp_code(1, code, code == TAK_CODE_VIEW ? 2u : 0u);
        ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    }
    ASSERT_EQ_INT(0, (int)w->console.double_shot);
    ASSERT_EQ_INT(0, (int)w->cfg.line_of_sight);
    ASSERT_EQ_INT(CONSOLE_RAN_SENT, ConsoleCmd_Run("ShareManaLimit 0.25"));
    cp_tick();
    ASSERT(w->console.share_limit[1] == 0.25f);
    ASSERT_EQ_INT(CONSOLE_RAN_NOTHING, ConsoleCmd_Run("ShareManaPct 2"));
    ASSERT_EQ_INT(CONSOLE_RAN_NOTHING, ConsoleCmd_Run("Profile"));
    cp_end();
}

/* What each code changes, the original's handler for it. */
TEST(each_power_code_does_what_the_original_did) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    w->cfg.power_codes = 1;
    ASSERT_EQ_INT(0, Fog_Init(w));

    /* ATM: the pool to its cap. */
    Economy_AdjustCaps(&w->economy, 1, 2000, 0.0f);
    Economy_TrySpend(&w->economy, 1, Economy_GetMana(&w->economy, 1));
    cp_code(1, TAK_CODE_ATM, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(Economy_GetMaxMana(&w->economy, 1), Economy_GetMana(&w->economy, 1));

    /* DoubleShot and HalfShot: one or the other, on every hit. */
    int archer = Units_Spawn(CP_DEF_ARCHER, 1, 0, 400, 400);
    int target = Units_Spawn(CP_DEF_WALKER, 2, 1, 2800, 400);
    ASSERT(archer >= 0 && target >= 0);
    int32_t plain = Units_HitDamage(archer, target, 40);
    cp_code(1, TAK_CODE_DOUBLE_SHOT, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(plain * 2, Units_HitDamage(archer, target, 40));
    cp_code(2, TAK_CODE_HALF_SHOT, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(0, (int)w->console.double_shot);
    ASSERT_EQ_INT(plain / 2, Units_HitDamage(archer, target, 40));
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(plain, Units_HitDamage(archer, target, 40));

    /* Mapping hides the map again, NowISee shows everything. */
    w->cfg.line_of_sight = 1;
    w->cfg.map_revealed = 1;
    cp_code(1, TAK_CODE_MAPPING, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(0, w->cfg.map_revealed);
    ASSERT_EQ_INT(TAK_FOG_UNEXPLORED, Fog_StateAtForPlayer(w, 1, 2800, 2800));
    ASSERT_EQ_INT(TAK_FOG_VISIBLE, Fog_StateAtForPlayer(w, 1, 400, 400));
    cp_code(1, TAK_CODE_NOW_I_SEE, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(0, w->cfg.line_of_sight);
    ASSERT_EQ_INT(1, w->cfg.map_revealed);
    ASSERT_EQ_INT(TAK_FOG_EXPLORED, Fog_StateAtForPlayer(w, 1, 2800, 2800));
    ASSERT_EQ_INT(1, Fog_IsVisibleForPlayer(w, 1, 2800, 2800));

    /* LOS flips alone and sets with On or Off. */
    cp_code(1, TAK_CODE_LOS, TAK_CODE_LOS_TOGGLE);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, w->cfg.line_of_sight);
    cp_code(1, TAK_CODE_LOS, TAK_CODE_LOS_ON);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, w->cfg.line_of_sight);
    cp_code(1, TAK_CODE_LOS, TAK_CODE_LOS_OFF);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(0, w->cfg.line_of_sight);

    /* Radar and View belong to the seat that typed them. */
    cp_code(1, TAK_CODE_RADAR, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, (int)w->console.radar[1]);
    ASSERT_EQ_INT(0, (int)w->console.radar[2]);
    cp_code(1, TAK_CODE_VIEW, 2);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(2, Fog_Viewer());
    cp_code(1, TAK_CODE_VIEW, 6);            /* a closed seat */
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    cp_code(1, TAK_CODE_VIEW, 1);            /* back to its own */
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, Fog_Viewer());

    /* ManaMe and NoMana: the named units' own mana, the typist's only. */
    int mage = Units_Spawn(CP_DEF_HARPY, 1, 0, 400, 2800);
    int theirs = Units_Spawn(CP_DEF_HARPY, 2, 1, 2800, 2800);
    ASSERT(mage >= 0 && theirs >= 0);
    Units_DebugSetMana(mage, 100.0f);
    Units_DebugSetMana(theirs, 100.0f);
    cp_code(1, TAK_CODE_MANA_ME, 0);
    cp_cmd_unit(mage);
    cp_cmd_unit(theirs);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    float cur = 0.0f, max = 0.0f;
    ASSERT_EQ_INT(1, Units_GetMana(mage, &cur, &max));
    ASSERT(cur == max && max > 100.0f);
    ASSERT_EQ_INT(1, Units_GetMana(theirs, &cur, &max));
    ASSERT(cur == 100.0f);
    g_cmd.arg = TAK_CODE_NO_MANA;
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, Units_GetMana(mage, &cur, &max));
    ASSERT(cur == 0.0f);

    /* The share settings take 0 to 1 and nothing else. */
    cp_code(3, TAK_CODE_SHARE_PCT, 0);
    g_cmd.target_x = 0x8000;
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT(w->console.share_pct[3] == 0.5f);
    g_cmd.target_x = 0x10001;
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    ASSERT(w->console.share_pct[3] == 0.5f);
    ASSERT(w->console.share_pct[1] == ECONOMY_SHARE_PCT);
    cp_end();
}

/* IWin takes every enemy army, ILose the typist's own, Kill all. */
TEST(the_ending_codes_take_the_armies_they_name) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    w->cfg.power_codes = 1;
    int a = Units_Spawn(CP_DEF_WALKER, 1, 0, 400, 400);
    int b = Units_Spawn(CP_DEF_WALKER, 2, 1, 2800, 400);
    int c = Units_Spawn(CP_DEF_WALKER, 3, 2, 400, 2800);
    ASSERT(a >= 0 && b >= 0 && c >= 0);
    cp_code(1, TAK_CODE_I_WIN, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)cp_unit(a)->alive);
    ASSERT(cp_unit(b)->alive != UNIT_ALIVE_ACTIVE);
    ASSERT(cp_unit(c)->alive != UNIT_ALIVE_ACTIVE);
    ASSERT_EQ_INT(1, w->stats[2].losses);
    cp_end();

    w = cp_world();
    ASSERT_NOT_NULL(w);
    w->cfg.power_codes = 1;
    a = Units_Spawn(CP_DEF_WALKER, 1, 0, 400, 400);
    b = Units_Spawn(CP_DEF_WALKER, 2, 1, 2800, 400);
    cp_code(1, TAK_CODE_I_LOSE, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT(cp_unit(a)->alive != UNIT_ALIVE_ACTIVE);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)cp_unit(b)->alive);
    cp_code(2, TAK_CODE_KILL, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT(cp_unit(b)->alive != UNIT_ALIVE_ACTIVE);
    cp_end();
}

/* A rider dies with its side: the ending codes walk every unit, carried
 * or not (legacy:227486-227507, legacy:227546-227580). */
TEST(the_ending_codes_take_a_transports_riders) {
    for (int k = 0; k < 3; k++) {
        GameWorld *w = cp_world();
        ASSERT_NOT_NULL(w);
        w->cfg.power_codes = 1;
        int mine = Units_Spawn(CP_DEF_WALKER, 1, 0, 400, 400);
        int boat = Units_Spawn(CP_DEF_CARRIER, 2, 1, 900, 800);
        int rider = Units_Spawn(CP_DEF_WALKER, 2, 1, 900, 840);
        ASSERT(mine >= 0 && boat >= 0 && rider >= 0);
        Unit *bu = (Unit *)cp_unit(boat);    /* test-only mutation */
        Unit *ru = (Unit *)cp_unit(rider);   /* test-only mutation */
        ru->alive = UNIT_ALIVE_TRANSPORTED;
        ru->carried_by = (int16_t)boat;
        ru->world_x = bu->world_x;
        ru->world_y = bu->world_y;
        bu->cargo_count = 1;

        if (k == 0) cp_code(2, TAK_CODE_I_LOSE, 0);
        else if (k == 1) cp_code(1, TAK_CODE_I_WIN, 0);
        else cp_code(1, TAK_CODE_KILL, 0);
        ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
        for (int t = 0; t < 120; t++) cp_tick();

        ASSERT_EQ_INT(UNIT_ALIVE_DEAD, (int)cp_unit(rider)->alive);
        ASSERT_EQ_INT(2, w->stats[2].losses);
        int count = 0, left = 0;
        const Unit *units = Units_GetActive(&count);
        for (int i = 0; i < count; i++)
            if (units[i].player_id == 2 && units[i].alive != UNIT_ALIVE_DEAD) left++;
        ASSERT_EQ_INT(0, left);
        cp_end();
    }
}

/* IWin and ILose end the battle on the spot with the typist's result,
 * a mission whatever its objectives say. In a skirmish one seat's ILose
 * leaves the others fighting. */
TEST(the_ending_codes_call_the_battle_at_once) {
    for (int k = 0; k < 2; k++) {
        GameWorld *w = cp_world();
        ASSERT_NOT_NULL(w);
        w->cfg.power_codes = 1;
        /* A mission won by holding out an hour, not by the kill. */
        w->mission.objectives = (MissionObjective *)tak_calloc(1, sizeof(MissionObjective));
        ASSERT_NOT_NULL(w->mission.objectives);
        w->mission.objectives[0].type = MISSION_OBJ_VICTORY_TIMER_RUNS_OUT;
        w->mission.objectives[0].role = MISSION_ROLE_VICTORY;
        w->mission.objectives[0].a = 3600;
        w->mission.objective_count = 1;
        w->mission.victory_count = 1;
        int a = Units_Spawn(CP_DEF_WALKER, 1, 0, 400, 400);
        int b = Units_Spawn(CP_DEF_WALKER, 2, 1, 2800, 400);
        ASSERT(a >= 0 && b >= 0);
        InGame_DebugRunSimTicks(2);
        ASSERT_EQ_INT(0, w->skirmish_game_over);
        ASSERT_EQ_INT(CONSOLE_RAN_SENT, ConsoleCmd_Run(k == 0 ? "IWin" : "ILose"));
        InGame_DebugRunSimTicks(1);
        ASSERT_EQ_INT(1, w->skirmish_game_over);
        ASSERT_EQ_INT(k == 0 ? 1 : -1, w->skirmish_local_result);
        ASSERT_EQ_INT(w->mission_elapsed_ticks, w->skirmish_end_tick);
        cp_end();
    }

    /* A skirmish: IWin ends it on the tick it lands. */
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    w->cfg.power_codes = 1;
    int a = Units_Spawn(CP_DEF_WALKER, 1, 0, 400, 400);
    int b = Units_Spawn(CP_DEF_WALKER, 2, 1, 2800, 400);
    int c = Units_Spawn(CP_DEF_WALKER, 3, 2, 400, 2800);
    ASSERT(a >= 0 && b >= 0 && c >= 0);
    ASSERT_EQ_INT(CONSOLE_RAN_SENT, ConsoleCmd_Run("IWin"));
    InGame_DebugRunSimTicks(1);
    ASSERT_EQ_INT(1, (int)w->console.called[1]);
    ASSERT_EQ_INT(1, w->stats[2].eliminated);
    ASSERT_EQ_INT(1, w->stats[3].eliminated);
    ASSERT_EQ_INT(1, w->skirmish_game_over);
    ASSERT_EQ_INT(1, w->skirmish_local_result);
    ASSERT_EQ_INT(Units_PlayerTeamId(1), w->skirmish_winner_team);
    cp_end();

    /* Seat 2 giving up leaves seats 1 and 3 at war. Seat 1's own ILose
     * is its defeat at once, and seat 3 is left the winner. */
    w = cp_world();
    ASSERT_NOT_NULL(w);
    w->cfg.power_codes = 1;
    a = Units_Spawn(CP_DEF_WALKER, 1, 0, 400, 400);
    b = Units_Spawn(CP_DEF_WALKER, 2, 1, 2800, 400);
    c = Units_Spawn(CP_DEF_WALKER, 3, 2, 400, 2800);
    ASSERT(a >= 0 && b >= 0 && c >= 0);
    cp_code(2, TAK_CODE_I_LOSE, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    InGame_DebugRunSimTicks(1);
    ASSERT_EQ_INT(0, w->skirmish_game_over);
    ASSERT_EQ_INT(0, w->skirmish_local_result);
    ASSERT_EQ_INT(CONSOLE_RAN_SENT, ConsoleCmd_Run("ILose"));
    InGame_DebugRunSimTicks(1);
    ASSERT_EQ_INT(-1, w->skirmish_local_result);
    ASSERT_EQ_INT(1, w->skirmish_game_over);
    ASSERT_EQ_INT(Units_PlayerTeamId(3), w->skirmish_winner_team);
    cp_end();
}

/* A mana gift takes any amount above 0, fractions too, and the giver's
 * pool caps it (legacy:206055-206087). */
TEST(a_mana_gift_takes_a_fraction_and_stops_at_the_givers_pool) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    Economy_AdjustCaps(&w->economy, 1, 1000, 0.0f);
    Economy_AdjustCaps(&w->economy, 2, 2000, 0.0f);
    Economy_TrySpend(&w->economy, 2, Economy_GetMana(&w->economy, 2));
    Economy_TrySpend(&w->economy, 1, 990);
    ASSERT(w->economy.players[0].mana == 10.0f);

    cp_cmd(TAK_CMD_MANA_GIFT, 1);
    g_cmd.arg = 2;
    g_cmd.target_x = 0x8000;                 /* half a mana */
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT(w->economy.players[0].mana == 9.5f);
    ASSERT(w->economy.players[1].mana == 0.5f);
    g_cmd.target_x = 1;                      /* the least there is */
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT(w->economy.players[1].mana == 0.5f + 1.0f / 65536.0f);
    g_cmd.target_x = 0;
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    g_cmd.target_x = -0x10000;
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));

    /* More than the giver holds sends what it holds. */
    float held = w->economy.players[0].mana;
    float got = w->economy.players[1].mana;
    g_cmd.target_x = 100 << 16;
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT(w->economy.players[0].mana == 0.0f);
    ASSERT(w->economy.players[1].mana == got + held);

    /* Typed, a fraction goes and nothing at or below 0 does. */
    Economy_Earn(&w->economy, 1, 10);
    ASSERT_EQ_INT(CONSOLE_RAN_SENT, ConsoleCmd_Run("GiveMana 1 0.25"));
    ASSERT_EQ_INT(CONSOLE_RAN_NOTHING, ConsoleCmd_Run("GiveMana 1 0"));
    ASSERT_EQ_INT(CONSOLE_RAN_NOTHING, ConsoleCmd_Run("GiveMana 1 -3"));
    cp_tick();
    ASSERT(w->economy.players[0].mana == 9.75f);
    cp_end();
}

/* A typed code leaves through the queue and lands on the next tick,
 * and the line needs the room's word before it goes. */
TEST(a_typed_code_reaches_the_world_through_the_stream) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    w->cfg.power_codes = 1;
    Economy_AdjustCaps(&w->economy, 1, 2000, 0.0f);
    Economy_TrySpend(&w->economy, 1, Economy_GetMana(&w->economy, 1));
    ASSERT_EQ_INT(CONSOLE_RAN_SENT, ConsoleCmd_Run("ATM"));
    ASSERT_EQ_INT(0, Economy_GetMana(&w->economy, 1));
    cp_tick();
    ASSERT_EQ_INT(2000, Economy_GetMana(&w->economy, 1));

    int mage = Units_Spawn(CP_DEF_HARPY, 1, 0, 400, 2800);
    ASSERT(mage >= 0);
    Units_DebugSetMana(mage, 0.0f);
    Units_SelectSingle(mage);
    ASSERT_EQ_INT(CONSOLE_RAN_SENT, ConsoleCmd_Run("manaMe"));
    cp_tick();
    float cur = 0.0f, max = 0.0f;
    ASSERT_EQ_INT(1, Units_GetMana(mage, &cur, &max));
    ASSERT(cur == max);

    ASSERT_EQ_INT(CONSOLE_RAN_SENT, ConsoleCmd_Run("LOS On"));
    ASSERT_EQ_INT(CONSOLE_RAN_NOTHING, ConsoleCmd_Run("LOS maybe"));
    ASSERT_EQ_INT(CONSOLE_RAN_NOTHING, ConsoleCmd_Run("Kill everyone"));
    ASSERT_EQ_INT(CONSOLE_RAN_NOTHING, ConsoleCmd_Run("View 7"));
    ASSERT_EQ_INT(CONSOLE_RAN_SENT, ConsoleCmd_Run("View 1"));
    cp_tick();
    ASSERT_EQ_INT(1, w->cfg.line_of_sight);
    ASSERT_EQ_INT(2, (int)w->console.view[1]);
    ASSERT_EQ_INT(CONSOLE_RAN_NOT_IN_YET, ConsoleCmd_Run("Gods"));
    ASSERT_EQ_INT(0, TAK_CmdQueue_Pending());

    /* GiveMana names a player from 0, as the original does. */
    Economy_AdjustCaps(&w->economy, 2, 2000, 0.0f);
    Economy_TrySpend(&w->economy, 2, Economy_GetMana(&w->economy, 2));
    ASSERT_EQ_INT(CONSOLE_RAN_SENT, ConsoleCmd_Run("GiveMana 1 250"));
    ASSERT_EQ_INT(CONSOLE_RAN_NOTHING, ConsoleCmd_Run("GiveMana 1"));
    cp_tick();
    ASSERT_EQ_INT(250, Economy_GetMana(&w->economy, 2));
    ASSERT_EQ_INT(1750, Economy_GetMana(&w->economy, 1));
    cp_end();
}

/* The console's changes are simulation state: the hash sees each one. */
TEST(the_hash_sees_every_console_change) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    uint32_t h = TAK_SimHash();
    uint8_t *fields[] = { &w->console.double_shot, &w->console.half_shot,
                          &w->console.radar[3], &w->console.view[2] };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        *fields[i] = 1;
        ASSERT(TAK_SimHash() != h);
        *fields[i] = 0;
        ASSERT_EQ_INT((int)h, (int)TAK_SimHash());
    }
    w->console.share_limit[4] = 0.75f;
    ASSERT(TAK_SimHash() != h);
    w->console.share_limit[4] = ECONOMY_SHARE_LIMIT;
    w->console.share_pct[2] = 0.5f;
    ASSERT(TAK_SimHash() != h);
    w->console.share_pct[2] = ECONOMY_SHARE_PCT;
    w->console.called[3] = -1;
    ASSERT(TAK_SimHash() != h);
    w->console.called[3] = 0;
    ASSERT_EQ_INT((int)h, (int)TAK_SimHash());
    w->cfg.map_revealed = !w->cfg.map_revealed;
    ASSERT(TAK_SimHash() != h);
    w->cfg.map_revealed = !w->cfg.map_revealed;
    w->cfg.line_of_sight = !w->cfg.line_of_sight;
    ASSERT(TAK_SimHash() != h);
    cp_end();
}

/* NoShake keeps any shake from starting. */
TEST(no_shake_starts_no_shake) {
    ViewShake_Reset();
    ViewShake_SetNoShake(1);
    ViewShake_Start(8, 30);
    ASSERT_EQ_INT(0, ViewShake_Active());
    ViewShake_SetNoShake(0);
    ViewShake_Start(8, 30);
    ASSERT_EQ_INT(1, ViewShake_Active());
    ViewShake_Reset();
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
    /* The order carries the ground under the pointer, not the flat
     * reading, so the expectation is asked of the same mapping. */
    int32_t want_x = 0, want_y = 0;
    Units_GroundUnderPoint(1200, 1000, &want_x, &want_y);
    InGame_WorldClick(1200, 1000, 0);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(mine)->cmd_kind);
    ASSERT_EQ_INT(1, TAK_CmdQueue_Pending());
    TAK_CmdQueue_Run();
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)cp_unit(mine)->cmd_kind);
    ASSERT_EQ_INT(want_x, cp_unit(mine)->cmd_x);
    ASSERT_EQ_INT(want_y, cp_unit(mine)->cmd_y);

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
            out[t / 60] = TAK_SimHash();
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

/* ── a replay file plays back exactly ──────────────────────────────── */

#define CP_FILE_TICKS  720
#define CP_TURN_TICKS  3
#define CP_FILE_TURNS  (CP_FILE_TICKS / CP_TURN_TICKS)
#define CP_FILE_PATH   "test_cp_replay.okreplay"

/* One tick the way the battle screen runs it: the replay's orders in,
 * the queue, the movers, then the hash on the ticks a replay keeps. */
static void cp_file_tick(void) {
    Replay_BeforeOrders();
    cp_tick();
    uint32_t done = TAK_CmdQueue_Tick();
    if (Replay_WantsHash(done)) Replay_NoteHash(done, TAK_SimHash());
}

/* The same clicks as the session above, from two seats, recorded to a
 * file as the battle screen records one. */
static int cp_file_record(uint32_t *out) {
    if (!cp_world()) return 0;
    int h[8];
    cp_replay_spawn(h);
    char err[TAK_REPLAY_ERR_MAX];
    if (Replay_RecordOpen(CP_FILE_PATH, err, sizeof err) != 0) return 0;
    for (int t = 0; t < CP_FILE_TICKS; t++) {
        if (t == 10) {
            Units_SelectSingle(h[0]);
            Units_SelectAdd(h[1]);
            InGame_WorldClick(1500, 1100, 0);
        }
        if (t == 200) {
            Units_SetLocalPlayer(2);
            Units_SelectSingle(h[5]);
            Units_SelectAdd(h[6]);
            TAK_Cmd_EmitSelection(TAK_CMD_MOVE, 900, 700, -1, 0, 0);
            Units_SetLocalPlayer(1);
        }
        if (t == 201) {
            Units_SelectSingle(h[2]);
            TAK_Cmd_EmitSelection(TAK_CMD_PATROL, 1200, 600, -1, 0, TAK_CMD_ARG_QUEUE);
        }
        if (t == 450) {
            Units_SelectSingle(h[0]);
            TAK_Cmd_EmitSelection(TAK_CMD_STOP, 0, 0, -1, 0, 0);
        }
        cp_file_tick();
        if ((t + 1) % CP_TURN_TICKS == 0) out[t / CP_TURN_TICKS] = TAK_SimHash();
    }
    Replay_RecordClose();
    cp_end();
    return 1;
}

/* Played back from the file into a fresh world: nothing local, only
 * what the file holds. `nudge` slips in an order the recording never
 * had, part way through. */
static int cp_file_play(uint32_t *out, int nudge) {
    char err[TAK_REPLAY_ERR_MAX] = "";
    if (Replay_Open(CP_FILE_PATH, err, sizeof err) != 0) {
        printf("(%s) ", err);
        return 0;
    }
    if (!cp_world()) return 0;
    int h[8];
    cp_replay_spawn(h);
    Replay_Attach();
    int t = 0;
    for (; Replay_CanAdvance(); t++) {
        if (nudge && t == 300) {
            /* Straight into the queue, past the lock a click meets. */
            cp_cmd(TAK_CMD_MOVE, 2);
            cp_cmd_unit(h[4]);
            g_cmd.target_x = 400;
            g_cmd.target_y = 400;
            (void)TAK_CmdQueue_Submit(2, &g_cmd);
        }
        cp_file_tick();
        if ((t + 1) % CP_TURN_TICKS == 0 && t / CP_TURN_TICKS < CP_FILE_TURNS)
            out[t / CP_TURN_TICKS] = TAK_SimHash();
    }
    return t;
}

TEST(a_replay_file_plays_back_to_the_same_hash_every_turn) {
    static uint32_t live[CP_FILE_TURNS], played[CP_FILE_TURNS];
    ASSERT(cp_file_record(live));

    ASSERT_EQ_INT(CP_FILE_TICKS, cp_file_play(played, 0));
    const TAK_ReplayHeader *hdr = Replay_Header();
    ASSERT_NOT_NULL(hdr);
    ASSERT_EQ_INT((int)g_cp_seed, (int)hdr->cfg.seed);
    ASSERT_EQ_INT(CP_FILE_TICKS, (int)hdr->end_tick);
    ASSERT_EQ_INT(4, (int)hdr->command_count);
    ASSERT_EQ_INT(0, (int)Replay_DriftTick());
    int moved = 0;
    for (int i = 1; i < CP_FILE_TURNS; i++) if (live[i] != live[i - 1]) moved = 1;
    ASSERT(moved);
    for (int i = 0; i < CP_FILE_TURNS; i++) {
        if (live[i] != played[i]) printf("(turn %d) ", i);
        ASSERT_EQ_INT((int)live[i], (int)played[i]);
    }

    /* Looking only: a click during playback orders nothing. */
    Units_SelectSingle(0);
    ASSERT_EQ_INT(-1, TAK_Cmd_EmitSelection(TAK_CMD_MOVE, 100, 100, -1, 0, 0));
    ASSERT_EQ_INT(0, TAK_CmdQueue_Pending());
    Replay_Stop();
    ASSERT_EQ_INT(0, TAK_Cmd_Locked());
    cp_end();
    remove(CP_FILE_PATH);
}

/* A world that does something the recording never did is caught at the
 * first checkpoint after it, and said so. */
TEST(a_replay_that_drifts_says_where) {
    static uint32_t live[CP_FILE_TURNS], played[CP_FILE_TURNS];
    ASSERT(cp_file_record(live));
    ASSERT_EQ_INT(CP_FILE_TICKS, cp_file_play(played, 1));
    uint32_t at = Replay_DriftTick();
    ASSERT(at > 300);
    ASSERT_EQ_INT(0, (int)(at % TAK_REPLAY_HASH_EVERY));
    char line[160];
    ASSERT(Replay_DriftLine(line, sizeof line));
    ASSERT(strstr(line, "drifted") != NULL);
    Replay_Stop();
    cp_end();
    remove(CP_FILE_PATH);
}

/* A recording from another engine build is refused before any world is
 * built, in words that say why. */
TEST(a_replay_from_another_build_is_refused) {
    static uint32_t live[CP_FILE_TURNS];
    ASSERT(cp_file_record(live));
    TAK_ReplayHeader h;
    char err[TAK_REPLAY_ERR_MAX] = "";
    ASSERT_EQ_INT(0, TAK_Replay_ReadHeader(CP_FILE_PATH, &h, err, sizeof err));
    ASSERT_EQ_INT(TAK_ENGINE_BUILD_ID, (int)h.engine_build_id);
    /* Rewritten as a build before this one, checksum and all. */
    TAK_ReplayReader *r = TAK_Replay_Open(CP_FILE_PATH, &h, NULL, err, sizeof err);
    ASSERT_NOT_NULL(r);
    TAK_Replay_Close(r);
    h.engine_build_id = TAK_ENGINE_BUILD_ID - 1;
    TAK_ReplayWriter *w = TAK_ReplayWriter_Open("test_cp_old.okreplay", &h, err, sizeof err);
    ASSERT_NOT_NULL(w);
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Checkpoint(w, 60, 1));
    ASSERT_EQ_INT(0, TAK_ReplayWriter_Close(w, 120));
    ASSERT_EQ_INT(-1, Replay_Open("test_cp_old.okreplay", err, sizeof err));
    ASSERT(strstr(err, "engine build") != NULL);
    ASSERT_NULL(Replay_Header());
    ASSERT_EQ_INT(0, Replay_IsPlaying());
    remove("test_cp_old.okreplay");
    remove(CP_FILE_PATH);
}

/* ── what replays keep on disk ─────────────────────────────────────── */

#define CP_PREFS "test_cp_prefs"

/* A small finished replay in the saved game directory, recorded at
 * `when`, padded with checkpoints to about `ticks` / 6 bytes. */
static int cp_fake_replay(const char *slug, uint64_t when, uint32_t ticks) {
    TAK_ReplayHeader h;
    memset(&h, 0, sizeof h);
    h.engine_build_id = TAK_ENGINE_BUILD_ID;
    h.local_seat = 1;
    h.recorded_at_utc = when;
    BattleConfig_SetDefaults(&h.cfg);
    snprintf(h.cfg.map_name, sizeof h.cfg.map_name, "synthetic");
    char path[TAK_SAVE_PATH_MAX];
    snprintf(path, sizeof path, "%s%s%s", Paths_SaveDir(), slug, TAK_REPLAY_EXT);
    TAK_ReplayWriter *w = TAK_ReplayWriter_Open(path, &h, NULL, 0);
    if (!w) return -1;
    for (uint32_t t = 60; t <= ticks; t += 60) TAK_ReplayWriter_Checkpoint(w, t, t);
    return TAK_ReplayWriter_Close(w, ticks);
}

static void cp_prefs_begin(void) {
    Paths_SetOverride(CP_PREFS);
    TAK_SaveEntry *rows = NULL;
    int n = Replay_List(&rows);
    for (int i = 0; i < n; i++) remove(rows[i].path);
    SaveList_Free(rows);
    for (int round = 0; round < 3; round++) {   /* past the list's cap */
        rows = NULL;
        n = Replay_List(&rows);
        for (int i = 0; i < n; i++) remove(rows[i].path);
        SaveList_Free(rows);
    }
}

static void cp_prefs_end(void) {
    cp_prefs_begin();
    char save[TAK_SAVE_PATH_MAX];
    if (Paths_SaveFile("keep", save, sizeof save) == 0) remove(save);
    Paths_SetOverride(NULL);
}

/* The list reads each file's header and no more: a replay whose stream
 * is damaged is listed from its header and refused only when chosen. */
TEST(the_replay_list_reads_headers_only) {
    cp_prefs_begin();
    ASSERT_EQ_INT(0, cp_fake_replay("one", 1000, 120));
    char path[TAK_SAVE_PATH_MAX];
    snprintf(path, sizeof path, "%sone%s", Paths_SaveDir(), TAK_REPLAY_EXT);
    FILE *f = fopen(path, "r+b");
    ASSERT_NOT_NULL(f);
    fseek(f, TAK_REPLAY_HEADER_BYTES + 3, SEEK_SET);
    fputc(0x5a, f);
    fclose(f);
    TAK_SaveEntry *rows = NULL;
    ASSERT_EQ_INT(1, Replay_List(&rows));
    ASSERT_EQ_INT(1, rows[0].readable);
    ASSERT_EQ_STR("00:00:02", rows[0].game_time);
    SaveList_Free(rows);
    char err[TAK_REPLAY_ERR_MAX] = "";
    ASSERT_EQ_INT(-1, Replay_Open(path, err, sizeof err));
    ASSERT(strstr(err, "damaged") != NULL);
    cp_prefs_end();
}

TEST(the_replay_list_stops_at_its_limit) {
    cp_prefs_begin();
    char slug[32];
    for (int i = 0; i < TAK_REPLAY_LIST_MAX + 6; i++) {
        snprintf(slug, sizeof slug, "r%03d", i);
        ASSERT_EQ_INT(0, cp_fake_replay(slug, 1000u + (uint64_t)i, 60));
    }
    TAK_SaveEntry *rows = NULL;
    ASSERT_EQ_INT(TAK_REPLAY_LIST_MAX, Replay_List(&rows));
    SaveList_Free(rows);
    cp_prefs_end();
}

/* A finished recording leaves the newest replays, the new one among
 * them, and never touches a save. */
TEST(old_replays_are_pruned_and_saves_never) {
    cp_prefs_begin();
    char slug[32];
    for (int i = 0; i < TAK_REPLAY_KEEP + 5; i++) {
        snprintf(slug, sizeof slug, "old%02d", i);
        ASSERT_EQ_INT(0, cp_fake_replay(slug, 1000u + (uint64_t)i, 60));
    }
    char save[TAK_SAVE_PATH_MAX];
    ASSERT_EQ_INT(0, Paths_SaveFile("keep", save, sizeof save));
    FILE *f = fopen(save, "wb");
    ASSERT_NOT_NULL(f);
    fputs("not a replay", f);
    fclose(f);

    ASSERT(cp_world());
    char path[TAK_SAVE_PATH_MAX];
    snprintf(path, sizeof path, "%snew%s", Paths_SaveDir(), TAK_REPLAY_EXT);
    ASSERT_EQ_INT(0, Replay_RecordOpen(path, NULL, 0));
    for (int t = 0; t < 120; t++) cp_file_tick();
    Replay_RecordClose();
    cp_end();

    TAK_SaveEntry *rows = NULL;
    int n = Replay_List(&rows);
    ASSERT_EQ_INT(TAK_REPLAY_KEEP, n);
    int have_new = 0, have_oldest = 0;
    for (int i = 0; i < n; i++) {
        if (strstr(rows[i].path, "new")) have_new = 1;
        if (strstr(rows[i].path, "old00") || strstr(rows[i].path, "old05")) have_oldest = 1;
    }
    SaveList_Free(rows);
    ASSERT(have_new);
    ASSERT(!have_oldest);
    f = fopen(save, "rb");
    ASSERT_NOT_NULL(f);
    fclose(f);
    cp_prefs_end();
}

TEST(the_byte_budget_prunes_the_oldest) {
    cp_prefs_begin();
    char slug[32];
    for (int i = 0; i < 5; i++) {
        snprintf(slug, sizeof slug, "b%d", i);
        ASSERT_EQ_INT(0, cp_fake_replay(slug, 1000u + (uint64_t)i, 6000));
    }
    TAK_SaveEntry *rows = NULL;
    ASSERT_EQ_INT(5, Replay_List(&rows));
    uint32_t one = rows[0].bytes;
    SaveList_Free(rows);
    ASSERT(one > TAK_REPLAY_HEADER_BYTES);
    /* Room for two and a half: the two newest stay. */
    ASSERT_EQ_INT(3, Replay_Prune(100, one * 5 / 2, NULL));
    rows = NULL;
    ASSERT_EQ_INT(2, Replay_List(&rows));
    ASSERT(strstr(rows[0].path, "b4") != NULL);
    ASSERT(strstr(rows[1].path, "b3") != NULL);
    SaveList_Free(rows);
    cp_prefs_end();
}

/* A browser tab that closes loses no more than the flush cadence says:
 * the page is asked to copy the file out as often as it is flushed. */
TEST(a_recording_reaches_storage_every_ten_seconds) {
    cp_prefs_begin();
    ASSERT(cp_world());
    char path[TAK_SAVE_PATH_MAX];
    snprintf(path, sizeof path, "%scadence%s", Paths_SaveDir(), TAK_REPLAY_EXT);
    ASSERT_EQ_INT(0, Replay_RecordOpen(path, NULL, 0));
    unsigned before = Paths_NotifyCount();
    for (int t = 0; t < 600; t++) cp_file_tick();
    ASSERT(Paths_NotifyCount() > before);
    /* And what was asked for is on disk: it plays to ten seconds. */
    uint32_t end = 0;
    TAK_ReplayReader *r = TAK_Replay_Open(path, NULL, &end, NULL, 0);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_INT(600, (int)end);
    TAK_Replay_Close(r);
    Replay_RecordClose();
    cp_end();
    cp_prefs_end();
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
            uint32_t v = TAK_SimHash();
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
    *ai_hash = TAK_SimHash_AI(TAK_SIM_HASH_SEED);
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
    "\tBuilder=1;\n\tBuilderLimited=1;\n\tWorkerTime=10;\n}\n";
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

/* The builderlimited line reads into the def, and a def without it is
 * not limited (legacy:163099-163100). */
TEST(the_builderlimited_line_is_read) {
    cp_order_begin();
    ASSERT_EQ_INT(0, cp_order_one_archive());
    ASSERT_EQ_INT(3, cp_order_load());
    ASSERT_EQ_INT(1, Units_GetDef(Units_FindDefByName("MIDBUILD"))->builder_limited);
    ASSERT_EQ_INT(0, Units_GetDef(Units_FindDefByName("ALPHA"))->builder_limited);
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

/* Four seats of two thousand, the lobby's most, all stand at once and
 * the battle ticks with them: the pool holds 8192. It held 2000. */
TEST(four_full_seats_fit_on_the_map_at_once) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    int made = 0;
    for (int i = 0; i < 8000; i++) {
        int32_t x = 48 + (i % 90) * 33, y = 48 + (i / 90) * 33;
        if (Units_Spawn(CP_DEF_WALKER, 1 + i / 2000, i / 2000, x, y) >= 0) made++;
    }
    ASSERT_EQ_INT(8000, made);
    for (int t = 0; t < 3; t++) cp_tick();
    int count = 0, alive = 0;
    const Unit *units = Units_GetActive(&count);
    for (int i = 0; i < count; i++) if (units[i].alive == UNIT_ALIVE_ACTIVE) alive++;
    ASSERT_EQ_INT(8000, alive);
    printf("(%u bytes a unit) ", (unsigned)sizeof(Unit));
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

/* ── capture ───────────────────────────────────────────────────────── */

/* The live unit of one def that a seat owns, or -1. */
static int cp_find_owned(int def_idx, int seat) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    for (int i = 0; i < count; i++) {
        if (units[i].alive == UNIT_ALIVE_ACTIVE &&
            (int)units[i].def_idx == def_idx &&
            (int)units[i].player_id == seat) return i;
    }
    return -1;
}

/* A harpy with its reserve full. A caster's own mana starts empty
 * (legacy:226669), and these cases are about the spell, not the wait. */
static int cp_harpy(int seat, int32_t x, int32_t y) {
    int h = Units_Spawn(CP_DEF_HARPY, seat, seat - 1, x, y);
    float cur = 0.0f, max = 0.0f;
    if (h >= 0 && Units_GetMana(h, &cur, &max)) Units_DebugSetMana(h, max);
    return h;
}

/* The Harpy's attack is its mind control shot. Ordered onto an enemy
 * it pays for each shot from its own reserve, and the unit it strikes
 * comes over to its side (legacy:247761-247798). */
TEST(a_harpys_mind_control_turns_an_enemy_to_its_side) {
    ASSERT_NOT_NULL(cp_world());
    int harpy = cp_harpy(1, 800, 800);
    int prey = Units_Spawn(CP_DEF_SQUAD2, 2, 1, 900, 800);
    ASSERT(harpy >= 0 && prey >= 0);
    uint32_t prey_id = Units_GetStableId(prey);

    cp_cmd(TAK_CMD_ATTACK, 1);
    g_cmd.target_unit_id = prey_id;
    cp_cmd_unit(harpy);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));

    int mine = -1;
    for (int t = 0; t < 900 && mine < 0; t++) {
        cp_tick();
        mine = cp_find_owned(CP_DEF_SQUAD2, 1);
    }
    ASSERT(mine >= 0);
    /* The unit that came over stands where the enemy stood, unhurt,
     * and the enemy's own unit is gone. */
    ASSERT_EQ_INT(-1, cp_find_owned(CP_DEF_SQUAD2, 2));
    ASSERT_EQ_INT(-1, Units_FindByStableId(prey_id));
    ASSERT_EQ_INT(900, cp_unit(mine)->world_x);
    ASSERT_EQ_INT(800, cp_unit(mine)->world_y);
    ASSERT_EQ_INT(200, cp_unit(mine)->health);
    ASSERT(cp_unit(harpy)->mana < 1000.0f);
    cp_end();
}

/* The capture order on the wire is the attack with that weapon. */
TEST(a_capture_order_sends_the_harpy_to_attack) {
    ASSERT_NOT_NULL(cp_world());
    int harpy = cp_harpy(1, 800, 800);
    int prey = Units_Spawn(CP_DEF_WALKER, 2, 1, 900, 800);
    ASSERT(harpy >= 0 && prey >= 0);

    cp_cmd(TAK_CMD_CAPTURE, 1);
    g_cmd.target_unit_id = Units_GetStableId(prey);
    cp_cmd_unit(harpy);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_CMD_ATTACK, (int)cp_unit(harpy)->cmd_kind);
    ASSERT_EQ_INT(prey, (int)cp_unit(harpy)->target);
    cp_end();
}

/* Only a unit that carries cancapture takes the capture order. */
TEST(a_unit_that_cannot_capture_refuses_the_order) {
    ASSERT_NOT_NULL(cp_world());
    int archer = Units_Spawn(CP_DEF_ARCHER, 1, 0, 800, 800);
    int prey = Units_Spawn(CP_DEF_WALKER, 2, 1, 900, 800);
    ASSERT(archer >= 0 && prey >= 0);

    cp_cmd(TAK_CMD_CAPTURE, 1);
    g_cmd.target_unit_id = Units_GetStableId(prey);
    cp_cmd_unit(archer);
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(archer)->cmd_kind);
    cp_end();
}

/* The attack order as a click delivers it. */
static int cp_order_attack(int attacker, int prey) {
    cp_cmd(TAK_CMD_ATTACK, 1);
    g_cmd.target_unit_id = Units_GetStableId(prey);
    cp_cmd_unit(attacker);
    return TAK_CommandExec_Apply(&g_cmd);
}

/* What comes over is a fresh unit of the same kind. It keeps the health
 * it had and where it stood, and loses its kills, its experience and
 * its stance (legacy:228975-228998). */
TEST(a_captured_unit_starts_over_as_a_recruit) {
    ASSERT_NOT_NULL(cp_world());
    int harpy = cp_harpy(1, 800, 800);
    int prey = Units_Spawn(CP_DEF_SQUAD2, 2, 1, 900, 800);
    ASSERT(harpy >= 0 && prey >= 0);
    Unit *pu = (Unit *)cp_unit(prey);   /* test-only mutation */
    pu->health = 120;
    pu->kills = 7;
    pu->experience_pts = 50;
    pu->aggro_mode = UNIT_AGGRO_PASSIVE;

    ASSERT_EQ_INT(1, cp_order_attack(harpy, prey));
    int mine = -1;
    for (int t = 0; t < 900 && mine < 0; t++) {
        cp_tick();
        mine = cp_find_owned(CP_DEF_SQUAD2, 1);
    }
    ASSERT(mine >= 0);
    const Unit *nu = cp_unit(mine);
    ASSERT_EQ_INT(120, nu->health);
    ASSERT_EQ_INT(900, nu->world_x);
    ASSERT_EQ_INT(0, (int)nu->kills);
    ASSERT_EQ_INT(0, nu->experience_pts);
    ASSERT_EQ_INT(UNIT_AGGRO_OFFENSIVE, (int)nu->aggro_mode);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)nu->cmd_kind);

    /* It answers to its new seat and not to its old one. */
    cp_cmd(TAK_CMD_MOVE, 2);
    g_cmd.target_x = 1600; g_cmd.target_y = 1600;
    cp_cmd_unit(mine);
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    g_cmd.seat = 1;
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    cp_end();
}

/* cantbecaptured keeps the shot from being fired at all, so the Harpy
 * pays nothing and lets the order go (legacy:15151, legacy:247785). */
TEST(a_unit_that_cannot_be_captured_is_never_fired_on) {
    ASSERT_NOT_NULL(cp_world());
    int harpy = cp_harpy(1, 800, 800);
    int prey = Units_Spawn(CP_DEF_GUARDED, 2, 1, 900, 800);
    ASSERT(harpy >= 0 && prey >= 0);
    ASSERT_EQ_INT(1, cp_order_attack(harpy, prey));
    for (int t = 0; t < 300; t++) cp_tick();
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)cp_unit(prey)->alive);
    ASSERT_EQ_INT(2, (int)cp_unit(prey)->player_id);
    ASSERT(cp_unit(harpy)->mana >= 999.0f);
    ASSERT_EQ_INT(-1, (int)cp_unit(harpy)->target);
    cp_end();
}

/* A monarch is fired on and paid for, and the hit neither takes it nor
 * wounds it: mind control deals no damage (legacy:247776, legacy:245361). */
TEST(a_monarch_is_struck_and_stays_its_own) {
    ASSERT_NOT_NULL(cp_world());
    int harpy = cp_harpy(1, 800, 800);
    int king = Units_Spawn(CP_DEF_MONARCH, 2, 1, 900, 800);
    ASSERT(harpy >= 0 && king >= 0);
    ASSERT_EQ_INT(1, cp_order_attack(harpy, king));
    for (int t = 0; t < 400; t++) cp_tick();
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)cp_unit(king)->alive);
    ASSERT_EQ_INT(2, (int)cp_unit(king)->player_id);
    ASSERT_EQ_INT(500, cp_unit(king)->health);
    ASSERT(cp_unit(harpy)->mana < 800.0f);
    cp_end();
}

/* The new unit is made before the old one goes, so a seat at its unit
 * limit takes nothing and the enemy keeps its unit (legacy:228975). */
TEST(a_seat_at_its_unit_limit_captures_nothing) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    w->cfg.units_per_side = 1;
    int harpy = cp_harpy(1, 800, 800);
    int prey = Units_Spawn(CP_DEF_SQUAD2, 2, 1, 900, 800);
    ASSERT(harpy >= 0 && prey >= 0);
    ASSERT_EQ_INT(1, cp_order_attack(harpy, prey));
    for (int t = 0; t < 400; t++) cp_tick();
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)cp_unit(prey)->alive);
    ASSERT_EQ_INT(2, (int)cp_unit(prey)->player_id);
    ASSERT_EQ_INT(-1, cp_find_owned(CP_DEF_SQUAD2, 1));
    ASSERT(cp_unit(harpy)->mana < 800.0f);
    cp_end();
}

/* A loaded transport can be taken (legacy:247782 asks only whether the
 * victim is a rider). Its riders step out where it stood, still their
 * own side's, and the unit that came over is empty. */
TEST(a_captured_transport_sets_its_riders_down) {
    ASSERT_NOT_NULL(cp_world());
    int boat = Units_Spawn(CP_DEF_CARRIER, 2, 1, 900, 800);
    int rider = Units_Spawn(CP_DEF_WALKER, 2, 1, 900, 840);
    int harpy = cp_harpy(1, 800, 800);
    ASSERT(boat >= 0 && rider >= 0 && harpy >= 0);
    Unit *bu = (Unit *)cp_unit(boat);    /* test-only mutation */
    Unit *ru = (Unit *)cp_unit(rider);   /* test-only mutation */
    ru->alive = UNIT_ALIVE_TRANSPORTED;
    ru->carried_by = (int16_t)boat;
    ru->world_x = bu->world_x;
    ru->world_y = bu->world_y;
    bu->cargo_count = 1;

    ASSERT_EQ_INT(1, cp_order_attack(harpy, boat));
    int mine = -1;
    for (int t = 0; t < 900 && mine < 0; t++) {
        cp_tick();
        mine = cp_find_owned(CP_DEF_CARRIER, 1);
    }
    ASSERT(mine >= 0);
    cp_tick();
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)cp_unit(rider)->alive);
    ASSERT_EQ_INT(2, (int)cp_unit(rider)->player_id);
    ASSERT_EQ_INT(-1, (int)cp_unit(rider)->carried_by);
    ASSERT_EQ_INT(0, (int)cp_unit(mine)->cargo_count);
    cp_end();
}

/* A splash rolls for every enemy it reaches and wounds none of them,
 * and leaves alone the one it does not reach (legacy:245217-245224). */
TEST(a_mind_control_splash_rolls_for_each_unit_in_reach) {
    ASSERT_NOT_NULL(cp_world());
    int mage = Units_Spawn(CP_DEF_MINDMAGE, 1, 0, 800, 800);
    int near_a = Units_Spawn(CP_DEF_SQUAD2, 2, 1, 900, 800);
    int near_b = Units_Spawn(CP_DEF_SQUAD2, 2, 1, 960, 800);
    int far_c = Units_Spawn(CP_DEF_SQUAD2, 2, 1, 900, 1060);
    ASSERT(mage >= 0 && near_a >= 0 && near_b >= 0 && far_c >= 0);
    /* Passive, so the one ordered shot is the only shot, and a full
     * reserve to pay for it. */
    Units_DebugSetAggro(mage, UNIT_AGGRO_PASSIVE);
    {
        float cur = 0.0f, max = 0.0f;
        if (Units_GetMana(mage, &cur, &max)) Units_DebugSetMana(mage, max);
    }
    uint32_t draws_before = World_RandState();

    ASSERT_EQ_INT(1, cp_order_attack(mage, near_a));
    for (int t = 0; t < 80; t++) cp_tick();

    /* Two units in reach, so the generator moved, and with a recruit's
     * 80 in 100 apiece this seed brings at least one of them over. */
    ASSERT(World_RandState() != draws_before);
    int count = 0, came_over = 0;
    const Unit *units = Units_GetActive(&count);
    for (int i = 0; i < count; i++) {
        if (units[i].alive != UNIT_ALIVE_ACTIVE) continue;
        if ((int)units[i].def_idx != CP_DEF_SQUAD2) continue;
        ASSERT_EQ_INT(200, units[i].health);
        if (units[i].player_id == 1) came_over++;
    }
    ASSERT(came_over >= 1);
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)cp_unit(far_c)->alive);
    ASSERT_EQ_INT(2, (int)cp_unit(far_c)->player_id);
    cp_end();
}

/* The roll out of 100 by the victim's rank: (rank + 16) * 5 held to 99
 * (legacy:247788-247793). A veteran is the easier one to take. */
TEST(the_capture_roll_rises_with_the_victims_rank) {
    ASSERT_EQ_INT(80, Units_CaptureThreshold(0));
    ASSERT_EQ_INT(85, Units_CaptureThreshold(1));
    ASSERT_EQ_INT(90, Units_CaptureThreshold(2));
    ASSERT_EQ_INT(95, Units_CaptureThreshold(3));
    ASSERT_EQ_INT(99, Units_CaptureThreshold(4));
    ASSERT_EQ_INT(99, Units_CaptureThreshold(10));
}

/* ── a mind control shot's own flight ──────────────────────────────── */

/* The caster's shot in the air, or NULL. */
static const Projectile *cp_mind_shot(int caster) {
    int n = 0;
    const Projectile *p = Units_GetProjectiles(&n);
    for (int i = 0; p && i < n; i++)
        if (p[i].alive && p[i].mind_control && p[i].shooter == caster) return &p[i];
    return NULL;
}

/* A Harpy and its prey on two cells a side, like every shipped unit,
 * both passive so the one shot fired by hand is the only one. */
static int cp_mind_pair(int32_t prey_x, int32_t prey_y, int *out_prey) {
    int harpy = cp_harpy(1, 800, 800);
    int prey = Units_Spawn(CP_DEF_SQUAD2, 2, 1, prey_x, prey_y);
    if (harpy < 0 || prey < 0) return -1;
    Units_DebugSetAggro(harpy, UNIT_AGGRO_PASSIVE);
    Units_DebugSetAggro(prey, UNIT_AGGRO_PASSIVE);
    *out_prey = prey;
    return harpy;
}

/* The shot strikes only a body in its own cell, inside that unit's
 * selection quad (legacy:236955-237027). A unit that steps 20 px off
 * the line still holds the cell the shot crosses and is passed, and
 * nothing is rolled. */
TEST(a_mind_control_shot_passes_a_unit_that_stepped_aside) {
    ASSERT_NOT_NULL(cp_world());
    int prey = -1;
    int harpy = cp_mind_pair(900, 800, &prey);
    ASSERT(harpy >= 0);
    ASSERT_EQ_INT(1, Units_DebugFireAt(harpy, 0, prey));
    ASSERT_NOT_NULL(cp_mind_shot(harpy));
    ASSERT_EQ_INT(0, Units_DebugPlace(prey, 900, 820));
    uint32_t draws = World_RandState();
    for (int t = 0; t < 300 && cp_mind_shot(harpy); t++) cp_tick();
    ASSERT_NULL(cp_mind_shot(harpy));
    ASSERT_EQ_INT((int)draws, (int)World_RandState());
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)cp_unit(prey)->alive);
    ASSERT_EQ_INT(2, (int)cp_unit(prey)->player_id);
    cp_end();
}

/* The shot leaves for where a walker will be: its sweet spot plus four
 * fifths of its velocity over the flight from the caster
 * (legacy:234031-234060). */
TEST(a_mind_control_shot_leads_a_walker_by_four_fifths) {
    ASSERT_NOT_NULL(cp_world());
    int prey = -1;
    int harpy = cp_mind_pair(1000, 800, &prey);
    ASSERT(harpy >= 0);
    Unit *pu = (Unit *)cp_unit(prey);   /* test-only mutation */
    pu->heading = 3.14159265f;          /* walking south */
    pu->cur_speed_ppt = 0.7f;
    ASSERT_EQ_INT(1, Units_DebugFireAt(harpy, 0, prey));
    const Projectile *s = cp_mind_shot(harpy);
    ASSERT_NOT_NULL(s);
    /* 200 px across and 16 up to the sweet spot at 169 px/s is 71.2
     * ticks, and 0.8 of that at 0.7 px a tick is 40 px. */
    ASSERT_EQ_INT(1000, s->dest_x);
    ASSERT_EQ_INT(840, s->dest_y);
    cp_end();
}

/* A miss flies its range at its speed across the ground, then the
 * frame it is in, and is gone (legacy:246919-246922). */
TEST(a_mind_control_shot_ends_at_its_range) {
    ASSERT_NOT_NULL(cp_world());
    int prey = -1;
    int harpy = cp_mind_pair(900, 800, &prey);
    ASSERT(harpy >= 0);
    ASSERT_EQ_INT(1, Units_DebugFireAt(harpy, 0, prey));
    ASSERT_EQ_INT(0, Units_DebugPlace(prey, 900, 1000));
    int lived = 0;
    while (cp_mind_shot(harpy) && lived < 400) {
        cp_tick();
        lived++;
    }
    /* 300 px at 169 px/s is 106.5 ticks. */
    ASSERT(lived >= 106 && lived <= 110);
    cp_end();
}

/* A shot is freed once its caster is dead or dying
 * (legacy:247710-247713), so a Harpy struck down mid cast takes
 * nothing, not even a veteran it would surely have taken. */
TEST(a_mind_control_shot_dies_with_its_caster) {
    ASSERT_NOT_NULL(cp_world());
    int prey = -1;
    int harpy = cp_mind_pair(1000, 800, &prey);
    ASSERT(harpy >= 0);
    Units_DebugSetVeteranLevel(prey, 4);
    ASSERT_EQ_INT(1, Units_DebugFireAt(harpy, 0, prey));
    for (int t = 0; t < 20; t++) cp_tick();
    ASSERT_NOT_NULL(cp_mind_shot(harpy));
    ASSERT_EQ_INT(1, Units_KillAllOf(1));
    cp_tick();
    ASSERT_NULL(cp_mind_shot(harpy));
    for (int t = 0; t < 200; t++) cp_tick();
    ASSERT_EQ_INT(UNIT_ALIVE_ACTIVE, (int)cp_unit(prey)->alive);
    ASSERT_EQ_INT(2, (int)cp_unit(prey)->player_id);
    cp_end();
}

/* A still recruit is struck by every shot and taken on a draw under 80
 * (legacy:247788-247793). Each seed's first draw says which, and these
 * ten draw under 80 eight times. Seeding forces the low bit, so the
 * seeds are odd. */
TEST(a_still_recruit_is_struck_and_taken_four_times_in_five) {
    int taken = 0;
    for (uint32_t seed = 1; seed < 20; seed += 2) {
        ASSERT_NOT_NULL(cp_world());
        int prey = -1;
        int harpy = cp_mind_pair(900, 800, &prey);
        ASSERT(harpy >= 0);
        World_SeedRand(seed);
        int roll = (int)World_Rand(100);
        uint32_t after = World_RandState();
        World_SeedRand(seed);
        ASSERT_EQ_INT(1, Units_DebugFireAt(harpy, 0, prey));
        for (int t = 0; t < 200 && cp_mind_shot(harpy); t++) cp_tick();
        ASSERT_EQ_INT((int)after, (int)World_RandState());
        int came = cp_find_owned(CP_DEF_SQUAD2, 1) >= 0;
        ASSERT_EQ_INT(roll < 80, came);
        taken += came;
        cp_end();
    }
    ASSERT_EQ_INT(8, taken);
}

/* The quad turns with its unit: facing north a long body runs north
 * and south, facing east it runs east and west (legacy:237007-237020). */
TEST(a_units_selection_quad_turns_with_it) {
    ASSERT_NOT_NULL(cp_world());
    UnitDef *d = (UnitDef *)Units_GetDef(CP_DEF_SQUAD3);   /* test-only mutation */
    d->body_span_set = 1;
    d->body_bottom_px = 0;
    d->body_top_px = 32;
    /* 8 px across and 40 along, the fourth corner first. */
    const int32_t q[4][2] = { { -4 * 65536, 20 * 65536 }, { 4 * 65536, 20 * 65536 },
                              { 4 * 65536, -20 * 65536 }, { -4 * 65536, -20 * 65536 } };
    memcpy(d->body_quad, q, sizeof(q));
    d->body_quad_set = 1;
    int u = Units_Spawn(CP_DEF_SQUAD3, 2, 1, 1000, 1000);
    ASSERT(u >= 0);
    Unit *uu = (Unit *)cp_unit(u);   /* test-only mutation */
    uu->heading = 0.0f;
    ASSERT_EQ_INT(1, Units_DebugQuadHolds(u, 1000, 1000));
    ASSERT_EQ_INT(1, Units_DebugQuadHolds(u, 1001, 1016));
    ASSERT_EQ_INT(0, Units_DebugQuadHolds(u, 1016, 1001));
    uu->heading = 1.5707963f;
    ASSERT_EQ_INT(1, Units_DebugQuadHolds(u, 1016, 1001));
    ASSERT_EQ_INT(0, Units_DebugQuadHolds(u, 1001, 1016));
    /* Its edges are outside. */
    uu->heading = 0.0f;
    ASSERT_EQ_INT(0, Units_DebugQuadHolds(u, 1004, 1000));
    ASSERT_EQ_INT(1, Units_DebugQuadHolds(u, 1003, 1000));
    cp_end();
}

#define CP_CAPTURE_TICKS 240

/* One capture battle from a fixed seed: the hash after every tick, and
 * the tick the unit changed hands, -1 for never, -2 for no battle. */
static int cp_capture_run(uint32_t *out) {
    g_cp_seed = 77u;
    int took = -2;
    if (cp_world()) {
        int harpy = cp_harpy(1, 800, 800);
        int prey = Units_Spawn(CP_DEF_SQUAD2, 2, 1, 900, 800);
        if (harpy >= 0 && prey >= 0 && cp_order_attack(harpy, prey) == 1) {
            took = -1;
            for (int t = 0; t < CP_CAPTURE_TICKS; t++) {
                cp_tick();
                out[t] = TAK_SimHash();
                if (took < 0 && cp_find_owned(CP_DEF_SQUAD2, 1) >= 0) took = t;
            }
        }
        cp_end();
    }
    g_cp_seed = 0;
    return took;
}

/* The roll comes off the session seed, so two machines given the same
 * battle change the unit's owner on the same tick and hash alike. */
TEST(a_capture_lands_on_the_same_tick_on_every_machine) {
    static uint32_t a[CP_CAPTURE_TICKS], b[CP_CAPTURE_TICKS];
    int ta = cp_capture_run(a);
    int tb = cp_capture_run(b);
    ASSERT(ta >= 0);
    ASSERT_EQ_INT(ta, tb);
    for (int i = 0; i < CP_CAPTURE_TICKS; i++) {
        ASSERT_EQ_INT((int)a[i], (int)b[i]);
    }
}

/* Issue #234, the other half. A caster with no maxmana pays out of its
 * seat's pool, and when the pool is short of the slot it has selected
 * it drops to one the pool can cover and throws that
 * (legacy:245905-245913). */
TEST(a_caster_on_the_seats_pool_drops_to_a_spell_it_can_pay_for) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    int mage = Units_Spawn(CP_DEF_POOLMAGE, 1, 0, 800, 800);
    int prey = Units_Spawn(CP_DEF_WALKER, 2, 1, 900, 800);
    ASSERT(mage >= 0 && prey >= 0);
    ASSERT_EQ_INT(0, (int)Units_GetDef(CP_DEF_POOLMAGE)->max_mana);

    /* The seat holds enough for the cheap spell and not the dear one. */
    Economy_AdjustCaps(&w->economy, 1, 1000, 0.0f);
    int have = Economy_GetMana(&w->economy, 1);
    ASSERT(have > 150);
    ASSERT_EQ_INT(1, Economy_TrySpend(&w->economy, 1, have - 150));
    ASSERT_EQ_INT(150, Economy_GetMana(&w->economy, 1));
    ASSERT_EQ_INT(1, Units_OrderSetWeaponSlot(mage, 1));

    cp_cmd(TAK_CMD_ATTACK, 1);
    g_cmd.target_unit_id = Units_GetStableId(prey);
    cp_cmd_unit(mage);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));

    cp_tick();
    ASSERT_EQ_INT(0, (int)cp_unit(mage)->weapon_slot);

    int hp0 = cp_unit(prey)->health;
    int hit = 0;
    for (int t = 0; t < 600 && !hit; t++) {
        cp_tick();
        if (cp_unit(prey)->alive != UNIT_ALIVE_ACTIVE ||
            cp_unit(prey)->health < hp0) hit = 1;
    }
    ASSERT(hit);
    /* The cheap spell is what the pool paid for. */
    ASSERT_EQ_INT(50, Economy_GetMana(&w->economy, 1));
    cp_end();
}

/* ── formation moves ───────────────────────────────────────────────── */

/* A formation as a turn bundle delivers it, from seat 1: xy holds a
 * point per unit and the first is the one the rest are measured from. */
static void cp_formation(const int *h, const int32_t *xy, int n,
                         uint16_t flags, uint16_t heading) {
    cp_cmd(TAK_CMD_MOVE_FORMATION, 1);
    g_cmd.target_x = xy[0];
    g_cmd.target_y = xy[1];
    g_cmd.arg = flags;
    g_cmd.build_type_id = heading;
    for (int i = 0; i < n; i++) {
        g_cmd.unit_dx[g_cmd.unit_count] = (int16_t)(xy[2 * i] - xy[0]);
        g_cmd.unit_dy[g_cmd.unit_count] = (int16_t)(xy[2 * i + 1] - xy[1]);
        cp_cmd_unit(h[i]);
    }
}

static int cp_near(int h, int32_t x, int32_t y, int r) {
    const Unit *u = cp_unit(h);
    int64_t dx = u->world_x - x, dy = u->world_y - y;
    return dx * dx + dy * dy <= (int64_t)r * r;
}

static float cp_turn_gap(float a, float b) {
    float d = a - b;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    return d < 0.0f ? -d : d;
}

/* Tick until every unit named has no order and no leg left, or the
 * budget runs out. Returns the ticks taken, or -1. */
static int cp_until_idle(const int *h, int n, int budget) {
    for (int t = 0; t < budget; t++) {
        int busy = 0;
        for (int i = 0; i < n; i++)
            if (cp_unit(h[i])->cmd_kind != UNIT_CMD_NONE || cp_unit(h[i])->leg_count) busy = 1;
        if (!busy) return t;
        cp_tick();
    }
    return -1;
}

TEST(a_formation_sends_each_unit_to_its_own_point) {
    ASSERT_NOT_NULL(cp_world());
    int h[3];
    for (int i = 0; i < 3; i++) h[i] = Units_Spawn(CP_DEF_WALKER, 1, 0, 800 + i * 48, 800);
    ASSERT(h[0] >= 0 && h[1] >= 0 && h[2] >= 0);
    static const int32_t xy[6] = { 1400, 1200, 1464, 1200, 1400, 1264 };
    cp_formation(h, xy, 3, 0, 0);
    ASSERT_EQ_INT(3, TAK_CommandExec_Apply(&g_cmd));
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)cp_unit(h[i])->cmd_kind);
        ASSERT_EQ_INT(xy[2 * i], cp_unit(h[i])->cmd_x);
        ASSERT_EQ_INT(xy[2 * i + 1], cp_unit(h[i])->cmd_y);
    }
    ASSERT(cp_until_idle(h, 3, 3000) >= 0);
    for (int i = 0; i < 3; i++) ASSERT(cp_near(h[i], xy[2 * i], xy[2 * i + 1], 24));
    cp_end();
}

/* The heading comes after the walk and stays: a unit knocked off it
 * turns back, and only the next order lets it go. */
TEST(a_formation_turns_to_its_heading_on_arrival_and_holds_it) {
    ASSERT_NOT_NULL(cp_world());
    int h = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
    ASSERT(h >= 0);
    /* Walking east, then facing the other way. */
    static const int32_t xy[2] = { 1100, 800 };
    const uint16_t west = 49152;
    const float want = Units_HeadingFromTurn(west);
    cp_formation(&h, xy, 1, TAK_FORMATION_FACE, west);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_FACE_ARRIVE, (int)cp_unit(h)->face_mode);
    int mid = 0;
    for (int t = 0; t < 3000 && cp_unit(h)->cmd_kind != UNIT_CMD_NONE; t++) {
        cp_tick();
        if (t == 120) mid = cp_turn_gap(cp_unit(h)->heading, want) > 2.5f;
    }
    ASSERT_EQ_INT(1, mid);          /* it walked facing the way it went */
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(h)->cmd_kind);
    for (int t = 0; t < 60; t++) cp_tick();
    ASSERT(cp_turn_gap(cp_unit(h)->heading, want) < 0.01f);
    ASSERT_EQ_INT(UNIT_FACE_HOLD, (int)cp_unit(h)->face_mode);

    ((Unit *)cp_unit(h))->heading = 0.0f;
    for (int t = 0; t < 60; t++) cp_tick();
    ASSERT(cp_turn_gap(cp_unit(h)->heading, want) < 0.01f);

    /* A plain move lets it go: after it the unit keeps the heading its
     * walk left it with. */
    cp_cmd(TAK_CMD_MOVE, 1);
    g_cmd.target_x = 1100;
    g_cmd.target_y = 1100;
    cp_cmd_unit(h);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_FACE_NONE, (int)cp_unit(h)->face_mode);
    ASSERT(cp_until_idle(&h, 1, 3000) >= 0);
    for (int t = 0; t < 60; t++) cp_tick();
    ASSERT(cp_turn_gap(cp_unit(h)->heading, want) > 1.0f);
    cp_end();
}

/* A fast unit beside a slow one keeps the slow one's pace, and the two
 * arrive together. Without the pace it gets there far ahead. */
TEST(a_formation_at_group_pace_keeps_to_its_slowest_unit) {
    for (int paced = 1; paced >= 0; paced--) {
        ASSERT_NOT_NULL(cp_world());
        int h[2];
        h[0] = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);    /* 1.4 */
        h[1] = Units_Spawn(CP_DEF_HARPY, 1, 0, 800, 860);     /* 3.5 */
        ASSERT(h[0] >= 0 && h[1] >= 0);
        Units_DebugSetAggro(h[1], UNIT_AGGRO_PASSIVE);
        static const int32_t xy[4] = { 1500, 800, 1500, 860 };
        cp_formation(h, xy, 2, paced ? TAK_FORMATION_GROUP_PACE : 0, 0);
        ASSERT_EQ_INT(2, TAK_CommandExec_Apply(&g_cmd));
        int done[2] = { -1, -1 };
        float fastest = 0.0f;
        for (int t = 0; t < 4000 && (done[0] < 0 || done[1] < 0); t++) {
            cp_tick();
            if (cp_unit(h[1])->cur_speed_ppt > fastest) fastest = cp_unit(h[1])->cur_speed_ppt;
            for (int i = 0; i < 2; i++)
                if (done[i] < 0 && cp_unit(h[i])->cmd_kind == UNIT_CMD_NONE) done[i] = t;
        }
        ASSERT(done[0] >= 0 && done[1] >= 0);
        printf("(%s: walker %d ticks, harpy %d, top %.2f) ", paced ? "paced" : "free",
               done[0], done[1], (double)fastest);
        if (paced) {
            ASSERT(fastest <= 1.4f * 0.5f + 0.0001f);
            ASSERT(done[1] >= done[0] - 30);
        } else {
            ASSERT(fastest > 1.4f);
            ASSERT(done[1] * 2 < done[0]);
        }
        ASSERT_EQ_INT(0, (int)cp_unit(h[1])->move_group);
        cp_end();
    }
}

/* Shift: a queued formation waits for the order in hand, legs run in
 * the order given, and an order that is not queued forgets them all. */
TEST(a_queued_formation_waits_for_the_order_in_hand) {
    ASSERT_NOT_NULL(cp_world());
    int h = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
    ASSERT(h >= 0);
    static const int32_t a[2] = { 1100, 800 }, b[2] = { 1100, 1100 }, c[2] = { 800, 1100 };
    cp_formation(&h, a, 1, 0, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    cp_formation(&h, b, 1, TAK_FORMATION_QUEUE, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    cp_formation(&h, c, 1, TAK_FORMATION_QUEUE | TAK_FORMATION_FACE, 16384);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(a[0], cp_unit(h)->cmd_x);
    ASSERT_EQ_INT(a[1], cp_unit(h)->cmd_y);
    ASSERT_EQ_INT(2, (int)cp_unit(h)->leg_count);

    int seen_b = 0, b_from_a = 0;
    for (int t = 0; t < 6000 && (cp_unit(h)->cmd_kind != UNIT_CMD_NONE ||
                                 cp_unit(h)->leg_count); t++) {
        cp_tick();
        if (!seen_b && cp_unit(h)->cmd_x == b[0] && cp_unit(h)->cmd_y == b[1]) {
            seen_b = 1;
            b_from_a = cp_near(h, a[0], a[1], 24);
        }
    }
    ASSERT_EQ_INT(1, seen_b);
    ASSERT_EQ_INT(1, b_from_a);
    ASSERT(cp_near(h, c[0], c[1], 24));
    for (int t = 0; t < 60; t++) cp_tick();
    ASSERT(cp_turn_gap(cp_unit(h)->heading, Units_HeadingFromTurn(16384)) < 0.01f);

    /* Queued on a unit with nothing to do, it goes at once. */
    cp_formation(&h, a, 1, TAK_FORMATION_QUEUE, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)cp_unit(h)->cmd_kind);
    ASSERT_EQ_INT(0, (int)cp_unit(h)->leg_count);

    /* Stop forgets what was queued. */
    cp_formation(&h, b, 1, TAK_FORMATION_QUEUE, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, (int)cp_unit(h)->leg_count);
    cp_cmd(TAK_CMD_STOP, 1);
    cp_cmd_unit(h);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(0, (int)cp_unit(h)->leg_count);
    for (int t = 0; t < 120; t++) cp_tick();
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(h)->cmd_kind);
    cp_end();
}

/* More units than one command holds go as several, and every unit
 * gets its own point all the same. */
TEST(a_formation_past_a_commands_units_goes_as_several) {
    ASSERT_NOT_NULL(cp_world());
    enum { N = 300 };
    static int h[N];
    static int32_t xy[2 * N];
    for (int i = 0; i < N; i++) {
        int32_t x = 400 + (i % 20) * 32, y = 400 + (i / 20) * 32;
        h[i] = Units_Spawn(CP_DEF_WALKER, 1, 0, x, y);
        ASSERT(h[i] >= 0);
        xy[2 * i] = x + 900;
        xy[2 * i + 1] = y + 100;
    }
    ASSERT_EQ_INT(0, TAK_Cmd_EmitFormation(h, xy, N, 0, 0));
    /* 128, 128 and 44, one move. */
    ASSERT_EQ_INT(3, TAK_CmdQueue_Pending());
    uint32_t move = 0;
    for (int q = 0, seen = 0; q < TAK_CMD_QUEUE_MAX; q++) {
        TAK_GameCommand c;
        uint32_t arrival = 0;
        if (!TAK_CmdQueue_At(q, &c, &arrival)) continue;
        ASSERT(c.unit_count <= TAK_FORMATION_CHUNK);
        if (seen++ == 0) move = c.target_unit_id;
        ASSERT_EQ_INT((int)move, (int)c.target_unit_id);
    }
    ASSERT(move != 0);
    TAK_CmdQueue_Run();
    for (int i = 0; i < N; i++) {
        ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)cp_unit(h[i])->cmd_kind);
        ASSERT_EQ_INT(xy[2 * i], cp_unit(h[i])->cmd_x);
        ASSERT_EQ_INT(xy[2 * i + 1], cp_unit(h[i])->cmd_y);
    }
    cp_end();
}

/* A leg in hand is state: two worlds alike but for one queued leg hash
 * apart, so a peer that lost it would be caught. */
TEST(the_hash_sees_a_queued_leg) {
    uint32_t v[2];
    for (int k = 0; k < 2; k++) {
        ASSERT_NOT_NULL(cp_world());
        int h = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
        ASSERT(h >= 0);
        static const int32_t a[2] = { 1100, 800 }, b[2] = { 1100, 1100 };
        cp_formation(&h, a, 1, TAK_FORMATION_FACE, 100);
        ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
        if (k) {
            cp_formation(&h, b, 1, TAK_FORMATION_QUEUE, 0);
            ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
        }
        v[k] = TAK_SimHash();
        cp_end();
    }
    ASSERT(v[0] != v[1]);
}

/* The formation path end to end, from the emitter through the queue: a
 * live session and its replay from the recording hash alike at every
 * sample, queue, pace and heading included. */
#define CP_FORM_TICKS 1800
static TAK_GameCommand g_form_rec[8];
static int g_form_rec_n;

static void cp_form_record(const TAK_GameCommand *cmd, void *user) {
    (void)user;
    if (g_form_rec_n < 8) g_form_rec[g_form_rec_n++] = *cmd;
}

static int cp_form_run(uint32_t *out, int live) {
    if (!cp_world()) return 0;
    int h[4];
    for (int i = 0; i < 4; i++) {
        h[i] = Units_Spawn(i == 3 ? CP_DEF_HARPY : CP_DEF_WALKER, 1, 0, 800 + i * 48, 800);
        if (h[i] < 0) return 0;
        Units_DebugSetAggro(h[i], UNIT_AGGRO_PASSIVE);
    }
    if (live) {
        g_form_rec_n = 0;
        TAK_CmdQueue_SetObserver(cp_form_record, NULL);
    } else {
        for (int i = 0; i < g_form_rec_n; i++)
            if (TAK_CmdQueue_SubmitAt(&g_form_rec[i]) != 0) return 0;
    }
    static const int32_t line[8] = { 1300, 1000, 1348, 1000, 1396, 1000, 1444, 1000 };
    static const int32_t box[8]  = { 1000, 1400, 1048, 1400, 1000, 1448, 1048, 1448 };
    for (int t = 0; t < CP_FORM_TICKS; t++) {
        if (live && t == 10 &&
            TAK_Cmd_EmitFormation(h, line, 4, TAK_FORMATION_GROUP_PACE | TAK_FORMATION_FACE,
                                  20000) != 0) return 0;
        if (live && t == 30 &&
            TAK_Cmd_EmitFormation(h, box, 4, TAK_FORMATION_QUEUE | TAK_FORMATION_FACE,
                                  60000) != 0) return 0;
        cp_tick();
        if ((t + 1) % 60 == 0) out[t / 60] = TAK_SimHash();
    }
    cp_end();
    return 1;
}

TEST(a_formation_session_replays_to_the_same_hashes) {
    static uint32_t live[CP_FORM_TICKS / 60], replay[CP_FORM_TICKS / 60];
    ASSERT(cp_form_run(live, 1));
    ASSERT_EQ_INT(2, g_form_rec_n);
    ASSERT_EQ_INT(TAK_CMD_MOVE_FORMATION, (int)g_form_rec[1].type);
    ASSERT_EQ_INT(48, (int)g_form_rec[1].unit_dx[1]);
    ASSERT_EQ_INT(48, (int)g_form_rec[1].unit_dy[2]);
    ASSERT(cp_form_run(replay, 0));
    int moved = 0;
    for (int i = 1; i < CP_FORM_TICKS / 60; i++) if (live[i] != live[i - 1]) moved = 1;
    ASSERT(moved);
    for (int i = 0; i < CP_FORM_TICKS / 60; i++) ASSERT_EQ_INT((int)live[i], (int)replay[i]);
}

/* ── the spatial grid ─────────────────────────────────────────────── */

static uint32_t g_grid_rng = 12345u;
static int32_t grid_rand(int32_t lo, int32_t hi) {
    g_grid_rng = g_grid_rng * 1103515245u + 12345u;
    return lo + (int32_t)((g_grid_rng >> 8) % (uint32_t)(hi - lo + 1));
}

/* Every active unit whose centre is in the box is among the grid's
 * candidates, or the grid says it cannot answer. Returns the number of
 * boxes it answered, or -1 on a miss. */
static int grid_check_boxes(int boxes) {
    static int cand[TAK_MAX_UNITS];
    int answered = 0;
    for (int b = 0; b < boxes; b++) {
        int32_t cx = grid_rand(-300, 3400), cy = grid_rand(-300, 3400);
        /* Some boxes sit on a cell border, 128 px apart. */
        if (b % 4 == 0) { cx = (cx / 128) * 128; cy = (cy / 128) * 128; }
        int32_t r = grid_rand(0, 400);
        int32_t x0 = cx - r, y0 = cy - r, x1 = cx + r, y1 = cy + r;
        int n = Units_Candidates(x0, y0, x1, y1, cand, TAK_MAX_UNITS);
        if (n < 0) continue;
        answered++;
        int count = 0;
        const Unit *units = Units_GetActive(&count);
        for (int i = 0; i < count; i++) {
            const Unit *u = &units[i];
            if (u->alive != UNIT_ALIVE_ACTIVE) continue;
            if (u->world_x < x0 || u->world_x > x1 || u->world_y < y0 || u->world_y > y1) continue;
            int found = 0;
            for (int k = 0; k < n && !found; k++) found = cand[k] == i;
            if (!found) {
                printf("(unit %d at %d,%d missed by %d,%d..%d,%d) ", i, u->world_x,
                       u->world_y, x0, y0, x1, y1);
                return -1;
            }
        }
    }
    return answered;
}

/* The grid's answers against a scan of every unit, over seeded random
 * layouts: units on cell borders, off the map on every side, a cell
 * width of the grid apart, fast units a tick after the grid was built,
 * slots freed and taken again, and units that came to stand since the
 * grid was built until there are too many and it hands back to a scan. */
TEST(the_grid_names_every_unit_a_scan_would) {
    ASSERT_NOT_NULL(cp_world());
    static int h[1200];
    int n = 0;
    for (int i = 0; i < 600; i++) {
        int32_t x = grid_rand(-200, 3300), y = grid_rand(-200, 3300);
        if (i % 5 == 0) { x = (x / 128) * 128 + grid_rand(-1, 1); }
        if (i % 7 == 0) { x += 16384; }               /* the grid wraps here */
        int def = i % 3 == 0 ? CP_DEF_DART : CP_DEF_WALKER;
        h[n] = Units_Spawn(def, 1 + i % 4, i % 4, x, y);
        if (h[n] >= 0) n++;
    }
    ASSERT(n > 500);
    for (int i = 0; i < n; i += 2)
        Units_OrderMove(h[i], grid_rand(0, 3000), grid_rand(0, 3000));
    for (int round = 0; round < 6; round++) {
        /* The tick builds the grid and then everything walks a step. */
        cp_tick();
        ASSERT(grid_check_boxes(200) > 150);
        /* Free some slots and fill them again, and add a few more. */
        for (int k = 0; k < 20; k++) {
            int victim = h[grid_rand(0, n - 1)];
            (void)Units_DebugRemove(victim);
        }
        for (int k = 0; k < 30; k++)
            (void)Units_Spawn(CP_DEF_WALKER, 2, 1, grid_rand(0, 3000), grid_rand(0, 3000));
        ASSERT(grid_check_boxes(200) > 150);
    }
    /* A box as wide as the grid is not answered from it. */
    static int cand[TAK_MAX_UNITS];
    ASSERT_EQ_INT(-1, Units_Candidates(-9000, 0, 9000, 100, cand, TAK_MAX_UNITS));
    /* Past the late list's room the grid hands back to a scan. */
    cp_tick();
    for (int k = 0; k < 600; k++)
        (void)Units_Spawn(CP_DEF_WALKER, 3, 2, grid_rand(0, 3000), grid_rand(0, 3000));
    ASSERT_EQ_INT(-1, Units_Candidates(0, 0, 100, 100, cand, TAK_MAX_UNITS));
    cp_tick();
    ASSERT(grid_check_boxes(100) > 50);
    cp_end();
}

/* A formation of real footprints, packed as a player packs one, ends
 * with every unit on its own point: a block moved as a block, and a row
 * turned end for end so each unit crosses the others. A crowd's rule of
 * stopping short beside whoever is in the way left them up to 95 px
 * off. */
static int cp_formation_error(int def, int spacing, int reverse, int *out_worst) {
    if (!cp_world()) return 0;
    enum { N = 12 };
    int h[N];
    int32_t xy[2 * N];
    for (int i = 0; i < N; i++) {
        int col = i % 4, row = i / 4;
        h[i] = Units_Spawn(def, 1, 0, 600 + col * spacing, 600 + row * spacing);
        if (h[i] < 0) return 0;
        Units_DebugSetAggro(h[i], UNIT_AGGRO_PASSIVE);
        if (reverse) {
            /* The last in the row goes first: every unit crosses. */
            int to = N - 1 - i;
            xy[2 * i] = 700 + (to % 4) * spacing;
            xy[2 * i + 1] = 700 + (to / 4) * spacing;
        } else {
            xy[2 * i] = 1100 + col * spacing;
            xy[2 * i + 1] = 900 + row * spacing;
        }
    }
    cp_formation(h, xy, N, 0, 0);
    if (TAK_CommandExec_Apply(&g_cmd) != N) return 0;
    if (cp_until_idle(h, N, 6000) < 0) return 0;
    int worst = 0;
    for (int i = 0; i < N; i++) {
        const Unit *u = cp_unit(h[i]);
        int64_t dx = u->world_x - xy[2 * i], dy = u->world_y - xy[2 * i + 1];
        int d = (int)sqrt((double)(dx * dx + dy * dy));
        if (d > worst) worst = d;
    }
    *out_worst = worst;
    cp_end();
    return 1;
}

TEST(a_packed_formation_ends_on_its_points) {
    /* Moved as a block the ranks touch. Turned end for end each unit
     * has to pass between those already standing, so the ranks keep a
     * footprint's gap: into touching ranks nobody could walk. */
    static const struct { int def, spacing, crossing; } kinds[] = {
        { CP_DEF_WALKER, 24, 24 }, { CP_DEF_SQUAD2, 32, 64 }, { CP_DEF_SQUAD3, 48, 96 },
    };
    for (int k = 0; k < 3; k++)
        for (int reverse = 0; reverse <= 1; reverse++) {
            int worst = -1;
            int spacing = reverse ? kinds[k].crossing : kinds[k].spacing;
            ASSERT(cp_formation_error(kinds[k].def, spacing, reverse, &worst));
            printf("(%dx%d %s: %d px) ", k + 1, k + 1, reverse ? "reversed" : "moved", worst);
            ASSERT(worst <= 16);
        }
}

/* A unit of another seat named in the middle is left alone, and the
 * units after it still go to their own points, not a neighbour's. */
TEST(a_formation_skips_a_foreign_unit_and_keeps_the_rest_in_place) {
    ASSERT_NOT_NULL(cp_world());
    int h[3];
    h[0] = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
    h[1] = Units_Spawn(CP_DEF_WALKER, 2, 1, 848, 800);
    h[2] = Units_Spawn(CP_DEF_WALKER, 1, 0, 896, 800);
    ASSERT(h[0] >= 0 && h[1] >= 0 && h[2] >= 0);
    static const int32_t xy[6] = { 1200, 1000, 1300, 1000, 1400, 1000 };
    cp_formation(h, xy, 3, 0, 0);
    ASSERT_EQ_INT(2, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1200, cp_unit(h[0])->cmd_x);
    ASSERT_EQ_INT(1400, cp_unit(h[2])->cmd_x);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)cp_unit(h[1])->cmd_kind);
    cp_end();
}

/* The group's pace is its slowest unit still walking: once that one is
 * gone the rest keep their own. A unit whose queue is full refuses the
 * move and holds nobody back. */
TEST(a_formation_pace_drops_the_dead_and_the_refused) {
    ASSERT_NOT_NULL(cp_world());
    int slow = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
    int fast = Units_Spawn(CP_DEF_HARPY, 1, 0, 800, 860);
    int full = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 920);
    ASSERT(slow >= 0 && fast >= 0 && full >= 0);
    Units_DebugSetAggro(fast, UNIT_AGGRO_PASSIVE);
    /* The third unit is busy with eight queued legs already. */
    static const int32_t far[2] = { 700, 1500 };
    cp_formation(&full, far, 1, 0, 0);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    for (int k = 0; k < UNIT_MOVE_LEGS_MAX; k++) {
        cp_formation(&full, far, 1, TAK_FORMATION_QUEUE, 0);
        ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    }
    /* A fast pair, and the full one asked to queue with them. */
    int both[3] = { fast, full, slow };
    static const int32_t xy[6] = { 2200, 860, 2200, 920, 2200, 800 };
    cp_formation(both, xy, 3, TAK_FORMATION_GROUP_PACE | TAK_FORMATION_QUEUE, 0);
    ASSERT_EQ_INT(2, TAK_CommandExec_Apply(&g_cmd));
    float top = 0.0f;
    for (int t = 0; t < 200; t++) {
        cp_tick();
        if (cp_unit(fast)->cur_speed_ppt > top) top = cp_unit(fast)->cur_speed_ppt;
    }
    ASSERT(top <= 0.7f + 0.0001f);
    ASSERT_EQ_INT(0, Units_DebugRemove(slow));
    top = 0.0f;
    for (int t = 0; t < 200; t++) {
        cp_tick();
        if (cp_unit(fast)->cur_speed_ppt > top) top = cp_unit(fast)->cur_speed_ppt;
    }
    printf("(%.2f px a tick once the slow one is gone) ", (double)top);
    ASSERT(top > 1.4f);
    cp_end();
}

/* Every order that is not queued lets the heading and the legs go, and
 * so does a queued pickup that takes over from a formation's walk. */
TEST(any_other_order_forgets_the_formation) {
    for (int kind = 0; kind < 6; kind++) {
        ASSERT_NOT_NULL(cp_world());
        int h = Units_Spawn(kind >= 4 ? CP_DEF_CARRIER : CP_DEF_ARCHER, 1, 0, 800, 800);
        int friend_ = Units_Spawn(CP_DEF_WALKER, 1, 0, 860, 800);
        int enemy = Units_Spawn(CP_DEF_WALKER, 2, 1, 900, 800);
        ASSERT(h >= 0 && friend_ >= 0 && enemy >= 0);
        static const int32_t a[2] = { 1100, 800 }, b[2] = { 1100, 1100 };
        cp_formation(&h, a, 1, TAK_FORMATION_FACE | TAK_FORMATION_GROUP_PACE, 100);
        ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
        cp_formation(&h, b, 1, TAK_FORMATION_QUEUE, 0);
        ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
        ASSERT_EQ_INT(1, (int)cp_unit(h)->leg_count);
        static const uint8_t type[6] = { TAK_CMD_PATROL, TAK_CMD_ATTACK, TAK_CMD_GUARD,
                                         TAK_CMD_STOP, TAK_CMD_LOAD, TAK_CMD_LOAD };
        cp_cmd(type[kind], 1);
        g_cmd.arg = kind == 5 ? 1 : 0;   /* Shift */
        g_cmd.target_x = 700;
        g_cmd.target_y = 700;
        g_cmd.target_unit_id = Units_GetStableId(kind == 1 ? enemy : friend_);
        cp_cmd_unit(h);
        ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
        ASSERT_EQ_INT(UNIT_FACE_NONE, (int)cp_unit(h)->face_mode);
        ASSERT_EQ_INT(0, (int)cp_unit(h)->leg_count);
        ASSERT_EQ_INT(0, (int)cp_unit(h)->move_group);
        cp_end();
    }
}

/* A command that names a point past the edge of what an int holds is
 * clamped to the map instead of wrapping to the other side. */
TEST(a_formation_point_stays_on_the_map) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    int h = Units_Spawn(CP_DEF_WALKER, 1, 0, 800, 800);
    ASSERT(h >= 0);
    int both[2] = { h, h };
    cp_cmd(TAK_CMD_MOVE_FORMATION, 1);
    g_cmd.target_x = INT32_MAX;
    g_cmd.target_y = INT32_MIN;
    g_cmd.unit_dx[0] = 32767;
    g_cmd.unit_dy[0] = -32768;
    cp_cmd_unit(both[0]);
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(w->map_pixels_w - 1, cp_unit(h)->cmd_x);
    ASSERT_EQ_INT(0, cp_unit(h)->cmd_y);
    cp_end();
}


/* Who plays a seat changes only by the relay's command, which the match
 * makes from its entry in a turn. Each disposition does what it says and
 * a second one of the same changes nothing. */
TEST(a_seat_changes_hands_by_the_relays_command) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    w->cfg.players[1].kind = TAK_SLOT_AI;
    int theirs = Units_Spawn(CP_DEF_WALKER, 3, 2, 1600, 800);
    int fourth = Units_Spawn(CP_DEF_WALKER, 4, 3, 1600, 1600);
    ASSERT(theirs >= 0 && fourth >= 0);

    cp_cmd(TAK_CMD_SEAT_CONTROL, 2);
    g_cmd.arg = TAK_SEAT_TO_HUMAN;
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(TAK_SLOT_HUMAN, w->cfg.players[1].kind);
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));

    cp_cmd(TAK_CMD_SEAT_CONTROL, 1);
    g_cmd.arg = TAK_SEAT_TO_COMPUTER;
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(TAK_SLOT_AI, w->cfg.players[0].kind);

    /* A resigned seat stays out, and still loses its army when its
     * player goes and the room takes the army away. */
    cp_cmd(TAK_CMD_SEAT_CONTROL, 4);
    g_cmd.arg = TAK_SEAT_RESIGNED;
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, (int)w->resigned[4]);
    g_cmd.arg = TAK_SEAT_TO_COMPUTER;
    ASSERT_EQ_INT(0, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(TAK_SLOT_HUMAN, w->cfg.players[3].kind);

    cp_cmd(TAK_CMD_SEAT_CONTROL, 3);
    g_cmd.arg = TAK_SEAT_ARMY_REMOVED;
    ASSERT_EQ_INT(1, TAK_CommandExec_Apply(&g_cmd));
    ASSERT_EQ_INT(1, (int)w->resigned[3]);
    ASSERT_EQ_INT(1, (int)w->stats[3].eliminated);
    ASSERT_EQ_INT(0, cp_unit(theirs)->health);
    ASSERT(cp_unit(fourth)->health > 0);
    cp_end();
}

/* The battle's message line says a seat changed hands, on the tick it
 * did, and for five seconds of battle after. */
TEST(the_battle_says_when_a_person_takes_over_from_the_computer) {
    GameWorld *w = cp_world();
    ASSERT_NOT_NULL(w);
    w->cfg.players[1].kind = TAK_SLOT_AI;
    InGame_DebugRunSimTicks(2);
    cp_cmd(TAK_CMD_SEAT_CONTROL, 2);
    g_cmd.arg = TAK_SEAT_TO_HUMAN;
    g_cmd.tick = TAK_CmdQueue_Tick();
    ASSERT_EQ_INT(0, TAK_CmdQueue_SubmitAt(&g_cmd));
    InGame_DebugRunSimTicks(1);
    const char *line = InGame_SeatNotice();
    ASSERT_NOT_NULL(line);
    ASSERT(strstr(line, "takes over from the computer") != NULL);
    InGame_DebugRunSimTicks(5 * 60 + 2);
    ASSERT(InGame_SeatNotice() == NULL);

    cp_cmd(TAK_CMD_SEAT_CONTROL, 2);
    g_cmd.arg = TAK_SEAT_TO_COMPUTER;
    g_cmd.tick = TAK_CmdQueue_Tick();
    ASSERT_EQ_INT(0, TAK_CmdQueue_SubmitAt(&g_cmd));
    InGame_DebugRunSimTicks(1);
    line = InGame_SeatNotice();
    ASSERT_NOT_NULL(line);
    ASSERT(strstr(line, "The computer takes over from") != NULL);
    cp_end();
}

/* ── co-op with drop in seats: every world agrees (#292) ───────────── */

/* A match played through the real relay by clients that do not simulate,
 * each keeping every frame the relay sent it. Then each client's frames
 * are replayed alone through the real client and the real match module
 * into a fresh synthetic world, the way the game runs a match, and the
 * hash traces are compared. The engine holds one world at a time, which
 * is why the two halves are apart. */

#define DI_CLIENTS  3
#define DI_REC_MAX  (4u << 20)
#define DI_TICKS    1800
#define DI_SAMPLES  (DI_TICKS / 60)

static TAK_Relay        g_di_relay;
static uint8_t          g_di_arena[TAK_RELAY_ROOMS_MAX * (256u << 10)];
static TAK_TurnLogEntry g_di_entries[TAK_RELAY_ROOMS_MAX * 8192u];
static TAK_NetClient    g_di_net[DI_CLIENTS];
static TAK_NetClient    g_di_rep;
static struct {
    int      connected, loaded;
    uint8_t  rec[DI_REC_MAX];
    uint32_t rec_len;
    uint32_t takeover_turn;      /* 0 until one was seen */
} g_di[DI_CLIENTS];
static uint64_t g_di_now;
static uint32_t g_di_ids[6];     /* the units' stable ids */

static int di_send(void *ctx, TAK_ConnId conn, const uint8_t *f, size_t n) {
    (void)ctx;
    int i = (int)conn - 1;
    if (i < 0 || i >= DI_CLIENTS) return -1;
    if (g_di[i].rec_len + 4u + n <= DI_REC_MAX) {
        tak_put_u32(g_di[i].rec + g_di[i].rec_len, (uint32_t)n);
        memcpy(g_di[i].rec + g_di[i].rec_len + 4, f, n);
        g_di[i].rec_len += 4u + (uint32_t)n;
    }
    (void)TAK_NetClient_OnMessage(&g_di_net[i], f, n, g_di_now);
    return 0;
}
static void di_close(void *ctx, TAK_ConnId conn) { (void)ctx; (void)conn; }

static void di_hello(TAK_MsgHello *h, int i) {
    memset(h, 0, sizeof *h);
    h->protocol_version = TAK_NET_PROTOCOL_VERSION;
    h->engine_build_id = TAK_ENGINE_BUILD_ID;
    h->determinism_class = TAK_CLASS_TEST;
    for (int k = 0; k < TAK_NET_TOKEN_BYTES; k++) h->device_token[k] = (uint8_t)(0x30 + i * 16 + k);
    snprintf(h->name, sizeof h->name, "%s", i == 0 ? "Host" : i == 1 ? "Ally" : "Late");
}

static void di_connect(int i) {
    TAK_MsgHello h;
    di_hello(&h, i);
    TAK_NetClient_Init(&g_di_net[i], &h);
    TAK_Relay_OnConnect(&g_di_relay, (TAK_ConnId)(i + 1), g_di_now);
    g_di[i].connected = 1;
}

/* Everything the clients queued goes to the relay, in order. */
static void di_flush(void) {
    static uint8_t out[TAK_NET_FRAME_MAX];
    for (int i = 0; i < DI_CLIENTS; i++) {
        size_t n;
        while (g_di[i].connected &&
               (n = TAK_NetClient_TakeMessage(&g_di_net[i], out, sizeof out)) > 0)
            TAK_Relay_OnFrame(&g_di_relay, (TAK_ConnId)(i + 1), out, n, g_di_now);
    }
}

/* One 10 ms step: the relay's clock, then each client takes what it
 * holds and acknowledges it without simulating, which is all the relay
 * asks of a client to count it caught up. */
static void di_step(void) {
    TAK_Relay_Tick(&g_di_relay, g_di_now);
    for (int i = 0; i < DI_CLIENTS; i++) {
        TAK_NetClient *c = &g_di_net[i];
        if (!g_di[i].connected) continue;
        TAK_NetClient_Heartbeat(c, g_di_now);
        if (c->state == TAK_NC_LOADING && !g_di[i].loaded) {
            (void)TAK_NetClient_ReportLoaded(c, 0x5eedull);
            g_di[i].loaded = 1;
        }
        if (c->state != TAK_NC_PLAYING) continue;
        TAK_NetTurn t;
        int any = 0;
        uint32_t last = 0;
        while (TAK_NetClient_TakeTurn(c, &t)) {
            any = 1;
            last = t.turn;
            for (int e = 0; e < t.entry_count; e++) {
                if (t.entry[e].seat != TAK_NET_SEAT_SERVER) continue;
                for (int k = 0; k < t.entry[e].count; k++)
                    if (t.entry[e].data[k][0] == TAK_SYS_SEAT_TAKEOVER)
                        g_di[i].takeover_turn = t.turn;
            }
        }
        if (any) (void)TAK_NetClient_Ack(c, last, TAK_NET_NO_HASH, 0);
    }
    di_flush();
    g_di_now += 10;
}

static void di_run(uint64_t ms) {
    for (uint64_t end = g_di_now + ms; g_di_now < end;) di_step();
}

static void di_edit(int i, uint8_t field, uint8_t seat, uint32_t value) {
    TAK_MsgRoomEdit e;
    memset(&e, 0, sizeof e);
    e.field = field;
    e.seat = seat;
    e.value = value;
    if (field == TAK_EDIT_HAVE_MAP) memset(e.fingerprint, 0x5a, sizeof e.fingerprint);
    (void)TAK_NetClient_EditRoom(&g_di_net[i], &e);
    di_flush();
}

/* A move order for one unit, sent the way the match sends a click. */
static void di_move(int i, uint32_t unit, int32_t x, int32_t y) {
    TAK_GameCommand c;
    memset(&c, 0, sizeof c);
    c.type = TAK_CMD_MOVE;
    c.unit_count = 1;
    c.unit_ids[0] = unit;
    c.target_x = x;
    c.target_y = y;
    uint8_t b[64];
    size_t n = 0;
    if (TAK_CommandSerialize(&c, b, sizeof b, &n) != 0) return;
    TAK_CmdBlob blob = { b, (uint16_t)n };
    (void)TAK_NetClient_SendCommands(&g_di_net[i], &blob, 1);
    di_flush();
}

/* Seat 0 the host's, seat 1 an ally's, seats 2 and 3 the computer's. The
 * computer's archer in seat 2 stands idle, far from anyone, so while the
 * computer plays it the archer walks at the first enemy that comes near,
 * and once a person plays it the archer waits for orders. */
static int di_spawn(int *h) {
    static const struct { int def, player; int32_t x, y; } u[6] = {
        { CP_DEF_ARCHER, 1,  700,  700 }, { CP_DEF_WALKER, 1,  800,  700 },
        { CP_DEF_ARCHER, 2,  700, 2400 }, { CP_DEF_ARCHER, 3, 2400,  700 },
        { CP_DEF_WALKER, 3, 2450,  780 }, { CP_DEF_ARCHER, 4, 2400, 2400 },
    };
    for (int i = 0; i < 6; i++) {
        h[i] = Units_Spawn(u[i].def, u[i].player, u[i].player - 1, u[i].x, u[i].y);
        if (h[i] < 0) return 0;
    }
    return 1;
}

/* The live half. Returns 1 when the late client took seat 2. */
static int di_play(void) {
    memset(g_di, 0, sizeof g_di);
    g_di_now = 1000;
    TAK_RelayCfg rc;
    memset(&rc, 0, sizeof rc);
    strcpy(rc.server_name, "pipeline");
    rc.seed = 0x1234567u;
    TAK_NetTransport tx = { NULL, di_send, di_close };
    TAK_Relay_Init(&g_di_relay, &rc, tx, g_di_arena, sizeof g_di_arena, g_di_entries,
                   (uint32_t)(sizeof g_di_entries / sizeof g_di_entries[0]));

    di_connect(0);
    di_flush();
    di_run(100);
    TAK_MsgCreateRoom cr;
    memset(&cr, 0, sizeof cr);
    strcpy(cr.name, "Co-op");
    strcpy(cr.map_name, "synthetic");
    memset(cr.map_fingerprint, 0x5a, sizeof cr.map_fingerprint);
    cr.flags = TAK_ROOMF_LISTED | TAK_ROOMF_DROP_IN | TAK_ROOMF_AI_TAKES_OVER;
    cr.max_players = TAK_NET_SEATS;
    cr.unit_cap = 500;
    cr.timeout_secs = 60;
    (void)TAK_NetClient_CreateRoom(&g_di_net[0], &cr);
    di_flush();
    di_run(100);
    if (g_di_net[0].state != TAK_NC_ROOM) return 0;

    di_connect(1);
    di_flush();
    di_run(100);
    TAK_MsgJoinRoom jr;
    memset(&jr, 0, sizeof jr);
    jr.room_id = g_di_net[0].room.room_id;
    (void)TAK_NetClient_JoinRoom(&g_di_net[1], &jr);
    di_flush();
    di_run(100);
    di_edit(0, TAK_EDIT_ADD_COMPUTER, 2, 0);
    di_edit(0, TAK_EDIT_ADD_COMPUTER, 3, 0);
    for (int i = 0; i < 2; i++) {
        di_edit(i, TAK_EDIT_HAVE_MAP, 0, 0);
        di_edit(i, TAK_EDIT_READY, 0, 0);
    }
    di_run(100);
    (void)TAK_NetClient_Start(&g_di_net[0]);
    di_flush();
    di_run(300);
    if (g_di_net[0].state != TAK_NC_PLAYING || g_di_net[1].state != TAK_NC_PLAYING) return 0;

    di_run(1000);
    di_move(0, g_di_ids[1], 1000, 700);
    di_move(1, g_di_ids[2], 900, 2200);
    di_run(3000);

    /* The late player joins the game under way. */
    di_connect(2);
    di_flush();
    di_run(100);
    (void)TAK_NetClient_JoinRoom(&g_di_net[2], &jr);
    di_flush();
    for (int k = 0; k < 400 && !g_di[2].takeover_turn; k++) di_step();
    if (!g_di[2].takeover_turn || g_di_net[2].seat != 2) return 0;

    /* Its own order, and the host walking up to the archer it now plays. */
    di_run(500);
    di_move(2, g_di_ids[4], 2300, 1000);
    di_move(0, g_di_ids[0], 2150, 700);
    while (g_di_net[0].last_turn_held * 3u < DI_TICKS + 30u) di_step();
    return 1;
}

/* A client's frames replayed into a fresh world. `ignore_seat` drops the
 * relay's own entries from every turn, which is a client that never
 * heard a seat change hands. Fills the trace and the tick the late
 * player's seat went to a person, 0 for never. */
static int di_replay(int i, uint32_t *trace, uint32_t *handover, int ignore_seat) {
    static uint8_t frame[TAK_NET_FRAME_MAX];
    static TAK_MsgTurn t;
    TAK_MsgHello h;
    di_hello(&h, i);
    TAK_NetClient_Init(&g_di_rep, &h);
    int built = 0;
    for (uint32_t off = 0; off < g_di[i].rec_len;) {
        uint32_t n = tak_get_u32(g_di[i].rec + off);
        const uint8_t *f = g_di[i].rec + off + 4;
        off += 4u + n;
        TAK_NetFrame fr;
        if (ignore_seat && TAK_Net_Split(f, n, &fr) == 0 && fr.type == TAK_MSG_TURN &&
            TAK_Msg_TurnDecode(&t, fr.payload, fr.payload_len) == 0 && t.entry_count &&
            t.entry[t.entry_count - 1].seat == TAK_NET_SEAT_SERVER) {
            t.entry_count--;
            n = (uint32_t)TAK_Msg_TurnEncode(&t, frame, sizeof frame);
            f = frame;
        }
        if (TAK_NetClient_OnMessage(&g_di_rep, f, n, 0) != 0) return 0;
        if (!built && g_di_rep.state >= TAK_NC_LOADING && g_di_rep.state <= TAK_NC_PLAYING) {
            /* START_GAME: the world is built from it, as the room builds it. */
            g_cp_start = &g_di_rep.start;
            GameWorld *w = cp_world();
            g_cp_start = NULL;
            int hd[6];
            if (!w || !di_spawn(hd)) return 0;
            for (int k = 0; k < 6; k++)
                if (Units_GetStableId(hd[k]) != g_di_ids[k]) return 0;
            Units_SetLocalPlayer((int)g_di_rep.start.your_seat + 1);
            built = 1;
        }
    }
    if (!built || g_di_rep.state != TAK_NC_PLAYING) return 0;
    GameWorld *w = World_Get();
    TAK_Match_Begin(&g_di_rep, g_di_rep.start.your_seat, g_di_rep.start.turn_ticks);
    *handover = 0;
    int kind = (int)w->cfg.players[2].kind;
    for (int tick = 0; tick < DI_TICKS; tick++) {
        (void)TAK_Match_Pump();
        if (!TAK_Match_CanAdvance()) return 0;
        InGame_DebugRunSimTicks(1);
        if ((int)w->cfg.players[2].kind != kind) {
            if (kind != TAK_SLOT_AI || w->cfg.players[2].kind != TAK_SLOT_HUMAN) return 0;
            kind = (int)w->cfg.players[2].kind;
            *handover = TAK_CmdQueue_Tick();
        }
        if ((tick + 1) % 60 == 0) trace[tick / 60] = TAK_SimHash();
        /* Its acknowledgements have nowhere to go. */
        uint8_t out[64];
        while (TAK_NetClient_TakeMessage(&g_di_rep, out, sizeof out) > 0) {}
    }
    TAK_Match_End();
    Units_SetLocalPlayer(1);
    cp_end();
    return 1;
}

TEST(a_player_who_drops_in_mid_game_sees_the_same_world_as_everyone) {
    /* The units' stable ids, the same in every world built this way. */
    ASSERT_NOT_NULL(cp_world());
    int h[6];
    ASSERT(di_spawn(h));
    for (int k = 0; k < 6; k++) g_di_ids[k] = Units_GetStableId(h[k]);
    cp_end();

    ASSERT(di_play());
    /* Every client was handed the seat on the same turn. */
    ASSERT(g_di[0].takeover_turn > 60);
    ASSERT_EQ_INT((int)g_di[0].takeover_turn, (int)g_di[1].takeover_turn);
    ASSERT_EQ_INT((int)g_di[0].takeover_turn, (int)g_di[2].takeover_turn);
    /* The late player was told of the world as it was at turn 0. */
    ASSERT_EQ_INT(TAK_NSLOT_COMPUTER, g_di_net[2].start.slot[2].kind);
    ASSERT_EQ_INT(0, memcmp(g_di_net[2].start.slot, g_di_net[0].start.slot,
                            sizeof g_di_net[0].start.slot));

    static uint32_t trace[DI_CLIENTS][DI_SAMPLES], blind[DI_SAMPLES];
    uint32_t handover[DI_CLIENTS], none = 0;
    for (int i = 0; i < DI_CLIENTS; i++) ASSERT(di_replay(i, trace[i], &handover[i], 0));
    /* The seat changed hands on one tick in every world, the tick of the
     * turn the relay put the takeover in. */
    ASSERT_EQ_INT((int)(g_di[0].takeover_turn * 3u + 1u), (int)handover[0]);
    ASSERT_EQ_INT((int)handover[0], (int)handover[1]);
    ASSERT_EQ_INT((int)handover[0], (int)handover[2]);
    int moved = 0;
    for (int k = 0; k < DI_SAMPLES; k++) {
        ASSERT_EQ_INT((int)trace[0][k], (int)trace[1][k]);
        ASSERT_EQ_INT((int)trace[0][k], (int)trace[2][k]);
        if (k && trace[0][k] != trace[0][k - 1]) moved = 1;
    }
    ASSERT(moved);
    /* And the check can fail: a world that never heard the seat change
     * hands leaves the others once the computer would have acted. */
    ASSERT(di_replay(2, blind, &none, 1));
    ASSERT_EQ_INT(0, (int)none);
    int differs = 0;
    for (int k = 0; k < DI_SAMPLES; k++) {
        if (blind[k] == trace[0][k]) continue;
        ASSERT((uint32_t)(k + 1) * 60u >= handover[0]);
        differs = 1;
        break;
    }
    ASSERT(differs);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("The one ownership check");
    RUN(a_command_moves_only_the_units_its_seat_owns);
    RUN(a_command_with_no_standing_units_does_nothing);
    RUN(the_executor_runs_every_unit_command);
    RUN(the_executor_runs_the_seat_commands);
    TEST_SUITE("The console's + commands");
    RUN(a_typed_line_names_its_command);
    RUN(a_power_code_is_refused_unless_the_room_allows_it);
    RUN(each_power_code_does_what_the_original_did);
    RUN(the_ending_codes_take_the_armies_they_name);
    RUN(the_ending_codes_take_a_transports_riders);
    RUN(the_ending_codes_call_the_battle_at_once);
    RUN(a_mana_gift_takes_a_fraction_and_stops_at_the_givers_pool);
    RUN(a_typed_code_reaches_the_world_through_the_stream);
    RUN(the_hash_sees_every_console_change);
    RUN(no_shake_starts_no_shake);
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
    RUN(a_replay_file_plays_back_to_the_same_hash_every_turn);
    RUN(a_replay_that_drifts_says_where);
    RUN(a_replay_from_another_build_is_refused);
    RUN(the_replay_list_reads_headers_only);
    RUN(the_replay_list_stops_at_its_limit);
    RUN(old_replays_are_pruned_and_saves_never);
    RUN(the_byte_budget_prunes_the_oldest);
    RUN(a_recording_reaches_storage_every_ten_seconds);
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
    RUN(four_full_seats_fit_on_the_map_at_once);
    RUN(the_grid_names_every_unit_a_scan_would);
    RUN(a_reused_slot_forgets_the_unit_that_had_it);
    RUN(a_seat_stops_at_its_unit_limit);
    TEST_SUITE("Capture");
    RUN(a_harpys_mind_control_turns_an_enemy_to_its_side);
    RUN(a_capture_order_sends_the_harpy_to_attack);
    RUN(a_unit_that_cannot_capture_refuses_the_order);
    RUN(a_captured_unit_starts_over_as_a_recruit);
    RUN(a_unit_that_cannot_be_captured_is_never_fired_on);
    RUN(a_monarch_is_struck_and_stays_its_own);
    RUN(a_seat_at_its_unit_limit_captures_nothing);
    RUN(a_captured_transport_sets_its_riders_down);
    RUN(a_mind_control_splash_rolls_for_each_unit_in_reach);
    RUN(the_capture_roll_rises_with_the_victims_rank);
    RUN(a_mind_control_shot_passes_a_unit_that_stepped_aside);
    RUN(a_mind_control_shot_leads_a_walker_by_four_fifths);
    RUN(a_mind_control_shot_ends_at_its_range);
    RUN(a_mind_control_shot_dies_with_its_caster);
    RUN(a_still_recruit_is_struck_and_taken_four_times_in_five);
    RUN(a_units_selection_quad_turns_with_it);
    RUN(a_capture_lands_on_the_same_tick_on_every_machine);
    RUN(a_caster_on_the_seats_pool_drops_to_a_spell_it_can_pay_for);
    TEST_SUITE("Formation moves");
    RUN(a_formation_sends_each_unit_to_its_own_point);
    RUN(a_formation_turns_to_its_heading_on_arrival_and_holds_it);
    RUN(a_formation_at_group_pace_keeps_to_its_slowest_unit);
    RUN(a_queued_formation_waits_for_the_order_in_hand);
    RUN(a_formation_past_a_commands_units_goes_as_several);
    RUN(a_packed_formation_ends_on_its_points);
    RUN(a_formation_skips_a_foreign_unit_and_keeps_the_rest_in_place);
    RUN(a_formation_pace_drops_the_dead_and_the_refused);
    RUN(any_other_order_forgets_the_formation);
    RUN(a_formation_point_stays_on_the_map);
    RUN(the_hash_sees_a_queued_leg);
    RUN(a_formation_session_replays_to_the_same_hashes);
    TEST_SUITE("Co-op with drop in seats");
    RUN(a_seat_changes_hands_by_the_relays_command);
    RUN(the_battle_says_when_a_person_takes_over_from_the_computer);
    RUN(a_player_who_drops_in_mid_game_sees_the_same_world_as_everyone);
    TEST_SUITE("The def order");
    RUN(the_def_order_does_not_depend_on_the_archives);
    RUN(the_builderlimited_line_is_read);
    RUN(a_new_load_reads_its_own_build_menus);
    RUN(every_build_menu_is_read_when_the_match_loads);
    RUN(the_content_hash_covers_the_order_and_the_menus);
    TEST_REPORT();
}
