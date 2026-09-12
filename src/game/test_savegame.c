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
#include "tak_cob_vm.h"
#include "tak_bytes.h"
#include "tak_features.h"
#include "tak_hpi.h"
#include "tak_map_fingerprint.h"
#include "tak_memory.h"
#include "tak_savefile.h"
#include "tak_savegame.h"
#include "tak_sha256.h"
#include "tak_sim_hash.h"
#include "tak_sim_rand.h"
#include "tak_unit.h"
#include "tak_util.h"
#include "tak_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define SCRATCH "test_savegame_scratch.oksave"

#define FIX_UNITS    5
#define FIX_DEFS     4
#define FIX_FEATDEFS 3
#define FIX_FEAT     2
#define FIX_PROJ     3
#define FIX_PIECES   4
#define FIX_STATICS  3

/* ── the state savegame.c reads ───────────────────────────────────── */

static GameWorld  *g_world;
static Unit       *g_units;
static int         g_unit_count;
static uint32_t    g_next_stable = 1;
static Projectile *g_projectiles;
static int         g_projectile_count;
static UnitDef     g_defs[FIX_DEFS];
static int         g_def_count;
static FeatureDef  g_featdefs[FIX_FEATDEFS];
static int         g_featdef_count;
static CobScript   g_script;
static uint32_t    g_script_code[8];
static char       *g_script_piece_names[FIX_PIECES];
static char       *g_script_names[1];
static uint32_t    g_script_offsets[1];
static uint32_t    g_ai_rng;            /* stands in for src/game/ai.c */
static int32_t     g_ai_words[16];

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

/* ── the unit array, as the loader sees it ────────────────────────── */

/* The real ones live in src/render/units.c over its file statics. Here
 * the same contract is kept over the fixture's own array, so the
 * serialiser is exercised end to end with no window and no game data.
 * The engine test in src/game/test_savestate.c drives the real ones. */

static void free_engine(CobEngine *e) {
    if (!e) return;
    tak_free(e->pieces);
    tak_free(e->static_vars);
    tak_free(e);
}

static CobEngine *make_engine(int seed) {
    CobEngine *e = (CobEngine *)tak_calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->script = &g_script;
    e->piece_count = FIX_PIECES;
    e->pieces = (CobPiece *)tak_calloc(FIX_PIECES, sizeof(CobPiece));
    e->static_vars = (int32_t *)tak_calloc(FIX_STATICS, sizeof(int32_t));
    if (!e->pieces || !e->static_vars) { free_engine(e); return NULL; }
    if (seed < 0) return e;   /* a blank engine, the way a load binds one */
    for (int i = 0; i < FIX_PIECES; i++) {
        e->pieces[i].rot[1] = 1024 * (seed + i);
        e->pieces[i].pos[0] = 16 * (seed + i);
        e->pieces[i].rot_target[1] = COB_ROT_SPINNING;
        e->pieces[i].hidden = (uint8_t)((i + seed) & 1);
    }
    for (int i = 0; i < FIX_STATICS; i++) e->static_vars[i] = seed * 10 + i;
    /* One live thread mid execution and one that has already returned.
     * The dead one still answers a weapon's aim query. */
    e->threads[0].alive = 1;
    e->threads[0].pc = (uint32_t)(40 + seed);
    e->threads[0].sp = 3;
    e->threads[0].stack[0] = seed;
    e->threads[0].stack[1] = seed + 1;
    e->threads[0].stack[2] = seed + 2;
    e->threads[0].stack[7] = 0x5eed;     /* above sp, never written */
    e->threads[0].wait_kind = COB_WAIT_TURN;
    e->threads[0].wait_piece = 1;
    e->threads[0].wait_child = -1;
    e->threads[0].signal_mask = 0x30u;
    e->threads[3].alive = 0;
    e->threads[3].has_return_value = 1;
    e->threads[3].return_value = 1;
    e->active_thread_count = 1;
    return e;
}

static void drop_units(void) {
    for (int i = 0; i < g_unit_count; i++) free_engine(g_units[i].cob);
    tak_free(g_units);
    g_units = NULL;
    g_unit_count = 0;
}

uint32_t Units_NextStableId(void) { return g_next_stable; }

int Units_LoadBegin(int slot_count, uint32_t next_stable_id) {
    if (slot_count < 0) return -1;
    drop_units();
    if (slot_count > 0) {
        g_units = (Unit *)tak_calloc((size_t)slot_count, sizeof(Unit));
        if (!g_units) return -1;
    }
    g_unit_count = slot_count;
    g_next_stable = next_stable_id ? next_stable_id : 1;
    return 0;
}

