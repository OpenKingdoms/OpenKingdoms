/*
 * test_mission_script.c -- a mission's order lists and its map script,
 * against a unit store of the test's own.
 *
 * With no argument it needs nothing but itself. With --data it loads
 * the first mission's script from the install and plays it.
 */

#include "test_framework.h"
#include "tak_mission_script.h"
#include "tak_cob.h"
#include "tak_cob_vm.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_sim_hash.h"
#include "tak_unit.h"
#include "tak_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

/* ── the unit store ───────────────────────────────────────────────── */

#define MOCK_DEFS  32
#define MOCK_UNITS 96

static UnitDef g_defs[MOCK_DEFS];
static int g_def_count;
static Unit g_units[MOCK_UNITS];
static int g_unit_count;
static uint32_t g_next_id;
static int g_move_calls, g_attack_calls, g_patrol_calls, g_kill_calls;
static int g_last_move_handle, g_last_attack_handle, g_last_attack_target;
static int32_t g_last_move_x, g_last_move_y;
static int g_factory_orders;

static void mock_reset(void) {
    memset(g_defs, 0, sizeof(g_defs));
    memset(g_units, 0, sizeof(g_units));
    g_def_count = g_unit_count = 0;
    g_next_id = 1;
    g_move_calls = g_attack_calls = g_patrol_calls = g_kill_calls = 0;
    g_last_move_handle = g_last_attack_handle = g_last_attack_target = -1;
    g_factory_orders = 0;
}

static int mock_def(const char *name, float speed) {
    if (g_def_count >= MOCK_DEFS) { fputs("mock def store is full", stderr); exit(1); }
    UnitDef *d = &g_defs[g_def_count];
    strncpy(d->unitname, name, sizeof(d->unitname) - 1);
    d->max_velocity = speed;
    return g_def_count++;
}

const Unit *Units_GetActive(int *out_count) {
    if (out_count) *out_count = g_unit_count;
    return g_units;
}

int Units_FindDefByName(const char *unitname) {
    for (int i = 0; i < g_def_count; i++) {
        if (tak_stricmp(g_defs[i].unitname, unitname) == 0) return i;
    }
    return -1;
}

const UnitDef *Units_GetDef(int idx) {
    return (idx >= 0 && idx < g_def_count) ? &g_defs[idx] : NULL;
}

int Units_Spawn(int def_idx, int player_id, int team_color_idx,
                int32_t world_x, int32_t world_y) {
    (void)team_color_idx;
    if (g_unit_count >= MOCK_UNITS) { fputs("mock unit store is full", stderr); exit(1); }
    Unit *u = &g_units[g_unit_count];
    memset(u, 0, sizeof(*u));
    u->stable_id = g_next_id++;
    u->def_idx = (uint16_t)def_idx;
    u->player_id = (uint8_t)player_id;
    u->world_x = world_x;
    u->world_y = world_y;
    u->health = u->max_health = 100;
    u->alive = UNIT_ALIVE_ACTIVE;
    u->aggro_mode = UNIT_AGGRO_OFFENSIVE;
    u->target = -1;
    return g_unit_count++;
}

int Units_OrderMove(int handle, int32_t x, int32_t y) {
    g_move_calls++;
    g_last_move_handle = handle;
    g_last_move_x = x;
    g_last_move_y = y;
    g_units[handle].cmd_kind = UNIT_CMD_MOVE;
    g_units[handle].cmd_x = x;
    g_units[handle].cmd_y = y;
    return 1;
}

int Units_OrderPatrol(int handle, int32_t x, int32_t y) {
    g_patrol_calls++;
    g_units[handle].cmd_kind = UNIT_CMD_PATROL;
    g_units[handle].cmd_x = x;
    g_units[handle].cmd_y = y;
    return 1;
}

int Units_OrderUnload(int handle, int32_t x, int32_t y) {
    return Units_OrderMove(handle, x, y);
}

