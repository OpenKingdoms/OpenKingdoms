/* One number for the whole simulation.
 *
 * The simulation is file scope statics inside units.c and ai.c, so
 * this test stands in for them: it owns the world, the unit array and
 * the projectile array, and sim_hash.c reads them through the same
 * public accessors the engine uses. That lets every case poke one
 * field and watch the number move, which is the property the save
 * round trip and the lockstep desync report both rest on.
 *
 * Data free, so CI runs it. */

#include "tak_cob.h"
#include "tak_cob_vm.h"
#include "tak_economy.h"
#include "tak_sim_hash.h"
#include "tak_sim_rand.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(x) do { \
    if (!(x)) { \
        fprintf(stderr, "ASSERT failed at %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

/* ── the state sim_hash.c reads ───────────────────────────────────── */

#define FIX_UNITS  4
#define FIX_PROJ   3
#define FIX_FEAT   3
#define FIX_FOG_W  8
#define FIX_FOG_H  8
#define FIX_PIECES 3
#define FIX_STATICS 4

static GameWorld  *g_world;
static Unit       *g_units;
static int         g_unit_count;
static Projectile *g_projectiles;
static int         g_projectile_count;
static uint32_t    g_ai_state;        /* stands in for src/game/ai.c */

GameWorld *World_Get(void) { return g_world; }

const Unit *Units_GetActive(int *out_count) {
    if (out_count) *out_count = g_unit_count;
    return g_units;
}

const Projectile *Units_GetProjectiles(int *out_count) {
    if (out_count) *out_count = g_projectile_count;
    return g_projectiles;
}

/* The real one lives in src/game/ai.c over its own file statics. Here
 * it is one value, so a case can prove the AI reaches the composite. */
uint32_t TAK_SimHash_AI(uint32_t h) { return TAK_HashU32(h, g_ai_state); }

/* ── fixture ──────────────────────────────────────────────────────── */

static CobScript g_script;

static CobEngine *make_engine(int seed) {
    CobEngine *e = (CobEngine *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->script = &g_script;
    e->piece_count = FIX_PIECES;
    e->pieces = (CobPiece *)calloc(FIX_PIECES, sizeof(CobPiece));
    e->static_vars = (int32_t *)calloc(FIX_STATICS, sizeof(int32_t));
    if (!e->pieces || !e->static_vars) return e;
    for (int i = 0; i < FIX_PIECES; i++) {
        e->pieces[i].rot[1] = 1024 * (seed + i);
        e->pieces[i].pos[0] = 16 * (seed + i);
        e->pieces[i].rot_target[1] = COB_ROT_SPINNING;
        e->pieces[i].hidden = (uint8_t)((i + seed) & 1);
    }
    for (int i = 0; i < FIX_STATICS; i++) e->static_vars[i] = seed * 10 + i;
    /* One live thread and one that has already returned. The dead one
     * still answers a weapon's aim query, so its return value is part
     * of the state. */
    e->threads[0].alive = 1;
    e->threads[0].pc = (uint32_t)(40 + seed);
    e->threads[0].sp = 3;
    e->threads[0].stack[0] = seed;
    e->threads[0].stack[1] = seed + 1;
    e->threads[0].stack[2] = seed + 2;
    e->threads[0].stack[7] = 0x5eed;     /* above sp, must not count */
    e->threads[0].wait_kind = COB_WAIT_TURN;
    e->threads[0].wait_piece = 1;
    e->threads[0].wait_child = -1;
    e->threads[3].alive = 0;
    e->threads[3].has_return_value = 1;
    e->threads[3].return_value = 1;
    e->active_thread_count = 1;
    return e;
}

static void free_engine(CobEngine *e) {
    if (!e) return;
    free(e->pieces);
    free(e->static_vars);
    free(e);
}

static void fill_unit(Unit *u, int i) {
    memset(u, 0, sizeof(*u));
    u->stable_id = (uint32_t)(100 + i);
    u->alive = UNIT_ALIVE_ACTIVE;
    u->world_x = 1000 + i * 37;
    u->world_y = 2000 + i * 11;
    u->heading = 0.5f * (float)i;
    u->health = 400 - i;
    u->max_health = 400;
    u->def_idx = (uint16_t)(3 + i);
    u->player_id = (uint8_t)(1 + (i % 2));
    u->target = (int16_t)((i + 1) % FIX_UNITS);
    u->build_target = -1;
    u->carried_by = -1;
    u->cmd_kind = 1;
    u->mana = 25.0f + (float)i;
    u->mana_max = 100.0f;
    u->subpixel_x = 0.25f;
    u->subpixel_y = 0.75f;
    u->cur_speed_ppt = 1.5f;
    u->path_len = 3;
    u->path_index = 1;
    for (int k = 0; k < UNIT_PATH_MAX_WAYPOINTS; k++) {
        /* The tail is deliberately non zero: no save writes it and the
         * hash must not read it. */
        u->path_x[k] = 7000 + k;
        u->path_y[k] = 8000 + k;
    }
    u->anim_state = 2;
    u->walk_thread_slot = 0;
    u->killed_thread_slot = -1;
    u->build_thread_slot = -1;
    u->script_ev[UNIT_SCRIPT_EV_ACTIVATE] = 1;
    u->weapon_state[0].cooldown_ticks = 12 + i;
    u->weapon_state[0].aim_thread_slot = 3;
    u->weapon_state[0].aim_target = (int16_t)i;
    u->weapon_state[1].aim_thread_slot = -1;
    u->weapon_state[2].aim_thread_slot = -1;
    u->prod_queue_len = 2;
    u->prod_queue[0] = 5;
    u->prod_queue[1] = 6;
    u->prod_queue[4] = 99;               /* past the live length */
    u->load_queue_len = 2;
    u->load_queue[0] = 1;
    u->load_queue[1] = 2;
    u->load_queue[5] = 77;               /* past the live length */
    u->xfer_cargo = -1;
    u->carry_seq = 1;
    u->carry_next = 3;
    u->cob = make_engine(i + 1);
}

static void teardown(void);

static int setup(void) {
    teardown();
    g_script.num_static_vars = FIX_STATICS;

    g_world = (GameWorld *)calloc(1, sizeof(GameWorld));
    if (!g_world) return -1;
    g_world->loaded = 1;
    g_world->skirmish_elapsed_ticks = 1234;
    g_world->water_height = 64;
    g_world->cam_x = 512;
    g_world->cam_y = 640;
    g_world->occ_version = 9;
    snprintf(g_world->skirmish_end_reason, sizeof(g_world->skirmish_end_reason),
             "%s", "undecided");
    for (int p = 0; p <= TAK_MAX_PLAYERS; p++) {
        g_world->stats[p].units_built = p * 3;
        g_world->stats[p].kills = p;
    }

    g_world->economy.active_count = 2;
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        g_world->economy.players[i].mana = 100.0f + (float)i;
        g_world->economy.players[i].max_mana = 1000 + i;
        g_world->economy.players[i].regen_per_sec = 20.0f;
        g_world->economy.players[i].earned_accum = 0.25f;
        g_world->economy.players[i].ticks_since_window_reset = i;
    }

    g_world->feature_count = FIX_FEAT;
    g_world->feature_cap = FIX_FEAT;
    g_world->features = (struct MapFeature *)calloc(FIX_FEAT, sizeof(*g_world->features));
    if (!g_world->features) return -1;
    for (int i = 0; i < FIX_FEAT; i++) {
        g_world->features[i].feat_id = (uint16_t)(10 + i);
        g_world->features[i].tile_x = (uint16_t)(4 + i);
        g_world->features[i].tile_z = (uint16_t)(5 + i);
        g_world->features[i].global_idx = 2 + i;
        g_world->features[i].world_x = 300 + i;
        g_world->features[i].world_y = 400 + i;
        g_world->features[i].heading = (uint16_t)(1000 * i);
        g_world->features[i].color_idx = (int16_t)(i - 1);
        g_world->features[i].decompose_ticks = 1800 - i * 60;
        g_world->features[i].sink_ticks = 0;
    }

    g_world->fog_w = FIX_FOG_W;
    g_world->fog_h = FIX_FOG_H;
    g_world->fog_cell_px = 32;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (p > 2) break;                /* two seats have layers */
        g_world->fog_layers[p] = (uint8_t *)calloc(FIX_FOG_W * FIX_FOG_H, 1);
        if (!g_world->fog_layers[p]) return -1;
        for (int c = 0; c < FIX_FOG_W * FIX_FOG_H; c++) {
            g_world->fog_layers[p][c] = (uint8_t)((c + p) % 3);
        }
    }
    g_world->fog_state = g_world->fog_layers[1];

    g_units = (Unit *)calloc(FIX_UNITS, sizeof(Unit));
    if (!g_units) return -1;
    g_unit_count = FIX_UNITS;
    for (int i = 0; i < FIX_UNITS; i++) fill_unit(&g_units[i], i);

    g_projectiles = (Projectile *)calloc(FIX_PROJ, sizeof(Projectile));
    if (!g_projectiles) return -1;
    g_projectile_count = FIX_PROJ;
    for (int i = 0; i < FIX_PROJ; i++) {
        g_projectiles[i].alive = (uint8_t)(i != 1);
        g_projectiles[i].world_x = 50 + i;
        g_projectiles[i].world_y = 60 + i;
        g_projectiles[i].speed_ppt = 4.0f + (float)i;
        g_projectiles[i].damage = 30 + i;
        g_projectiles[i].target = (int16_t)i;
        g_projectiles[i].shooter = (int16_t)((i + 1) % FIX_UNITS);
        g_projectiles[i].ttl_ticks = (int16_t)(120 - i);
        g_projectiles[i].height = 12.5f;
        g_projectiles[i].art_idx = (int16_t)(7 + i);        /* derived */
        g_projectiles[i].explosion_idx = (int16_t)(9 + i);  /* derived */
        g_projectiles[i].damage_scale_count = 2;
        snprintf(g_projectiles[i].damage_scales[0].category,
                 sizeof(g_projectiles[i].damage_scales[0].category),
                 "ARMOURED");
        g_projectiles[i].damage_scales[0].scale = 0.5f;
        snprintf(g_projectiles[i].damage_scales[1].category,
                 sizeof(g_projectiles[i].damage_scales[1].category), "FLESH");
        g_projectiles[i].damage_scales[1].scale = 1.75f;
        snprintf(g_projectiles[i].hit_sound_class,
                 sizeof(g_projectiles[i].hit_sound_class), "ARROWHIT");
        snprintf(g_projectiles[i].hit_sound,
                 sizeof(g_projectiles[i].hit_sound), "thwack");
        snprintf(g_projectiles[i].water_sound,
                 sizeof(g_projectiles[i].water_sound), "splash");
    }

    g_ai_state = 0xa1a1a1a1u;
    return 0;
}

static void teardown(void) {
    if (g_units) {
        for (int i = 0; i < g_unit_count; i++) free_engine(g_units[i].cob);
        free(g_units);
        g_units = NULL;
    }
    g_unit_count = 0;
    free(g_projectiles);
    g_projectiles = NULL;
    g_projectile_count = 0;
    if (g_world) {
        free(g_world->features);
        for (int p = 0; p <= TAK_MAX_PLAYERS; p++) free(g_world->fog_layers[p]);
        free(g_world);
        g_world = NULL;
    }
}

/* ── cases ────────────────────────────────────────────────────────── */

static int test_no_world_is_zero(void) {
    teardown();
    ASSERT(TAK_SimHash() == 0);
    return 0;
}

static int test_identical_states_agree(void) {
    ASSERT(setup() == 0);
    uint32_t a = TAK_SimHash();
    uint32_t b = TAK_SimHash();
    ASSERT(a == b);
    ASSERT(a != 0);
    ASSERT(a != TAK_SIM_HASH_SEED);

    /* A second run built the same way lands on the same number. */
    ASSERT(setup() == 0);
    ASSERT(TAK_SimHash() == a);
    return 0;
}

/* Poke one field, expect the number to move, put it back, expect it to
 * come home. The restore half is what catches a hash that is really
 * just reading a counter somewhere. */
#define POKE(label, expr_set, expr_restore) do { \
    uint32_t before = TAK_SimHash(); \
    expr_set; \
    uint32_t after = TAK_SimHash(); \
    if (before == after) { \
        fprintf(stderr, "  %s does not reach the hash (%s:%d)\n", \
                (label), __FILE__, __LINE__); \
        return 1; \
    } \
    expr_restore; \
    if (TAK_SimHash() != before) { \
        fprintf(stderr, "  %s did not restore (%s:%d)\n", \
                (label), __FILE__, __LINE__); \
        return 1; \
    } \
} while (0)

