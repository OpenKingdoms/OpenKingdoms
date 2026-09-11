#include "tak_ai.h"
#include "tak_ai_influence.h"
#include "tak_ai_plan.h"
#include "tak_economy.h"
#include "tak_unit.h"
#include "tak_world.h"
#include "tak_features.h"
#include "tak_hpi.h"

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
static int32_t g_last_build_x;
static int32_t g_last_build_y;
static int g_attack_calls;
static int g_move_calls;
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

/* One mock sacred site, wired up by the expansion test. */
static FeatureDef g_sacred_def;
static int g_sacred_registered;

/* Stubs for profile/expansion deps — no data dir in the mock harness. */
int VFS_ReadFile(const char *path, void **out_data, uint32_t *out_size) {
    (void)path; (void)out_data; (void)out_size; return -1;
}
const FeatureDef *Features_GetByIndex(int idx) {
    if (g_sacred_registered && idx == 0) return &g_sacred_def;
    return NULL;
}
int Units_FindDefByName(const char *unitname) { (void)unitname; return -1; }
void *tak_malloc(size_t n) { return malloc(n); }
void tak_free(void *p) { free(p); }

const UnitDef *Units_GetDef(int idx) {
    if (idx < 0 || idx >= MOCK_DEFS) return NULL;
    return &g_defs[idx];
}

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

int Units_GetBuildables(int builder_def_idx, int *out_def_idxs, int max_out) {
    if (builder_def_idx < 0 || builder_def_idx >= MOCK_DEFS || !out_def_idxs || max_out <= 0)
        return 0;
    int n = g_buildable_counts[builder_def_idx];
    if (n > max_out) n = max_out;
    for (int i = 0; i < n; i++) out_def_idxs[i] = g_buildables[builder_def_idx][i];
    return n;
}

int Units_IsBuildSiteClear(int def_idx, int32_t world_x, int32_t world_y) {
    (void)def_idx;
    (void)world_x;
    (void)world_y;
    return g_site_clear;
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
    g_last_build_x = 0;
    g_last_build_y = 0;
    g_attack_calls = 0;
    g_move_calls = 0;
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
    g_sacred_registered = 0;
    memset(&g_sacred_def, 0, sizeof(g_sacred_def));
    g_world = w;
    w->loaded = 1;
    w->skirmish_elapsed_ticks = 60;
    w->cfg.line_of_sight = 1;
    w->map_pixels_w = 4096;
    w->map_pixels_h = 4096;
    TAK_AI_ResetProfile();
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

    /* With the handler standing it trains the army, and the monarch
     * summons no second handler. */
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

    g_units[0].cmd_kind = UNIT_CMD_NONE;
    g_units[0].build_target = -1;
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
    g_defs[0].commander = 1;
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
    g_defs[0].commander = 1;
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

    /* Lodestone and castle standing: the monarch has nothing to build
     * and stays on the raider while the castle trains. */
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
    /* The same tick again starts a fresh match and a fresh pick. */
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

/* Starved and without a lodestone: the builder feeds the economy and
 * the idle factory waits rather than spend the last mana on a troop. */
static int test_plan_starved_feeds_the_lodestone_first(void) {
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
    ASSERT_EQ_INT(AI_ACT_NONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_FACTORY, &goal));
    /* Lodestone on its way: the factory still waits while starved,
     * and trains once the mana is back. */
    s.lodestones_pending = 1;
    ASSERT_EQ_INT(AI_ACT_NONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_FACTORY, &goal));
    s.mana_pct = 60;
    s.stalling = 0;
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
    ASSERT_EQ_INT(0, AI_Plan_GoalPriority(&s, AI_GOAL_ECONOMY));
    /* Free sites do not get past the cap: no expansion plan, and the
     * builder has nothing to do. */
    AiPlan plan;
    ASSERT_EQ_INT(0, AI_Plan_Solve(&s, &c, AI_GOAL_EXPAND, &plan));
    ASSERT_EQ_INT(AI_ACT_NONE, AI_Plan_NextAction(&s, &c, AI_ACTOR_BUILDER, &goal));
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
 * builds the lodestone and the castle waits; with mana back the castle
 * trains. */
static int test_ai_starved_feeds_the_lodestone_before_training(void) {
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
    ASSERT_EQ_INT(1, g_begin_calls);
    ASSERT_EQ_INT(0, g_last_builder);
    ASSERT_EQ_INT(1, g_last_build_def);

    g_mock_mana = 900;
    w.skirmish_elapsed_ticks = 120;
    TAK_AI_TickSkirmish(&w);
    ASSERT_EQ_INT(2, g_begin_calls);
    ASSERT_EQ_INT(1, g_last_builder);
    ASSERT_EQ_INT(3, g_last_build_def);
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
    if (test_ai_builds_lodestone_on_sacred_pad() != 0) return 1;
    if (test_ai_wave_targets_follow_the_teams() != 0) return 1;
    if (test_ai_wave_target_moves_on_when_it_dies() != 0) return 1;
    if (test_ai_defends_its_base_when_hit() != 0) return 1;
    if (test_ai_helps_an_allied_base() != 0) return 1;
    if (test_ai_helps_a_human_ally() != 0) return 1;
    if (test_ai_builder_freeze_after_a_hit() != 0) return 1;
    if (test_ai_freeze_holds_only_the_monarch() != 0) return 1;
    if (test_ai_fighting_builder_is_retasked() != 0) return 1;
    if (test_influence_maps_follow_units_and_fog() != 0) return 1;
    if (test_influence_tilts_the_wave_target() != 0) return 1;
    if (test_influence_exposure_calls_the_defence() != 0) return 1;
    if (test_plan_starved_feeds_the_lodestone_first() != 0) return 1;
    if (test_plan_threatened_defends_before_expanding() != 0) return 1;
    if (test_plan_profile_forbids_and_caps() != 0) return 1;
    if (test_ai_threatened_builds_a_tower_before_expanding() != 0) return 1;
    if (test_ai_starved_feeds_the_lodestone_before_training() != 0) return 1;
    if (test_ai_mobile_producer_trains_the_army() != 0) return 1;

    puts("test_ai: ok");
    return 0;
}
