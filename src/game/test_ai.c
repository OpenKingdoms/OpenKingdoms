#include "tak_ai.h"
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
    g_sacred_registered = 0;
    memset(&g_sacred_def, 0, sizeof(g_sacred_def));
    g_world = w;
    w->loaded = 1;
    w->skirmish_elapsed_ticks = 60;
    w->cfg.line_of_sight = 1;
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

    strcpy(g_defs[HF_TROOP].unitname, "TARTROOP");
    strcpy(g_defs[HF_TROOP].category, "TAR MELEE ATTACK");
    g_defs[HF_TROOP].max_velocity = 1.0f;
    g_defs[HF_TROOP].num_weapons = 1;
    g_defs[HF_TROOP].sight_distance = 140;
    g_defs[HF_TROOP].weapons[0].range = 40;

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

/* A builder that takes a hit freezes the AI's construction for 1 to
 * 31 seconds (legacy:15092). */
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
    if (test_ai_builder_freeze_after_a_hit() != 0) return 1;

    puts("test_ai: ok");
    return 0;
}