int Units_OrderAttack(int handle, int target) {
    g_attack_calls++;
    g_last_attack_handle = handle;
    g_last_attack_target = target;
    g_units[handle].cmd_kind = UNIT_CMD_ATTACK;
    g_units[handle].target = (int16_t)target;
    return 1;
}

int Units_OrderSetAggro(int handle, int aggro_mode) {
    g_units[handle].aggro_mode = (uint8_t)aggro_mode;
    return 1;
}

int Units_OrderLoadList(int carrier, const int *riders, int count, int queued) {
    (void)carrier; (void)riders; (void)count; (void)queued;
    return 1;
}

int Units_CanAttackTarget(int handle, int target) {
    (void)handle; (void)target;
    return 1;
}

int Units_PlayersAreEnemies(int a, int b) { return a != b && a != 10 && b != 10; }

int Units_FactoryEnqueue(int factory, int def) {
    (void)factory; (void)def;
    g_factory_orders++;
    return 0;
}

int Units_BeginBuildingForUnit(int builder, int def, int32_t x, int32_t y) {
    (void)def; (void)x; (void)y;
    g_units[builder].cmd_kind = UNIT_CMD_BUILD;
    return 0;
}

int Units_DebugKillHandle(int handle) {
    g_kill_calls++;
    g_units[handle].alive = UNIT_ALIVE_DEAD;
    return 0;
}

void Units_SetHealthPercent(int handle, int pct) {
    g_units[handle].health = g_units[handle].max_health * pct / 100;
}

int Units_GetMana(int handle, float *cur, float *max) {
    (void)handle;
    if (cur) *cur = 0.0f;
    if (max) *max = 0.0f;
    return 0;
}

void Units_DebugSetMana(int handle, float value) { (void)handle; (void)value; }

int Units_Capture(int handle, int player_id) {
    g_units[handle].player_id = (uint8_t)player_id;
    return handle;
}

int Units_IsUnderConstruction(int handle) { (void)handle; return 0; }

/* The unit gets where it was going, or its target dies, as the engine
 * would have it after a while. */
static void mock_arrive(int handle) {
    Unit *u = &g_units[handle];
    if (u->cmd_kind == UNIT_CMD_MOVE) {
        u->world_x = u->cmd_x;
        u->world_y = u->cmd_y;
    }
    u->cmd_kind = UNIT_CMD_NONE;
}

static void ticks(int n) {
    for (int i = 0; i < n; i++) MissionScript_Tick();
}

/* ── order lists ──────────────────────────────────────────────────── */

TEST(a_list_is_worked_through_one_order_at_a_time) {
    mock_reset();
    int sword = mock_def("ARASWORD", 1.0f);
    ASSERT_EQ_INT(0, MissionScript_Begin(NULL, 1, 0));
    int u = Units_Spawn(sword, 2, 1, 100, 100);
    ASSERT_EQ_INT(4, MissionOrders_Give(u, "w 2, m 10 20, o 1, m 30 40,"));
    ASSERT_EQ_INT(1, MissionOrders_Running(u));

    /* Two seconds of nothing. */
    ticks(119);
    ASSERT_EQ_INT(0, g_move_calls);
    ticks(2);
    ASSERT_EQ_INT(1, g_move_calls);
    ASSERT_EQ_INT(160, g_last_move_x);
    ASSERT_EQ_INT(320, g_last_move_y);
    /* The next order waits for this one. */
    ticks(300);
    ASSERT_EQ_INT(1, g_move_calls);
    ASSERT_EQ_INT(UNIT_AGGRO_OFFENSIVE, (int)g_units[u].aggro_mode);
    mock_arrive(u);
    ticks(2);
    /* o 1 is the standing orders, and the unit is whose it was. */
    ASSERT_EQ_INT(UNIT_AGGRO_DEFENSIVE, (int)g_units[u].aggro_mode);
    ASSERT_EQ_INT(2, (int)g_units[u].player_id);
    ASSERT_EQ_INT(2, g_move_calls);
    ASSERT_EQ_INT(480, g_last_move_x);
    mock_arrive(u);
    ticks(2);
    ASSERT_EQ_INT(0, MissionOrders_Running(u));
    ASSERT_EQ_INT(4, MissionOrders_DebugDone());
    MissionScript_End();
}