Unit *Units_LoadSlot(int i) {
    if (i < 0 || i >= g_unit_count) return NULL;
    return &g_units[i];
}

int Units_LoadAttachScript(int i) {
    if (i < 0 || i >= g_unit_count) return -1;
    free_engine(g_units[i].cob);
    g_units[i].cob = make_engine(-1);
    return g_units[i].cob ? g_units[i].cob->piece_count : -1;
}

void Units_LoadSyncThreadCount(int i) {
    if (i < 0 || i >= g_unit_count || !g_units[i].cob) return;
    int live = 0;
    for (int t = 0; t < COB_THREADS_PER_UNIT; t++) {
        if (g_units[i].cob->threads[t].alive) live++;
    }
    g_units[i].cob->active_thread_count = (uint16_t)live;
}

Projectile *Units_LoadProjectiles(int count) {
    if (count < 0) return NULL;
    tak_free(g_projectiles);
    g_projectiles = NULL;
    g_projectile_count = count;
    if (count == 0) return NULL;
    g_projectiles = (Projectile *)tak_calloc((size_t)count, sizeof(Projectile));
    return g_projectiles;
}

void Units_LoadFinish(void) { /* no occupancy layer in this fixture */ }

/* ── the AI, as the loader sees it ────────────────────────────────── */

#define FIX_AI_BYTES (4u + sizeof(g_ai_words))

unsigned int TAK_AI_StateBytes(void) { return (unsigned int)FIX_AI_BYTES; }

void TAK_AI_SaveState(unsigned char *out) {
    tak_put_u32(out, g_ai_rng);
    for (size_t i = 0; i < sizeof(g_ai_words) / sizeof(g_ai_words[0]); i++) {
        tak_put_i32(out + 4 + i * 4, g_ai_words[i]);
    }
}

int TAK_AI_LoadState(const unsigned char *in, unsigned int len) {
    if (!in || len < FIX_AI_BYTES) return -1;
    g_ai_rng = tak_get_u32(in);
    for (size_t i = 0; i < sizeof(g_ai_words) / sizeof(g_ai_words[0]); i++) {
        g_ai_words[i] = tak_get_i32(in + 4 + i * 4);
    }
    return 0;
}

/* The real one folds in the AI's own statics. The fixture's stand-in
 * covers the same bytes, so the header's state hash is still a real
 * number over a real world. */
uint32_t TAK_SimHash_AI(uint32_t h) {
    h = TAK_HashU32(h, g_ai_rng);
    for (size_t i = 0; i < sizeof(g_ai_words) / sizeof(g_ai_words[0]); i++) {
        h = TAK_HashI32(h, g_ai_words[i]);
    }
    return h;
}

/* ── fixture ──────────────────────────────────────────────────────── */

static void teardown(void) {
    drop_units();
    tak_free(g_projectiles);
    g_projectiles = NULL;
    g_projectile_count = 0;
    if (g_world) {
        tak_free(g_world->features);
        tak_free(g_world->fog_layers[1]);
        tak_free(g_world->fog_layers[2]);
        free(g_world);
        g_world = NULL;
    }
    g_next_stable = 1;
}

