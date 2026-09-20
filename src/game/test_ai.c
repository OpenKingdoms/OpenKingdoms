#include "tak_ai.h"
#include "tak_ai_influence.h"
#include "tak_ai_plan.h"
#include "tak_economy.h"
#include "tak_unit.h"
#include "tak_world.h"
#include "tak_features.h"
#include "tak_hpi.h"
#include "tak_pathing.h"
#include "tak_sim_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_DEFS  8
#define MOCK_UNITS 16

static UnitDef g_defs[MOCK_DEFS];
static Unit g_units[MOCK_UNITS];
static int g_unit_count;
static int g_buildable_counts[MOCK_DEFS];
static int g_buildables[MOCK_DEFS][4];
static int g_begin_calls;
static int g_last_builder;
static int g_last_build_def;
static int g_site_clear = 1;
static int g_site_blocked_def = -1;
static int32_t g_last_build_x;
static int32_t g_last_build_y;
static int g_attack_calls;
static int g_move_calls;
static int g_stop_calls;
static int g_last_attack_handle;
static int g_last_attack_target;
static int g_last_move_handle;
static int32_t g_last_move_x;
static int32_t g_last_move_y;
/* Team stubs read the fixture world; fog stub answers g_visible. */
static const GameWorld *g_world;
static int g_visible = 1;
static int32_t g_mock_mana;
static int32_t g_mock_max_mana;
static int32_t g_mock_income;
static int32_t g_mock_spend;
/* The path stub: every goal west of g_path_wall_x has no route when
 * the wall is set, and every query is counted. */
static int32_t g_path_wall_x;
static int g_path_walled;
static int g_path_calls;

int TAK_PathPlanQuery(const struct GameWorld *world,
                      int32_t start_x, int32_t start_y,
                      int32_t goal_x, int32_t goal_y,
                      const TAK_PathQuery *query, TAK_Path *out_path) {
    (void)world; (void)start_x; (void)start_y; (void)query;
    g_path_calls++;
    if (!out_path) return 0;
    memset(out_path, 0, sizeof(*out_path));
    if (g_path_walled && goal_x < g_path_wall_x) return 0;
    out_path->count = 1;
    out_path->x[0] = goal_x;
    out_path->y[0] = goal_y;
    return 1;
}

/* The connected-ground stub: with the split on, ground each side of
 * g_conn_x is a landmass of its own, and so is ground each side of
 * g_conn_y, which is how a test puts a seat on an island.
 * g_conn_y at 0 leaves the split to x alone. */
static int32_t g_conn_x, g_conn_y;
static int g_conn_split;
static int g_conn_calls;
/* A class whose max slope reaches this crosses the split, which
 * is how a test gives a seat two kinds of walker. 0 turns it off. */
static int g_conn_climbs;

int TAK_PathGroundConnected(const struct GameWorld *world,
                            const struct MoveClassDef *move_class,
                            int fallback_max_slope,
                            int32_t ax, int32_t ay,
                            int32_t bx, int32_t by) {
    (void)world; (void)move_class;
    g_conn_calls++;
    if (!g_conn_split) return 1;
    if (g_conn_climbs > 0 && fallback_max_slope >= g_conn_climbs)
        return 1;
    return (ax >= g_conn_x) == (bx >= g_conn_x) &&
           (ay >= g_conn_y) == (by >= g_conn_y);
}

const MoveClassDef *TAK_MoveInfo_Find(const MoveInfoTable *table,
                                      const char *name) {
    (void)table; (void)name;
    return NULL;
}

int32_t Economy_GetMana(const EconomyState *eco, int player_id) {
    (void)eco; (void)player_id; return g_mock_mana;
}
int32_t Economy_GetMaxMana(const EconomyState *eco, int player_id) {
    (void)eco; (void)player_id; return g_mock_max_mana;
}
int32_t Economy_GetIncome(const EconomyState *eco, int player_id) {
    (void)eco; (void)player_id; return g_mock_income;
}
int32_t Economy_GetSpend(const EconomyState *eco, int player_id) {
    (void)eco; (void)player_id; return g_mock_spend;
}

static int ai_test_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        int cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* One mock sacred site, wired up by the expansion test. */
static FeatureDef g_sacred_def;
static int g_sacred_registered;

/* Stubs for profile/expansion deps — no data dir in the mock harness.
 * A test that cares about the profile's per-type limits sets
 * g_mock_profile and calls TAK_AI_ResetProfile. */
static const char *g_mock_profile;

int VFS_ReadFile(const char *path, void **out_data, uint32_t *out_size) {
    (void)path;
    if (!g_mock_profile || !out_data || !out_size) return -1;
    uint32_t n = (uint32_t)strlen(g_mock_profile);
    void *buf = malloc(n ? n : 1);
    if (!buf) return -1;
    memcpy(buf, g_mock_profile, n);
    *out_data = buf;
    *out_size = n;
    return 0;
}
const FeatureDef *Features_GetByIndex(int idx) {
    if (g_sacred_registered && idx == 0) return &g_sacred_def;
    return NULL;
}
int Units_FindDefByName(const char *unitname) {
    if (!unitname) return -1;
    for (int i = 0; i < MOCK_DEFS; i++) {
        if (g_defs[i].unitname[0] &&
            ai_test_stricmp(g_defs[i].unitname, unitname) == 0) return i;
    }
    return -1;
}
void *tak_malloc(size_t n) { return malloc(n); }
void tak_free(void *p) { free(p); }

const UnitDef *Units_GetDef(int idx) {
    if (idx < 0 || idx >= MOCK_DEFS) return NULL;
    return &g_defs[idx];
}

GameWorld *World_Get(void) { return (GameWorld *)g_world; }

const Unit *Units_GetActive(int *out_count) {
    if (out_count) *out_count = g_unit_count;
    return g_units;
}

/* Same resolution as units.c: a slot's team, else the player number,
 * 0 for a closed slot; enemies when the teams differ. */
int Units_PlayerTeamId(int player_id) {
    if (!g_world || player_id < 1 || player_id > TAK_MAX_PLAYERS) return player_id;
    const PlayerSlot *slot = &g_world->cfg.players[player_id - 1];
    if (slot->kind == TAK_SLOT_CLOSED) return 0;
    return slot->team > 0 ? slot->team : TAK_MAX_PLAYERS + player_id;
}

int Units_PlayersAreEnemies(int a, int b) {
    int ta = Units_PlayerTeamId(a);
    int tb = Units_PlayerTeamId(b);
    if (ta <= 0 || tb <= 0) return a != b;
    return ta != tb;
}

int Units_CanAttackTarget(int handle, int target_handle) {
    return handle >= 0 && target_handle >= 0 && handle != target_handle;
}

void Units_CommandMoveUnit(int handle, int32_t world_x, int32_t world_y) {
    if (handle < 0 || handle >= g_unit_count) return;
    g_units[handle].cmd_kind = UNIT_CMD_MOVE;
    g_units[handle].cmd_x = world_x;
    g_units[handle].cmd_y = world_y;
    g_units[handle].target = -1;
    g_move_calls++;
    g_last_move_handle = handle;
    g_last_move_x = world_x;
    g_last_move_y = world_y;
}

void Units_CommandAttackUnit(int handle, int target_handle) {
    if (handle < 0 || handle >= g_unit_count) return;
    if (target_handle < 0 || target_handle >= g_unit_count) return;
    /* units.c refuses an attack on a friend; the mock must too. */
    if (!Units_PlayersAreEnemies(g_units[handle].player_id,
                                 g_units[target_handle].player_id)) return;
    g_units[handle].cmd_kind = UNIT_CMD_ATTACK;
    g_units[handle].target = (int16_t)target_handle;
    g_attack_calls++;
    g_last_attack_handle = handle;
    g_last_attack_target = target_handle;
}

void Units_StopUnit(int handle) {
    if (handle < 0 || handle >= g_unit_count) return;
    g_units[handle].cmd_kind = UNIT_CMD_NONE;
    g_units[handle].target = -1;
    g_units[handle].build_target = -1;
    g_stop_calls++;
}

int Units_GetBuildables(int builder_def_idx, int *out_def_idxs, int max_out) {
    if (builder_def_idx < 0 || builder_def_idx >= MOCK_DEFS || !out_def_idxs || max_out <= 0)
        return 0;
    int n = g_buildable_counts[builder_def_idx];
    if (n > max_out) n = max_out;
    for (int i = 0; i < n; i++) out_def_idxs[i] = g_buildables[builder_def_idx][i];
    return n;
}

int Units_IsBuildSiteClear(int def_idx, int32_t world_x, int32_t world_y) {
    (void)world_x;
    (void)world_y;
    return g_site_clear && def_idx != g_site_blocked_def;
}

int Units_BeginBuildingForUnit(int builder_handle,
                               int building_def_idx,
                               int32_t world_x,
                               int32_t world_y) {
    g_last_build_x = world_x;
    g_last_build_y = world_y;
    g_begin_calls++;
    g_last_builder = builder_handle;
    g_last_build_def = building_def_idx;
    if (builder_handle >= 0 && builder_handle < g_unit_count) {
        g_units[builder_handle].cmd_kind = UNIT_CMD_BUILD;
        g_units[builder_handle].build_target = (int16_t)g_unit_count;
        g_units[builder_handle].cmd_x = world_x;
        g_units[builder_handle].cmd_y = world_y;
    }
    return g_unit_count;
}

int Fog_IsVisibleForPlayer(const struct GameWorld *world, int player_id,
                           int32_t world_x, int32_t world_y) {
    (void)world;
    (void)player_id;
    (void)world_x;
    (void)world_y;
    return g_visible;
}

#define ASSERT_EQ_INT(exp, got) do { \
    int _e = (exp); \
    int _g = (got); \
    if (_e != _g) { \
        fprintf(stderr, "ASSERT_EQ_INT failed at %s:%d: expected %d got %d\n", \
                __FILE__, __LINE__, _e, _g); \
        return 1; \
    } \
} while (0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "ASSERT_TRUE failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static void reset_mock(GameWorld *w) {
    memset(w, 0, sizeof(*w));
    memset(g_defs, 0, sizeof(g_defs));
    memset(g_units, 0, sizeof(g_units));
    memset(g_buildable_counts, 0, sizeof(g_buildable_counts));
    memset(g_buildables, 0, sizeof(g_buildables));
    g_unit_count = 0;
    g_begin_calls = 0;
    g_last_builder = -1;
    g_last_build_def = -1;
    g_site_clear = 1;
    g_site_blocked_def = -1;
    g_last_build_x = 0;
    g_last_build_y = 0;
    g_attack_calls = 0;
    g_move_calls = 0;
    g_stop_calls = 0;
    g_last_attack_handle = -1;
    g_last_attack_target = -1;
    g_last_move_handle = -1;
    g_last_move_x = 0;
    g_last_move_y = 0;
    g_visible = 1;
    g_mock_mana = 0;
    g_mock_max_mana = 0;
    g_mock_income = 0;
    g_mock_spend = 0;
    g_path_wall_x = 0;
    g_path_walled = 0;
    g_path_calls = 0;
    g_conn_x = 0;
    g_conn_y = 0;
    g_conn_split = 0;
    g_conn_calls = 0;
    g_conn_climbs = 0;
    g_sacred_registered = 0;
    g_mock_profile = NULL;
    memset(&g_sacred_def, 0, sizeof(g_sacred_def));
    g_world = w;
    w->loaded = 1;
    w->skirmish_elapsed_ticks = 60;
    w->cfg.line_of_sight = 1;
    w->map_pixels_w = 4096;
    w->map_pixels_h = 4096;
    TAK_AI_ResetProfile();
    TAK_AI_BeginMatch(0);
}