TEST(a_wait_ends_early_when_an_enemy_comes_near) {
    mock_reset();
    int sword = mock_def("ARASWORD", 1.0f);
    ASSERT_EQ_INT(0, MissionScript_Begin(NULL, 1, 0));
    int guard = Units_Spawn(sword, 2, 1, 1600, 1600);
    int foe = Units_Spawn(sword, 1, 0, 100, 100);
    int friend_ = Units_Spawn(sword, 2, 1, 1600, 1616);
    (void)friend_;
    ASSERT_EQ_INT(2, MissionOrders_Give(guard, "w 3600 30, m 5 5"));
    ticks(600);
    ASSERT_EQ_INT(0, g_move_calls);
    /* Thirty squares is 480 pixels. */
    g_units[foe].world_x = 1600 - 600;
    g_units[foe].world_y = 1600;
    ticks(60);
    ASSERT_EQ_INT(0, g_move_calls);
    g_units[foe].world_x = 1600 - 400;
    ticks(16);
    ASSERT_EQ_INT(1, g_move_calls);
    ASSERT_EQ_INT(guard, g_last_move_handle);
    MissionScript_End();
}

TEST(an_attack_on_a_type_hunts_every_one_of_it) {
    mock_reset();
    int sword = mock_def("ARASWORD", 1.0f);
    int zombie = mock_def("TARZOM", 1.0f);
    ASSERT_EQ_INT(0, MissionScript_Begin(NULL, 1, 0));
    int u = Units_Spawn(sword, 1, 0, 0, 0);
    int far_one = Units_Spawn(zombie, 2, 1, 900, 0);
    int near_one = Units_Spawn(zombie, 2, 1, 300, 0);
    int own = Units_Spawn(zombie, 1, 0, 10, 0);
    (void)own;
    ASSERT_EQ_INT(2, MissionOrders_Give(u, "a TARZOM, m 1 1"));
    ticks(1);
    ASSERT_EQ_INT(near_one, g_last_attack_target);
    g_units[near_one].alive = UNIT_ALIVE_DEAD;
    mock_arrive(u);
    ticks(1);
    ASSERT_EQ_INT(far_one, g_last_attack_target);
    ASSERT_EQ_INT(0, g_move_calls);
    g_units[far_one].alive = UNIT_ALIVE_DEAD;
    mock_arrive(u);
    ticks(2);
    /* None left, its own side's never counted, and the list goes on. */
    ASSERT_EQ_INT(2, g_attack_calls);
    ASSERT_EQ_INT(1, g_move_calls);
    MissionScript_End();
}

TEST(a_wait_for_attack_ends_when_the_named_unit_is_hurt) {
    mock_reset();
    int sword = mock_def("ARASWORD", 1.0f);
    ASSERT_EQ_INT(0, MissionScript_Begin(NULL, 1, 0));
    int lord = Units_Spawn(sword, 2, 1, 0, 0);
    int guard = Units_Spawn(sword, 2, 1, 50, 0);
    MissionOrders_NameUnit("buri", lord);
    ASSERT_EQ_INT(2, MissionOrders_Give(guard, "wa buri, m 9 9"));
    ticks(200);
    ASSERT_EQ_INT(0, g_move_calls);
    g_units[lord].health -= 10;
    ticks(2);
    ASSERT_EQ_INT(1, g_move_calls);

    /* With no name it is the unit itself. */
    int lone = Units_Spawn(sword, 2, 1, 500, 0);
    ASSERT_EQ_INT(2, MissionOrders_Give(lone, "wa, m 7 7"));
    ticks(100);
    ASSERT_EQ_INT(1, g_move_calls);
    g_units[lone].health -= 1;
    ticks(2);
    ASSERT_EQ_INT(2, g_move_calls);
    ASSERT_EQ_INT(lone, g_last_move_handle);
    MissionScript_End();
}

