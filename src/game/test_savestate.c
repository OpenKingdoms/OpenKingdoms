/*
 * test_savestate.c -- a saved battle is the battle that was saved.
 *
 * The proof this file exists for is not that the file parsed. It runs
 * a real skirmish on a real map with the real engine, saves part way
 * through, keeps running and records the simulation hash on every
 * tick, then brings the battle back from the file into a fresh world
 * and runs the same ticks again. The two streams have to agree tick
 * for tick. A field the serialiser dropped shows up as the tick the
 * two answers part company, which is also the tick a lockstep peer
 * would report a desync on.
 *
 * The cases beside it are the states a save is most likely to catch
 * mid flight and get wrong: a builder feeding a nanoframe, a transport
 * with someone aboard, shots in the air, and a caster part way through
 * raising a corpse.
 *
 * This one needs the game data and it opens a window, so it carries
 * the needs-data label and runs under the shared test lock.
 */

#include "test_framework.h"

#include "tak_ai.h"
#include "tak_cob_vm.h"
#include "tak_battle_config.h"
#include "tak_economy.h"
#include "tak_features.h"
#include "tak_gameloop.h"
#include "tak_hpi.h"
#include "tak_ingame.h"
#include "tak_loading.h"
#include "tak_memory.h"
#include "tak_platform.h"
#include "tak_savefile.h"
#include "tak_sides.h"
#include "tak_util.h"
#include "tak_savegame.h"
#include "tak_sim_hash.h"
#include "tak_sim_rand.h"
#include "tak_ui.h"
#include "tak_unit.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define SCRATCH   "test_savestate_scratch.oksave"
#define MAP_NAME  "King of the Hill"
#define MAP_WORLD "aramon"

/* Ticks compared after the save. Long enough that a dropped field has
 * to show, short enough that the case stays under a minute. */
#define COMPARE_TICKS 600

static uint32_t g_want[COMPARE_TICKS];

/* ── platform and data ────────────────────────────────────────────── */

static int setup_platform(TAK_Platform *p) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SKIP (SDL init failed: %s) ", SDL_GetError());
        return -1;
    }
    memset(p, 0, sizeof(*p));
    p->window = SDL_CreateWindow("tak-savestate", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, 640, 480,
                                 SDL_WINDOW_HIDDEN);
    if (!p->window) { printf("SKIP (window failed) "); return -1; }
    p->renderer = SDL_CreateRenderer(p->window, -1, SDL_RENDERER_SOFTWARE);
    if (!p->renderer) { printf("SKIP (renderer failed) "); return -1; }
    p->canvas_w = 640; p->canvas_h = 480;
    p->window_w = 640; p->window_h = 480;
    p->scale = 1.0f;
    p->has_focus = 1;
    p->canvas_tex = SDL_CreateTexture(p->renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      p->canvas_w, p->canvas_h);
    if (!p->canvas_tex) { printf("SKIP (canvas texture failed) "); return -1; }
    return 0;
}

static void teardown_platform(TAK_Platform *p) {
    if (p->canvas_tex) SDL_DestroyTexture(p->canvas_tex);
    if (p->renderer) SDL_DestroyRenderer(p->renderer);
    if (p->window) SDL_DestroyWindow(p->window);
    SDL_Quit();
}

static int setup_vfs(void) {
    if (VFS_IsInitialized()) VFS_Shutdown();
    return VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR);
}

/* ── bringing a battle up ─────────────────────────────────────────── */

static void fill_cfg(BattleConfig *cfg) {
    static const int sides[4] = {
        TAK_SIDE_ARAMON, TAK_SIDE_TAROS, TAK_SIDE_VERUNA, TAK_SIDE_ZHON
    };
    BattleConfig_SetDefaults(cfg);
    strncpy(cfg->map_name, MAP_NAME, sizeof(cfg->map_name) - 1);
    cfg->monarch_expendable = 1;
    for (int p = 0; p < 4; p++) {
        cfg->players[p].kind = TAK_SLOT_AI;
        cfg->players[p].side = sides[p];
        cfg->players[p].team = p + 1;
        cfg->players[p].color = p;
        cfg->players[p].ai_difficulty = 1;
    }
}