#define POKE_IGNORED(label, expr_set, expr_restore) do { \
    uint32_t before = TAK_SimHash(); \
    expr_set; \
    if (TAK_SimHash() != before) { \
        fprintf(stderr, "  %s must not reach the hash (%s:%d)\n", \
                (label), __FILE__, __LINE__); \
        return 1; \
    } \
    expr_restore; \
} while (0)

static int test_every_subsystem_contributes(void) {
    ASSERT(setup() == 0);

    POKE("world elapsed ticks",
         g_world->skirmish_elapsed_ticks = 1235,
         g_world->skirmish_elapsed_ticks = 1234);
    POKE("player battle stats",
         g_world->stats[2].kills += 1,
         g_world->stats[2].kills -= 1);

    POKE("unit count",
         g_unit_count = FIX_UNITS - 1,
         g_unit_count = FIX_UNITS);
    POKE("unit integer field",
         g_units[1].world_x += 1,
         g_units[1].world_x -= 1);
    POKE("unit float field",
         g_units[1].subpixel_x = 0.26f,
         g_units[1].subpixel_x = 0.25f);
    POKE("unit caster mana",
         g_units[0].mana = 26.0f,
         g_units[0].mana = 25.0f);
    POKE("fog reveal anchor",
         g_units[0].fog_x = 1040, g_units[0].fog_x = 0);
    POKE("fog reveal anchor sight",
         g_units[0].fog_sight = 320, g_units[0].fog_sight = 0);
    POKE("fog reveal anchor latch",
         g_units[0].fog_lit = 1, g_units[0].fog_lit = 0);
    POKE("unit script event latch",
         g_units[0].script_ev[UNIT_SCRIPT_EV_ACTIVATE] = 2,
         g_units[0].script_ev[UNIT_SCRIPT_EV_ACTIVATE] = 1);
    POKE("unit occupancy retry flag",
         g_units[0].occ_pending = 1,
         g_units[0].occ_pending = 0);
    POKE("unit production queue",
         g_units[0].prod_queue[1] = 7,
         g_units[0].prod_queue[1] = 6);
    POKE("transport pickup queue",
         g_units[0].load_queue[1] = 3,
         g_units[0].load_queue[1] = 2);
    POKE("transport drop order counter",
         g_units[0].carry_next = 4,
         g_units[0].carry_next = 3);
    POKE("transport set down goal",
         g_units[0].unload_gx = 64,
         g_units[0].unload_gx = 0);

    POKE("live path waypoint",
         g_units[0].path_x[2] += 1,
         g_units[0].path_x[2] -= 1);
    POKE("path length",
         g_units[0].path_len = 2,
         g_units[0].path_len = 3);

    POKE("weapon cooldown",
         g_units[0].weapon_state[0].cooldown_ticks += 1,
         g_units[0].weapon_state[0].cooldown_ticks -= 1);
    POKE("weapon aim thread slot",
         g_units[0].weapon_state[2].aim_thread_slot = 5,
         g_units[0].weapon_state[2].aim_thread_slot = -1);

    POKE("COB piece rotation",
         g_units[0].cob->pieces[1].rot[1] += 1,
         g_units[0].cob->pieces[1].rot[1] -= 1);
    POKE("COB piece hidden flag",
         g_units[0].cob->pieces[0].hidden ^= 1,
         g_units[0].cob->pieces[0].hidden ^= 1);
    POKE("COB static variable",
         g_units[0].cob->static_vars[2] += 1,
         g_units[0].cob->static_vars[2] -= 1);
    POKE("COB live thread program counter",
         g_units[0].cob->threads[0].pc += 1,
         g_units[0].cob->threads[0].pc -= 1);
    POKE("COB live thread stack",
         g_units[0].cob->threads[0].stack[1] += 1,
         g_units[0].cob->threads[0].stack[1] -= 1);
    /* The one that makes a unit fire when its script said hold. */
    POKE("COB dead thread return value",
         g_units[0].cob->threads[3].return_value = 0,
         g_units[0].cob->threads[3].return_value = 1);
    POKE("COB dead thread has-return flag",
         g_units[0].cob->threads[3].has_return_value = 0,
         g_units[0].cob->threads[3].has_return_value = 1);

    POKE("projectile position",
         g_projectiles[0].world_x += 1,
         g_projectiles[0].world_x -= 1);
    POKE("projectile lifetime",
         g_projectiles[2].ttl_ticks -= 1,
         g_projectiles[2].ttl_ticks += 1);
    /* The shot's own copy of the firing weapon's multipliers, which
     * decides how much damage lands when it arrives. */
    POKE("projectile damage multiplier",
         g_projectiles[0].damage_scales[0].scale = 0.75f,
         g_projectiles[0].damage_scales[0].scale = 0.5f);
    POKE("projectile damage category",
         g_projectiles[0].damage_scales[0].category[0] = 'a',
         g_projectiles[0].damage_scales[0].category[0] = 'A');
    POKE("projectile hit sound",
         g_projectiles[0].hit_sound[0] = 'T',
         g_projectiles[0].hit_sound[0] = 't');

    POKE("feature decompose counter",
         g_world->features[1].decompose_ticks -= 1,
         g_world->features[1].decompose_ticks += 1);
    POKE("feature count",
         g_world->feature_count = FIX_FEAT - 1,
         g_world->feature_count = FIX_FEAT);

    POKE("fog cell",
         g_world->fog_layers[1][5] = 2,
         g_world->fog_layers[1][5] = (uint8_t)((5 + 1) % 3));
    /* Every seat's layer, not just the local player's. */
    POKE("second player fog cell",
         g_world->fog_layers[2][9] = 0,
         g_world->fog_layers[2][9] = 2);   /* the fixture's (9 + 2) % 3 */

    POKE("economy mana",
         g_world->economy.players[1].mana += 1.0f,
         g_world->economy.players[1].mana -= 1.0f);
    POKE("economy window accumulator",
         g_world->economy.players[0].earned_accum = 0.5f,
         g_world->economy.players[0].earned_accum = 0.25f);

    POKE("AI state",
         g_ai_state ^= 0xffu,
         g_ai_state ^= 0xffu);

    /* One draw moves the simulation generator, and reseeding puts it
     * back where it was. */
    World_SeedRand(12345u);
    POKE("simulation generator",
         (void)World_Rand(100u),
         World_SeedRand(12345u));
    return 0;
}