TEST(a_patrol_of_several_points_goes_round_them) {
    mock_reset();
    int sword = mock_def("ARASWORD", 1.0f);
    ASSERT_EQ_INT(0, MissionScript_Begin(NULL, 1, 0));
    int u = Units_Spawn(sword, 2, 1, 0, 0);
    ASSERT_EQ_INT(3, MissionOrders_Give(u, "p 10 0, p 10 10, p 0 10"));
    ticks(1);
    ASSERT_EQ_INT(160, g_units[u].cmd_x);
    g_units[u].world_x = 160; g_units[u].world_y = 0;
    ticks(2);
    ASSERT_EQ_INT(160, g_units[u].cmd_y);
    g_units[u].world_x = 160; g_units[u].world_y = 160;
    ticks(2);
    ASSERT_EQ_INT(0, g_units[u].cmd_x);
    g_units[u].world_x = 0; g_units[u].world_y = 160;
    ticks(2);
    /* And round again. */
    ASSERT_EQ_INT(160, g_units[u].cmd_x);
    ASSERT_EQ_INT(0, g_units[u].cmd_y);
    ASSERT_EQ_INT(1, MissionOrders_Running(u));
    MissionScript_End();
}

/* ── the map script ───────────────────────────────────────────────── */

#define OP_PUSHC   0x10021001u
#define OP_PUSHL   0x10021002u
#define OP_PUSHS   0x10021004u
#define OP_ALLOC   0x10022000u
#define OP_POPS    0x10023004u
#define OP_POP     0x10024000u
#define OP_EQ      0x10055000u
#define OP_JF      0x10066000u
#define OP_RET     0x10065000u
#define OP_CMD     0x10073000u
#define OP_SET     0x10082000u
#define OP_GETARGS 0x10043000u

/* Start makes a hero and a box, a unit in the box brings a zombie with
 * orders, and the hero's death loses the mission: the shape of the
 * first mission's own script. */
static uint32_t g_code[] = {
    /* Start(where) */
    /*  0 */ OP_PUSHC, 0, OP_PUSHC, 72, OP_PUSHC, 172, OP_CMD, 0, 3, OP_POPS, 0,
    /* 11 */ OP_PUSHC, 3, OP_PUSHC, 10, OP_PUSHC, 10, OP_PUSHC, 20, OP_PUSHC, 20,
    /* 21 */ OP_CMD, 1, 5, OP_POP,
    /* 25 */ OP_PUSHC, 30, OP_PUSHS, 0, OP_PUSHC, 0, OP_PUSHC, 0, OP_PUSHC, 0,
    /* 35 */ OP_GETARGS, OP_POPS, 1,
    /* 38 */ OP_RET,
    /* TriggerHit(trigger, unit, player) */
    /* 39 */ OP_ALLOC, OP_ALLOC, OP_ALLOC,
    /* 42 */ OP_PUSHL, 2, OP_PUSHC, 0, OP_EQ, OP_JF, 67,
    /* 49 */ OP_PUSHC, 1, OP_PUSHC, 15, OP_PUSHC, 15, OP_CMD, 2, 3, OP_POPS, 2,
    /* 60 */ OP_PUSHS, 2, OP_CMD, 3, 1, OP_POP,
    /* 66 */ OP_RET,
    /* 67 */ OP_RET,
    /* UnitDestroyed(unit) */
    /* 68 */ OP_ALLOC,
    /* 69 */ OP_PUSHL, 0, OP_PUSHS, 0, OP_EQ, OP_JF, 81,
    /* 76 */ OP_PUSHC, 2, OP_PUSHC, 0, OP_SET,
    /* 81 */ OP_RET,
};