static void setup_ai_progression_fixture(GameWorld *w) {
    reset_mock(w);
    g_unit_count = 1;
    w->cfg.players[1].kind = TAK_SLOT_AI;
    w->cfg.players[1].team = 2;

    strcpy(g_defs[0].unitname, "TARNECRO");
    strcpy(g_defs[0].category, "TAR Monarch");
    g_defs[0].cap_flags = UNIT_CAP_BUILDER;
    g_defs[0].max_velocity = 1.5f;
    g_defs[0].worker_time = 10.0f;
    g_defs[0].commander = 1;

    strcpy(g_defs[1].unitname, "TARLODE");
    strcpy(g_defs[1].category, "TAR");
    g_defs[1].mogrium_storage = 1000;
    g_defs[1].mogrium_income_per_sec = 10.0f;

    strcpy(g_defs[2].unitname, "TARCASTL");
    strcpy(g_defs[2].category, "TAR FACTORY");
    g_defs[2].cap_flags = UNIT_CAP_BUILDER;
    g_defs[2].worker_time = 10.0f;

    strcpy(g_defs[3].unitname, "TARTROOP");
    strcpy(g_defs[3].category, "TAR MELEE ATTACK");
    g_defs[3].max_velocity = 1.0f;
    g_defs[3].num_weapons = 1;
    g_defs[3].sight_distance = 140;
    g_defs[3].weapons[0].range = 40;

    g_buildable_counts[0] = 2;
    g_buildables[0][0] = 1;
    g_buildables[0][1] = 2;
    g_buildable_counts[2] = 1;
    g_buildables[2][0] = 3;

    g_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_units[0].player_id = 2;
    g_units[0].def_idx = 0;
    g_units[0].build_target = -1;
    g_units[0].target = -1;
}

/* Zhon's army comes from walking producers: the monarch summons a
 * beast handler and the handler summons the troops. Without the
 * handler counted as a producer the AI never trains and never
 * attacks. */
static int test_ai_mobile_producer_trains_the_army(void) {
    GameWorld w;
    reset_mock(&w);
    w.cfg.players[1].kind = TAK_SLOT_AI;
    w.cfg.players[1].team = 2;
    g_mock_mana = 500;
    g_mock_max_mana = 1000;
    g_mock_income = 10;

    strcpy(g_defs[0].unitname, "ZONHUNT");
    strcpy(g_defs[0].category, "ZON Monarch");
    g_defs[0].cap_flags = UNIT_CAP_BUILDER;
    g_defs[0].max_velocity = 1.5f;
    g_defs[0].worker_time = 10.0f;
    g_defs[0].num_weapons = 1;
    g_defs[0].commander = 1;

    strcpy(g_defs[1].unitname, "ZONLODE");
    strcpy(g_defs[1].category, "ZON");
    g_defs[1].mogrium_storage = 1000;
    g_defs[1].mogrium_income_per_sec = 10.0f;

    strcpy(g_defs[2].unitname, "ZONHAND");
    strcpy(g_defs[2].category, "ZON BUILDER");
    g_defs[2].cap_flags = UNIT_CAP_BUILDER;
    g_defs[2].max_velocity = 1.2f;
    g_defs[2].worker_time = 10.0f;
    g_defs[2].num_weapons = 1;

    strcpy(g_defs[3].unitname, "ZONGOB");
    strcpy(g_defs[3].category, "ZON MELEE ATTACK");
    g_defs[3].max_velocity = 1.0f;
    g_defs[3].num_weapons = 1;
    g_defs[3].sight_distance = 140;
    g_defs[3].weapons[0].range = 40;

    g_buildable_counts[0] = 2;
    g_buildables[0][0] = 1;
    g_buildables[0][1] = 2;
    g_buildable_counts[2] = 1;
    g_buildables[2][0] = 3;

    /* The monarch with its lodestone standing summons a handler. */
    g_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_units[0].player_id = 2;
    g_units[0].def_idx = 0;
    g_units[0].build_target = -1;
    g_units[0].target = -1;
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 2;
    g_units[1].def_idx = 1;
    g_units[1].build_target = -1;
    g_units[1].target = -1;
    g_unit_count = 2;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(0, g_last_builder);
    ASSERT_EQ_INT(2, g_last_build_def);

    /* With the handler standing it trains the army. The shipped
     * profile allows ten handlers, so cap it at one here to leave the
     * training as the only thing the tick can do. */
    g_mock_profile = "limit ZONHAND 1\n";
    TAK_AI_ResetProfile();
    g_units[0].cmd_kind = UNIT_CMD_NONE;
    g_units[0].build_target = -1;
    g_units[2].alive = UNIT_ALIVE_ACTIVE;
    g_units[2].player_id = 2;
    g_units[2].def_idx = 2;
    g_units[2].build_target = -1;
    g_units[2].target = -1;
    g_unit_count = 3;
    g_begin_calls = 0;
    g_last_builder = -1;
    g_last_build_def = -1;
    w.skirmish_elapsed_ticks = 120;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(2, g_last_builder);
    ASSERT_EQ_INT(3, g_last_build_def);
    return 0;
}

static int test_ai_builds_economy_then_production_then_combat(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(0, g_last_builder);
    ASSERT_EQ_INT(1, g_last_build_def);

    g_units[0].cmd_kind = UNIT_CMD_NONE;
    g_units[0].build_target = -1;
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 2;
    g_units[1].def_idx = 1;
    g_units[1].under_construction = 0;
    g_unit_count = 2;
    g_begin_calls = 0;
    g_last_build_def = -1;
    w.skirmish_elapsed_ticks = 120;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(0, g_last_builder);
    ASSERT_EQ_INT(2, g_last_build_def);

    /* The monarch stays at work, so this step is about the castle:
     * a free monarch would start a second castle beside it, which is
     * test_ai_every_idle_producer_starts_a_unit's subject. */
    g_units[0].cmd_kind = UNIT_CMD_BUILD;
    g_units[0].build_target = 1;
    g_units[2].alive = UNIT_ALIVE_ACTIVE;
    g_units[2].player_id = 2;
    g_units[2].def_idx = 2;
    g_units[2].under_construction = 0;
    g_units[2].build_target = -1;
    g_unit_count = 3;
    g_begin_calls = 0;
    g_last_build_def = -1;
    w.skirmish_elapsed_ticks = 180;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(2, g_last_builder);
    ASSERT_EQ_INT(3, g_last_build_def);
    return 0;
}

/* Taros has three factories in its profile at equal weight, each
 * limited to two. A seat at its castle limit has to start a dungeon
 * or a hell next, because that is where eleven of its unit types come
 * from. It stopped instead, having only ever considered the first
 * factory in the builder's list. */
static int test_ai_moves_to_the_next_factory_at_the_limit(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    g_mock_mana = 5000;
    g_mock_max_mana = 5000;
    g_mock_income = 50;

    /* A second factory kind after the castle in the builder's list. */
    strcpy(g_defs[4].unitname, "TARDUNG");
    strcpy(g_defs[4].category, "TAR FACTORY");
    g_defs[4].cap_flags = UNIT_CAP_BUILDER;
    g_defs[4].worker_time = 10.0f;
    strcpy(g_defs[5].unitname, "TARFIRE");
    strcpy(g_defs[5].category, "TAR ATTACK BALLISTIC");
    g_defs[5].max_velocity = 1.0f;
    g_defs[5].num_weapons = 1;
    g_defs[5].sight_distance = 156;
    g_defs[5].weapons[0].range = 900;
    g_buildable_counts[0] = 3;
    g_buildables[0][0] = 1;
    g_buildables[0][1] = 2;
    g_buildables[0][2] = 4;
    g_buildable_counts[4] = 1;
    g_buildables[4][0] = 5;
    TAK_AI_DebugSetLimit(2, 2);
    TAK_AI_DebugSetLimit(4, 2);
    TAK_AI_DebugSetWeight(2, 10.0f);
    TAK_AI_DebugSetWeight(4, 10.0f);

    /* Two castles owned and complete, a lodestone, and the monarch
     * free. The castle is at its limit. */
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 2;
    g_units[1].def_idx = 1;
    g_units[2].alive = UNIT_ALIVE_ACTIVE;
    g_units[2].player_id = 2;
    g_units[2].def_idx = 2;
    g_units[2].build_target = -1;
    g_units[2].cmd_kind = UNIT_CMD_BUILD;   /* busy, so it is not the picker */
    g_units[3].alive = UNIT_ALIVE_ACTIVE;
    g_units[3].player_id = 2;
    g_units[3].def_idx = 2;
    g_units[3].build_target = -1;
    g_units[3].cmd_kind = UNIT_CMD_BUILD;
    g_unit_count = 4;
    g_begin_calls = 0;
    g_last_build_def = -1;
    w.skirmish_elapsed_ticks = 600;

    /* Only a start inside this loop counts. */
    g_begin_calls = 0;
    g_last_build_def = -1;
    int started = -1;
    for (int k = 0; k < 20 && started < 0; k++) {
        w.skirmish_elapsed_ticks += 60;
        TAK_AI_TickSkirmish(&w);
        if (g_begin_calls > 0) started = g_last_build_def;
    }
    printf("[started def %d] ", started);
    ASSERT_EQ_INT(4, started);
    return 0;
}

/* The AI aims a yardmap-'S' building at the pad itself, never beside
 * it (legacy:21427 dispatch, :20483 nearest passing pad). */
static int test_ai_builds_lodestone_on_sacred_pad(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);

    g_defs[1].yardmap_sacred = 1;
    g_defs[1].footprint_x = 2;
    g_defs[1].footprint_z = 2;
    /* Only the lodestone is buildable, so the expansion pass is the
     * one under test. */
    g_buildable_counts[0] = 1;

    g_sacred_registered = 1;
    g_sacred_def.sacred_site = 2.0f;
    g_sacred_def.footprint_x = 2;
    g_sacred_def.footprint_z = 2;
    strcpy(g_sacred_def.category, "mana");

    static struct MapFeature pad;
    pad.feat_id = 0;
    pad.tile_x = 40;
    pad.tile_z = 24;
    pad.global_idx = 0;
    w.features = &pad;
    w.feature_count = 1;

    /* Builder parked far from the pad; the economy pass must leave the
     * sacred building alone and the expansion pass must take it. */
    g_units[0].world_x = 4000;
    g_units[0].world_y = 4000;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(1, g_last_build_def);
    /* Centre chosen so the footprint's top-left cell is the pad's own
     * cell, which is how legacy reads a build cell back (legacy:21442):
     * the 2x2 lodestone covers the 2x2 pad exactly. */
    ASSERT_EQ_INT(40 * 16, g_last_build_x - 2 * 8);
    ASSERT_EQ_INT(24 * 16, g_last_build_y - 2 * 8);

    /* A pad that fails placement is skipped, not built beside. */
    setup_ai_progression_fixture(&w);
    g_defs[1].yardmap_sacred = 1;
    g_defs[1].footprint_x = 2;
    g_defs[1].footprint_z = 2;
    g_buildable_counts[0] = 1;
    g_sacred_registered = 1;
    g_sacred_def.sacred_site = 2.0f;
    g_sacred_def.footprint_x = 2;
    g_sacred_def.footprint_z = 2;
    w.features = &pad;
    w.feature_count = 1;
    g_site_clear = 0;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(0, g_begin_calls);
    return 0;
}

/* ── Hostility fixture ─────────────────────────────────────────────────
 *
 * Four corners, one player each. Player 1 holds a monarch, every AI a
 * troop and a lodestone at its start. Nothing is buildable, so only
 * the target, march and defence passes act. */

#define HF_MONARCH 0
#define HF_LODE    1
#define HF_TROOP   3

static const int hf_start_x[5] = { 0, 10, 200, 10, 200 };
static const int hf_start_z[5] = { 0, 10, 10, 200, 200 };

static int hf_add_unit(int player, int def, int32_t x, int32_t y) {
    int h = g_unit_count++;
    g_units[h].alive = UNIT_ALIVE_ACTIVE;
    g_units[h].player_id = (uint8_t)player;
    g_units[h].def_idx = (uint16_t)def;
    g_units[h].world_x = x;
    g_units[h].world_y = y;
    g_units[h].target = -1;
    g_units[h].build_target = -1;
    g_units[h].stable_id = 100u + (uint32_t)h;
    g_units[h].health = 100;
    g_units[h].max_health = 100;
    return h;
}