/* Derived and local state must stay out, or two lockstep peers looking
 * at different corners of the map report a desync. */
static int test_derived_and_local_state_stay_out(void) {
    ASSERT(setup() == 0);
    POKE_IGNORED("camera x", g_world->cam_x = 999, g_world->cam_x = 512);
    POKE_IGNORED("camera y", g_world->cam_y = 999, g_world->cam_y = 640);
    POKE_IGNORED("occupancy version",
                 g_world->occ_version = 77, g_world->occ_version = 9);
    POKE_IGNORED("projectile art cache index",
                 g_projectiles[0].art_idx = 42, g_projectiles[0].art_idx = 7);
    POKE_IGNORED("COB active thread count",
                 g_units[0].cob->active_thread_count = 9,
                 g_units[0].cob->active_thread_count = 1);
    /* This machine's own verdict, read off the seat it plays. The seat
     * that was beaten holds a defeat while the seats still fighting
     * hold nothing, so two peers are entitled to differ. What they
     * must agree on, game over and the winning team, is hashed. */
    POKE_IGNORED("local verdict",
                 g_world->skirmish_local_result = -1,
                 g_world->skirmish_local_result = 0);
    POKE_IGNORED("local end reason",
                 g_world->skirmish_end_reason[0] = 'U',
                 g_world->skirmish_end_reason[0] = 'u');
    return 0;
}