/* Run the loading screen to the end. `restoring` tells its final phase
 * that the battle is coming out of a file, so it spawns nothing. */
static int run_loading(TAK_Platform *plat, const BattleConfig *cfg,
                       const char *map, const char *kingdom, int restoring) {
    World_SetRestoring(restoring);
    if (World_BeginLoad(plat, cfg, map, kingdom) != 0) return -1;
    if (Loading_Init(plat) != 0) return -1;
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 4000 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(plat, 1.0f / 60.0f);
    }
    World_SetRestoring(0);
    return next == GAMESTATE_IN_GAME ? 0 : -1;
}

static void end_battle(TAK_Platform *plat) {
    InGame_Shutdown();
    Loading_Shutdown();
    World_End(plat);
}

static int boot_battle(TAK_Platform *plat, BattleConfig *cfg,
                       GameWorld **out) {
    fill_cfg(cfg);
    if (run_loading(plat, cfg, MAP_NAME, MAP_WORLD, 0) != 0) return -1;
    *out = World_Get();
    if (!*out || (*out)->num_start_positions < 2) return -1;
    if (InGame_Init(plat) != 0) return -1;
    return 0;
}

/* ── armies that actually fight ───────────────────────────────────── */

static int start_of(const GameWorld *w, int player, int32_t *x, int32_t *y) {
    for (int i = 0; i < w->num_start_positions; i++) {
        if (w->start_positions[i].player != player) continue;
        *x = w->start_positions[i].x * 16;
        *y = w->start_positions[i].z * 16;
        return 0;
    }
    return -1;
}

/* A mobile attacker of `side` that is not the monarch: the first def
 * of that side with a weapon, a move class and a build cost. */
static int combat_def_for_side(int side) {
    const TakSideInfo *info = Sides_Get(side);
    if (!info) return -1;
    int n = Units_GetDefCount();
    for (int i = 0; i < n; i++) {
        const UnitDef *d = Units_GetDef(i);
        if (!d || d->commander || d->is_feature) continue;
        if (d->num_weapons <= 0 || d->bmcode == 0) continue;
        if (d->can_fly) continue;
        if (tak_stricmp(d->side, info->prefix) != 0) continue;
        if (d->max_velocity <= 0.0f) continue;
        return i;
    }
    return -1;
}

/* Two armies nose to nose in the middle of the map, so the battle is
 * joined within the warm up rather than after ten minutes of walking. */
static int spawn_brawl(const GameWorld *w, const BattleConfig *cfg) {
    int32_t ax, ay, bx, by;
    if (start_of(w, 1, &ax, &ay) != 0 || start_of(w, 2, &bx, &by) != 0) return -1;
    int32_t mx = (ax + bx) / 2, my = (ay + by) / 2;
    int spawned = 0;
    for (int p = 1; p <= 2; p++) {
        int def = combat_def_for_side(cfg->players[p - 1].side);
        if (def < 0) return -1;
        int32_t ox = (p == 1) ? -160 : 160;
        for (int i = 0; i < 6; i++) {
            int h = Units_Spawn(def, p, cfg->players[p - 1].color,
                                mx + ox, my + (i - 3) * 48);
            if (h < 0) continue;
            Units_DebugSetAggro(h, UNIT_AGGRO_OFFENSIVE);
            spawned++;
        }
    }
    return spawned;
}

/* ── where two battles part company ───────────────────────────────
 *
 * The hash says a save is wrong but not which field. This keeps the
 * state at the moment of the save and, when the restored battle does
 * not match, names the first thing that differs. */

typedef struct Snapshot {
    int          unit_count;
    Unit        *units;
    int32_t     *cob_pieces;     /* piece rotations, flattened */
    int          cob_piece_words;
    int          proj_count;
    Projectile  *projs;
    int          feature_count;
    struct MapFeature *features;
    int          fog_w, fog_h;
    uint8_t     *fog;
    EconomyState econ;
    uint32_t     rand_state;
    uint32_t     ai_hash;
    int          skirmish_ticks;
} Snapshot;