/* Handles: 0 p1 monarch, then per AI p: troop 2p-3, lodestone 2p-2. */
static void setup_hostility_fixture(GameWorld *w, const int *teams) {
    reset_mock(w);
    for (int p = 1; p <= 4; p++) {
        w->cfg.players[p - 1].kind = p == 1 ? TAK_SLOT_HUMAN : TAK_SLOT_AI;
        w->cfg.players[p - 1].team = teams[p];
        w->start_positions[p - 1].player = p;
        w->start_positions[p - 1].x = hf_start_x[p];
        w->start_positions[p - 1].z = hf_start_z[p];
    }
    w->num_start_positions = 4;

    strcpy(g_defs[HF_MONARCH].unitname, "ARAKING");
    strcpy(g_defs[HF_MONARCH].category, "ARA Monarch");
    g_defs[HF_MONARCH].cap_flags = UNIT_CAP_BUILDER;
    g_defs[HF_MONARCH].max_velocity = 1.5f;
    g_defs[HF_MONARCH].num_weapons = 1;
    g_defs[HF_MONARCH].sight_distance = 200;
    g_defs[HF_MONARCH].weapons[0].range = 250;

    strcpy(g_defs[HF_LODE].unitname, "TARLODE");
    strcpy(g_defs[HF_LODE].category, "TAR");
    g_defs[HF_LODE].mogrium_storage = 1000;
    g_defs[HF_LODE].build_cost = 500;

    strcpy(g_defs[HF_TROOP].unitname, "TARTROOP");
    strcpy(g_defs[HF_TROOP].category, "TAR MELEE ATTACK");
    g_defs[HF_TROOP].max_velocity = 1.0f;
    g_defs[HF_TROOP].num_weapons = 1;
    g_defs[HF_TROOP].sight_distance = 140;
    g_defs[HF_TROOP].weapons[0].range = 40;
    g_defs[HF_TROOP].weapons[0].damage = 40;

    hf_add_unit(1, HF_MONARCH, hf_start_x[1] * 16, hf_start_z[1] * 16);
    for (int p = 2; p <= 4; p++) {
        int32_t bx = hf_start_x[p] * 16, by = hf_start_z[p] * 16;
        hf_add_unit(p, HF_TROOP, bx + 40, by);
        hf_add_unit(p, HF_LODE, bx, by + 60);
    }
}

static int hf_troop(int p) { return 2 * p - 3; }
static int hf_lode(int p)  { return 2 * p - 2; }

static int hf_run_ticks(GameWorld *w, int first_tick, int n) {
    for (int k = 0; k < n; k++) {
        w->skirmish_elapsed_ticks = first_tick + 60 * k;
        TAK_AI_TickSkirmish(w);
    }
    return w->skirmish_elapsed_ticks;
}

/* Every unit of every other team is a target and no ally ever is
 * (legacy:15365 walks all non-allied players). Teams come from the
 * battle config; a player with no team is everyone's enemy. */
static int test_ai_wave_targets_follow_the_teams(void) {
    GameWorld w;
    /* Team 2: AIs 2 and 3. Team 3: the human and AI 4. */
    static const int teamed[5] = { 0, 3, 2, 2, 3 };
    setup_hostility_fixture(&w, teamed);
    g_visible = 0;   /* fogged: waves march, nothing is attacked yet */

    for (int k = 0; k < 12; k++) {
        w.skirmish_elapsed_ticks = 60 * (k + 1);
        TAK_AI_TickSkirmish(&w);
        int t2 = TAK_AI_DebugAttackPlayer(2);
        int t3 = TAK_AI_DebugAttackPlayer(3);
        int t4 = TAK_AI_DebugAttackPlayer(4);
        ASSERT_TRUE(t2 == 1 || t2 == 4);
        ASSERT_TRUE(t3 == 1 || t3 == 4);
        ASSERT_TRUE(t4 == 2 || t4 == 3);
        /* Idle again so the next tick re-issues against the live pick. */
        for (int p = 2; p <= 4; p++) {
            const Unit *u = &g_units[hf_troop(p)];
            ASSERT_EQ_INT(UNIT_CMD_MOVE, u->cmd_kind);
            int at_enemy = 0;
            for (int h = 0; h < g_unit_count; h++) {
                if (g_units[h].world_x != u->cmd_x ||
                    g_units[h].world_y != u->cmd_y) continue;
                if (Units_PlayersAreEnemies(p, g_units[h].player_id)) at_enemy = 1;
            }
            ASSERT_TRUE(at_enemy);
            g_units[hf_troop(p)].cmd_kind = UNIT_CMD_NONE;
        }
    }
    ASSERT_EQ_INT(0, TAK_AI_DebugHostileOrders(2, 3, 0));
    ASSERT_EQ_INT(0, TAK_AI_DebugHostileOrders(3, 2, 0));
    ASSERT_EQ_INT(0, TAK_AI_DebugHostileOrders(4, 1, 0));
    ASSERT_TRUE(TAK_AI_DebugHostileOrders(2, 1, 0) +
                TAK_AI_DebugHostileOrders(2, 4, 0) == 12);
    ASSERT_TRUE(TAK_AI_DebugHostileOrders(3, 1, 0) +
                TAK_AI_DebugHostileOrders(3, 4, 0) == 12);
    ASSERT_TRUE(TAK_AI_DebugHostileOrders(4, 2, 0) +
                TAK_AI_DebugHostileOrders(4, 3, 0) == 12);

    /* No team: AI 2 fights everyone, and AI 3 (team 2 with 1 and 4)
     * has only AI 2 left to fight. */
    static const int loner[5] = { 0, 2, 0, 2, 2 };
    setup_hostility_fixture(&w, loner);
    g_visible = 0;
    int seen[5] = { 0 };
    for (int k = 0; k < 240; k++) {
        w.skirmish_elapsed_ticks = 60 * (k + 1);
        TAK_AI_TickSkirmish(&w);
        int t2 = TAK_AI_DebugAttackPlayer(2);
        ASSERT_TRUE(t2 == 1 || t2 == 3 || t2 == 4);
        seen[t2] = 1;
        ASSERT_EQ_INT(2, TAK_AI_DebugAttackPlayer(3));
        ASSERT_EQ_INT(2, TAK_AI_DebugAttackPlayer(4));
        for (int p = 2; p <= 4; p++) g_units[hf_troop(p)].cmd_kind = UNIT_CMD_NONE;
    }
    /* The pick is an evaluation, not a fixed nearest: over four
     * minutes of re-picks AI 2 spreads its waves over more than one
     * enemy. */
    ASSERT_TRUE(seen[1] + seen[3] + seen[4] >= 2);
    return 0;
}

/* Issue #107. A wall scores nothing in the wave pick (legacy:20042),
 * however close it stands. */
#define HF_WALL 4
static int test_ai_waves_never_pick_a_wall(void) {
    GameWorld w;
    static const int loner[5] = { 0, 2, 0, 2, 2 };
    setup_hostility_fixture(&w, loner);
    g_visible = 1;
    strcpy(g_defs[HF_WALL].unitname, "ARAWALL");
    strcpy(g_defs[HF_WALL].category, "ARA");
    g_defs[HF_WALL].is_feature = 1;
    /* The human's wall stands beside AI 2's troop, nearer than anything
     * of the human's. */
    int troop = hf_troop(2);
    int wall = hf_add_unit(1, HF_WALL, g_units[troop].world_x + 24,
                           g_units[troop].world_y);
    int picks = 0;
    for (int k = 0; k < 60; k++) {
        w.skirmish_elapsed_ticks = 60 * (k + 1);
        TAK_AI_TickSkirmish(&w);
        int target = TAK_AI_DebugWaveTarget(2);
        if (target >= 0) picks++;
        ASSERT_TRUE(target != wall);
        g_units[troop].cmd_kind = UNIT_CMD_NONE;
    }
    ASSERT_TRUE(picks > 0);
    return 0;
}

/* Issue #232. An island seat holds the walkers that cannot cross and
 * sends the flyers and the walkers that can, as the original only
 * takes and marches on a target the pathfinder answers for
 * (legacy:15473, legacy:18385). */
#define HF_FLYER 5
#define HF_CLIMBER 6
static int test_ai_waves_need_a_land_route_to_the_target(void) {
    GameWorld w;
    static const int loner[5] = { 0, 2, 0, 2, 2 };
    setup_hostility_fixture(&w, loner);
    g_visible = 0;
    int troop = hf_troop(2);

    /* Seat 2 is alone on its own ground: east of the wall and north
     * of the shore, where nobody else's units stand. */
    g_conn_split = 1;
    g_conn_x = 3100;
    g_conn_y = 1000;
    g_conn_calls = 0;
    hf_run_ticks(&w, 60, 1);
    printf("[island seat: %d ground asks for three seats] ", g_conn_calls);
    ASSERT_EQ_INT(UNIT_CMD_NONE, g_units[troop].cmd_kind);
    ASSERT_EQ_INT(0, TAK_AI_DebugHostileOrders(2, 1, 0));
    ASSERT_EQ_INT(0, TAK_AI_DebugHostileOrders(2, 3, 0));
    ASSERT_EQ_INT(0, TAK_AI_DebugHostileOrders(2, 4, 0));
    ASSERT_EQ_INT(0, TAK_AI_DebugWaveTargetReachable(2));
    ASSERT_TRUE(g_conn_calls > 0);

    /* The seat still has a target for what flies. */
    ASSERT_TRUE(TAK_AI_DebugWaveTarget(2) >= 0);
    strcpy(g_defs[HF_FLYER].unitname, "TARDRAGON");
    strcpy(g_defs[HF_FLYER].category, "TAR AIR ATTACK");
    g_defs[HF_FLYER].max_velocity = 3.0f;
    g_defs[HF_FLYER].can_fly = 1;
    g_defs[HF_FLYER].num_weapons = 1;
    g_defs[HF_FLYER].sight_distance = 300;
    g_defs[HF_FLYER].weapons[0].range = 100;
    int flyer = hf_add_unit(2, HF_FLYER, g_units[troop].world_x,
                            g_units[troop].world_y);
    hf_run_ticks(&w, 120, 1);
    ASSERT_EQ_INT(0, TAK_AI_DebugWaveTargetReachable(2));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, g_units[flyer].cmd_kind);
    ASSERT_EQ_INT(UNIT_CMD_NONE, g_units[troop].cmd_kind);

    /* Issue #232 again, found on Lake Lokken: a seat's classes do
     * not share ground. One walker that climbs reaches the mainland
     * and one that does not stays. The pick follows the one that
     * can, and only that one marches. */
    setup_hostility_fixture(&w, loner);
    g_visible = 0;
    g_conn_split = 1;
    g_conn_x = 3100;
    g_conn_y = 1000;
    g_conn_climbs = 40;
    troop = hf_troop(2);
    strcpy(g_defs[HF_CLIMBER].unitname, "TARCLIMB");
    strcpy(g_defs[HF_CLIMBER].category, "TAR MELEE ATTACK");
    strcpy(g_defs[HF_CLIMBER].movement_class, "CLIMBER");
    g_defs[HF_CLIMBER].max_velocity = 1.0f;
    g_defs[HF_CLIMBER].max_slope = 40;
    g_defs[HF_CLIMBER].num_weapons = 1;
    g_defs[HF_CLIMBER].sight_distance = 140;
    g_defs[HF_CLIMBER].weapons[0].range = 40;
    int climber = hf_add_unit(2, HF_CLIMBER, g_units[troop].world_x,
                              g_units[troop].world_y + 32);
    hf_run_ticks(&w, 60, 1);
    ASSERT_EQ_INT(1, TAK_AI_DebugWaveTargetReachable(2));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, (int)g_units[climber].cmd_kind);
    ASSERT_EQ_INT(UNIT_CMD_NONE, (int)g_units[troop].cmd_kind);

    /* Share the ground with one enemy and the walkers march again.
     * Only player 4 stands east of the wall. */
    setup_hostility_fixture(&w, loner);
    g_visible = 0;
    g_conn_split = 1;
    g_conn_x = 3100;
    troop = hf_troop(2);
    hf_run_ticks(&w, 60, 1);
    ASSERT_EQ_INT(1, TAK_AI_DebugWaveTargetReachable(2));
    ASSERT_EQ_INT(4, TAK_AI_DebugAttackPlayer(2));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, g_units[hf_troop(2)].cmd_kind);
    return 0;
}