static void fill_defs(void) {
    static const char *const names[FIX_DEFS] = {
        "ARAKING", "ARABOWMAN", "ARAGUARD", "TARNECRO"
    };
    static const char *const fnames[FIX_FEATDEFS] = {
        "AraHenge01", "AraLodestone", "AraKingCorpse"
    };
    /* A script the definition check can be shown to cover. */
    static char piece0[] = "base";
    static char piece1[] = "torso";
    static char piece2[] = "head";
    static char piece3[] = "arm";
    static char create[] = "Create";
    g_script_piece_names[0] = piece0;
    g_script_piece_names[1] = piece1;
    g_script_piece_names[2] = piece2;
    g_script_piece_names[3] = piece3;
    g_script_names[0] = create;
    g_script_offsets[0] = 0;
    for (int i = 0; i < 8; i++) g_script_code[i] = 0x10000000u + (uint32_t)i;
    memset(&g_script, 0, sizeof(g_script));
    g_script.version = 6;
    g_script.num_static_vars = FIX_STATICS;
    g_script.code = g_script_code;
    g_script.num_code_words = 8;
    g_script.script_names = g_script_names;
    g_script.script_offsets = g_script_offsets;
    g_script.num_scripts = 1;
    g_script.num_pieces = FIX_PIECES;
    g_script.piece_names = g_script_piece_names;

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
        g_defs[i].cob_script = &g_script;
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
    cfg->seed = 0xC0FFEEu;
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
    g_world->features = (struct MapFeature *)tak_calloc(
        FIX_FEAT, sizeof(*g_world->features));
    if (!g_world->features) return -1;
    for (int i = 0; i < FIX_FEAT; i++) {
        g_world->features[i].feat_id = (uint16_t)(10 + i);
        g_world->features[i].tile_x = (uint16_t)(4 + i);
        g_world->features[i].tile_z = (uint16_t)(5 + i);
        /* The first is authored scenery, the last a corpse. */
        g_world->features[i].global_idx = (i == 0) ? 0 : 2;
        g_world->features[i].decompose_ticks = (i == 0) ? -1 : 900;
        g_world->features[i].world_x = 640 + i * 32;
        g_world->features[i].world_y = 704 + i * 32;
        g_world->features[i].heading = (uint16_t)(i * 4096);
        g_world->features[i].pitch = (uint16_t)(i * 311);
        g_world->features[i].roll = (uint16_t)(65536 - i * 517);
        g_world->features[i].color_idx = (int16_t)(i == 0 ? -1 : 3);
        g_world->features[i].sink_ticks = (int16_t)(i * 7);
    }

    /* Fog is history: two seats, each with its own explored ground. */
    g_world->fog_cell_px = 32;
    g_world->fog_w = 12;
    g_world->fog_h = 9;
    size_t fog_cells = (size_t)g_world->fog_w * (size_t)g_world->fog_h;
    for (int p = 1; p <= 2; p++) {
        g_world->fog_layers[p] = (uint8_t *)tak_malloc(fog_cells);
        if (!g_world->fog_layers[p]) return -1;
        for (size_t c = 0; c < fog_cells; c++) {
            g_world->fog_layers[p][c] = (uint8_t)((c + (size_t)p) % 3u);
        }
    }
    g_world->fog_state = g_world->fog_layers[1];

    /* Every seat off its default, so a dropped field cannot pass by
     * matching a zero. */
    g_world->economy.active_count = 3;
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        PlayerEconomy *e = &g_world->economy.players[i];
        e->mana = 120.5f + (float)i;
        e->max_mana = 1000 + i;
        e->regen_per_sec = 3.25f + (float)i;
        e->spent_last_sec = i * 2;
        e->earned_last_sec = i * 3;
        e->earned_accum = 0.5f * (float)i;
        e->spent_accum = 0.25f * (float)i;
        e->ticks_since_window_reset = 17 + i;
    }

    g_ai_rng = 0xfeedu;
    for (size_t i = 0; i < sizeof(g_ai_words) / sizeof(g_ai_words[0]); i++) {
        g_ai_words[i] = (int32_t)(100 + i * 7);
    }

    g_units = (Unit *)tak_calloc(FIX_UNITS, sizeof(Unit));
    if (!g_units) return -1;
    g_unit_count = FIX_UNITS;
    g_next_stable = 100 + FIX_UNITS;
    for (int i = 0; i < FIX_UNITS; i++) {
        Unit *u = &g_units[i];
        u->stable_id = (uint32_t)(100 + i);
        u->alive = UNIT_ALIVE_ACTIVE;
        /* ARAGUARD appears on the dead slot only, so the definition
         * test can prove a tombstone's stale index is not followed. */
        static const uint16_t slot_def[FIX_UNITS] = { 0, 1, 2, 0, 0 };
        u->def_idx = slot_def[i];
        u->player_id = (uint8_t)(1 + (i % 2));
        u->team_color_idx = (uint8_t)(i % 12);
        u->world_x = 1000 + i * 37;
        u->world_y = 2000 + i * 11;
        u->heading = 0.5f * (float)i;
        u->pitch = 0.01f * (float)i;
        u->roll = -0.02f * (float)i;
        u->velocity = 3 + i;
        u->health = 400 - i;
        u->max_health = 400;
        u->cmd_kind = 1;
        u->cmd_x = 1500 + i;
        u->cmd_y = 2500 + i;
        u->patrol_x = 900 + i;
        u->patrol_y = 950 + i;
        u->target = (int16_t)((i + 1) % FIX_UNITS);
        u->attack_cooldown = (int16_t)(10 + i);
        u->aggro_mode = UNIT_AGGRO_DEFENSIVE;
        u->weapon_slot = (uint8_t)(i % 3);
        u->experience_pts = 40 * i;
        u->kills = (uint16_t)i;
        u->build_target = -1;
        u->reclaim_tile_x = -1;
        u->reclaim_tile_y = -1;
        u->reclaim_accum = 0.125f * (float)i;
        u->carried_by = -1;
        u->xfer_cargo = -1;
        u->mana = 25.0f + (float)i;
        u->mana_max = 100.0f;
        u->subpixel_x = 0.25f;
        u->subpixel_y = 0.75f;
        u->cur_speed_ppt = 1.5f;
        u->flight_alt = 12.5f * (float)i;
        u->occ_on = 1;
        u->occ_tx = (int16_t)(60 + i);
        u->occ_ty = (int16_t)(70 + i);
        u->occ_fx = 2;
        u->occ_fz = 2;
        u->still_ticks = (uint16_t)(5 * i);
        u->path_len = 3;
        u->path_index = 1;
        u->path_goal_x = 1600 + i;
        u->path_goal_y = 2600 + i;
        for (int k = 0; k < UNIT_PATH_MAX_WAYPOINTS; k++) {
            /* The tail is deliberately non zero: no save writes it. */
            u->path_x[k] = 7000 + k + i;
            u->path_y[k] = 8000 + k + i;
        }
        /* The stall ladder mid climb, rungs already taken included. */
        u->route_serial = (uint16_t)(3 + i);
        u->stall_px = 640 + i;
        u->stall_py = 660 + i;
        u->stall_route_left = 900 - i;
        u->stall_route_best = 880 - i;
        u->stall_route_mark = 890 - i;
        u->stall_line_best = 700 - i;
        u->stall_line_mark = 710 - i;
        u->stall_tail = 512 + i;
        u->stall_tail_serial = (uint16_t)(3 + i);
        u->stall_tail_index = (uint8_t)(1 + i);
        u->stall_order_x = 1200 + i;
        u->stall_order_y = 1300 + i;
        u->stall_ticks = (int16_t)(180 + i);
        u->stall_esc = (uint8_t)(i % 4);
        u->route_seg_x = 1100 + i;
        u->route_seg_y = 1150 + i;
        u->route_check_cd = (int16_t)(4 + i);
        u->route_flags = UNIT_ROUTE_BLOCKED;
        u->fog_x = 1000 + i * 37 - 9;
        u->fog_y = 2000 + i * 11 + 5;
        u->fog_sight = (int16_t)(320 + i);
        u->fog_lit = 1;
        u->anim_state = 2;
        u->walk_thread_slot = 0;
        u->killed_thread_slot = -1;
        u->build_thread_slot = -1;
        u->move_rate_tier = (int8_t)(i - 1);
        u->turn_dir_sign = -1;
        u->script_ev[UNIT_SCRIPT_EV_ACTIVATE] = (uint16_t)(1 + i);
        u->cob_activation = 1;
        u->cob_yard_open = (uint8_t)(i & 1);
        for (int wi = 0; wi < 3; wi++) {
            u->weapon_state[wi].cooldown_ticks = 12 + i + wi;
            u->weapon_state[wi].burst_ticks = 3 * wi;
            u->weapon_state[wi].burst_remaining = (int16_t)wi;
            u->weapon_state[wi].burst_target = -1;
            u->weapon_state[wi].aim_thread_slot = (int8_t)(wi == 0 ? 3 : -1);
            u->weapon_state[wi].aim_ticks = (int16_t)(wi * 5);
            u->weapon_state[wi].aim_target = (int16_t)(wi == 0 ? i : -1);
        }
        u->cob = make_engine(i + 1);
        if (!u->cob) return -1;
    }
    /* A factory with something queued, so a definition reached only
     * through a production queue is still named in the file. */
    g_units[0].prod_queue_len = 1;
    g_units[0].prod_queue[0] = 3;
    g_units[0].rally_set = 1;
    g_units[0].rally_x = 1888;
    g_units[0].rally_y = 1999;
    /* A transport with a passenger aboard and one still queued. */
    g_units[1].cargo_count = 1;
    g_units[1].cargo_size_used = 2;
    g_units[1].carry_next = 4;
    g_units[1].load_queue_len = 2;
    g_units[1].load_queue[0] = 3;
    g_units[1].load_queue[1] = 0;
    g_units[1].load_queue[5] = 77;       /* past the live length */
    g_units[1].xfer_ticks = 6;
    g_units[1].xfer_cargo = 3;
    g_units[1].unload_stage = 2;
    g_units[1].unload_gx = 1440;
    g_units[1].unload_gy = 1470;
    g_units[3].alive = UNIT_ALIVE_TRANSPORTED;
    g_units[3].carried_by = 1;
    g_units[3].carry_seq = 1;
    /* A builder part way through a nanoframe, and the frame itself. */
    g_units[0].cmd_kind = UNIT_CMD_BUILD;
    g_units[0].build_target = 4;
    g_units[4].under_construction = 1;
    g_units[4].health = 120;
    g_units[4].build_hp_accum = 0.75f;
    g_units[4].nano_idle_ticks = 44;
    /* A dead slot keeps a stale definition index nothing may follow. */
    g_units[2].alive = UNIT_ALIVE_DEAD;
    /* A caster part way through raising a corpse. */
    g_units[1].raise_mode = 1;
    g_units[1].raise_left = 32768;
    g_units[1].cmd_kind = UNIT_CMD_RESURRECT;

    g_projectiles = (Projectile *)tak_calloc(FIX_PROJ, sizeof(Projectile));
    if (!g_projectiles) return -1;
    g_projectile_count = FIX_PROJ;
    for (int i = 0; i < FIX_PROJ; i++) {
        Projectile *p = &g_projectiles[i];
        /* The middle slot is a dead one the pool will reuse. */
        if (i == 1) { p->alive = 0; continue; }
        p->alive = 1;
        p->world_x = 3000 + i * 13;
        p->world_y = 3100 + i * 17;
        p->sub_x = 0.5f;
        p->sub_y = 0.125f;
        p->dir_x = 0.6f;
        p->dir_y = -0.8f;
        p->speed_ppt = 4.5f;
        p->damage = 55 + i;
        p->area_of_effect = 32;
        p->edge_effectiveness = 0.4f;
        p->target = (int16_t)i;
        p->shooter = (int16_t)((i + 2) % FIX_UNITS);
        p->ttl_ticks = (int16_t)(120 - i);
        p->player_id = (uint8_t)(1 + (i % 2));
        p->visual_kind = UNIT_PROJECTILE_VIS_ARROW;
        p->friendly_fire = (uint8_t)(i & 1);
        p->dest_x = 3400;
        p->dest_y = 3500;
        p->is_beam = 0;
        p->src_x = 2900;
        p->src_y = 3000;
        p->height = 40.0f;
        p->vel_up_ppt = 1.75f;
        p->gravity_ppt2 = -0.05f;
        p->heading = 1.1f;
        p->pitch = 0.2f;
        p->roll = -0.3f;
        p->spin_pitch = 0.01f;
        p->spin_heading = 0.02f;
        p->spin_roll = 0.03f;
        p->src_height = 64;
        p->age_ticks = (uint16_t)(9 + i);
        p->color_idx = (uint8_t)(2 + i);
        snprintf(p->hit_sound_class, sizeof(p->hit_sound_class), "ARROWHIT");
        snprintf(p->hit_sound, sizeof(p->hit_sound), "thwack");
        snprintf(p->water_sound, sizeof(p->water_sound), "splash");
        p->damage_scale_count = 2;
        snprintf(p->damage_scales[0].category,
                 sizeof(p->damage_scales[0].category), "ARMOURED");
        p->damage_scales[0].scale = 0.5f;
        snprintf(p->damage_scales[1].category,
                 sizeof(p->damage_scales[1].category), "FLESH");
        p->damage_scales[1].scale = 1.75f;
    }

    World_SeedRand(0x4d2);
    return 0;
}