static Snapshot g_snap;

static void snap_free(void) {
    tak_free(g_snap.units);
    tak_free(g_snap.cob_pieces);
    tak_free(g_snap.projs);
    tak_free(g_snap.features);
    tak_free(g_snap.fog);
    memset(&g_snap, 0, sizeof(g_snap));
}

static void snap_take(void) {
    snap_free();
    GameWorld *w = World_Get();
    const Unit *u = Units_GetActive(&g_snap.unit_count);
    if (g_snap.unit_count > 0) {
        g_snap.units = (Unit *)tak_malloc(sizeof(Unit) * (size_t)g_snap.unit_count);
        memcpy(g_snap.units, u, sizeof(Unit) * (size_t)g_snap.unit_count);
        g_snap.cob_piece_words = g_snap.unit_count * 64 * 8;
        g_snap.cob_pieces = (int32_t *)tak_calloc((size_t)g_snap.cob_piece_words,
                                                  sizeof(int32_t));
        for (int i = 0; i < g_snap.unit_count; i++) {
            const CobEngine *e = u[i].cob;
            if (!e || !e->pieces) continue;
            for (int k = 0; k < e->piece_count && k < 64; k++) {
                int32_t *dst = g_snap.cob_pieces + ((size_t)i * 64 + (size_t)k) * 8;
                dst[0] = e->pieces[k].rot[0];
                dst[1] = e->pieces[k].rot[1];
                dst[2] = e->pieces[k].rot[2];
                dst[3] = e->pieces[k].pos[0];
                dst[4] = e->pieces[k].pos[1];
                dst[5] = e->pieces[k].pos[2];
                dst[6] = (int32_t)e->pieces[k].hidden;
                dst[7] = (int32_t)e->threads[k % COB_THREADS_PER_UNIT].pc;
            }
        }
    }
    const Projectile *pr = Units_GetProjectiles(&g_snap.proj_count);
    if (g_snap.proj_count > 0) {
        g_snap.projs = (Projectile *)tak_malloc(sizeof(Projectile) *
                                                (size_t)g_snap.proj_count);
        memcpy(g_snap.projs, pr, sizeof(Projectile) * (size_t)g_snap.proj_count);
    }
    if (w) {
        g_snap.feature_count = w->feature_count;
        if (w->feature_count > 0 && w->features) {
            size_t n = sizeof(*w->features) * (size_t)w->feature_count;
            g_snap.features = (struct MapFeature *)tak_malloc(n);
            memcpy(g_snap.features, w->features, n);
        }
        g_snap.fog_w = w->fog_w;
        g_snap.fog_h = w->fog_h;
        size_t cells = (size_t)w->fog_w * (size_t)w->fog_h;
        if (cells) {
            g_snap.fog = (uint8_t *)tak_calloc(cells, TAK_MAX_PLAYERS + 1);
            for (int q = 1; q <= TAK_MAX_PLAYERS; q++) {
                if (w->fog_layers[q]) {
                    memcpy(g_snap.fog + cells * (size_t)q, w->fog_layers[q], cells);
                }
            }
        }
        g_snap.econ = w->economy;
        g_snap.skirmish_ticks = w->skirmish_elapsed_ticks;
    }
    g_snap.rand_state = World_RandState();
    g_snap.ai_hash = TAK_AI_DebugStateHash();
}