static void fabricate(CobScript *s) {
    static char *names[] = { "Start", "TriggerHit", "UnitDestroyed" };
    static uint32_t offsets[] = { 0, 39, 68 };
    static char *commands[] = { "create NPCEMEN", "SetTrigger", "create TARZOM",
                                "SetMission w 1, a NPCEMEN" };
    memset(s, 0, sizeof(*s));
    s->version = 6;
    s->num_static_vars = 3;
    s->code = g_code;
    s->num_code_words = (uint32_t)(sizeof(g_code) / sizeof(g_code[0]));
    s->script_names = names;
    s->script_offsets = offsets;
    s->num_scripts = 3;
    s->sound_names = commands;
    s->num_sound_names = 4;
}

TEST(a_map_script_creates_triggers_orders_and_calls_the_mission) {
    mock_reset();
    int sword = mock_def("ARASWORD", 1.0f);
    int emen = mock_def("NPCEMEN", 1.0f);
    int zombie = mock_def("TARZOM", 1.0f);
    CobScript s;
    fabricate(&s);
    ASSERT_EQ_INT(0, MissionScript_BeginWith(&s, 1, 0));
    ASSERT_EQ_INT(1, MissionScript_HasScript());
    int mine = Units_Spawn(sword, 1, 0, 800, 800);
    ticks(1);

    /* Start: the hero for the first seat at 72,172, whose number the
     * script keeps, and a box from 10,10 to 20,20. */
    ASSERT_EQ_INT(2, g_unit_count);
    ASSERT_EQ_INT(emen, (int)g_units[1].def_idx);
    ASSERT_EQ_INT(1, (int)g_units[1].player_id);
    ASSERT_EQ_INT(72 * 16, g_units[1].world_x);
    ASSERT_EQ_INT(172 * 16, g_units[1].world_y);
    int32_t kept = 0, type = 0;
    ASSERT_EQ_INT(1, MissionScript_DebugStatic(0, &kept));
    ASSERT_EQ_INT(2, kept);
    /* A query with arguments takes its port first (legacy:306687). */
    ASSERT_EQ_INT(1, MissionScript_DebugStatic(1, &type));
    ASSERT_EQ_INT(emen + 1, type);
    int x, z, x2, z2, r;
    ASSERT_EQ_INT(1, MissionScript_DebugTrigger(3, &x, &z, &x2, &z2, &r));
    ASSERT_EQ_INT(10, x);
    ASSERT_EQ_INT(20, z2);
    ASSERT_EQ_INT(0, r);

    /* Outside the box nothing happens, inside it a zombie comes for
     * the second seat with orders, once. */
    ticks(30);
    ASSERT_EQ_INT(2, g_unit_count);
    g_units[mine].world_x = 15 * 16;
    g_units[mine].world_y = 12 * 16;
    ticks(2);
    ASSERT_EQ_INT(3, g_unit_count);
    ASSERT_EQ_INT(zombie, (int)g_units[2].def_idx);
    ASSERT_EQ_INT(2, (int)g_units[2].player_id);
    ASSERT_EQ_INT(1, MissionOrders_Running(2));
    ticks(120);
    ASSERT_EQ_INT(3, g_unit_count);
    ASSERT_EQ_INT(1, g_attack_calls);
    ASSERT_EQ_INT(2, g_last_attack_handle);
    ASSERT_EQ_INT(1, g_last_attack_target);

    /* A save taken here puts the same mission back. */
    uint32_t before = TAK_SimHash_Mission(TAK_SIM_HASH_SEED);
    unsigned size = MissionScript_SaveSize();
    ASSERT(size > 0);
    unsigned char *blob = (unsigned char *)malloc(size);
    ASSERT_NOT_NULL(blob);
    MissionScript_SaveState(blob);
    ASSERT_EQ_INT(0, MissionScript_BeginWith(&s, 1, 0));
    ASSERT(TAK_SimHash_Mission(TAK_SIM_HASH_SEED) != before);
    ASSERT_EQ_INT(0, MissionScript_LoadState(blob, size));
    ASSERT_EQ_INT((int)before, (int)TAK_SimHash_Mission(TAK_SIM_HASH_SEED));
    ASSERT_EQ_INT(-1, MissionScript_LoadState(blob, size - 5));
    ASSERT_EQ_INT(0, MissionScript_LoadState(blob, size));
    free(blob);
    /* Start does not run a second time over the restored state. */
    ticks(1);
    ASSERT_EQ_INT(3, g_unit_count);

    /* The hero dies and the script calls it lost. */
    ASSERT_EQ_INT(0, MissionScript_Verdict());
    g_units[1].alive = UNIT_ALIVE_DEAD;
    ticks(2);
    ASSERT_EQ_INT(-1, MissionScript_Verdict());
    MissionScript_End();
    ASSERT_EQ_INT(0, MissionScript_Active());
}

