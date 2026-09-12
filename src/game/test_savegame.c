/*
 * The game state inside the save container: the battle that was set
 * up, the map it was played on, the world scalars, the generator and
 * the camera.
 *
 * This is the headless entry point. It writes and reads a save with no
 * platform and no window. SDL headers arrive through tak_world.h, but
 * no window is opened and no SDL subsystem is ever started.
 *
 * The simulation is file scope statics inside units.c and features.c,
 * so this test stands
 * in for them the way test_sim_hash does: it owns the world, the unit
 * array and both registries, and savegame.c reads them through the
 * same public accessors the engine uses.
 */

#include "test_framework.h"

#include "tak_battle_config.h"
#include "tak_bytes.h"
#include "tak_features.h"
#include "tak_memory.h"
#include "tak_savefile.h"
#include "tak_savegame.h"
#include "tak_sim_hash.h"
#include "tak_sim_rand.h"
#include "tak_unit.h"
#include "tak_util.h"
#include "tak_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH "test_savegame_scratch.oksave"

#define FIX_UNITS    3
#define FIX_DEFS     4
#define FIX_FEATDEFS 3
#define FIX_FEAT     2

/* ── the state savegame.c reads ───────────────────────────────────── */

static GameWorld  *g_world;
static Unit       *g_units;
static int         g_unit_count;
static Projectile *g_projectiles;
static int         g_projectile_count;
static UnitDef     g_defs[FIX_DEFS];
static int         g_def_count;
static FeatureDef  g_featdefs[FIX_FEATDEFS];
static int         g_featdef_count;

GameWorld *World_Get(void) { return g_world; }

const Unit *Units_GetActive(int *out_count) {
    if (out_count) *out_count = g_unit_count;
    return g_units;
}

const Projectile *Units_GetProjectiles(int *out_count) {
    if (out_count) *out_count = g_projectile_count;
    return g_projectiles;
}

const UnitDef *Units_GetDef(int idx) {
    if (idx < 0 || idx >= g_def_count) return NULL;
    return &g_defs[idx];
}

int Units_GetDefCount(void) { return g_def_count; }

int Units_FindDefByName(const char *unitname) {
    if (!unitname) return -1;
    for (int i = 0; i < g_def_count; i++) {
        if (tak_stricmp(g_defs[i].unitname, unitname) == 0) return i;
    }
    return -1;
}

const FeatureDef *Features_GetByIndex(int idx) {
    if (idx < 0 || idx >= g_featdef_count) return NULL;
    return &g_featdefs[idx];
}

int Features_GetCount(void) { return g_featdef_count; }

int Features_FindByName(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < g_featdef_count; i++) {
        if (tak_stricmp(g_featdefs[i].name, name) == 0) return i;
    }
    return -1;
}

/* The real one folds in the AI's own statics. One value here, so the
 * header's state hash is still a real number over a real world. */
uint32_t TAK_SimHash_AI(uint32_t h) { return TAK_HashU32(h, 0xa1a1a1a1u); }

/* ── fixture ──────────────────────────────────────────────────────── */

static void teardown(void) {
    free(g_units);
    g_units = NULL;
    g_unit_count = 0;
    free(g_projectiles);
    g_projectiles = NULL;
    g_projectile_count = 0;
    if (g_world) {
        free(g_world->features);
        free(g_world);
        g_world = NULL;
    }
}

static void fill_defs(void) {
    static const char *const names[FIX_DEFS] = {
        "ARAKING", "ARABOWMAN", "ARAGUARD", "TARNECRO"
    };
    static const char *const fnames[FIX_FEATDEFS] = {
        "AraHenge01", "AraLodestone", "AraKingCorpse"
    };
    memset(g_defs, 0, sizeof(g_defs));
    for (int i = 0; i < FIX_DEFS; i++) {
        snprintf(g_defs[i].unitname, sizeof(g_defs[i].unitname), "%s", names[i]);
        g_defs[i].max_health = 400 + i * 10;
        g_defs[i].sight_distance = 320 + i;
        g_defs[i].build_cost = 100 + i;
        g_defs[i].max_velocity = 1.5f + (float)i;
        g_defs[i].footprint_x = 2 + i;
        g_defs[i].footprint_z = 2;
        g_defs[i].cap_flags = UNIT_CAP_MOVE | UNIT_CAP_ATTACK;
        g_defs[i].num_weapons = (i == 1) ? 2 : 1;
        for (int w = 0; w < g_defs[i].num_weapons; w++) {
            snprintf(g_defs[i].weapons[w].name,
                     sizeof(g_defs[i].weapons[w].name), "weapon%d_%d", i, w);
            g_defs[i].weapons[w].range = 200 + w * 10;
            g_defs[i].weapons[w].damage = 40 + w;
            g_defs[i].weapons[w].reload_ticks = 60 + w;
        }
    }
    g_def_count = FIX_DEFS;

    memset(g_featdefs, 0, sizeof(g_featdefs));
    for (int i = 0; i < FIX_FEATDEFS; i++) {
        snprintf(g_featdefs[i].name, sizeof(g_featdefs[i].name), "%s", fnames[i]);
        g_featdefs[i].footprint_x = 1 + i;
        g_featdefs[i].footprint_z = 1;
        g_featdefs[i].blocking = 1;
        g_featdefs[i].reclaimable = (i != 2);
        g_featdefs[i].decompose_time = (i == 2) ? 1800 : 0;
    }
    g_featdef_count = FIX_FEATDEFS;
}