static void snap_report_first_difference(void) {
    GameWorld *w = World_Get();
    int n = 0;
    const Unit *u = Units_GetActive(&n);
    if (n != g_snap.unit_count) {
        printf("[unit count %d, was %d] ", n, g_snap.unit_count);
        return;
    }
    size_t head = offsetof(Unit, cob);
    for (int i = 0; i < n; i++) {
        const uint8_t *a = (const uint8_t *)&g_snap.units[i];
        const uint8_t *b = (const uint8_t *)&u[i];
        for (size_t o = 0; o < head; o++) {
            if (a[o] != b[o]) {
                printf("[unit %d byte %u: %u, was %u] ", i, (unsigned)o,
                       (unsigned)b[o], (unsigned)a[o]);
                return;
            }
        }
        const CobEngine *e = u[i].cob;
        if (!e || !e->pieces) continue;
        for (int k = 0; k < e->piece_count && k < 64; k++) {
            const int32_t *want = g_snap.cob_pieces +
                                  ((size_t)i * 64 + (size_t)k) * 8;
            int32_t got[8] = {
                e->pieces[k].rot[0], e->pieces[k].rot[1], e->pieces[k].rot[2],
                e->pieces[k].pos[0], e->pieces[k].pos[1], e->pieces[k].pos[2],
                (int32_t)e->pieces[k].hidden,
                (int32_t)e->threads[k % COB_THREADS_PER_UNIT].pc
            };
            for (int f = 0; f < 8; f++) {
                if (got[f] != want[f]) {
                    printf("[unit %d piece %d field %d: %d, was %d] ",
                           i, k, f, got[f], want[f]);
                    return;
                }
            }
        }
    }
    int pn = 0;
    const Projectile *pr = Units_GetProjectiles(&pn);
    if (pn != g_snap.proj_count) {
        printf("[projectile slots %d, was %d] ", pn, g_snap.proj_count);
        return;
    }
    for (int i = 0; i < pn; i++) {
        if (pr[i].alive != g_snap.projs[i].alive) {
            printf("[projectile %d alive %d, was %d] ", i, pr[i].alive,
                   g_snap.projs[i].alive);
            return;
        }
    }
    if (!w) { printf("[no world] "); return; }
    if (w->feature_count != g_snap.feature_count) {
        printf("[feature count %d, was %d] ", w->feature_count,
               g_snap.feature_count);
        return;
    }
    for (int i = 0; i < w->feature_count; i++) {
        if (memcmp(&w->features[i], &g_snap.features[i],
                   sizeof(*w->features)) != 0) {
            printf("[feature %d differs] ", i);
            return;
        }
    }
    size_t cells = (size_t)w->fog_w * (size_t)w->fog_h;
    if (w->fog_w != g_snap.fog_w || w->fog_h != g_snap.fog_h) {
        printf("[fog size %dx%d, was %dx%d] ", w->fog_w, w->fog_h,
               g_snap.fog_w, g_snap.fog_h);
        return;
    }
    for (int q = 1; q <= TAK_MAX_PLAYERS && cells; q++) {
        if (!w->fog_layers[q]) continue;
        if (memcmp(w->fog_layers[q], g_snap.fog + cells * (size_t)q, cells) != 0) {
            printf("[fog layer %d differs] ", q);
            return;
        }
    }
    if (memcmp(&w->economy, &g_snap.econ, sizeof(w->economy)) != 0) {
        printf("[economy differs] ");
        return;
    }
    if (World_RandState() != g_snap.rand_state) {
        printf("[generator %u, was %u] ", World_RandState(), g_snap.rand_state);
        return;
    }
    if (TAK_AI_DebugStateHash() != g_snap.ai_hash) {
        printf("[the AI differs] ");
        return;
    }
    if (w->skirmish_elapsed_ticks != g_snap.skirmish_ticks) {
        printf("[clock %d, was %d] ", w->skirmish_elapsed_ticks,
               g_snap.skirmish_ticks);
        return;
    }
    printf("[nothing this check covers] ");
}

/* ── the comparison ───────────────────────────────────────────────── */

static int live_projectiles(void) {
    int n = 0;
    const Projectile *p = Units_GetProjectiles(&n);
    int live = 0;
    for (int i = 0; i < n && p; i++) if (p[i].alive) live++;
    return live;
}

static int units_with(int (*pred)(const Unit *)) {
    int n = 0;
    const Unit *u = Units_GetActive(&n);
    int hits = 0;
    for (int i = 0; i < n && u; i++) if (pred(&u[i])) hits++;
    return hits;
}