/* The world the way a load meets it: the map is up, so the feature
 * array and the fog layers exist at their map sizes, and everything
 * the battle put in them is gone. */
static void empty_the_battle(void) {
    drop_units();
    tak_free(g_projectiles);
    g_projectiles = NULL;
    g_projectile_count = 0;
    struct MapFeature *keep_features = g_world->features;
    int keep_cap = g_world->feature_cap;
    uint8_t *fog1 = g_world->fog_layers[1];
    uint8_t *fog2 = g_world->fog_layers[2];
    int fw = g_world->fog_w, fh = g_world->fog_h, fc = g_world->fog_cell_px;
    memset(g_world, 0, sizeof(*g_world));
    g_world->loaded = 1;
    g_world->features = keep_features;
    g_world->feature_cap = keep_cap;
    g_world->fog_layers[1] = fog1;
    g_world->fog_layers[2] = fog2;
    g_world->fog_state = fog1;
    g_world->fog_w = fw;
    g_world->fog_h = fh;
    g_world->fog_cell_px = fc;
    if (keep_features && keep_cap > 0) {
        memset(keep_features, 0, (size_t)keep_cap * sizeof(*keep_features));
    }
    size_t cells = (size_t)(fw > 0 ? fw : 0) * (size_t)(fh > 0 ? fh : 0);
    if (fog1 && cells) memset(fog1, 0, cells);
    if (fog2 && cells) memset(fog2, 0, cells);
    g_ai_rng = 0;
    memset(g_ai_words, 0, sizeof(g_ai_words));
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

/* A world part way through the loading screen carries a map name and
 * little else. A save taken there would be a file nothing could load,
 * so it is refused while the player can still be told why. */
TEST(a_battle_that_is_still_loading_is_refused) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    g_world->loaded = 0;
    ASSERT_EQ_INT(-1, Save_Write(SCRATCH, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "still loading"));
    g_world->loaded = 1;
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));
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
    ASSERT_EQ_INT((int)want.seed, (int)got->seed);
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

    empty_the_battle();

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