/* A battle with every option off its default, so a field the writer
 * drops cannot pass by matching a zero. */
static void fill_cfg(BattleConfig *cfg) {
    BattleConfig_SetDefaults(cfg);
    snprintf(cfg->map_name, sizeof(cfg->map_name), "%s", "two castles");
    cfg->units_per_side = 700;
    cfg->line_of_sight = 0;
    cfg->map_revealed = 1;
    cfg->monarch_expendable = 1;
    cfg->random_start_locations = 1;
    cfg->power_codes = 1;
    cfg->slow_game = 1;
    cfg->crusades_balance = 1;
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        cfg->players[i].kind = (i < 3) ? TAK_SLOT_AI : TAK_SLOT_CLOSED;
        cfg->players[i].side = i % TAK_SIDE_COUNT;
        cfg->players[i].team = 1 + (i % 4);
        cfg->players[i].color = i;
        cfg->players[i].ai_difficulty = i % 4;
        snprintf(cfg->players[i].name, sizeof(cfg->players[i].name),
                 "seat %d", i);
    }
    cfg->players[0].kind = TAK_SLOT_HUMAN;
}

static int setup(const char *map_name) {
    teardown();
    fill_defs();

    g_world = (GameWorld *)calloc(1, sizeof(GameWorld));
    if (!g_world) return -1;
    fill_cfg(&g_world->cfg);
    if (map_name) {
        snprintf(g_world->cfg.map_name, sizeof(g_world->cfg.map_name), "%s",
                 map_name);
    }
    snprintf(g_world->map_name, sizeof(g_world->map_name), "%s",
             g_world->cfg.map_name);
    snprintf(g_world->map_kingdom, sizeof(g_world->map_kingdom), "%s", "aramon");
    g_world->loaded = 1;
    g_world->water_height = 64;
    g_world->skirmish_elapsed_ticks = 4321;
    g_world->skirmish_game_over = 1;
    g_world->skirmish_winner_team = 2;
    g_world->skirmish_local_result = -1;
    snprintf(g_world->skirmish_end_reason, sizeof(g_world->skirmish_end_reason),
             "%s", "the monarch fell");
    g_world->skirmish_end_tick = 4300;
    g_world->skirmish_stats_open = 1;
    g_world->mission_elapsed_ticks = 17;
    g_world->mission_elapsed_seconds = 0;
    g_world->mission_objectives_satisfied = 1;
    g_world->mission_victory = 1;
    g_world->cam_x = 1536;
    g_world->cam_y = 992;
    for (int p = 0; p <= TAK_MAX_PLAYERS; p++) {
        g_world->stats[p].units_built = p * 3 + 1;
        g_world->stats[p].kills = p * 2;
        g_world->stats[p].losses = p;
        g_world->stats[p].score = p * 100;
        g_world->stats[p].eliminated = (p == 3) ? 1 : 0;
        g_world->stats[p].last_alive_tick = 4000 + p;
    }

    g_world->feature_count = FIX_FEAT;
    g_world->feature_cap = FIX_FEAT;
    g_world->features = (struct MapFeature *)calloc(FIX_FEAT,
                                                    sizeof(*g_world->features));
    if (!g_world->features) return -1;
    for (int i = 0; i < FIX_FEAT; i++) {
        g_world->features[i].feat_id = (uint16_t)(10 + i);
        g_world->features[i].tile_x = (uint16_t)(4 + i);
        g_world->features[i].tile_z = (uint16_t)(5 + i);
        /* The first is authored scenery, the last a corpse. */
        g_world->features[i].global_idx = (i == 0) ? 0 : 2;
        g_world->features[i].decompose_ticks = (i == 0) ? -1 : 900;
    }

    g_units = (Unit *)calloc(FIX_UNITS, sizeof(Unit));
    if (!g_units) return -1;
    g_unit_count = FIX_UNITS;
    for (int i = 0; i < FIX_UNITS; i++) {
        g_units[i].stable_id = (uint32_t)(100 + i);
        g_units[i].alive = UNIT_ALIVE_ACTIVE;
        g_units[i].def_idx = (uint16_t)i;
        g_units[i].player_id = (uint8_t)(1 + (i % 2));
        g_units[i].target = -1;
        g_units[i].build_target = -1;
        g_units[i].carried_by = -1;
    }
    /* A factory with something queued, so a definition reached only
     * through a production queue is still named in the file. */
    g_units[0].prod_queue_len = 1;
    g_units[0].prod_queue[0] = 3;
    /* A dead slot keeps a stale definition index nothing may follow. */
    g_units[2].alive = UNIT_ALIVE_DEAD;

    World_SeedRand(0x4d2);
    return 0;
}

