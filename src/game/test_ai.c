#include "tak_ai.h"
#include "tak_unit.h"
#include "tak_world.h"
#include "tak_features.h"
#include "tak_hpi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UnitDef g_defs[4];
static Unit g_units[4];
static int g_unit_count;
static int g_buildable_counts[4];
static int g_buildables[4][4];
static int g_begin_calls;
static int g_last_builder;
static int g_last_build_def;
static int g_site_clear = 1;
static int32_t g_last_build_x;
static int32_t g_last_build_y;

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
    if (idx < 0 || idx >= 4) return NULL;
    return &g_defs[idx];
}

const Unit *Units_GetActive(int *out_count) {
    if (out_count) *out_count = g_unit_count;
    return g_units;
}

void Units_CommandMoveUnit(int handle, int32_t world_x, int32_t world_y) {
    (void)handle;
    (void)world_x;
    (void)world_y;
}

void Units_CommandAttackUnit(int handle, int target_handle) {
    (void)handle;
    (void)target_handle;
}

int Units_GetBuildables(int builder_def_idx, int *out_def_idxs, int max_out) {
    if (builder_def_idx < 0 || builder_def_idx >= 4 || !out_def_idxs || max_out <= 0)
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
    return 1;
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

static void setup_ai_progression_fixture(GameWorld *w) {
    memset(w, 0, sizeof(*w));
    memset(g_defs, 0, sizeof(g_defs));
    memset(g_units, 0, sizeof(g_units));
    memset(g_buildable_counts, 0, sizeof(g_buildable_counts));
    memset(g_buildables, 0, sizeof(g_buildables));
    g_unit_count = 1;
    g_begin_calls = 0;
    g_last_builder = -1;
    g_last_build_def = -1;
    g_site_clear = 1;
    g_last_build_x = 0;
    g_last_build_y = 0;
    g_sacred_registered = 0;
    memset(&g_sacred_def, 0, sizeof(g_sacred_def));

    w->loaded = 1;
    w->skirmish_elapsed_ticks = 60;
    w->cfg.line_of_sight = 1;
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

    g_buildable_counts[0] = 2;
    g_buildables[0][0] = 1;
    g_buildables[0][1] = 2;
    g_buildable_counts[2] = 1;
    g_buildables[2][0] = 3;

    g_units[0].alive = UNIT_ALIVE_ACTIVE;
    g_units[0].player_id = 2;
    g_units[0].def_idx = 0;
    g_units[0].build_target = -1;
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

    puts("test_ai: ok");
    return 0;
}