/* A dead slot keeps its slot and whatever its fields held when it
 * died. The save writes it as a tombstone of two fields, so the hash
 * reads exactly those two. */
static int test_dead_slot_is_a_tombstone(void) {
    ASSERT(setup() == 0);
    free_engine(g_units[2].cob);
    g_units[2].cob = NULL;
    g_units[2].alive = UNIT_ALIVE_DEAD;

    /* Where it fell is part of the tombstone. The slot will be reused,
     * and unit_forget_slot reads that spot to send a shot still
     * chasing the dead unit there rather than to the map corner. */
    POKE("dead unit position",
         g_units[2].world_x = 1, g_units[2].world_x = 1000 + 2 * 37);
    POKE("dead unit position y",
         g_units[2].world_y = 1, g_units[2].world_y = 2000 + 2 * 11);
    POKE_IGNORED("dead unit health",
                 g_units[2].health = 1, g_units[2].health = 0);
    POKE("dead unit stable id",
         g_units[2].stable_id = 999, g_units[2].stable_id = 102);
    POKE("dead unit lifecycle byte",
         g_units[2].alive = UNIT_ALIVE_DYING, g_units[2].alive = UNIT_ALIVE_DEAD);
    return 0;
}

/* Only the live prefix of a path is state. The tail is whatever the
 * last longer path left there and no save writes it. */