static int write_scratch(char *err, size_t cap) {
    remove(SCRATCH);
    return Save_Write(SCRATCH, err, cap);
}

/* ── the sections ─────────────────────────────────────────────────── */

TEST(a_battle_with_no_world_is_refused) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    teardown();
    ASSERT_EQ_INT(-1, Save_Write(SCRATCH, err, sizeof(err)));
    ASSERT(err[0] != 0);
}

TEST(every_battle_config_field_survives) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    BattleConfig want = g_world->cfg;
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    const TAK_SaveInfo *info = Save_Info(sg);
    ASSERT_NOT_NULL(info);
    const BattleConfig *got = &info->cfg;

    ASSERT_EQ_STR(want.map_name, got->map_name);
    ASSERT_EQ_INT(want.units_per_side, got->units_per_side);
    ASSERT_EQ_INT(want.line_of_sight, got->line_of_sight);
    ASSERT_EQ_INT(want.map_revealed, got->map_revealed);
    ASSERT_EQ_INT(want.monarch_expendable, got->monarch_expendable);
    ASSERT_EQ_INT(want.random_start_locations, got->random_start_locations);
    ASSERT_EQ_INT(want.power_codes, got->power_codes);
    ASSERT_EQ_INT(want.slow_game, got->slow_game);
    ASSERT_EQ_INT(want.crusades_balance, got->crusades_balance);
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        ASSERT_EQ_INT((int)want.players[i].kind, (int)got->players[i].kind);
        ASSERT_EQ_INT(want.players[i].side, got->players[i].side);
        ASSERT_EQ_INT(want.players[i].team, got->players[i].team);
        ASSERT_EQ_INT(want.players[i].color, got->players[i].color);
        ASSERT_EQ_INT(want.players[i].ai_difficulty,
                      got->players[i].ai_difficulty);
        ASSERT_EQ_STR(want.players[i].name, got->players[i].name);
    }
    Save_ReadClose(sg);
}

TEST(every_world_scalar_survives) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    GameWorld want = *g_world;
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    /* The world is torn down and comes back empty, the way a load
     * meets it. */
    struct MapFeature *keep = g_world->features;
    int keep_count = g_world->feature_count;
    memset(g_world, 0, sizeof(*g_world));
    g_world->features = keep;
    g_world->feature_count = keep_count;
    g_world->loaded = 1;

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    ASSERT_EQ_INT(0, Save_Apply(sg, err, sizeof(err)));

    ASSERT_EQ_STR(want.map_name, g_world->map_name);
    ASSERT_EQ_STR(want.map_kingdom, g_world->map_kingdom);
    ASSERT_EQ_INT(want.water_height, g_world->water_height);
    ASSERT_EQ_INT(want.skirmish_elapsed_ticks, g_world->skirmish_elapsed_ticks);
    ASSERT_EQ_INT(want.skirmish_game_over, g_world->skirmish_game_over);
    ASSERT_EQ_INT(want.skirmish_winner_team, g_world->skirmish_winner_team);
    ASSERT_EQ_INT(want.skirmish_local_result, g_world->skirmish_local_result);
    ASSERT_EQ_STR(want.skirmish_end_reason, g_world->skirmish_end_reason);
    ASSERT_EQ_INT(want.skirmish_end_tick, g_world->skirmish_end_tick);
    ASSERT_EQ_INT(want.skirmish_stats_open, g_world->skirmish_stats_open);
    ASSERT_EQ_INT(want.mission_elapsed_ticks, g_world->mission_elapsed_ticks);
    ASSERT_EQ_INT(want.mission_elapsed_seconds,
                  g_world->mission_elapsed_seconds);
    ASSERT_EQ_INT(want.mission_objectives_satisfied,
                  g_world->mission_objectives_satisfied);
    ASSERT_EQ_INT(want.mission_victory, g_world->mission_victory);
    for (int p = 0; p <= TAK_MAX_PLAYERS; p++) {
        ASSERT_EQ_INT(want.stats[p].units_built, g_world->stats[p].units_built);
        ASSERT_EQ_INT(want.stats[p].kills, g_world->stats[p].kills);
        ASSERT_EQ_INT(want.stats[p].losses, g_world->stats[p].losses);
        ASSERT_EQ_INT(want.stats[p].score, g_world->stats[p].score);
        ASSERT_EQ_INT(want.stats[p].eliminated, g_world->stats[p].eliminated);
        ASSERT_EQ_INT(want.stats[p].last_alive_tick,
                      g_world->stats[p].last_alive_tick);
    }
    Save_ReadClose(sg);
}