/* ── the first mission's own script ───────────────────────────────── */

TEST(the_first_mission_makes_emen_and_loses_with_him) {
    tak_mem_init();
    ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR));
    mock_reset();
    int sword = mock_def("ARASWORD", 1.0f);
    int emen = mock_def("NPCEMEN", 1.0f);
    mock_def("ARABROAD", 1.0f);
    mock_def("ARAARCH", 1.0f);
    mock_def("TARZOM", 1.0f);
    mock_def("NPCPEAS", 1.0f);
    mock_def("NPCBEG", 1.0f);
    ASSERT_EQ_INT(0, MissionScript_Begin("takmission01_mt", 1, 0));
    ASSERT_EQ_INT(1, MissionScript_HasScript());
    int mine = Units_Spawn(sword, 1, 0, 69 * 16, 175 * 16);
    ticks(2);
    /* Nothing in the .ota places Emen. The script does, at 72,172. */
    ASSERT_EQ_INT(2, g_unit_count);
    ASSERT_EQ_INT(emen, (int)g_units[1].def_idx);
    ASSERT_EQ_INT(72 * 16, g_units[1].world_x);
    int x, z, x2, z2, r;
    ASSERT_EQ_INT(1, MissionScript_DebugTrigger(0, &x, &z, &x2, &z2, &r));
    ASSERT_EQ_INT(130, x);
    ASSERT_EQ_INT(76, z);
    ASSERT_EQ_INT(25, r);
    ASSERT_EQ_INT(1, MissionScript_DebugTrigger(1, &x, &z, &x2, &z2, &r));
    ASSERT_EQ_INT(112, x2);

    /* Reaching Abiad brings its five defenders and spends the trigger. */
    g_units[mine].world_x = 125 * 16;
    g_units[mine].world_y = 76 * 16;
    ticks(2);
    printf("(%d units after the town trigger) ", g_unit_count);
    ASSERT_EQ_INT(7, g_unit_count);
    ASSERT_EQ_INT(0, MissionScript_DebugTrigger(0, &x, &z, &x2, &z2, &r));

    g_units[1].alive = UNIT_ALIVE_DEAD;
    ticks(2);
    ASSERT_EQ_INT(-1, MissionScript_Verdict());
    MissionScript_End();
    VFS_Shutdown();
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--data") == 0) {
        TEST_SUITE("Mission script, the shipped first mission");
        RUN(the_first_mission_makes_emen_and_loses_with_him);
        TEST_REPORT();
    }
    TEST_SUITE("Mission order lists and map script");
    RUN(a_list_is_worked_through_one_order_at_a_time);
    RUN(a_wait_ends_early_when_an_enemy_comes_near);
    RUN(an_attack_on_a_type_hunts_every_one_of_it);
    RUN(a_wait_for_attack_ends_when_the_named_unit_is_hurt);
    RUN(a_patrol_of_several_points_goes_round_them);
    RUN(a_map_script_creates_triggers_orders_and_calls_the_mission);
    TEST_REPORT();
}