/* A save carries every script thread's program counter, which is a
 * word index into the definition's script. A changed script moves what
 * that index points at, so the definition check has to refuse it. */
TEST(a_definition_whose_script_changed_is_refused_by_name) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    /* One instruction moved under the save. */
    g_script_code[2] += 1;

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    err[0] = 0;
    ASSERT_EQ_INT(-1, Save_Apply(sg, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "ARAKING"));
    Save_ReadClose(sg);
    g_script_code[2] -= 1;
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

/* The proof that matters here: every field the simulation hash covers
 * goes into the file and comes back out of it. The engine level proof,
 * that a loaded battle then runs tick for tick with the one that was
 * saved, is src/game/test_savestate.c. */
TEST(the_whole_battle_survives_the_round_trip) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    uint32_t want = TAK_SimHash();
    ASSERT(want != 0);
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    /* The battle is torn down and the world comes back empty, the way
     * a load meets it: no units, no shots, no fog, no economy. */
    empty_the_battle();
    World_SeedRand(99);
    ASSERT(TAK_SimHash() != want);

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    ASSERT_EQ_INT(0, Save_Apply(sg, err, sizeof(err)));
    Save_ReadClose(sg);

    ASSERT(TAK_SimHash() == want);
    /* And the ids stay unique after the load rather than restarting. */
    ASSERT_EQ_INT(100 + FIX_UNITS, (int)Units_NextStableId());
}