/* Save now, run `n` ticks recording the hash each one, then bring the
 * battle back out of the file into a fresh world and run the same n
 * ticks comparing tick for tick. Returns 0 when every tick agreed, or
 * the one based tick number that first disagreed. A negative return is
 * the harness failing rather than the save. */
static int save_then_replay(TAK_Platform *plat, int n, char *err, size_t cap) {
    if (n > COMPARE_TICKS) n = COMPARE_TICKS;
    uint32_t at_save = TAK_SimHash();
    snap_take();
    remove(SCRATCH);
    if (Save_Write(SCRATCH, err, cap) != 0) return -1;

    for (int i = 0; i < n; i++) {
        InGame_DebugRunSimTicks(1);
        g_want[i] = TAK_SimHash();
    }

    end_battle(plat);

    TAK_SaveGame *sg = Save_Read(SCRATCH, err, cap);
    if (!sg) return -2;
    const TAK_SaveInfo *info = Save_Info(sg);
    if (info->sim_state_hash != at_save) { Save_ReadClose(sg); return -3; }
    BattleConfig cfg = info->cfg;
    char map[96], kingdom[32];
    snprintf(map, sizeof(map), "%s", info->map_name);
    snprintf(kingdom, sizeof(kingdom), "%s", info->map_kingdom);
    if (run_loading(plat, &cfg, map, kingdom, 1) != 0) {
        Save_ReadClose(sg);
        return -4;
    }
    if (Save_Apply(sg, err, cap) != 0) { Save_ReadClose(sg); return -5; }
    Save_ReadClose(sg);
    if (InGame_Init(plat) != 0) return -6;

    /* The battle as restored, before a single tick has run. */
    if (TAK_SimHash() != at_save) {
        snap_report_first_difference();
        return -7;
    }

    for (int i = 0; i < n; i++) {
        InGame_DebugRunSimTicks(1);
        if (TAK_SimHash() != g_want[i]) return i + 1;
    }
    return 0;
}

static void report(int rc) {
    switch (rc) {
    case -1: printf("(the save could not be written) "); break;
    case -2: printf("(the save could not be read back) "); break;
    case -3: printf("(the header's state hash is not the live one) "); break;
    case -4: printf("(the world would not come back up) "); break;
    case -5: printf("(the save would not apply) "); break;
    case -6: printf("(the battle screen would not open) "); break;
    case -7: printf("(the restored battle is not the one that was saved) ");
        break;
    default:
        if (rc > 0) printf("(diverged on tick %d after the save) ", rc);
        break;
    }
}

/* ── the cases ────────────────────────────────────────────────────── */

static int is_under_construction(const Unit *u) {
    return u->alive == UNIT_ALIVE_ACTIVE && u->under_construction;
}

static int is_transported(const Unit *u) {
    return u->alive == UNIT_ALIVE_TRANSPORTED && u->carried_by >= 0;
}

static int is_raising(const Unit *u) {
    return u->alive == UNIT_ALIVE_ACTIVE && u->cmd_kind == UNIT_CMD_RESURRECT;
}

/* The case that matters. Two armies fighting, a save part way through,
 * and the reloaded battle has to play out the same way tick for tick.
 */