static int test_path_tail_is_ignored(void) {
    ASSERT(setup() == 0);
    ASSERT(g_units[0].path_len == 3);
    POKE("last live waypoint",
         g_units[0].path_y[2] += 1, g_units[0].path_y[2] -= 1);
    POKE_IGNORED("first dead waypoint",
                 g_units[0].path_x[3] = 0, g_units[0].path_x[3] = 7003);
    POKE_IGNORED("last dead waypoint",
                 g_units[0].path_y[UNIT_PATH_MAX_WAYPOINTS - 1] = 0,
                 g_units[0].path_y[UNIT_PATH_MAX_WAYPOINTS - 1] =
                     8000 + UNIT_PATH_MAX_WAYPOINTS - 1);
    POKE_IGNORED("production queue past the live length",
                 g_units[0].prod_queue[4] = 0, g_units[0].prod_queue[4] = 99);
    POKE_IGNORED("pickup queue past the live length",
                 g_units[0].load_queue[5] = 0, g_units[0].load_queue[5] = 77);
    /* A COB local is a stack slot written by index, and a finished
     * thread's slots are read with no bound: Killed hands back the
     * corpse it asked for that way. There is no dead tail here. */
    POKE("COB stack above the stack pointer",
         g_units[0].cob->threads[0].stack[7] = 0,
         g_units[0].cob->threads[0].stack[7] = 0x5eed);
    POKE("COB stack of a thread that has ended",
         g_units[0].cob->threads[3].stack[1] = 9,
         g_units[0].cob->threads[3].stack[1] = 0);
    return 0;
}