/* A dead target is replaced by another enemy's unit. */
static int test_ai_wave_target_moves_on_when_it_dies(void) {
    GameWorld w;
    static const int ffa[5] = { 0, 0, 0, 0, 0 };
    setup_hostility_fixture(&w, ffa);
    g_visible = 0;
    hf_run_ticks(&w, 60, 1);
    int first = TAK_AI_DebugAttackPlayer(2);
    ASSERT_TRUE(first == 1 || first == 3 || first == 4);
    for (int h = 0; h < g_unit_count; h++) {
        if (g_units[h].player_id == first) g_units[h].alive = 0;
    }
    g_units[hf_troop(2)].cmd_kind = UNIT_CMD_NONE;
    hf_run_ticks(&w, 120, 1);
    int second = TAK_AI_DebugAttackPlayer(2);
    ASSERT_TRUE(second != first);
    ASSERT_TRUE(second == 1 || second == 3 || second == 4);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, g_units[hf_troop(2)].cmd_kind);
    return 0;
}

/* A hit on the base pulls the units at home onto the attacker: a
 * march to where the shots came from while it is unseen, an attack
 * order once it is seen, and the threat lapses ten seconds after the
 * last hit. */
static int test_ai_defends_its_base_when_hit(void) {
    GameWorld w;
    static const int ffa[5] = { 0, 0, 0, 0, 0 };
    setup_hostility_fixture(&w, ffa);
    g_visible = 0;
    int raider = hf_add_unit(1, HF_TROOP, hf_start_x[2] * 16 + 300,
                             hf_start_z[2] * 16 + 100);
    hf_run_ticks(&w, 60, 1);
    ASSERT_EQ_INT(0, TAK_AI_DebugDefenceOrders(2));

    /* The troop stands idle at home when the lodestone takes a hit. */
    g_units[hf_troop(2)].cmd_kind = UNIT_CMD_NONE;
    TAK_AI_NotifyDamage(hf_lode(2), raider);
    hf_run_ticks(&w, 120, 1);
    ASSERT_EQ_INT(1, TAK_AI_DebugDefenceOrders(2));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, g_units[hf_troop(2)].cmd_kind);
    ASSERT_EQ_INT(g_units[raider].world_x, g_units[hf_troop(2)].cmd_x);
    ASSERT_EQ_INT(g_units[raider].world_y, g_units[hf_troop(2)].cmd_y);

    g_visible = 1;
    hf_run_ticks(&w, 180, 1);
    ASSERT_EQ_INT(2, TAK_AI_DebugDefenceOrders(2));
    ASSERT_EQ_INT(UNIT_CMD_ATTACK, g_units[hf_troop(2)].cmd_kind);
    ASSERT_EQ_INT(raider, g_units[hf_troop(2)].target);

    /* Ten seconds after the hit the threat lapses: an idle unit goes
     * back to its wave instead of the raider. */
    g_visible = 0;
    g_units[hf_troop(2)].cmd_kind = UNIT_CMD_NONE;
    g_units[hf_troop(2)].target = -1;
    hf_run_ticks(&w, 780, 1);
    ASSERT_EQ_INT(2, TAK_AI_DebugDefenceOrders(2));

    /* A hit far from the base is not a base attack: the unit hit
     * goes on with its wave and no defence order is issued. */
    int far = hf_add_unit(2, HF_TROOP, hf_start_x[2] * 16 - 3000, 0);
    TAK_AI_NotifyDamage(far, raider);
    hf_run_ticks(&w, 840, 1);
    ASSERT_EQ_INT(2, TAK_AI_DebugDefenceOrders(2));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, g_units[far].cmd_kind);
    return 0;
}

/* An allied base under attack draws an idle AI's home units; a unit
 * already on the move is left to its errand. */
static int test_ai_helps_an_allied_base(void) {
    GameWorld w;
    static const int teamed[5] = { 0, 1, 2, 2, 3 };
    setup_hostility_fixture(&w, teamed);
    g_visible = 0;
    hf_run_ticks(&w, 60, 1);
    int before = TAK_AI_DebugDefenceOrders(3);

    /* AI 3's troop is idle at home when AI 4 hits AI 2's lodestone. */
    g_units[hf_troop(3)].cmd_kind = UNIT_CMD_NONE;
    TAK_AI_NotifyDamage(hf_lode(2), hf_troop(4));
    hf_run_ticks(&w, 120, 1);
    ASSERT_EQ_INT(before + 1, TAK_AI_DebugDefenceOrders(3));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, g_units[hf_troop(3)].cmd_kind);
    ASSERT_EQ_INT(g_units[hf_troop(4)].world_x, g_units[hf_troop(3)].cmd_x);
    ASSERT_EQ_INT(g_units[hf_troop(4)].world_y, g_units[hf_troop(3)].cmd_y);

    /* Busy allies stay busy: a marching troop is not turned around. */
    setup_hostility_fixture(&w, teamed);
    g_visible = 0;
    hf_run_ticks(&w, 60, 1);
    before = TAK_AI_DebugDefenceOrders(3);
    ASSERT_EQ_INT(UNIT_CMD_MOVE, g_units[hf_troop(3)].cmd_kind);
    int32_t errand_x = g_units[hf_troop(3)].cmd_x;
    TAK_AI_NotifyDamage(hf_lode(2), hf_troop(4));
    hf_run_ticks(&w, 120, 1);
    ASSERT_EQ_INT(before, TAK_AI_DebugDefenceOrders(3));
    ASSERT_EQ_INT(errand_x, g_units[hf_troop(3)].cmd_x);
    return 0;
}

/* A raid on a human teammate's base draws the AI ally's idle home
 * units, as a raid on an AI teammate does, and lapses the same way. */
static int test_ai_helps_a_human_ally(void) {
    GameWorld w;
    static const int teamed[5] = { 0, 1, 1, 2, 3 };
    setup_hostility_fixture(&w, teamed);
    g_visible = 0;
    hf_run_ticks(&w, 60, 1);
    int before = TAK_AI_DebugDefenceOrders(2);

    /* AI 2's troop is idle at home when AI 3 hits the human's monarch
     * at its start. */
    g_units[hf_troop(2)].cmd_kind = UNIT_CMD_NONE;
    TAK_AI_NotifyDamage(0, hf_troop(3));
    hf_run_ticks(&w, 120, 1);
    ASSERT_EQ_INT(before + 1, TAK_AI_DebugDefenceOrders(2));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, g_units[hf_troop(2)].cmd_kind);
    ASSERT_EQ_INT(g_units[hf_troop(3)].world_x, g_units[hf_troop(2)].cmd_x);
    ASSERT_EQ_INT(g_units[hf_troop(3)].world_y, g_units[hf_troop(2)].cmd_y);

    /* Ten seconds on the threat has lapsed: the idle troop goes back
     * to its wave. */
    g_units[hf_troop(2)].cmd_kind = UNIT_CMD_NONE;
    hf_run_ticks(&w, 780, 1);
    ASSERT_EQ_INT(before + 1, TAK_AI_DebugDefenceOrders(2));
    ASSERT_EQ_INT(UNIT_CMD_MOVE, g_units[hf_troop(2)].cmd_kind);
    return 0;
}

/* A hit on the monarch holds its construction for 1 to 31 seconds
 * (legacy:15092). */
static int test_ai_builder_freeze_after_a_hit(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 1;
    g_units[1].def_idx = 3;
    g_units[1].world_x = 5000;
    g_units[1].world_y = 5000;
    g_units[1].target = -1;
    g_unit_count = 2;
    g_visible = 0;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);

    g_units[0].cmd_kind = UNIT_CMD_NONE;
    g_units[0].build_target = -1;
    TAK_AI_NotifyDamage(0, 1);
    w.skirmish_elapsed_ticks = 120;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);

    w.skirmish_elapsed_ticks = 1980;   /* past the longest freeze */
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(2, g_begin_calls);
    return 0;
}

/* The freeze holds the monarch's build think and nothing else. The
 * original arms it for a hit on the monarch (legacy:15087-15096) and
 * reads it only in the monarch's branch (legacy:17257). A hit on
 * another builder freezes nobody, and while the monarch waits the
 * builder still builds and the castle still trains. */
static int test_ai_freeze_holds_only_the_monarch(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    g_mock_mana = 900;
    g_mock_max_mana = 1000;
    g_visible = 0;
    strcpy(g_defs[4].unitname, "TARTB");
    strcpy(g_defs[4].category, "TAR BUILDER");
    g_defs[4].cap_flags = UNIT_CAP_BUILDER;
    g_defs[4].max_velocity = 1.45f;
    g_defs[4].worker_time = 10.0f;
    g_buildable_counts[4] = 1;
    g_buildables[4][0] = 1;
    /* 1: an unseen raider far off, 2: the builder, 3: the castle. */
    static const int defs[3] = { 3, 4, 2 };
    static const int owners[3] = { 1, 2, 2 };
    for (int k = 0; k < 3; k++) {
        Unit *u = &g_units[1 + k];
        u->alive = UNIT_ALIVE_ACTIVE;
        u->player_id = (uint8_t)owners[k];
        u->def_idx = (uint16_t)defs[k];
        u->build_target = -1;
        u->target = -1;
        u->stable_id = 300u + (uint32_t)k;
    }
    g_units[1].world_x = 5000;
    g_units[1].world_y = 5000;
    g_unit_count = 4;

    /* The monarch takes a lodestone, the castle trains. */
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(2, g_begin_calls);

    /* A hit on the builder: both go on. */
    g_units[0].cmd_kind = UNIT_CMD_NONE;
    g_units[0].build_target = -1;
    g_units[3].cmd_kind = UNIT_CMD_NONE;
    g_units[3].build_target = -1;
    TAK_AI_NotifyDamage(2, 1);
    w.skirmish_elapsed_ticks = 120;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(4, g_begin_calls);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[0].cmd_kind);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[3].cmd_kind);

    /* A hit on the monarch: it waits, the builder takes the lodestone
     * and the castle trains. */
    g_units[0].cmd_kind = UNIT_CMD_NONE;
    g_units[0].build_target = -1;
    g_units[3].cmd_kind = UNIT_CMD_NONE;
    g_units[3].build_target = -1;
    TAK_AI_NotifyDamage(0, 1);
    w.skirmish_elapsed_ticks = 180;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(6, g_begin_calls);
    ASSERT_EQ_INT(UNIT_CMD_NONE, g_units[0].cmd_kind);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[2].cmd_kind);
    ASSERT_EQ_INT(3, g_last_builder);
    ASSERT_EQ_INT(3, g_last_build_def);

    /* 30 s at most: past it the monarch builds again. */
    g_units[2].cmd_kind = UNIT_CMD_NONE;
    g_units[2].build_target = -1;
    g_units[3].cmd_kind = UNIT_CMD_NONE;
    g_units[3].build_target = -1;
    w.skirmish_elapsed_ticks = 180 + 1860;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[0].cmd_kind);
    return 0;
}

/* A hit on the monarch drops the build the AI gave it, so the return
 * fire that follows answers the shooter (legacy:15097-15100, :15113).
 * Another builder that is hit keeps building and freezes nobody. */