/* Handles are array indices. Slot i goes back in slot i, tombstones
 * included, which is what keeps every reference pointing at the same
 * thing it did before the write. */
TEST(handles_still_point_at_the_same_units) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));
    empty_the_battle();

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    ASSERT_EQ_INT(0, Save_Apply(sg, err, sizeof(err)));
    Save_ReadClose(sg);

    ASSERT_EQ_INT(FIX_UNITS, g_unit_count);
    /* The dead slot is still a slot, still holds its identity, and
     * still says where it fell: that spot is where a shot still
     * chasing it is sent when the slot is reused. */
    ASSERT_EQ_INT(UNIT_ALIVE_DEAD, g_units[2].alive);
    ASSERT_EQ_INT(102, (int)g_units[2].stable_id);
    ASSERT_EQ_INT(1000 + 2 * 37, g_units[2].world_x);
    ASSERT_EQ_INT(2000 + 2 * 11, g_units[2].world_y);
    /* The builder still holds the frame it was feeding. */
    ASSERT_EQ_INT(4, g_units[0].build_target);
    ASSERT_EQ_INT(1, g_units[4].under_construction);
    /* The transport and its passenger still agree about each other. */
    ASSERT_EQ_INT(1, g_units[3].carried_by);
    ASSERT_EQ_INT(UNIT_ALIVE_TRANSPORTED, g_units[3].alive);
    ASSERT_EQ_INT(1, g_units[1].cargo_count);
    ASSERT_EQ_INT(2, (int)g_units[1].load_queue_len);
    ASSERT_EQ_INT(3, g_units[1].load_queue[0]);
    ASSERT_EQ_INT(3, g_units[1].xfer_cargo);
    /* The raise is still part way through. */
    ASSERT_EQ_INT(1, (int)g_units[1].raise_mode);
    ASSERT_EQ_INT(32768, g_units[1].raise_left);
    /* The shot still credits the unit that fired it. */
    ASSERT_EQ_INT(3, g_projectile_count);
    ASSERT_EQ_INT(1, (int)g_projectiles[0].alive);
    ASSERT_EQ_INT(0, (int)g_projectiles[1].alive);
    ASSERT_EQ_INT(2, g_projectiles[0].shooter);
    ASSERT_EQ_STR("ARMOURED", g_projectiles[0].damage_scales[0].category);
}

/* A production queue holds definition indices, and those do not
 * travel: a different installation orders its registry differently.
 * The file carries names, so the queue has to come back naming the
 * same unit rather than the same number. */
TEST(a_build_queue_survives_a_reordered_registry) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));
    empty_the_battle();

    /* The same data set, in a different order, the way a loose file
     * install or a mod presents it. */
    UnitDef moved = g_defs[3];
    g_defs[3] = g_defs[0];
    g_defs[0] = moved;

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    ASSERT_EQ_INT(0, Save_Apply(sg, err, sizeof(err)));
    Save_ReadClose(sg);

    ASSERT_EQ_STR("ARAKING", Units_GetDef(g_units[0].def_idx)->unitname);
    ASSERT_EQ_INT(1, (int)g_units[0].prod_queue_len);
    ASSERT_EQ_STR("TARNECRO",
                  Units_GetDef(g_units[0].prod_queue[0])->unitname);
    ASSERT_EQ_STR("ARABOWMAN", Units_GetDef(g_units[1].def_idx)->unitname);
}