/* A unit with no script is a different state from one whose engine
 * happens to be all zero. */
static int test_missing_engine_is_a_state(void) {
    ASSERT(setup() == 0);
    uint32_t with_engine = TAK_SimHash();
    CobEngine *saved = g_units[1].cob;
    g_units[1].cob = NULL;
    uint32_t without = TAK_SimHash();
    ASSERT(with_engine != without);

    CobEngine *blank = (CobEngine *)calloc(1, sizeof(CobEngine));
    ASSERT(blank != NULL);
    blank->script = &g_script;
    blank->pieces = (CobPiece *)calloc(FIX_PIECES, sizeof(CobPiece));
    blank->static_vars = (int32_t *)calloc(FIX_STATICS, sizeof(int32_t));
    blank->piece_count = FIX_PIECES;
    g_units[1].cob = blank;
    ASSERT(TAK_SimHash() != without);
    ASSERT(TAK_SimHash() != with_engine);
    free_engine(blank);
    g_units[1].cob = saved;
    ASSERT(TAK_SimHash() == with_engine);
    return 0;
}

/* A body keeps the angles its unit fell with and a raise gives all
 * three back, so a pitch and a roll are state like the heading beside
 * them. A save that dropped a tilt, or two peers that disagreed about
 * one, would otherwise land on the same number. */