/* The camera is local view state, so it is its own optional section
 * rather than part of the world the simulation hash covers. */
TEST(the_camera_comes_back_where_it_was) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    int32_t want_x = g_world->cam_x, want_y = g_world->cam_y;
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    g_world->cam_x = 0;
    g_world->cam_y = 0;
    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    const TAK_SaveInfo *info = Save_Info(sg);
    ASSERT_EQ_INT(1, info->has_camera);
    ASSERT_EQ_INT((int)want_x, (int)info->cam_x);
    ASSERT_EQ_INT((int)want_y, (int)info->cam_y);
    ASSERT_EQ_INT(0, Save_Apply(sg, err, sizeof(err)));
    ASSERT_EQ_INT((int)want_x, (int)g_world->cam_x);
    ASSERT_EQ_INT((int)want_y, (int)g_world->cam_y);
    Save_ReadClose(sg);
}

/* World_SeedRand cannot restore a live state: it xors and forces the
 * value odd, and a running generator is as often even. */
TEST(the_generator_comes_back_on_an_even_state) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    World_SetRandState(0x2444fc86u);          /* even on purpose */
    uint32_t want = World_RandState();
    ASSERT_EQ_INT(0, (int)(want & 1u));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    World_SeedRand(99);
    ASSERT(World_RandState() != want);
    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    ASSERT_EQ_INT(0, Save_Apply(sg, err, sizeof(err)));
    ASSERT(World_RandState() == want);
    Save_ReadClose(sg);

    /* And the next draw is the one the saved battle would have made. */
    uint32_t next = World_Rand(1000);
    World_SetRandState(want);
    ASSERT(World_Rand(1000) == next);
}

/* The state hash the container records is taken over the live world
 * before the write, which is what the unit sections compare against
 * once they land. */
TEST(the_header_records_the_state_hash_and_the_tick) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    uint32_t want_hash = TAK_SimHash();
    ASSERT(want_hash != 0);
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    const TAK_SaveInfo *info = Save_Info(sg);
    ASSERT(info->sim_state_hash == want_hash);
    ASSERT_EQ_INT(4321, (int)info->sim_tick);
    ASSERT_EQ_INT((int)TAK_SAVE_SCHEMA_VERSION, (int)info->schema_version);
    ASSERT(info->engine_build[0] != 0);
    Save_ReadClose(sg);
}

/* Every definition the battle reaches is written by name, including
 * one reached only through a production queue and one reached only
 * through a placed feature. A dead slot's stale definition is not. */
TEST(every_definition_the_battle_uses_is_named) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    TAK_SaveReader *r = Save_OpenFile(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(r);
    uint32_t count = 0;
    uint16_t stored = 0;
    const uint8_t *recs = (const uint8_t *)Save_Records(r, TAK_SECT_DEFS, NULL,
                                                        &count, &stored);
    ASSERT_NOT_NULL(recs);
    ASSERT_EQ_INT((int)TAK_DEFS_RECORD_BYTES, (int)stored);

    size_t strt_len = 0;
    const void *strt = Save_Section(r, TAK_SECT_STRT, NULL, &strt_len);
    ASSERT_NOT_NULL(strt);
    TAK_StringTable *t = StringTable_Parse(strt, strt_len);
    ASSERT_NOT_NULL(t);

    int saw_king = 0, saw_bowman = 0, saw_guard = 0, saw_necro = 0;
    int saw_henge = 0, saw_corpse = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = recs + (size_t)i * stored;
        const char *name = StringTable_Get(t, (int)tak_get_u16(p));
        ASSERT_NOT_NULL(name);
        if (strcmp(name, "ARAKING") == 0) saw_king = 1;
        if (strcmp(name, "ARABOWMAN") == 0) saw_bowman = 1;
        if (strcmp(name, "ARAGUARD") == 0) saw_guard = 1;
        if (strcmp(name, "TARNECRO") == 0) saw_necro = 1;
        if (strcmp(name, "AraHenge01") == 0) saw_henge = 1;
        if (strcmp(name, "AraKingCorpse") == 0) saw_corpse = 1;
    }
    StringTable_Free(t);
    Save_Close(r);

    ASSERT_EQ_INT(1, saw_king);      /* the unit in slot 0            */
    ASSERT_EQ_INT(1, saw_bowman);    /* the unit in slot 1            */
    ASSERT_EQ_INT(1, saw_necro);     /* only in a build queue         */
    ASSERT_EQ_INT(1, saw_henge);     /* a placed feature              */
    ASSERT_EQ_INT(1, saw_corpse);    /* a corpse on the ground        */
    ASSERT_EQ_INT(0, saw_guard);     /* only a dead slot's stale def  */
}