/* A refusal before anything has been written leaves the world exactly
 * as the loading screen made it, so the caller can put the message up
 * and stay where it is. A refusal past that point leaves a world
 * holding part of a battle, and the loaded flag goes down so nothing
 * can tick it while the caller gets round to tearing it down. */
TEST(a_refusal_says_whether_the_world_is_still_usable) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    ASSERT_EQ_INT(0, setup(NULL));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    /* Refused at the definition check, before a single field is put
     * back: the world is untouched and still usable. */
    g_defs[1].weapons[0].damage += 1;
    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    ASSERT_EQ_INT(-1, Save_Apply(sg, err, sizeof(err)));
    ASSERT_EQ_INT(1, g_world->loaded);
    Save_ReadClose(sg);
    g_defs[1].weapons[0].damage -= 1;

    /* Refused after the units are back, because the map this world
     * came up on is a different size. */
    empty_the_battle();
    g_world->fog_w += 1;
    sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    err[0] = 0;
    ASSERT_EQ_INT(-1, Save_Apply(sg, err, sizeof(err)));
    ASSERT(err[0] != 0);
    ASSERT_EQ_INT(0, g_world->loaded);
    Save_ReadClose(sg);
    g_world->fog_w -= 1;
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
    ASSERT_NOT_NULL(Save_Section(r, TAK_SECT_ECON, NULL, &len));
    ASSERT_EQ_INT((int)TAK_ECON_BYTES, (int)len);
    uint32_t n = 0; uint16_t stored = 0;
    ASSERT_NOT_NULL(Save_Records(r, TAK_SECT_UNIT, NULL, &n, &stored));
    ASSERT_EQ_INT((int)TAK_UNIT_RECORD_BYTES, (int)stored);
    ASSERT_EQ_INT(FIX_UNITS, (int)n);
    ASSERT_NOT_NULL(Save_Records(r, TAK_SECT_PROJ, NULL, &n, &stored));
    ASSERT_EQ_INT((int)TAK_PROJ_RECORD_BYTES, (int)stored);
    ASSERT_NOT_NULL(Save_Records(r, TAK_SECT_FEAT, NULL, &n, &stored));
    ASSERT_EQ_INT((int)TAK_FEAT_RECORD_BYTES, (int)stored);
    Save_Close(r);
}

TEST(a_file_that_is_not_there_is_refused) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    TAK_SaveGame *sg = Save_Read("no_such_save.oksave", err, sizeof(err));
    ASSERT_NULL(sg);
    ASSERT(err[0] != 0);
}

/* ── the map fingerprint ──────────────────────────────────────────── */

#define FP_MAP "ground war"

/* The digest covers the whole file with its own 32 bytes read as zero,
 * so an edited header has to be resealed or the damage check answers
 * before the fingerprint ever gets a say. */
#define H_DIGEST      32
#define H_FINGERPRINT 164

static int open_game_vfs(void) {
    if (VFS_IsInitialized()) VFS_Shutdown();
    return VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR);
}

static uint8_t *read_scratch(size_t *out_len) {
    FILE *fp = fopen(SCRATCH, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *bytes = NULL;
    if (n > 0) {
        bytes = (uint8_t *)malloc((size_t)n);
        if (bytes && fread(bytes, 1, (size_t)n, fp) != (size_t)n) {
            free(bytes);
            bytes = NULL;
        }
    }
    fclose(fp);
    if (out_len) *out_len = bytes ? (size_t)n : 0;
    return bytes;
}

static int overwrite_scratch(const uint8_t *bytes, size_t len) {
    FILE *fp = fopen(SCRATCH, "wb");
    if (!fp) return -1;
    size_t n = fwrite(bytes, 1, len, fp);
    fclose(fp);
    return n == len ? 0 : -1;
}

static void reseal(uint8_t *bytes, size_t len) {
    static const uint8_t zeros[TAK_SHA256_BYTES] = { 0 };
    TAK_Sha256 ctx;
    TAK_Sha256_Init(&ctx);
    TAK_Sha256_Update(&ctx, bytes, H_DIGEST);
    TAK_Sha256_Update(&ctx, zeros, TAK_SHA256_BYTES);
    TAK_Sha256_Update(&ctx, bytes + H_DIGEST + TAK_SHA256_BYTES,
                      len - H_DIGEST - TAK_SHA256_BYTES);
    TAK_Sha256_Final(&ctx, bytes + H_DIGEST);
}

TEST(a_save_records_the_fingerprint_of_the_map_it_was_played_on) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    if (open_game_vfs() != 0) { printf("SKIP (no game data) "); return; }
    uint8_t want[TAK_MAP_FINGERPRINT_BYTES];
    if (TAK_MapFingerprint_FromName(FP_MAP, want) != 0) {
        VFS_Shutdown();
        printf("SKIP (no game data) ");
        return;
    }

    ASSERT_EQ_INT(0, setup(FP_MAP));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    const TAK_SaveInfo *info = Save_Info(sg);
    ASSERT_EQ_STR(FP_MAP, info->map_name);
    ASSERT_EQ_INT(0, memcmp(info->map_fingerprint, want, sizeof(want)));
    Save_ReadClose(sg);
    VFS_Shutdown();
}