static int test_ai_hit_monarch_drops_its_build(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    g_visible = 0;
    strcpy(g_defs[4].unitname, "TARTB");
    strcpy(g_defs[4].category, "TAR BUILDER");
    g_defs[4].cap_flags = UNIT_CAP_BUILDER;
    g_defs[4].max_velocity = 1.45f;
    g_defs[4].worker_time = 10.0f;
    /* 1: an unseen raider far off, 2: a builder at work on a frame. */
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 1;
    g_units[1].def_idx = 3;
    g_units[1].world_x = 5000;
    g_units[1].world_y = 5000;
    g_units[1].build_target = -1;
    g_units[1].target = -1;
    g_units[2].alive = UNIT_ALIVE_ACTIVE;
    g_units[2].player_id = 2;
    g_units[2].def_idx = 4;
    g_units[2].cmd_kind = UNIT_CMD_BUILD;
    g_units[2].build_target = 9;
    g_units[2].target = -1;
    g_unit_count = 3;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[0].cmd_kind);

    /* The builder is hit: it builds on, and the monarch is not held. */
    TAK_AI_NotifyDamage(2, 1);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[2].cmd_kind);
    ASSERT_EQ_INT(9, g_units[2].build_target);
    g_units[0].cmd_kind = UNIT_CMD_NONE;
    g_units[0].build_target = -1;
    w.skirmish_elapsed_ticks = 120;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(2, g_begin_calls);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[0].cmd_kind);

    /* The monarch is hit at its frame: the build is dropped at once
     * and no new one starts while it is held. */
    TAK_AI_NotifyDamage(0, 1);
    ASSERT_EQ_INT(1, g_stop_calls);
    ASSERT_EQ_INT(UNIT_CMD_NONE, g_units[0].cmd_kind);
    ASSERT_EQ_INT(-1, g_units[0].build_target);
    w.skirmish_elapsed_ticks = 180;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(2, g_begin_calls);
    return 0;
}

/* The original gates a build pick on build efficiency, the pool over
 * what the frames being fed ask for, not on how full the pool is
 * (legacy:17201, :235975-235983). Starved with nothing building and no
 * pad to take a lodestone, the monarch still raises its castle. Once a
 * frame it cannot pay for stands, it starts nothing more, while the
 * castle keeps training on the lower 7/30 gate (legacy:17991). */
static int test_ai_build_picks_follow_build_efficiency(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    g_mock_mana = 300;
    g_mock_max_mana = 2000;
    g_mock_income = 30;
    g_defs[1].yardmap_sacred = 1;
    g_defs[1].footprint_x = 2;
    g_defs[1].footprint_z = 2;
    g_defs[1].build_cost = 18000;
    g_defs[1].buildtime = 1.0f;
    g_sacred_registered = 1;
    g_sacred_def.sacred_site = 2.0f;
    static struct MapFeature pad;
    pad.feat_id = 0;
    pad.tile_x = 20;
    pad.tile_z = 20;
    pad.global_idx = 0;
    w.features = &pad;
    w.feature_count = 1;
    g_site_blocked_def = 1;   /* something stands on the pad */

    /* Nothing building, so the pool covers every frame there is. */
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(0, g_last_builder);
    ASSERT_EQ_INT(2, g_last_build_def);

    /* A builder feeding a frame that asks 3000 a tick, against a pool
     * of 1800: 60 percent, under the pick gate and over the training
     * one. The monarch starts nothing. */
    g_units[0].cmd_kind = UNIT_CMD_NONE;
    g_units[0].build_target = -1;
    strcpy(g_defs[4].unitname, "TARTB");
    strcpy(g_defs[4].category, "TAR BUILDER");
    g_defs[4].cap_flags = UNIT_CAP_BUILDER;
    g_defs[4].max_velocity = 1.45f;
    g_defs[4].worker_time = 10.0f;
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 2;
    g_units[1].def_idx = 4;
    g_units[1].cmd_kind = UNIT_CMD_BUILD;
    g_units[1].build_target = 2;
    g_units[1].target = -1;
    g_units[2].alive = UNIT_ALIVE_ACTIVE;
    g_units[2].player_id = 2;
    g_units[2].def_idx = 1;
    g_units[2].under_construction = 1;
    g_units[2].build_target = -1;
    g_units[2].target = -1;
    g_unit_count = 3;
    g_mock_mana = 1800;
    g_begin_calls = 0;
    w.skirmish_elapsed_ticks = 120;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(0, g_begin_calls);

    /* The same tick with a castle standing: training runs on. */
    g_units[3].alive = UNIT_ALIVE_ACTIVE;
    g_units[3].player_id = 2;
    g_units[3].def_idx = 2;
    g_units[3].build_target = -1;
    g_units[3].target = -1;
    g_unit_count = 4;
    w.skirmish_elapsed_ticks = 180;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(3, g_last_builder);
    ASSERT_EQ_INT(3, g_last_build_def);
    return 0;
}

/* A hit on a mission map moves nothing. The seat is read from the
 * world, not from a record an earlier skirmish left behind. */
static int test_ai_mission_map_hit_leaves_the_build(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 1;
    g_units[1].def_idx = 3;
    g_units[1].world_x = 5000;
    g_units[1].world_y = 5000;
    g_units[1].build_target = -1;
    g_units[1].target = -1;
    g_unit_count = 2;
    /* One skirmish tick marks the seat as an AI. */
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);

    /* The next battle is a mission, where the AI tick returns at once. */
    w.mission.objective_count = 1;
    g_units[0].cmd_kind = UNIT_CMD_BUILD;
    g_units[0].build_target = 7;
    g_stop_calls = 0;
    TAK_AI_NotifyDamage(0, 1);
    ASSERT_EQ_INT(0, g_stop_calls);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[0].cmd_kind);
    ASSERT_EQ_INT(7, g_units[0].build_target);
    return 0;
}

/* The pad test uses the lodestone the builder there would place: a
 * priest whose mana building needs a bigger pad must not hide a pad
 * the monarch's own lodestone fits. */
static int test_ai_pad_is_free_for_the_lodestone_it_would_place(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    g_buildable_counts[0] = 1;
    g_buildables[0][0] = 1;
    g_defs[1].yardmap_sacred = 1;
    g_defs[1].footprint_x = 2;
    g_defs[1].footprint_z = 2;
    strcpy(g_defs[4].unitname, "ARAPRIES");
    strcpy(g_defs[4].category, "ARA BUILDER");
    g_defs[4].cap_flags = UNIT_CAP_BUILDER;
    g_defs[4].max_velocity = 1.2f;
    g_defs[4].worker_time = 10.0f;
    g_buildable_counts[4] = 1;
    g_buildables[4][0] = 5;
    strcpy(g_defs[5].unitname, "ARAMANA");
    strcpy(g_defs[5].category, "ARA");
    g_defs[5].mogrium_storage = 2000;
    g_defs[5].mogrium_income_per_sec = 20.0f;
    g_defs[5].yardmap_sacred = 1;
    g_defs[5].footprint_x = 3;
    g_defs[5].footprint_z = 3;
    g_defs[5].build_cost = 8564;
    g_site_blocked_def = 5;   /* the big one does not fit this pad */
    g_sacred_registered = 1;
    g_sacred_def.sacred_site = 2.0f;
    static struct MapFeature pad;
    pad.feat_id = 0;
    pad.tile_x = 20;
    pad.tile_z = 20;
    pad.global_idx = 0;
    w.features = &pad;
    w.feature_count = 1;

    /* The priest is scanned first, the monarch second. */
    g_units[0].def_idx = 4;
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 2;
    g_units[1].def_idx = 0;
    g_units[1].build_target = -1;
    g_units[1].target = -1;
    g_unit_count = 2;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(1, g_last_builder);
    ASSERT_EQ_INT(1, g_last_build_def);
    return 0;
}

/* A monarch that took on a raider by itself is still the builder: the
 * next tick replaces the chase with the lodestone its base lacks
 * (legacy:17163). With nothing to build it keeps fighting. */
static int test_ai_fighting_builder_is_retasked(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    g_defs[0].num_weapons = 1;
    g_defs[0].sight_distance = 232;
    g_defs[0].weapons[0].range = 250;
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 1;
    g_units[1].def_idx = 3;
    g_units[1].world_x = 240;
    g_units[1].build_target = -1;
    g_units[1].target = -1;
    g_units[1].stable_id = 301;
    g_unit_count = 2;
    g_units[0].cmd_kind = UNIT_CMD_ATTACK;
    g_units[0].target = 1;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(0, g_last_builder);
    ASSERT_EQ_INT(1, g_last_build_def);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[0].cmd_kind);

    /* Lodestone and castle standing, and the profile allows one castle
     * (the shipped one allows two): the monarch has nothing left to
     * build and stays on the raider while the castle trains. */
    g_mock_profile = "limit TARCASTL 1\n";
    TAK_AI_ResetProfile();
    for (int k = 2; k <= 3; k++) {
        g_units[k].alive = UNIT_ALIVE_ACTIVE;
        g_units[k].player_id = 2;
        g_units[k].def_idx = (uint16_t)(k - 1);
        g_units[k].build_target = -1;
        g_units[k].target = -1;
    }
    g_unit_count = 4;
    g_units[0].cmd_kind = UNIT_CMD_ATTACK;
    g_units[0].target = 1;
    g_units[0].build_target = -1;
    w.skirmish_elapsed_ticks = 120;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(2, g_begin_calls);
    ASSERT_EQ_INT(3, g_last_builder);
    ASSERT_EQ_INT(UNIT_CMD_ATTACK, g_units[0].cmd_kind);
    ASSERT_EQ_INT(1, g_units[0].target);

    /* An attack the AI ordered is a mission of its own: the monarch
     * keeps it and starts nothing (legacy:17229-17238). */
    setup_ai_progression_fixture(&w);
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    g_defs[0].num_weapons = 1;
    g_defs[0].sight_distance = 232;
    g_defs[0].weapons[0].range = 250;
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 1;
    g_units[1].def_idx = 3;
    g_units[1].world_x = 240;
    g_units[1].build_target = -1;
    g_units[1].target = -1;
    g_units[1].stable_id = 301;
    g_unit_count = 2;
    g_units[0].cmd_kind = UNIT_CMD_ATTACK;
    g_units[0].target = 1;
    g_units[0].attack_explicit = 1;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(0, g_begin_calls);
    ASSERT_EQ_INT(UNIT_CMD_ATTACK, g_units[0].cmd_kind);
    return 0;
}


/* ── Influence maps ────────────────────────────────────────────────── */

/* Presence, value and wealth follow the units; threat and enemy value
 * follow them only where the fog allows. */
