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
        SKIP_MARK("SDL init failed: %s", SDL_GetError());
        return -1;
    }
    memset(p, 0, sizeof(*p));
    p->window = SDL_CreateWindow("tak-savestate", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, 640, 480,
                                 SDL_WINDOW_HIDDEN);
    if (!p->window) { SKIP_MARK("window failed"); return -1; }
    p->renderer = SDL_CreateRenderer(p->window, -1, SDL_RENDERER_SOFTWARE);
    if (!p->renderer) { SKIP_MARK("renderer failed"); return -1; }
    p->canvas_w = 640; p->canvas_h = 480;
    p->window_w = 640; p->window_h = 480;
    p->scale = 1.0f;
    p->has_focus = 1;
    p->canvas_tex = SDL_CreateTexture(p->renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      p->canvas_w, p->canvas_h);
    if (!p->canvas_tex) { SKIP_MARK("canvas texture failed"); return -1; }
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
    if (World_BeginLoad(plat, cfg, map, kingdom) != 0) return -1;
    /* After BeginLoad, which puts the flag down on every battle so an
     * abandoned load cannot leak into the next one. */
    World_SetRestoring(restoring);
    if (Loading_Init(plat) != 0) return -1;
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 4000 && next == GAMESTATE_GAME_LOADING; i++) {
        next = Loading_Tick(plat, 1.0f / 60.0f);
    }
    /* The loading screen puts it down itself once it has acted on it. */
    if (World_IsRestoring()) return -1;
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

/* A mobile melee attacker of `side`: something that walks up and
 * swings, so the two lines meet inside the warm up. */
static int combat_def_for_side(int side) {
    static const char *const prefixes[4] = { "ARA", "TAR", "VER", "ZON" };
    if (side < 0 || side > 3) return -1;
    int n = Units_GetDefCount();
    for (int i = 0; i < n; i++) {
        const UnitDef *d = Units_GetDef(i);
        if (!d || strncmp(d->category, prefixes[side], 3) != 0) continue;
        if (!strstr(d->category, "MELEE")) continue;
        if (d->max_velocity <= 0.0f || d->num_weapons <= 0 || d->can_fly) continue;
        if (d->cap_flags & UNIT_CAP_BUILDER) continue;
        if (strstr(d->category, "Monarch")) continue;
        return i;
    }
    return -1;
}

/* An archer of `side`, so there are shots in the air when the save is
 * taken rather than only swords. */