TEST(a_definition_that_changed_is_refused_by_name) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    /* The data set moved under the save. */
    g_defs[1].weapons[0].damage += 1;

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    err[0] = 0;
    ASSERT_EQ_INT(-1, Save_Apply(sg, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "ARABOWMAN"));
    Save_ReadClose(sg);
}

TEST(a_definition_that_vanished_is_refused_by_name) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    snprintf(g_defs[0].unitname, sizeof(g_defs[0].unitname), "%s", "ARAOTHER");

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    err[0] = 0;
    ASSERT_EQ_INT(-1, Save_Apply(sg, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "ARAKING"));
    Save_ReadClose(sg);
}

/* A feature definition is checked the same way a unit is, because a
 * corpse that rots on a different schedule changes the battle. */
TEST(a_feature_definition_that_changed_is_refused_by_name) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    g_featdefs[2].decompose_time += 60;

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    err[0] = 0;
    ASSERT_EQ_INT(-1, Save_Apply(sg, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AraKingCorpse"));
    Save_ReadClose(sg);
}

/* Art is not simulation. Re-skinning a unit must not refuse a save. */
TEST(a_definition_whose_art_changed_still_loads) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    snprintf(g_defs[0].objectname, sizeof(g_defs[0].objectname), "%s", "OTHER");
    snprintf(g_defs[0].display_name, sizeof(g_defs[0].display_name), "%s",
             "A New Name");

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    ASSERT_EQ_INT(0, Save_Apply(sg, err, sizeof(err)));
    Save_ReadClose(sg);
}

/* The widths are the format, not an accident of the compiler. */
TEST(the_sections_are_the_width_the_format_says) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    TAK_SaveReader *r = Save_OpenFile(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(r);
    size_t len = 0;
    ASSERT_NOT_NULL(Save_Section(r, TAK_SECT_CFGB, NULL, &len));
    ASSERT_EQ_INT((int)TAK_CFGB_BYTES, (int)len);
    ASSERT_NOT_NULL(Save_Section(r, TAK_SECT_WRLD, NULL, &len));
    ASSERT_EQ_INT((int)TAK_WRLD_BYTES, (int)len);
    ASSERT_NOT_NULL(Save_Section(r, TAK_SECT_CAMR, NULL, &len));
    ASSERT_EQ_INT((int)TAK_CAMR_BYTES, (int)len);
    Save_Close(r);
}

TEST(a_file_that_is_not_there_is_refused) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    TAK_SaveGame *sg = Save_Read("no_such_save.oksave", err, sizeof(err));
    ASSERT_NULL(sg);
    ASSERT(err[0] != 0);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    tak_mem_init();

    TEST_SUITE("Save sections");
    RUN(a_battle_with_no_world_is_refused);
    RUN(every_battle_config_field_survives);
    RUN(every_world_scalar_survives);
    RUN(the_camera_comes_back_where_it_was);
    RUN(the_generator_comes_back_on_an_even_state);
    RUN(the_header_records_the_state_hash_and_the_tick);
    RUN(every_definition_the_battle_uses_is_named);
    RUN(a_definition_that_changed_is_refused_by_name);
    RUN(a_definition_that_vanished_is_refused_by_name);
    RUN(a_feature_definition_that_changed_is_refused_by_name);
    RUN(a_definition_whose_art_changed_still_loads);
    RUN(the_sections_are_the_width_the_format_says);
    RUN(a_file_that_is_not_there_is_refused);

    teardown();
    remove(SCRATCH);
    TEST_REPORT();
}