static int test_a_tilt_reaches_the_hash(void) {
    ASSERT(setup() == 0);
    POKE("unit pitch", g_units[1].pitch = 0.2f, g_units[1].pitch = 0.0f);
    POKE("unit roll", g_units[1].roll = -0.15f, g_units[1].roll = 0.0f);
    POKE("body pitch",
         g_world->features[1].pitch = 2086,
         g_world->features[1].pitch = 0);
    POKE("body roll",
         g_world->features[1].roll = 63971,
         g_world->features[1].roll = 0);
    return 0;
}

/* The stall recovery ladder. Rungs already taken cannot be derived
 * from anything else the unit carries, so a save that dropped them
 * would put a wedged unit back at the bottom of the ladder and the
 * hash would still call the load clean. */
static int test_the_stall_ladder_reaches_the_hash(void) {
    ASSERT(setup() == 0);
    POKE("stall route serial",
         g_units[0].route_serial = 5, g_units[0].route_serial = 0);
    POKE("stall last position",
         g_units[0].stall_px = 64, g_units[0].stall_px = 0);
    POKE("stall last position y",
         g_units[0].stall_py = 64, g_units[0].stall_py = 0);
    POKE("stall route left",
         g_units[0].stall_route_left = 900, g_units[0].stall_route_left = 0);
    POKE("stall route best",
         g_units[0].stall_route_best = 800, g_units[0].stall_route_best = 0);
    POKE("stall route mark",
         g_units[0].stall_route_mark = 850, g_units[0].stall_route_mark = 0);
    POKE("stall line best",
         g_units[0].stall_line_best = 700, g_units[0].stall_line_best = 0);
    POKE("stall line mark",
         g_units[0].stall_line_mark = 750, g_units[0].stall_line_mark = 0);
    POKE("stall route tail sum",
         g_units[0].stall_tail = 640, g_units[0].stall_tail = 0);
    POKE("stall route tail serial",
         g_units[0].stall_tail_serial = 3, g_units[0].stall_tail_serial = 0);
    POKE("stall route tail index",
         g_units[0].stall_tail_index = 2, g_units[0].stall_tail_index = 0);
    POKE("stall order point",
         g_units[0].stall_order_x = 128, g_units[0].stall_order_x = 0);
    POKE("stall order point y",
         g_units[0].stall_order_y = 128, g_units[0].stall_order_y = 0);
    POKE("stall clock",
         g_units[0].stall_ticks = 240, g_units[0].stall_ticks = 0);
    /* The one that cannot be worked out again: rungs already taken. */
    POKE("stall escalation rung",
         g_units[0].stall_esc = 2, g_units[0].stall_esc = 0);
    return 0;
}

int main(void) {
    struct { const char *name; int (*fn)(void); } cases[] = {
        { "no_world_is_zero",             test_no_world_is_zero },
        { "identical_states_agree",       test_identical_states_agree },
        { "every_subsystem_contributes",  test_every_subsystem_contributes },
        { "derived_and_local_stay_out",   test_derived_and_local_state_stay_out },
        { "dead_slot_is_a_tombstone",     test_dead_slot_is_a_tombstone },
        { "path_tail_is_ignored",         test_path_tail_is_ignored },
        { "missing_engine_is_a_state",    test_missing_engine_is_a_state },
        { "a_tilt_reaches_the_hash",       test_a_tilt_reaches_the_hash },
        { "stall_ladder_reaches_the_hash", test_the_stall_ladder_reaches_the_hash },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int rc = cases[i].fn();
        printf("%-32s %s\n", cases[i].name, rc == 0 ? "ok" : "FAILED");
        failed += rc;
    }
    teardown();
    if (failed) {
        fprintf(stderr, "test_sim_hash: %d case(s) failed\n", failed);
        return 1;
    }
    printf("test_sim_hash: all cases passed\n");
    return 0;
}
