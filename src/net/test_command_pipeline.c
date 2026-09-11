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
#include "tak_command_exec.h"
#include "tak_commands.h"
#include "tak_economy.h"
#include "tak_memory.h"
#include "tak_moveinfo.h"
#include "tak_occupancy.h"
#include "tak_pathing.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ── the harness ───────────────────────────────────────────────────── */

#define CP_TILES   192      /* 16 px tiles per side, so a 3072 px map */
#define CP_GROUND  64       /* flat height, clear of the water line */

enum { CP_DEF_WALKER = 0, CP_DEF_ARCHER, CP_DEF_BUILDER, CP_DEF_COUNT };

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

/* A flat world with an occupancy layer, two move classes and three
 * synthetic defs. Seats 1 through 4 are human and each on its own
 * team. Returns NULL if anything could not be built. */
static GameWorld *cp_world(void) {
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
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
    if (Units_DebugSetDefs(defs, CP_DEF_COUNT) != CP_DEF_COUNT) return NULL;

    Units_SetLocalPlayer(1);
    return w;
}

static void cp_end(void) {
    Units_ClearInstances();
    World_End(NULL);
    TAK_PathCacheReset();
}

static const Unit *cp_unit(int handle) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    return (handle >= 0 && handle < count) ? &units[handle] : NULL;
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

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("The one ownership check");
    RUN(a_command_moves_only_the_units_its_seat_owns);
    RUN(a_command_with_no_standing_units_does_nothing);
    RUN(the_executor_runs_every_unit_command);
    RUN(the_executor_runs_the_seat_commands);
    TEST_SUITE("Stable id index");
    RUN(a_stable_id_is_found_in_a_few_probes);
    TEST_REPORT();
}