static int test_influence_maps_follow_units_and_fog(void) {
    GameWorld w;
    static const int ffa[5] = { 0, 0, 0, 0, 0 };
    setup_hostility_fixture(&w, ffa);
    static struct MapFeature pad;
    pad.feat_id = 0;
    pad.tile_x = 100;
    pad.tile_z = 100;
    pad.global_idx = 0;
    w.features = &pad;
    w.feature_count = 1;
    g_sacred_registered = 1;
    g_sacred_def.sacred_site = 2.0f;
    g_visible = 0;
    hf_run_ticks(&w, 60, 1);

    int gw = 0, gh = 0;
    AI_Influence_Size(&gw, &gh);
    ASSERT_EQ_INT(16, gw);
    ASSERT_EQ_INT(16, gh);
    const Unit *troop = &g_units[hf_troop(2)];
    const Unit *lode = &g_units[hf_lode(2)];
    const Unit *king = &g_units[0];
    ASSERT_TRUE(AI_Influence_At(2, AI_INF_PRESENCE, troop->world_x, troop->world_y) > 0);
    ASSERT_TRUE(AI_Influence_At(2, AI_INF_OWN_VALUE, lode->world_x, lode->world_y) > 0);
    ASSERT_TRUE(AI_Influence_At(2, AI_INF_WEALTH, lode->world_x, lode->world_y) > 0);
    ASSERT_TRUE(AI_Influence_At(2, AI_INF_WEALTH, 1600, 1600) > 0);
    ASSERT_EQ_INT(0, AI_Influence_At(2, AI_INF_PRESENCE, 1600, 1600));
    ASSERT_EQ_INT(0, AI_Influence_At(2, AI_INF_THREAT, king->world_x, king->world_y));
    ASSERT_EQ_INT(0, AI_Influence_At(2, AI_INF_ENEMY_VALUE, king->world_x, king->world_y));
    /* No map for a human seat. */
    ASSERT_EQ_INT(0, AI_Influence_At(1, AI_INF_PRESENCE, king->world_x, king->world_y));

    g_visible = 1;
    hf_run_ticks(&w, 120, 1);
    ASSERT_TRUE(AI_Influence_At(2, AI_INF_THREAT, king->world_x, king->world_y) > 0);
    ASSERT_TRUE(AI_Influence_At(2, AI_INF_ENEMY_VALUE, king->world_x, king->world_y) > 0);
    /* A lone monarch is valuable and weak. */
    ASSERT_TRUE(AI_Influence_Weakness(2, king->world_x, king->world_y) > 0);
    /* Presence falls off over two cells and stops. */
    int cx = 0, cy = 0;
    ASSERT_TRUE(AI_Influence_CellOf(troop->world_x, troop->world_y, &cx, &cy));
    int32_t centre = AI_Influence_Cell(2, AI_INF_PRESENCE, cx, cy);
    ASSERT_TRUE(AI_Influence_Cell(2, AI_INF_PRESENCE, cx - 1, cy) < centre);
    ASSERT_TRUE(AI_Influence_Cell(2, AI_INF_PRESENCE, cx - 1, cy) > 0);
    ASSERT_EQ_INT(0, AI_Influence_Cell(2, AI_INF_PRESENCE, cx - 3, cy));
    return 0;
}

/* Two candidates at equal distance: the one in the weaker cell wins
 * once an army stands beside the other (A-002 over legacy:15365). */
static int test_influence_tilts_the_wave_target(void) {
    GameWorld w;
    static const int ffa[5] = { 0, 0, 0, 0, 0 };
    setup_hostility_fixture(&w, ffa);
    g_visible = 1;
    Unit *troop = &g_units[hf_troop(2)];
    troop->world_x = 10 * AI_INF_CELL_PX;   /* on a cell edge */
    troop->world_y = 1000;
    /* Within 8 px the distance terms are 1, so only the tilt and the
     * tie-break (lowest handle) decide. */
    int east = hf_add_unit(1, HF_TROOP, troop->world_x + 3, 1000);
    int west = hf_add_unit(1, HF_TROOP, troop->world_x - 4, 1000);
    hf_run_ticks(&w, 60, 1);
    ASSERT_EQ_INT(east, TAK_AI_DebugWaveTarget(2));

    for (int i = 0; i < 6; i++) {
        hf_add_unit(1, HF_TROOP, troop->world_x + 120 + 8 * i, 1000);
    }
    /* A fresh match picks again. The AI holds its plans until one is
     * declared, so the test declares one. */
    TAK_AI_BeginMatch(0);
    hf_run_ticks(&w, 60, 1);
    ASSERT_EQ_INT(west, TAK_AI_DebugWaveTarget(2));
    return 0;
}

/* Seen enemies massing on a lodestone call the home units before a
 * shot lands. */
static int test_influence_exposure_calls_the_defence(void) {
    GameWorld w;
    static const int ffa[5] = { 0, 0, 0, 0, 0 };
    setup_hostility_fixture(&w, ffa);
    g_visible = 1;
    const Unit *lode = &g_units[hf_lode(2)];
    int first = -1;
    for (int i = 0; i < 4; i++) {
        int h = hf_add_unit(1, HF_TROOP, lode->world_x + 10 + 16 * i,
                            lode->world_y - 20);
        if (first < 0) first = h;
    }
    hf_run_ticks(&w, 60, 1);
    ASSERT_TRUE(AI_Influence_Exposure(2, lode->world_x, lode->world_y) > 0);
    ASSERT_EQ_INT(1, TAK_AI_DebugDefenceOrders(2));
    ASSERT_EQ_INT(UNIT_CMD_ATTACK, g_units[hf_troop(2)].cmd_kind);
    ASSERT_EQ_INT(first, g_units[hf_troop(2)].target);
    return 0;
}


/* ── Planner ───────────────────────────────────────────────────────── */

static void plan_state_basic(AiPlanState *s, AiPlanCosts *c) {
    memset(s, 0, sizeof(*s));
    memset(c, 0, sizeof(*c));
    s->mana_pct = 100;
    s->build_eff = 100;
    s->lode_target = 1;
    for (int a = 0; a < AI_ACT_COUNT; a++) {
        c->allowed[a] = 1;
        c->cost[a] = 100;
    }
    c->cost[AI_ACT_HOLD] = 0;
    c->cost[AI_ACT_WAVE] = 0;
    c->unit_value = 6;
    c->tower_value = 20;
}

/* Starved with nothing building: the builder goes for the lodestone
 * and the factory still trains. The original's only training gate is
 * build efficiency (legacy:17991) and no rule makes a troop wait for a
 * lodestone (legacy:19859). A frame the pool cannot cover is what
 * stops it. */
static int test_plan_starved_builds_its_lodestone_and_trains(void) {
    AiPlanState s;
    AiPlanCosts c;
    plan_state_basic(&s, &c);
    s.mana_pct = 10;
    s.stalling = 1;
    s.builders_idle = 1;
    s.factories = 1;
    s.factories_idle = 1;
    AiGoal goal = AI_GOAL_NONE;
    ASSERT_EQ_INT(AI_ACT_BUILD_LODESTONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    ASSERT_EQ_INT(AI_GOAL_ECONOMY, goal);
    ASSERT_EQ_INT(AI_ACT_TRAIN, AI_Plan_NextAction(&s, &c, AI_ACTOR_FACTORY, &goal));
    /* The lodestone on its way eats the pool: the factory waits until
     * the frames are covered again. */
    s.lodestones_pending = 1;
    s.build_eff = 20;
    ASSERT_EQ_INT(AI_ACT_NONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_FACTORY, &goal));
    s.build_eff = 60;
    ASSERT_EQ_INT(AI_ACT_TRAIN, AI_Plan_NextAction(&s, &c, AI_ACTOR_FACTORY, &goal));
    ASSERT_EQ_INT(AI_GOAL_ARMY, goal);
    /* No factory at all: the army plan is build one, then train. */
    plan_state_basic(&s, &c);
    s.builders_idle = 1;
    AiPlan plan;
    ASSERT_EQ_INT(1, AI_Plan_Solve(&s, &c, AI_GOAL_ARMY, &plan));
    ASSERT_EQ_INT(2, plan.step_count);
    ASSERT_EQ_INT(AI_ACT_BUILD_FACTORY, plan.steps[0]);
    ASSERT_EQ_INT(AI_ACT_TRAIN, plan.steps[1]);
    ASSERT_EQ_INT(200, plan.cost);
    return 0;
}