static int ranged_def_for_side(int side) {
    static const char *const prefixes[4] = { "ARA", "TAR", "VER", "ZON" };
    if (side < 0 || side > 3) return -1;
    int n = Units_GetDefCount();
    for (int i = 0; i < n; i++) {
        const UnitDef *d = Units_GetDef(i);
        if (!d || strncmp(d->category, prefixes[side], 3) != 0) continue;
        if (strstr(d->category, "MELEE")) continue;
        if (d->max_velocity <= 0.0f || d->num_weapons <= 0 || d->can_fly) continue;
        if (d->cap_flags & UNIT_CAP_BUILDER) continue;
        if (strstr(d->category, "Monarch")) continue;
        if (d->weapons[0].range < 160) continue;
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
        int melee = combat_def_for_side(cfg->players[p - 1].side);
        int ranged = ranged_def_for_side(cfg->players[p - 1].side);
        if (melee < 0) return -1;
        int32_t ox = (p == 1) ? -192 : 192;
        for (int i = 0; i < 6; i++) {
            int def = (i < 3 || ranged < 0) ? melee : ranged;
            int32_t back = (def == melee) ? 0 : ox / 2;
            int h = Units_Spawn(def, p, cfg->players[p - 1].color,
                                mx + ox + back, my + (i - 3) * 48);
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
 * state at the moment of the save and at each of the first few ticks
 * after it, and names the first thing the restored battle disagrees
 * with. */

typedef struct Snapshot {
    int          unit_count;
    Unit        *units;
    int32_t     *cob_pieces;     /* piece and thread state, flattened */
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

#define SNAP_PIECES 64
#define SNAP_WORDS   8
#define SNAP_THREAD_WORDS 5
#define SNAP_TICKS  24

static Snapshot g_snap;
static Snapshot g_tick_snap[SNAP_TICKS];

static void snap_free_one(Snapshot *s) {
    tak_free(s->units);
    tak_free(s->cob_pieces);
    tak_free(s->projs);
    tak_free(s->features);
    tak_free(s->fog);
    memset(s, 0, sizeof(*s));
}

static void snap_free(void) {
    snap_free_one(&g_snap);
    for (int i = 0; i < SNAP_TICKS; i++) snap_free_one(&g_tick_snap[i]);
}

static void snap_take(Snapshot *s) {
    snap_free_one(s);
    GameWorld *w = World_Get();
    const Unit *u = Units_GetActive(&s->unit_count);
    if (s->unit_count > 0) {
        s->units = (Unit *)tak_malloc(sizeof(Unit) * (size_t)s->unit_count);
        memcpy(s->units, u, sizeof(Unit) * (size_t)s->unit_count);
        size_t per = SNAP_PIECES * SNAP_WORDS +
                     COB_THREADS_PER_UNIT * SNAP_THREAD_WORDS;
        size_t words = (size_t)s->unit_count * per;
        s->cob_pieces = (int32_t *)tak_calloc(words, sizeof(int32_t));
        for (int i = 0; i < s->unit_count; i++) {
            const CobEngine *e = u[i].cob;
            if (!e || !e->pieces) continue;
            int32_t *base = s->cob_pieces + (size_t)i * per;
            for (int k = 0; k < e->piece_count && k < SNAP_PIECES; k++) {
                int32_t *dst = base + (size_t)k * SNAP_WORDS;
                dst[0] = e->pieces[k].rot[0];
                dst[1] = e->pieces[k].rot[1];
                dst[2] = e->pieces[k].rot[2];
                dst[3] = e->pieces[k].pos[0];
                dst[4] = e->pieces[k].pos[1];
                dst[5] = e->pieces[k].pos[2];
                dst[6] = (int32_t)e->pieces[k].hidden;
                dst[7] = e->pieces[k].rot_speed[1];
            }
            int32_t *th = base + SNAP_PIECES * SNAP_WORDS;
            for (int t = 0; t < COB_THREADS_PER_UNIT; t++) {
                int32_t *dst = th + (size_t)t * SNAP_THREAD_WORDS;
                dst[0] = (int32_t)e->threads[t].pc;
                dst[1] = (int32_t)e->threads[t].sp;
                dst[2] = (int32_t)e->threads[t].alive;
                dst[3] = (int32_t)e->threads[t].sleep_remaining;
                dst[4] = e->threads[t].return_value;
            }
        }
    }
    const Projectile *pr = Units_GetProjectiles(&s->proj_count);
    if (s->proj_count > 0) {
        s->projs = (Projectile *)tak_malloc(sizeof(Projectile) *
                                            (size_t)s->proj_count);
        memcpy(s->projs, pr, sizeof(Projectile) * (size_t)s->proj_count);
    }
    if (w) {
        s->feature_count = w->feature_count;
        if (w->feature_count > 0 && w->features) {
            size_t n = sizeof(*w->features) * (size_t)w->feature_count;
            s->features = (struct MapFeature *)tak_malloc(n);
            memcpy(s->features, w->features, n);
        }
        s->fog_w = w->fog_w;
        s->fog_h = w->fog_h;
        size_t cells = (size_t)w->fog_w * (size_t)w->fog_h;
        if (cells) {
            s->fog = (uint8_t *)tak_calloc(cells, TAK_MAX_PLAYERS + 1);
            for (int q = 1; q <= TAK_MAX_PLAYERS; q++) {
                if (w->fog_layers[q]) {
                    memcpy(s->fog + cells * (size_t)q, w->fog_layers[q], cells);
                }
            }
        }
        s->econ = w->economy;
        s->skirmish_ticks = w->skirmish_elapsed_ticks;
    }
    s->rand_state = World_RandState();
    s->ai_hash = TAK_AI_DebugStateHash();
}

/* Byte offsets inside Unit, so a reported offset can be read as a
 * field without counting by hand. */
static const char *unit_field_at(size_t o) {
    struct { size_t off, size; const char *name; } map[] = {
        { offsetof(Unit, stable_id), 4, "stable_id" },
        { offsetof(Unit, world_x), 4, "world_x" },
        { offsetof(Unit, world_y), 4, "world_y" },
        { offsetof(Unit, heading), 4, "heading" },
        { offsetof(Unit, pitch), 4, "pitch" },
        { offsetof(Unit, roll), 4, "roll" },
        { offsetof(Unit, velocity), 4, "velocity" },
        { offsetof(Unit, health), 4, "health" },
        { offsetof(Unit, cmd_x), 4, "cmd_x" },
        { offsetof(Unit, cmd_y), 4, "cmd_y" },
        { offsetof(Unit, target), 2, "target" },
        { offsetof(Unit, cmd_kind), 2, "cmd_kind" },
        { offsetof(Unit, attack_cooldown), 2, "attack_cooldown" },
        { offsetof(Unit, alive), 1, "alive" },
        { offsetof(Unit, experience_pts), 4, "experience_pts" },
        { offsetof(Unit, build_target), 2, "build_target" },
        { offsetof(Unit, reclaim_accum), 4, "reclaim_accum" },
        { offsetof(Unit, carried_by), 2, "carried_by" },
        { offsetof(Unit, under_construction), 1, "under_construction" },
        { offsetof(Unit, cob_activation), 1, "cob_activation" },
        { offsetof(Unit, flight_alt), 4, "flight_alt" },
        { offsetof(Unit, mana), 4, "mana" },
        { offsetof(Unit, occ_on), 1, "occ_on" },
        { offsetof(Unit, occ_pending), 1, "occ_pending" },
        { offsetof(Unit, occ_tx), 2, "occ_tx" },
        { offsetof(Unit, build_hp_accum), 4, "build_hp_accum" },
        { offsetof(Unit, subpixel_x), 4, "subpixel_x" },
        { offsetof(Unit, subpixel_y), 4, "subpixel_y" },
        { offsetof(Unit, cur_speed_ppt), 4, "cur_speed_ppt" },
        { offsetof(Unit, path_goal_x), 4, "path_goal_x" },
        { offsetof(Unit, path_len), 1, "path_len" },
        { offsetof(Unit, path_index), 1, "path_index" },
        { offsetof(Unit, path_failed), 1, "path_failed" },
        { offsetof(Unit, path_pending), 1, "path_pending" },
        { offsetof(Unit, path_wait), 1, "path_wait" },
        { offsetof(Unit, blocked_ticks), 1, "blocked_ticks" },
        { offsetof(Unit, wp_stall), 2, "wp_stall" },
        { offsetof(Unit, wp_best_d2), 4, "wp_best_d2" },
        { offsetof(Unit, route_serial), 2, "route_serial" },
        { offsetof(Unit, stall_px), 4, "stall_px" },
        { offsetof(Unit, stall_route_left), 4, "stall_route_left" },
        { offsetof(Unit, stall_tail), 4, "stall_tail" },
        { offsetof(Unit, stall_ticks), 2, "stall_ticks" },
        { offsetof(Unit, stall_esc), 1, "stall_esc" },
        { offsetof(Unit, route_seg_x), 4, "route_seg_x" },
        { offsetof(Unit, route_flags), 1, "route_flags" },
        { offsetof(Unit, occ_parked), 1, "occ_parked" },
        { offsetof(Unit, still_ticks), 2, "still_ticks" },
        { offsetof(Unit, path_x), 4 * UNIT_PATH_MAX_WAYPOINTS, "path_x" },
        { offsetof(Unit, path_y), 4 * UNIT_PATH_MAX_WAYPOINTS, "path_y" },
        { offsetof(Unit, anim_state), 1, "anim_state" },
        { offsetof(Unit, walk_thread_slot), 1, "walk_thread_slot" },
        { offsetof(Unit, script_ev), 2 * UNIT_SCRIPT_EV_COUNT, "script_ev" },
        { offsetof(Unit, weapon_state), sizeof(UnitWeaponState) * 3,
          "weapon_state" },
        { offsetof(Unit, prod_queue), 2 * UNIT_PROD_QUEUE_MAX, "prod_queue" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (o >= map[i].off && o < map[i].off + map[i].size) return map[i].name;
    }
    return "?";
}

/* True for a byte past the live length of one of the variable length
 * arrays inside Unit. */
static int in_dead_tail(const Unit *u, size_t o) {
    size_t base, live;
    base = offsetof(Unit, path_x);
    live = (size_t)u->path_len * 4u;
    if (o >= base + live && o < base + 4u * UNIT_PATH_MAX_WAYPOINTS) return 1;
    base = offsetof(Unit, path_y);
    if (o >= base + live && o < base + 4u * UNIT_PATH_MAX_WAYPOINTS) return 1;
    base = offsetof(Unit, load_queue);
    live = (size_t)u->load_queue_len * 2u;
    if (o >= base + live && o < base + 2u * UNIT_LOAD_QUEUE_MAX) return 1;
    base = offsetof(Unit, prod_queue);
    live = (size_t)u->prod_queue_len * 2u;
    if (o >= base + live && o < base + 2u * UNIT_PROD_QUEUE_MAX) return 1;
    /* A dead slot is a tombstone of two fields and nothing else. */
    if (u->alive == UNIT_ALIVE_DEAD) {
        if (o == offsetof(Unit, alive)) return 0;
        if (o >= offsetof(Unit, stable_id) &&
            o < offsetof(Unit, stable_id) + 4u) return 0;
        return 1;
    }
    return 0;
}

static void snap_report_first_difference(const Snapshot *s) {
    GameWorld *w = World_Get();
    int n = 0;
    const Unit *u = Units_GetActive(&n);
    if (n != s->unit_count) {
        printf("[unit count %d, was %d] ", n, s->unit_count);
        return;
    }
    size_t head = offsetof(Unit, cob);
    for (int i = 0; i < n; i++) {
        const uint8_t *a = (const uint8_t *)&s->units[i];
        const uint8_t *b = (const uint8_t *)&u[i];
        for (size_t o = 0; o < head; o++) {
            /* Past the live length of a route or a queue the bytes are
             * whatever a longer one left there. No save writes them and
             * the hash does not read them, so a difference is expected
             * and this must not stop on one. */
            if (in_dead_tail(&u[i], o)) continue;
            if (a[o] != b[o]) {
                printf("[unit %d %s at byte %u: %u, was %u] ", i,
                       unit_field_at(o), (unsigned)o, (unsigned)b[o],
                       (unsigned)a[o]);
                return;
            }
        }
        const CobEngine *e = u[i].cob;
        if (!e || !e->pieces) continue;
        size_t per = SNAP_PIECES * SNAP_WORDS +
                     COB_THREADS_PER_UNIT * SNAP_THREAD_WORDS;
        const int32_t *base = s->cob_pieces + (size_t)i * per;
        for (int k = 0; k < e->piece_count && k < SNAP_PIECES; k++) {
            const int32_t *want = base + (size_t)k * SNAP_WORDS;
            int32_t got[SNAP_WORDS] = {
                e->pieces[k].rot[0], e->pieces[k].rot[1], e->pieces[k].rot[2],
                e->pieces[k].pos[0], e->pieces[k].pos[1], e->pieces[k].pos[2],
                (int32_t)e->pieces[k].hidden, e->pieces[k].rot_speed[1]
            };
            for (int f = 0; f < SNAP_WORDS; f++) {
                if (got[f] != want[f]) {
                    printf("[unit %d piece %d word %d: %d, was %d] ",
                           i, k, f, got[f], want[f]);
                    return;
                }
            }
        }
        const int32_t *tw = base + SNAP_PIECES * SNAP_WORDS;
        for (int t = 0; t < COB_THREADS_PER_UNIT; t++) {
            const int32_t *want = tw + (size_t)t * SNAP_THREAD_WORDS;
            int32_t got[SNAP_THREAD_WORDS] = {
                (int32_t)e->threads[t].pc, (int32_t)e->threads[t].sp,
                (int32_t)e->threads[t].alive,
                (int32_t)e->threads[t].sleep_remaining,
                e->threads[t].return_value
            };
            for (int f = 0; f < SNAP_THREAD_WORDS; f++) {
                if (got[f] != want[f]) {
                    printf("[unit %d thread %d word %d: %d, was %d] ",
                           i, t, f, got[f], want[f]);
                    return;
                }
            }
        }
    }
    int pn = 0;
    const Projectile *pr = Units_GetProjectiles(&pn);
    if (pn != s->proj_count) {
        printf("[projectile slots %d, was %d] ", pn, s->proj_count);
        return;
    }
    for (int i = 0; i < pn; i++) {
        if (pr[i].alive != s->projs[i].alive) {
            printf("[projectile %d alive %d, was %d] ", i, pr[i].alive,
                   s->projs[i].alive);
            return;
        }
        if (!pr[i].alive) continue;
        if (pr[i].world_x != s->projs[i].world_x ||
            pr[i].world_y != s->projs[i].world_y ||
            pr[i].ttl_ticks != s->projs[i].ttl_ticks ||
            pr[i].target != s->projs[i].target) {
            printf("[projectile %d at (%d,%d) ttl %d target %d, was (%d,%d) "
                   "ttl %d target %d] ", i, pr[i].world_x, pr[i].world_y,
                   pr[i].ttl_ticks, pr[i].target,
                   s->projs[i].world_x, s->projs[i].world_y,
                   s->projs[i].ttl_ticks, s->projs[i].target);
            return;
        }
    }
    if (!w) { printf("[no world] "); return; }
    if (w->feature_count != s->feature_count) {
        printf("[feature count %d, was %d] ", w->feature_count,
               s->feature_count);
        return;
    }
    for (int i = 0; i < w->feature_count; i++) {
        if (memcmp(&w->features[i], &s->features[i],
                   sizeof(*w->features)) != 0) {
            printf("[feature %d differs] ", i);
            return;
        }
    }
    size_t cells = (size_t)w->fog_w * (size_t)w->fog_h;
    if (w->fog_w != s->fog_w || w->fog_h != s->fog_h) {
        printf("[fog size %dx%d, was %dx%d] ", w->fog_w, w->fog_h,
               s->fog_w, s->fog_h);
        return;
    }
    for (int q = 1; q <= TAK_MAX_PLAYERS && cells; q++) {
        if (!w->fog_layers[q]) continue;
        if (memcmp(w->fog_layers[q], s->fog + cells * (size_t)q, cells) != 0) {
            printf("[fog layer %d differs] ", q);
            return;
        }
    }
    if (memcmp(&w->economy, &s->econ, sizeof(w->economy)) != 0) {
        printf("[economy differs] ");
        return;
    }
    if (World_RandState() != s->rand_state) {
        printf("[generator %u, was %u] ", World_RandState(), s->rand_state);
        return;
    }
    if (TAK_AI_DebugStateHash() != s->ai_hash) {
        printf("[the AI differs] ");
        return;
    }
    if (w->skirmish_elapsed_ticks != s->skirmish_ticks) {
        printf("[clock %d, was %d] ", w->skirmish_elapsed_ticks,
               s->skirmish_ticks);
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
    snap_take(&g_snap);
    remove(SCRATCH);
    if (Save_Write(SCRATCH, err, cap) != 0) return -1;

    for (int i = 0; i < n; i++) {
        InGame_DebugRunSimTicks(1);
        g_want[i] = TAK_SimHash();
        if (i < SNAP_TICKS) snap_take(&g_tick_snap[i]);
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
        snap_report_first_difference(&g_snap);
        return -7;
    }

    for (int i = 0; i < n; i++) {
        InGame_DebugRunSimTicks(1);
        if (TAK_SimHash() != g_want[i]) {
            if (i < SNAP_TICKS) snap_report_first_difference(&g_tick_snap[i]);
            return i + 1;
        }
    }
    snap_free();
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

/* A load can be abandoned between World_BeginLoad and Save_Apply: the
 * player backs out of the loading screen, or a phase fails. Nothing on
 * those paths has to reach World_End, so the restoring flag cannot be
 * left to a teardown. The next battle starts with a World_BeginLoad
 * and that is what puts it down, or the battle after an abandoned load
 * spawns nothing and looks completely broken. Needs no game data: it
 * is about the flag, not about a map. */
TEST(an_abandoned_load_does_not_leak_into_the_next_battle) {
    BattleConfig cfg;
    fill_cfg(&cfg);
    ASSERT_EQ_INT(0, World_BeginLoad(NULL, &cfg, MAP_NAME, MAP_WORLD));
    World_SetRestoring(1);
    ASSERT_EQ_INT(1, World_IsRestoring());

    /* The load is abandoned with the world still standing. */
    ASSERT_EQ_INT(0, World_BeginLoad(NULL, &cfg, MAP_NAME, MAP_WORLD));
    ASSERT_EQ_INT(0, World_IsRestoring());

    /* And a teardown puts it down as well. */
    World_SetRestoring(1);
    World_End(NULL);
    ASSERT_EQ_INT(0, World_IsRestoring());
}

/* The case that matters. Two armies fighting, a save part way through,
 * and the reloaded battle has to play out the same way tick for tick.
 */
TEST(a_saved_skirmish_runs_on_exactly_as_it_would_have) {
    if (setup_vfs() != 0) { SKIP_MARK("no game data"); return; }
    TAK_Platform plat;
    if (setup_platform(&plat) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    GameWorld *w = NULL;
    ASSERT_EQ_INT(0, boot_battle(&plat, &cfg, &w));
    ASSERT(spawn_brawl(w, &cfg) > 0);

    /* Long enough that the armies have met and the AI has given
     * orders, and stopping on a tick with arrows in the air. */
    InGame_DebugRunSimTicks(600);
    for (int i = 0; i < 120 && live_projectiles() == 0; i++) {
        InGame_DebugRunSimTicks(10);
    }
    {
        int slots = 0;
        (void)Units_GetActive(&slots);
        printf("(%d shots in the air, %d unit slots) ",
               live_projectiles(), slots);
    }
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

/* The first tick of a battle. Nothing has moved, every script has
 * just run Create and is sitting in whatever it settled into, no
 * order has been given and the economy is one tick old. A section
 * that quietly assumes a settled world goes wrong here. */
TEST(a_save_taken_before_anything_has_moved_still_runs_on) {
    if (setup_vfs() != 0) { SKIP_MARK("no game data"); return; }
    TAK_Platform plat;
    if (setup_platform(&plat) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    GameWorld *w = NULL;
    ASSERT_EQ_INT(0, boot_battle(&plat, &cfg, &w));
    ASSERT_EQ_INT(0, w->skirmish_elapsed_ticks);

    char err[TAK_SAVE_ERR_MAX] = { 0 };
    int rc = save_then_replay(&plat, 300, err, sizeof(err));
    if (rc != 0) { report(rc); printf("%s ", err); }
    ASSERT_EQ_INT(0, rc);

    end_battle(&plat);
    UI_Shutdown();
    teardown_platform(&plat);
    VFS_Shutdown();
}

/* The tick after a unit dies is the most half formed a battle gets.
 * The slot has become a tombstone, its Killed script is still running
 * on a thread the file has to carry, the corpse it dropped is a fresh
 * feature counting down, and the cells it held have just been given
 * up. */
TEST(a_save_taken_the_tick_after_a_death_still_runs_on) {
    if (setup_vfs() != 0) { SKIP_MARK("no game data"); return; }
    TAK_Platform plat;
    if (setup_platform(&plat) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    GameWorld *w = NULL;
    ASSERT_EQ_INT(0, boot_battle(&plat, &cfg, &w));

    int def = combat_def_for_side(cfg.players[0].side);
    ASSERT(def >= 0);
    int32_t sx, sy;
    ASSERT_EQ_INT(0, start_of(w, 1, &sx, &sy));
    int victim = Units_Spawn(def, 1, 0, sx + 160, sy + 160);
    ASSERT(victim >= 0);
    /* Let it settle so the death is the only thing half done. */
    InGame_DebugRunSimTicks(120);
    int features_before = w->feature_count;

    ASSERT(Units_DebugKillHandle(victim) >= 0);
    /* Exactly one tick later: the body is on its way down, the script
     * is mid Killed and the slot is not yet a settled tombstone. */
    InGame_DebugRunSimTicks(1);

    char err[TAK_SAVE_ERR_MAX] = { 0 };
    int rc = save_then_replay(&plat, 300, err, sizeof(err));
    if (rc != 0) { report(rc); printf("%s ", err); }
    ASSERT_EQ_INT(0, rc);
    /* And the corpse it left is on the ground on the other side. */
    ASSERT(World_Get()->feature_count >= features_before);

    end_battle(&plat);
    UI_Shutdown();
    teardown_platform(&plat);
    VFS_Shutdown();
}

/* A nanoframe is a real unit with a builder feeding it fractional HP.
 * The frame, the builder's handle on it and the mana the feeding has
 * already spent all have to come back. */
TEST(a_save_taken_mid_build_finishes_the_building) {
    if (setup_vfs() != 0) { SKIP_MARK("no game data"); return; }
    TAK_Platform plat;
    if (setup_platform(&plat) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    GameWorld *w = NULL;
    ASSERT_EQ_INT(0, boot_battle(&plat, &cfg, &w));

    /* The AI builds on its own once its monarch has mana. */
    InGame_DebugRunSimTicks(1800);
    if (units_with(is_under_construction) == 0) {
        SKIP_MARK("nothing under construction to catch");
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
    if (setup_vfs() != 0) { SKIP_MARK("no game data"); return; }
    TAK_Platform plat;
    if (setup_platform(&plat) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());

    BattleConfig cfg;
    GameWorld *w = NULL;
    ASSERT_EQ_INT(0, boot_battle(&plat, &cfg, &w));

    int transport_def = -1;
    int n = Units_GetDefCount();
    for (int i = 0; i < n && transport_def < 0; i++) {
        const UnitDef *d = Units_GetDef(i);
        if (d && !d->is_feature && (d->cap_flags & UNIT_CAP_TRANSPORT) &&
            d->transport_capacity > 0) {
            transport_def = i;
        }
    }
    ASSERT(transport_def >= 0);

    /* Set them down on the same spot so the pickup is a transfer in
     * place: neither has to walk, and the case is about what the save
     * does with the coupling rather than about the mover. */
    int32_t sx, sy;
    ASSERT_EQ_INT(0, start_of(w, 1, &sx, &sy));
    int carrier = Units_Spawn(transport_def, 1, 0, sx + 128, sy + 128);
    ASSERT(carrier >= 0);

    int rider = -1;
    for (int i = 0; i < n && units_with(is_transported) == 0; i++) {
        const UnitDef *d = Units_GetDef(i);
        if (!d || d->is_feature || d->commander || d->can_fly) continue;
        if (d->bmcode == 0 || d->cant_be_transported) continue;
        if (d->max_velocity <= 0.0f) continue;
        if (d->cap_flags & UNIT_CAP_TRANSPORT) continue;
        rider = Units_Spawn(i, 1, 0, sx + 128, sy + 128);
        if (rider < 0) continue;
        Units_SelectSingle(carrier);
        Units_CommandLoadSelected(rider, 0);
        for (int k = 0; k < 20 && units_with(is_transported) == 0; k++) {
            InGame_DebugRunSimTicks(15);
        }
        if (units_with(is_transported) > 0) break;
        Units_DebugKillHandle(rider);
        InGame_DebugRunSimTicks(120);
        rider = -1;
    }
    ASSERT(units_with(is_transported) > 0);

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
    if (setup_vfs() != 0) { SKIP_MARK("no game data"); return; }
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
        SKIP_MARK("no raiser in this data set");
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
        SKIP_MARK("nothing raisable where the body fell");
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
            SKIP_MARK("the raise never started");
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

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    tak_mem_init();
    TEST_SUITE("A saved battle is the battle that was saved");
    RUN(an_abandoned_load_does_not_leak_into_the_next_battle);
    RUN(a_saved_skirmish_runs_on_exactly_as_it_would_have);
    RUN(a_save_taken_before_anything_has_moved_still_runs_on);
    RUN(a_save_taken_the_tick_after_a_death_still_runs_on);
    RUN(a_save_taken_mid_build_finishes_the_building);
    RUN(a_save_with_a_loaded_transport_keeps_its_passengers);
    RUN(a_save_taken_mid_raise_keeps_the_work_owed);
    remove(SCRATCH);
    TEST_REPORT();
}