TEST(a_saved_skirmish_runs_on_exactly_as_it_would_have) {
    if (setup_vfs() != 0) { printf("SKIP (no game data) "); return; }
    TAK_Platform plat;
    if (setup_platform(&plat) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    GameWorld *w = NULL;
    ASSERT_EQ_INT(0, boot_battle(&plat, &cfg, &w));
    ASSERT(spawn_brawl(w, &cfg) > 0);

    /* Long enough that the armies have met, the AI has given orders
     * and arrows are in the air. */
    InGame_DebugRunSimTicks(900);
    ASSERT(live_projectiles() > 0);

    char err[TAK_SAVE_ERR_MAX] = { 0 };
    int rc = save_then_replay(&plat, COMPARE_TICKS, err, sizeof(err));
    if (rc != 0) { report(rc); printf("%s ", err); }
    ASSERT_EQ_INT(0, rc);

    end_battle(&plat);
    UI_Shutdown();
    teardown_platform(&plat);
    VFS_Shutdown();
}

/* A nanoframe is a real unit with a builder feeding it fractional HP.
 * The frame, the builder's handle on it and the mana the feeding has
 * already spent all have to come back. */
TEST(a_save_taken_mid_build_finishes_the_building) {
    if (setup_vfs() != 0) { printf("SKIP (no game data) "); return; }
    TAK_Platform plat;
    if (setup_platform(&plat) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    GameWorld *w = NULL;
    ASSERT_EQ_INT(0, boot_battle(&plat, &cfg, &w));

    /* The AI builds on its own once its monarch has mana. */
    InGame_DebugRunSimTicks(1800);
    if (units_with(is_under_construction) == 0) {
        printf("SKIP (nothing under construction to catch) ");
        end_battle(&plat);
        UI_Shutdown();
        teardown_platform(&plat);
        VFS_Shutdown();
        return;
    }

    char err[TAK_SAVE_ERR_MAX] = { 0 };
    int rc = save_then_replay(&plat, 300, err, sizeof(err));
    if (rc != 0) { report(rc); printf("%s ", err); }
    ASSERT_EQ_INT(0, rc);

    end_battle(&plat);
    UI_Shutdown();
    teardown_platform(&plat);
    VFS_Shutdown();
}

/* A carried unit still owns its slot and the coupling runs both ways:
 * the transport's cargo count and drop counter, the passenger's
 * carrier handle and the order it will be set down in. */
TEST(a_save_with_a_loaded_transport_keeps_its_passengers) {
    if (setup_vfs() != 0) { printf("SKIP (no game data) "); return; }
    TAK_Platform plat;
    if (setup_platform(&plat) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    GameWorld *w = NULL;
    ASSERT_EQ_INT(0, boot_battle(&plat, &cfg, &w));

    int transport_def = -1, rider_def = -1;
    int n = Units_GetDefCount();
    for (int i = 0; i < n; i++) {
        const UnitDef *d = Units_GetDef(i);
        if (!d || d->is_feature) continue;
        if (transport_def < 0 && d->transport_capacity > 0 &&
            d->transport_size_capacity > 0) {
            transport_def = i;
        }
        if (rider_def < 0 && d->bmcode != 0 && !d->cant_be_transported &&
            d->transported_size > 0 && d->max_velocity > 0.0f &&
            d->transport_capacity == 0) {
            rider_def = i;
        }
    }
    if (transport_def < 0 || rider_def < 0) {
        printf("SKIP (no transport in this data set) ");
        end_battle(&plat);
        UI_Shutdown();
        teardown_platform(&plat);
        VFS_Shutdown();
        return;
    }

    int32_t sx, sy;
    ASSERT_EQ_INT(0, start_of(w, 1, &sx, &sy));
    int carrier = Units_Spawn(transport_def, 1, 0, sx + 128, sy);
    int rider = Units_Spawn(rider_def, 1, 0, sx + 160, sy);
    ASSERT(carrier >= 0);
    ASSERT(rider >= 0);
    Units_SelectSingle(carrier);
    Units_CommandLoadSelected(rider, 0);

    /* Long enough for the pickup to complete. */
    for (int i = 0; i < 60 && units_with(is_transported) == 0; i++) {
        InGame_DebugRunSimTicks(30);
    }
    if (units_with(is_transported) == 0) {
        printf("SKIP (the pickup never completed) ");
        end_battle(&plat);
        UI_Shutdown();
        teardown_platform(&plat);
        VFS_Shutdown();
        return;
    }

    char err[TAK_SAVE_ERR_MAX] = { 0 };
    int rc = save_then_replay(&plat, 300, err, sizeof(err));
    if (rc != 0) { report(rc); printf("%s ", err); }
    ASSERT_EQ_INT(0, rc);
    /* And the passenger is still aboard on the other side. */
    ASSERT(units_with(is_transported) > 0);

    end_battle(&plat);
    UI_Shutdown();
    teardown_platform(&plat);
    VFS_Shutdown();
}

/* A raise is work owed in the original's frame units, counted down on
 * a body that keeps the spot, facing and tilt of the unit that fell,
 * and the body is rotting the whole time. */
TEST(a_save_taken_mid_raise_keeps_the_work_owed) {
    if (setup_vfs() != 0) { printf("SKIP (no game data) "); return; }
    TAK_Platform plat;
    if (setup_platform(&plat) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    GameWorld *w = NULL;
    ASSERT_EQ_INT(0, boot_battle(&plat, &cfg, &w));

    /* A raiser of any side, and something for it to raise. */
    int raiser_def = -1, victim_def = -1;
    int n = Units_GetDefCount();
    for (int i = 0; i < n; i++) {
        const UnitDef *d = Units_GetDef(i);
        if (!d || d->is_feature) continue;
        if (raiser_def < 0 && (d->cap_flags & UNIT_CAP_RESURRECT)) raiser_def = i;
        if (victim_def < 0 && d->corpse[0] && d->bmcode != 0) victim_def = i;
    }
    if (raiser_def < 0 || victim_def < 0) {
        printf("SKIP (no raiser in this data set) ");
        end_battle(&plat);
        UI_Shutdown();
        teardown_platform(&plat);
        VFS_Shutdown();
        return;
    }

    int32_t sx, sy;
    ASSERT_EQ_INT(0, start_of(w, 1, &sx, &sy));
    int victim = Units_Spawn(victim_def, 1, 0, sx + 128, sy + 128);
    int raiser = Units_Spawn(raiser_def, 1, 0, sx + 160, sy + 128);
    ASSERT(victim >= 0);
    ASSERT(raiser >= 0);
    int32_t body_x = 0, body_y = 0;
    {
        int uc = 0;
        const Unit *units = Units_GetActive(&uc);
        body_x = units[victim].world_x;
        body_y = units[victim].world_y;
    }
    Units_DebugKillHandle(victim);
    /* The death script runs and leaves the body behind. */
    InGame_DebugRunSimTicks(180);

    int handles[1] = { raiser };
    int ordered = Units_CommandResurrectFeatureFor(1, handles, 1, body_x, body_y);
    if (ordered <= 0) {
        printf("SKIP (nothing raisable where the body fell) ");
        end_battle(&plat);
        UI_Shutdown();
        teardown_platform(&plat);
        VFS_Shutdown();
        return;
    }
    /* Walk over and get part way through the work. */
    for (int i = 0; i < 40 && units_with(is_raising) > 0; i++) {
        InGame_DebugRunSimTicks(30);
        int uc = 0;
        const Unit *units = Units_GetActive(&uc);
        if (units[raiser].raise_left > 0) break;
    }
    {
        int uc = 0;
        const Unit *units = Units_GetActive(&uc);
        if (units[raiser].raise_left <= 0) {
            printf("SKIP (the raise never started) ");
            end_battle(&plat);
            UI_Shutdown();
            teardown_platform(&plat);
            VFS_Shutdown();
            return;
        }
    }

    char err[TAK_SAVE_ERR_MAX] = { 0 };
    int rc = save_then_replay(&plat, 300, err, sizeof(err));
    if (rc != 0) { report(rc); printf("%s ", err); }
    ASSERT_EQ_INT(0, rc);

    end_battle(&plat);
    UI_Shutdown();
    teardown_platform(&plat);
    VFS_Shutdown();
}

int main(void) {
    tak_mem_init();
    TEST_SUITE("A saved battle is the battle that was saved");
    RUN(a_saved_skirmish_runs_on_exactly_as_it_would_have);
    RUN(a_save_taken_mid_build_finishes_the_building);
    RUN(a_save_with_a_loaded_transport_keeps_its_passengers);
    RUN(a_save_taken_mid_raise_keeps_the_work_owed);
    remove(SCRATCH);
    TEST_REPORT();
}