/* A threatened home is answered before a free site is taken. */
static int test_plan_threatened_defends_before_expanding(void) {
    AiPlanState s;
    AiPlanCosts c;
    plan_state_basic(&s, &c);
    s.lodestones = 1;
    s.site_near = 1;
    s.free_sites = 1;
    s.lode_target = 1;
    s.builders_idle = 1;
    s.exposure = 30;
    s.threat_home = 40;
    s.army = 40;
    s.army_home = 10;
    AiGoal goal = AI_GOAL_NONE;
    ASSERT_EQ_INT(AI_ACT_BUILD_TOWER, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    ASSERT_EQ_INT(AI_GOAL_DEFEND, goal);
    ASSERT_EQ_INT(AI_ACT_HOLD, AI_Plan_NextAction(&s, &c, AI_ACTOR_ARMY, &goal));
    ASSERT_EQ_INT(AI_GOAL_DEFEND, goal);
    /* No tower to build: the defence plans a factory to train from,
     * still ahead of the expansion. */
    c.allowed[AI_ACT_BUILD_TOWER] = 0;
    ASSERT_EQ_INT(AI_ACT_BUILD_FACTORY, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    ASSERT_EQ_INT(AI_GOAL_DEFEND, goal);
    /* Threat gone, home held: the free site is the builder's job. */
    s.exposure = 0;
    s.threat_home = 0;
    s.army_home = 40;
    s.lode_target = 2;
    ASSERT_EQ_INT(AI_ACT_BUILD_LODESTONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    ASSERT_EQ_INT(AI_GOAL_EXPAND, goal);
    return 0;
}

/* Profile weight 0 forbids an action, a limit caps the lodestones. */
static int test_plan_profile_forbids_and_caps(void) {
    AiPlanState s;
    AiPlanCosts c;
    plan_state_basic(&s, &c);
    s.builders_idle = 1;
    c.allowed[AI_ACT_BUILD_LODESTONE] = 0;
    AiGoal goal = AI_GOAL_NONE;
    ASSERT_EQ_INT(AI_ACT_BUILD_FACTORY, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    ASSERT_EQ_INT(AI_GOAL_ARMY, goal);
    plan_state_basic(&s, &c);
    s.lodestones = 2;
    s.lode_target = 2;   /* capped by the limit */
    s.free_sites = 4;
    s.site_near = 4;
    s.army = 100;
    s.builders_idle = 1;
    /* The same profile caps the producers, so nothing else is left
     * for this builder to want. */
    c.allowed[AI_ACT_BUILD_FACTORY] = 0;
    ASSERT_EQ_INT(0, AI_Plan_GoalPriority(&s, AI_GOAL_ECONOMY));
    /* Free sites do not get past the cap: no expansion plan, and the
     * builder has nothing to do. */
    AiPlan plan;
    ASSERT_EQ_INT(0, AI_Plan_Solve(&s, &c, AI_GOAL_EXPAND, &plan));
    ASSERT_EQ_INT(AI_ACT_NONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    return 0;
}

/* The gates read build efficiency, not the pool: a structure pick at
 * 70 percent (legacy:17201) and training at 7/30 (legacy:17991). An
 * empty pool with nothing building gates neither. */
static int test_plan_build_efficiency_gates(void) {
    AiPlanState s;
    AiPlanCosts c;
    plan_state_basic(&s, &c);
    s.lodestones = 1;
    s.builders_idle = 1;
    s.build_eff = 69;
    AiGoal goal = AI_GOAL_NONE;
    ASSERT_EQ_INT(AI_ACT_NONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    s.build_eff = 70;
    ASSERT_EQ_INT(AI_ACT_BUILD_FACTORY, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    /* A pool at 4 percent with nothing building still builds. */
    s.mana_pct = 4;
    s.build_eff = 100;
    ASSERT_EQ_INT(AI_ACT_BUILD_FACTORY, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    s.factories = 1;
    s.factories_idle = 1;
    s.build_eff = 22;
    ASSERT_EQ_INT(AI_ACT_NONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_FACTORY, &goal));
    s.build_eff = 23;
    ASSERT_EQ_INT(AI_ACT_TRAIN, AI_Plan_NextAction(&s, &c, AI_ACTOR_FACTORY, &goal));
    return 0;
}

/* An army is never finished. The original draws from a weighted build
 * list every pass and stops only where a per-type limit bites
 * (legacy:21281-21294, :21339-21343). */
static int test_plan_army_is_never_finished(void) {
    AiPlanState s;
    AiPlanCosts c;
    plan_state_basic(&s, &c);
    s.threat_total = 0;      /* fog: nothing seen, the floor rules */
    s.army = 32;             /* already past that floor */
    s.lodestones = 1;        /* economy met, so it masks nothing */
    s.factories = 1;
    s.factories_idle = 1;
    AiGoal goal = AI_GOAL_NONE;
    ASSERT_TRUE(AI_Plan_GoalPriority(&s, AI_GOAL_ARMY) > 0);
    ASSERT_EQ_INT(AI_ACT_TRAIN, AI_Plan_NextAction(&s, &c, AI_ACTOR_FACTORY, &goal));
    ASSERT_EQ_INT(AI_GOAL_ARMY, goal);
    /* An army that size ranks behind the economy, and still behind a
     * free site, so it takes nothing away from either. */
    s.lodestones = 0;
    ASSERT_TRUE(AI_Plan_GoalPriority(&s, AI_GOAL_ECONOMY) >
                AI_Plan_GoalPriority(&s, AI_GOAL_ARMY));
    s.lodestones = 1;
    s.site_near = 1;
    s.free_sites = 1;
    ASSERT_TRUE(AI_Plan_GoalPriority(&s, AI_GOAL_EXPAND) >
                AI_Plan_GoalPriority(&s, AI_GOAL_ARMY));
    /* The profile's limit is what ends it: nothing left to train. */
    c.allowed[AI_ACT_TRAIN] = 0;
    ASSERT_EQ_INT(AI_ACT_NONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_FACTORY, &goal));
    return 0;
}

/* A producer already standing is no reason to plan no more of them.
 * The original grows its count as the match runs (legacy:16254-16266)
 * up to the profile's limit, and only a frame in flight waits. */
static int test_plan_a_standing_producer_does_not_stop_the_next(void) {
    AiPlanState s;
    AiPlanCosts c;
    plan_state_basic(&s, &c);
    s.lodestones = 1;        /* economy met, so it masks nothing */
    s.builders_idle = 1;
    s.factories = 1;         /* one already standing */
    AiGoal goal = AI_GOAL_NONE;
    ASSERT_EQ_INT(AI_ACT_BUILD_FACTORY,
                  AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    /* One frame at a time: while that one builds, the builder waits. */
    s.factories_pending = 1;
    ASSERT_EQ_INT(AI_ACT_NONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    /* The profile's limit is what finally stops it. */
    s.factories_pending = 0;
    c.allowed[AI_ACT_BUILD_FACTORY] = 0;
    ASSERT_EQ_INT(AI_ACT_NONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
    return 0;
}

/* Two producers standing idle start two units in the one tick. The
 * original asks each producer whether it already has a build mission
 * and never asks the player (legacy:17992-17997), so its output is
 * the number of producers it owns. */
static int test_ai_every_idle_producer_starts_a_unit(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    g_mock_mana = 6000;
    g_mock_max_mana = 6000;
    /* Two castles standing, the lodestone the economy wanted, and a
     * troop already on the way from an earlier tick. The only thing
     * left to want is more army. */
    static const int defs[4] = { 1, 2, 2, 3 };
    for (int k = 0; k < 4; k++) {
        Unit *u = &g_units[1 + k];
        u->alive = UNIT_ALIVE_ACTIVE;
        u->player_id = 2;
        u->def_idx = (uint16_t)defs[k];
        u->build_target = -1;
        u->target = -1;
        u->stable_id = 400u + (uint32_t)k;
    }
    g_units[4].under_construction = 1;
    g_unit_count = 5;
    /* The monarch is busy, so every call counted here is a producer. */
    g_units[0].cmd_kind = UNIT_CMD_BUILD;
    g_units[0].build_target = 1;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(2, g_begin_calls);
    ASSERT_EQ_INT(3, g_last_build_def);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[2].cmd_kind);
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[3].cmd_kind);

    /* One already working takes no second order, and the other still
     * does: one order per producer, not one per player. */
    g_units[3].cmd_kind = UNIT_CMD_NONE;
    g_units[3].build_target = -1;
    g_begin_calls = 0;
    w.skirmish_elapsed_ticks = 120;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(3, g_last_builder);
    return 0;
}

/* Through the tick: seen enemies at the monarch's feet and a tower on
 * the build list, the monarch raises the tower, not the castle and not
 * a lodestone on the free pad. */
static int test_ai_threatened_builds_a_tower_before_expanding(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    w.cfg.players[0].team = 1;
    g_mock_mana = 6000;
    g_mock_max_mana = 6000;
    strcpy(g_defs[4].unitname, "TARTOWER");
    strcpy(g_defs[4].category, "TAR TOWER");
    g_defs[4].num_weapons = 1;
    g_defs[4].weapons[0].range = 300;
    g_defs[4].weapons[0].damage = 40;
    g_buildable_counts[0] = 3;
    g_buildables[0][2] = 4;
    g_defs[1].yardmap_sacred = 1;
    g_sacred_registered = 1;
    g_sacred_def.sacred_site = 2.0f;
    static struct MapFeature pad;
    pad.feat_id = 0;
    pad.tile_x = 30;
    pad.tile_z = 30;
    pad.global_idx = 0;
    w.features = &pad;
    w.feature_count = 1;
    for (int i = 0; i < 4; i++) {
        int h = g_unit_count++;
        g_units[h].alive = UNIT_ALIVE_ACTIVE;
        g_units[h].player_id = 1;
        g_units[h].def_idx = 3;
        g_units[h].world_x = 100 + 16 * i;
        g_units[h].world_y = 50;
        g_units[h].target = -1;
        g_units[h].stable_id = 500u + (uint32_t)h;
    }
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(4, g_last_build_def);

    /* The same seat unthreatened and short of mana takes the pad. */
    setup_ai_progression_fixture(&w);
    g_mock_mana = 100;
    g_mock_max_mana = 1000;
    g_defs[1].yardmap_sacred = 1;
    g_defs[1].footprint_x = 2;
    g_defs[1].footprint_z = 2;
    g_sacred_registered = 1;
    g_sacred_def.sacred_site = 2.0f;
    w.features = &pad;
    w.feature_count = 1;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(1, g_last_build_def);
    return 0;
}

/* Through the tick: starved with a castle standing idle, the monarch
 * builds the lodestone and the castle trains beside it, since no frame
 * is eating the pool yet (legacy:17991). */
static int test_ai_starved_builds_and_trains(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 2;
    g_units[1].def_idx = 2;
    g_units[1].build_target = -1;
    g_units[1].target = -1;
    g_unit_count = 2;
    g_mock_mana = 100;
    g_mock_max_mana = 1000;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(2, g_begin_calls);
    ASSERT_EQ_INT(1, g_last_builder);
    ASSERT_EQ_INT(3, g_last_build_def);

    /* The castle idle again and the pool back: it trains once more,
     * while the monarch is still on its lodestone. */
    g_units[1].cmd_kind = UNIT_CMD_NONE;
    g_units[1].build_target = -1;
    g_mock_mana = 900;
    w.skirmish_elapsed_ticks = 120;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(3, g_begin_calls);
    ASSERT_EQ_INT(1, g_last_builder);
    ASSERT_EQ_INT(3, g_last_build_def);
    return 0;
}

/* The AI's share of the simulation hash covers the state a save has to
 * carry and a lockstep peer has to agree on: the generator, the tick
 * the AI last ran, the per player records and the order matrices. */
static int test_ai_state_reaches_the_sim_hash(void) {
    GameWorld w;
    static const int ffa[5] = { 0, 0, 0, 0, 0 };
    setup_hostility_fixture(&w, ffa);
    g_visible = 0;
    hf_run_ticks(&w, 60, 1);

    uint32_t a = TAK_SimHash_AI(TAK_SIM_HASH_SEED);
    ASSERT_TRUE(a == TAK_SimHash_AI(TAK_SIM_HASH_SEED));
    ASSERT_TRUE(a != TAK_SIM_HASH_SEED);

    /* The running value is carried through rather than discarded, so
     * the AI cannot mask a difference the caller already found. */
    ASSERT_TRUE(TAK_SimHash_AI(TAK_SIM_HASH_SEED + 1u) != a);

    /* Another round of waves moves orders, targets and the tick. */
    hf_run_ticks(&w, 180, 1);
    ASSERT_TRUE(TAK_SimHash_AI(TAK_SIM_HASH_SEED) != a);
    return 0;
}

/* The AI draws from the session seed. Two matches with different seeds
 * hold different AI state after their first tick, and the same seed
 * twice holds the same. Before, the first tick of a match reset the
 * stream to a fixed constant, so the seed could never reach it. */
static int test_ai_stream_follows_the_session_seed(void) {
    GameWorld w;
    reset_mock(&w);
    TAK_AI_BeginMatch(0);
    TAK_AI_TickSkirmish(&w);
    unsigned int h0 = TAK_SimHash_AI(TAK_SIM_HASH_SEED);

    reset_mock(&w);
    TAK_AI_BeginMatch(5);
    TAK_AI_TickSkirmish(&w);
    unsigned int h5 = TAK_SimHash_AI(TAK_SIM_HASH_SEED);
    ASSERT_TRUE(h0 != h5);

    reset_mock(&w);
    TAK_AI_BeginMatch(5);
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT((int)h5, (int)TAK_SimHash_AI(TAK_SIM_HASH_SEED));
    return 0;
}

/* Issue #191. The ring search asks the pathfinder before it takes a
 * clear spot, on the builder's own class, as the original does with
 * the site it chose (legacy:17327). A wall west of the monarch makes
 * every site on that side no route, so the pick lands east, and a
 * builder walled in on every side starts nothing after a bounded
 * number of route checks rather than searching the whole ring. */
static int test_ai_sites_only_where_the_builder_can_walk(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    g_units[0].world_x = 2000;
    g_units[0].world_y = 2000;
    g_path_walled = 1;
    g_path_wall_x = 2000;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(1, g_last_build_def);
    printf("[sited at %d,%d after %d route checks] ",
           g_last_build_x, g_last_build_y, g_path_calls);
    ASSERT_TRUE(g_path_calls > 0);
    ASSERT_TRUE(g_last_build_x >= 2000);

    /* Walled in: nothing reachable, so nothing is started, and the
     * search stops after a handful of checks. */
    setup_ai_progression_fixture(&w);
    g_units[0].world_x = 2000;
    g_units[0].world_y = 2000;
    g_path_walled = 1;
    g_path_wall_x = 100000;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(0, g_begin_calls);
    ASSERT_TRUE(g_path_calls > 0 && g_path_calls <= 12);
    return 0;
}

/* Issue #191. A builder's give up reaches the seat that ordered the
 * build, and for a minute its site search passes that spot by. */
static int test_ai_remembers_a_site_its_builder_gave_up_on(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    g_units[0].world_x = 2000;
    g_units[0].world_y = 2000;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    int32_t sx = g_last_build_x, sy = g_last_build_y;
    ASSERT_EQ_INT(UNIT_CMD_BUILD, g_units[0].cmd_kind);
    ASSERT_EQ_INT(0, TAK_AI_DebugFailedSites(2));

    /* units.c reports the give up, then ends the order. */
    TAK_AI_NotifyGiveUp(0);
    ASSERT_EQ_INT(1, TAK_AI_DebugFailedSites(2));
    g_units[0].cmd_kind = UNIT_CMD_NONE;
    g_units[0].build_target = -1;
    w.skirmish_elapsed_ticks = 120;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(2, g_begin_calls);
    int64_t dx = (int64_t)g_last_build_x - sx, dy = (int64_t)g_last_build_y - sy;
    printf("[gave up at %d,%d, next site %d,%d] ", sx, sy,
           g_last_build_x, g_last_build_y);
    ASSERT_TRUE(dx * dx + dy * dy > 128 * 128);

    /* The same report twice is one site, not two. */
    g_units[0].cmd_x = sx;
    g_units[0].cmd_y = sy;
    TAK_AI_NotifyGiveUp(0);
    ASSERT_EQ_INT(1, TAK_AI_DebugFailedSites(2));

    /* A minute on it is forgotten and the spot is the first pick again. */
    g_units[0].cmd_kind = UNIT_CMD_NONE;
    g_units[0].build_target = -1;
    w.skirmish_elapsed_ticks = 120 + 3600;
    ASSERT_EQ_INT(0, TAK_AI_DebugFailedSites(2));
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(3, g_begin_calls);
    ASSERT_EQ_INT(sx, g_last_build_x);
    ASSERT_EQ_INT(sy, g_last_build_y);

    /* A human's builder is nobody's to remember, and neither is a
     * unit that gave up anything but a build. */
    setup_ai_progression_fixture(&w);
    w.cfg.players[0].kind = TAK_SLOT_HUMAN;
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 1;
    g_units[1].def_idx = 0;
    g_units[1].cmd_kind = UNIT_CMD_BUILD;
    g_units[1].cmd_x = 500;
    g_units[1].cmd_y = 500;
    g_unit_count = 2;
    TAK_AI_NotifyGiveUp(1);
    ASSERT_EQ_INT(0, TAK_AI_DebugFailedSites(1));
    g_units[0].cmd_kind = UNIT_CMD_MOVE;
    TAK_AI_NotifyGiveUp(0);
    ASSERT_EQ_INT(0, TAK_AI_DebugFailedSites(2));
    return 0;
}

/* Issue #191. The failed sites are simulation state: they reach the
 * hash and they come back from a save, and a save from before them
 * loads with none. */
static int test_ai_failed_sites_are_hashed_and_saved(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    TAK_AI_TickSkirmish(&w);
    uint32_t before = TAK_SimHash_AI(TAK_SIM_HASH_SEED);
    TAK_AI_NotifyGiveUp(0);
    uint32_t after = TAK_SimHash_AI(TAK_SIM_HASH_SEED);
    ASSERT_TRUE(before != after);

    unsigned int n = TAK_AI_StateBytes();
    unsigned char *buf = (unsigned char *)malloc(n);
    ASSERT_TRUE(buf != NULL);
    TAK_AI_SaveState(buf);
    TAK_AI_BeginMatch(0);
    ASSERT_EQ_INT(0, TAK_AI_DebugFailedSites(2));
    ASSERT_EQ_INT(0, TAK_AI_LoadState(buf, n));
    ASSERT_EQ_INT(1, TAK_AI_DebugFailedSites(2));
    ASSERT_TRUE(TAK_SimHash_AI(TAK_SIM_HASH_SEED) == after);

    /* What a build before this one wrote is this payload's prefix: the
     * wave reachability tail first, then the failed sites. */
    unsigned int reach_n = n - (unsigned int)(TAK_MAX_PLAYERS + 1) * 4u;
    ASSERT_EQ_INT(0, TAK_AI_LoadState(buf, reach_n));
    ASSERT_EQ_INT(1, TAK_AI_DebugFailedSites(2));
    ASSERT_EQ_INT(1, TAK_AI_DebugWaveTargetReachable(2));
    unsigned int old_n = reach_n -
        (unsigned int)(TAK_MAX_PLAYERS + 1) * (16u * 3u + 1u) * 4u;
    ASSERT_EQ_INT(0, TAK_AI_LoadState(buf, old_n));
    ASSERT_EQ_INT(0, TAK_AI_DebugFailedSites(2));
    ASSERT_TRUE(TAK_SimHash_AI(TAK_SIM_HASH_SEED) == before);
    ASSERT_EQ_INT(-1, TAK_AI_LoadState(buf, old_n - 1u));
    free(buf);
    return 0;
}

/* Issue #191. A pad the builder has no way to is passed by for the
 * nearest one it can walk to, and is remembered, which takes it off
 * the plan's count of free sites too. */
static int test_ai_expands_to_a_pad_it_can_walk_to(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    g_defs[1].yardmap_sacred = 1;
    g_defs[1].footprint_x = 2;
    g_defs[1].footprint_z = 2;
    g_buildable_counts[0] = 1;
    g_sacred_registered = 1;
    g_sacred_def.sacred_site = 2.0f;
    g_sacred_def.footprint_x = 2;
    g_sacred_def.footprint_z = 2;
    static struct MapFeature pads[2];
    memset(pads, 0, sizeof(pads));
    pads[0].tile_x = 100;   /* the near one, west of the wall */
    pads[0].tile_z = 125;
    pads[1].tile_x = 200;   /* the far one, east of it */
    pads[1].tile_z = 125;
    w.features = pads;
    w.feature_count = 2;
    g_units[0].world_x = 2000;
    g_units[0].world_y = 2000;
    g_path_walled = 1;
    g_path_wall_x = 1900;

    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(1, g_last_build_def);
    ASSERT_EQ_INT(200 * 16 + 16, g_last_build_x);
    ASSERT_EQ_INT(1, TAK_AI_DebugFailedSites(2));
    return 0;
}

/* Issue #192. A factory's draw covers every unit it can make, with the
 * original's build score behind the profile weight (legacy:19859,
 * legacy:21300): a builder type with none owned scores 1 plus the
 * limit band of 30, four times over for the first of its kind and
 * capped at 100, against 21 four times over for an armed troop. So a
 * Taros castle with a monarch at work and a builder in its list starts
 * the builder within a few draws. */
static int test_ai_factory_trains_a_builder_by_the_originals_weight(void) {
    GameWorld w;
    setup_ai_progression_fixture(&w);
    g_mock_mana = 5000;
    g_mock_max_mana = 5000;
    g_mock_income = 50;
    strcpy(g_defs[3].movement_class, "GROUND2");
    strcpy(g_defs[4].unitname, "TARTB");
    strcpy(g_defs[4].category, "TAR BUILDER");
    strcpy(g_defs[4].movement_class, "GROUND2");
    g_defs[4].cap_flags = UNIT_CAP_BUILDER;
    g_defs[4].max_velocity = 1.45f;
    g_defs[4].worker_time = 10.0f;
    g_buildable_counts[2] = 2;
    g_buildables[2][0] = 3;
    g_buildables[2][1] = 4;
    g_mock_profile = "weight TARTROOP 8\nweight TARTB 10\nlimit TARTB 10\n";
    TAK_AI_ResetProfile();

    /* The monarch is at work on a frame, the castle stands idle. */
    g_units[0].cmd_kind = UNIT_CMD_BUILD;
    g_units[0].build_target = 1;
    g_units[1].alive = UNIT_ALIVE_ACTIVE;
    g_units[1].player_id = 2;
    g_units[1].def_idx = 1;
    g_units[1].build_target = -1;
    g_units[1].target = -1;
    g_units[2].alive = UNIT_ALIVE_ACTIVE;
    g_units[2].player_id = 2;
    g_units[2].def_idx = 2;
    g_units[2].build_target = -1;
    g_units[2].target = -1;
    g_unit_count = 3;

    int draws = 0, builder_at = -1;
    for (int k = 0; k < 12 && builder_at < 0; k++) {
        w.skirmish_elapsed_ticks = 60 * (k + 1);
        g_begin_calls = 0;
        TAK_AI_TickSkirmish(&w);
        ASSERT_EQ_INT(1, g_begin_calls);
        ASSERT_EQ_INT(2, g_last_builder);
        draws++;
        if (g_last_build_def == 4) builder_at = k;
        g_units[2].cmd_kind = UNIT_CMD_NONE;
        g_units[2].build_target = -1;
    }
    printf("[builder on draw %d of %d] ", builder_at + 1, draws);
    ASSERT_TRUE(builder_at >= 0);

    /* One at a time: while a builder is in the yard the band is off
     * (legacy:19951) and the castle trains troops. */
    g_units[3].alive = UNIT_ALIVE_ACTIVE;
    g_units[3].player_id = 2;
    g_units[3].def_idx = 4;
    g_units[3].under_construction = 1;
    g_units[3].build_target = -1;
    g_units[3].target = -1;
    g_unit_count = 4;
    for (int k = 0; k < 20; k++) {
        w.skirmish_elapsed_ticks = 60 * (k + 20);
        g_begin_calls = 0;
        TAK_AI_TickSkirmish(&w);
        ASSERT_EQ_INT(1, g_begin_calls);
        ASSERT_EQ_INT(3, g_last_build_def);
        g_units[2].cmd_kind = UNIT_CMD_NONE;
        g_units[2].build_target = -1;
    }
    return 0;
}

int main(void) {
    ASSERT_EQ_INT(0, TAK_AI_ClampDifficulty(-99));
    ASSERT_EQ_INT(0, TAK_AI_ClampDifficulty(0));
    ASSERT_EQ_INT(1, TAK_AI_ClampDifficulty(1));
    ASSERT_EQ_INT(2, TAK_AI_ClampDifficulty(2));
    ASSERT_EQ_INT(3, TAK_AI_ClampDifficulty(3));
    ASSERT_EQ_INT(3, TAK_AI_ClampDifficulty(99));

    ASSERT_EQ_INT(64, TAK_AI_PursuitRadius(128, 32, 0));
    ASSERT_EQ_INT(128, TAK_AI_PursuitRadius(128, 32, 1));
    ASSERT_EQ_INT(192, TAK_AI_PursuitRadius(128, 32, 2));
    ASSERT_EQ_INT(256, TAK_AI_PursuitRadius(128, 32, 3));
    ASSERT_EQ_INT(600, TAK_AI_PursuitRadius(100, 300, 3));
    ASSERT_EQ_INT(0, TAK_AI_PursuitRadius(0, 0, 3));
    if (test_ai_builds_economy_then_production_then_combat() != 0) return 1;
    if (test_ai_moves_to_the_next_factory_at_the_limit() != 0) return 1;
    if (test_ai_builds_lodestone_on_sacred_pad() != 0) return 1;
    if (test_ai_wave_targets_follow_the_teams() != 0) return 1;
    if (test_ai_wave_target_moves_on_when_it_dies() != 0) return 1;
    if (test_ai_waves_never_pick_a_wall() != 0) return 1;
    if (test_ai_waves_need_a_land_route_to_the_target() != 0) return 1;
    if (test_ai_defends_its_base_when_hit() != 0) return 1;
    if (test_ai_helps_an_allied_base() != 0) return 1;
    if (test_ai_helps_a_human_ally() != 0) return 1;
    if (test_ai_builder_freeze_after_a_hit() != 0) return 1;
    if (test_ai_freeze_holds_only_the_monarch() != 0) return 1;
    if (test_ai_fighting_builder_is_retasked() != 0) return 1;
    if (test_ai_hit_monarch_drops_its_build() != 0) return 1;
    if (test_ai_mission_map_hit_leaves_the_build() != 0) return 1;
    if (test_ai_pad_is_free_for_the_lodestone_it_would_place() != 0) return 1;
    if (test_ai_build_picks_follow_build_efficiency() != 0) return 1;
    if (test_influence_maps_follow_units_and_fog() != 0) return 1;
    if (test_influence_tilts_the_wave_target() != 0) return 1;
    if (test_influence_exposure_calls_the_defence() != 0) return 1;
    if (test_plan_starved_builds_its_lodestone_and_trains() != 0) return 1;
    if (test_plan_threatened_defends_before_expanding() != 0) return 1;
    if (test_plan_profile_forbids_and_caps() != 0) return 1;
    if (test_plan_build_efficiency_gates() != 0) return 1;
    if (test_plan_army_is_never_finished() != 0) return 1;
    if (test_plan_a_standing_producer_does_not_stop_the_next() != 0) return 1;
    if (test_ai_every_idle_producer_starts_a_unit() != 0) return 1;
    if (test_ai_threatened_builds_a_tower_before_expanding() != 0) return 1;
    if (test_ai_starved_builds_and_trains() != 0) return 1;
    if (test_ai_mobile_producer_trains_the_army() != 0) return 1;
    if (test_ai_state_reaches_the_sim_hash() != 0) return 1;
    if (test_ai_stream_follows_the_session_seed() != 0) return 1;
    if (test_ai_sites_only_where_the_builder_can_walk() != 0) return 1;
    if (test_ai_remembers_a_site_its_builder_gave_up_on() != 0) return 1;
    if (test_ai_failed_sites_are_hashed_and_saved() != 0) return 1;
    if (test_ai_expands_to_a_pad_it_can_walk_to() != 0) return 1;
    if (test_ai_factory_trains_a_builder_by_the_originals_weight() != 0) return 1;

    puts("test_ai: ok");
    return 0;
}