/* Two installs can serve different terrain under one name, and a save
 * restores exact positions, so loading onto the wrong ground puts units
 * inside hills. */
TEST(a_save_whose_map_has_changed_is_refused_by_name) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    if (open_game_vfs() != 0) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, setup(FP_MAP));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    size_t len = 0;
    uint8_t *bytes = read_scratch(&len);
    ASSERT_NOT_NULL(bytes);
    /* The map on this system is no longer the one that was played. */
    bytes[H_FINGERPRINT + 5] ^= 0xffu;
    reseal(bytes, len);
    int wrote = overwrite_scratch(bytes, len);
    free(bytes);
    ASSERT_EQ_INT(0, wrote);

    err[0] = 0;
    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    VFS_Shutdown();
    ASSERT_NULL(sg);
    ASSERT_NOT_NULL(strstr(err, FP_MAP));
}

TEST(a_save_whose_map_is_missing_is_refused_by_name) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    if (open_game_vfs() != 0) { printf("SKIP (no game data) "); return; }
    ASSERT_EQ_INT(0, setup(FP_MAP));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));
    /* The map this save names is not installed any more. */
    VFS_Shutdown();

    err[0] = 0;
    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NULL(sg);
    ASSERT_NOT_NULL(strstr(err, FP_MAP));
}

/* A save from a build that could not resolve its map carries zeros,
 * which is unknown rather than a mismatch. */
TEST(a_save_that_carries_no_fingerprint_still_loads) {
    char err[TAK_SAVE_ERR_MAX] = { 0 };
    if (VFS_IsInitialized()) VFS_Shutdown();
    ASSERT_EQ_INT(0, setup(FP_MAP));
    ASSERT_EQ_INT(0, write_scratch(err, sizeof(err)));

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, sizeof(err));
    ASSERT_NOT_NULL(sg);
    const TAK_SaveInfo *info = Save_Info(sg);
    for (int i = 0; i < TAK_MAP_FINGERPRINT_BYTES; i++) {
        ASSERT_EQ_INT(0, info->map_fingerprint[i]);
    }
    Save_ReadClose(sg);
}

int main(int argc, char **argv) {
    /* --no-data runs only the cases that need no install, for CI. */
    int no_data = argc > 1 && strcmp(argv[1], "--no-data") == 0;
    tak_mem_init();

    TEST_SUITE("Save sections");
    RUN(a_battle_with_no_world_is_refused);
    RUN(a_battle_that_is_still_loading_is_refused);
    RUN(every_battle_config_field_survives);
    RUN(every_world_scalar_survives);
    RUN(the_camera_comes_back_where_it_was);
    RUN(the_generator_comes_back_on_an_even_state);
    RUN(the_header_records_the_state_hash_and_the_tick);
    RUN(every_definition_the_battle_uses_is_named);
    RUN(a_definition_that_changed_is_refused_by_name);
    RUN(a_definition_that_vanished_is_refused_by_name);
    RUN(a_feature_definition_that_changed_is_refused_by_name);
    RUN(a_definition_whose_script_changed_is_refused_by_name);
    RUN(a_definition_whose_art_changed_still_loads);
    RUN(the_whole_battle_survives_the_round_trip);
    RUN(handles_still_point_at_the_same_units);
    RUN(a_build_queue_survives_a_reordered_registry);
    RUN(a_refusal_says_whether_the_world_is_still_usable);
    RUN(the_sections_are_the_width_the_format_says);
    RUN(a_file_that_is_not_there_is_refused);
    RUN(a_save_that_carries_no_fingerprint_still_loads);

    if (!no_data) {
        TEST_SUITE("Map fingerprint");
        RUN(a_save_records_the_fingerprint_of_the_map_it_was_played_on);
        RUN(a_save_whose_map_has_changed_is_refused_by_name);
        RUN(a_save_whose_map_is_missing_is_refused_by_name);
    }

    teardown();
    remove(SCRATCH);
    TEST_REPORT();
}
