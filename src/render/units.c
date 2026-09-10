/*
 * units.c — unit registry, active array, FBI parser, 3D mesh renderer.
 *
 * Phase C M4 lives here. Older M2/M3 work (3DO parse, texture atlas)
 * is in obj3d.c / tex_atlas.c — this file ties everything together at
 * render time.
 *
 * Pipeline:
 *   Units_LoadDefs       walks units/*.fbi, builds UnitDef registry
 *   Units_DebugSpawnMonarch  hotkey '3' drops one monarch at a coord;
 *                        lazy-bakes its mesh on first call
 *   Units_Render         per frame: build draw cmds, submit triangles
 *   Units_Submit         transforms model → world → screen, hands the
 *                        whole batch to SDL_RenderGeometryRaw
 *
 * The transform stack ("the 3D math") lives in Units_Submit. It's the
 * only function that has to change to swap rendering backends — the
 * UnitMesh data is already GPU-friendly.
 *
 * M4 deliberately ignores: textures (everything is flat white),
 * heading (no rotation around Y), team color, Y-sort. Those land in
 * M5/M7. M4's job is just "see a recognisable human-shaped ghost
 * standing on the terrain, scaled and tilted believably."
 */

#include "SDL.h"
#include "tak_unit.h"
#include "tak_obj3d.h"
#include "tak_tdf.h"
#include "tak_hpi.h"
#include "tak_features.h"
#include "tak_util.h"
#include "tak_gaf.h"
#include "tak_gpu.h"
#include "tak_tex_atlas.h"
#include "tak_memory.h"
#include "tak_world.h"
#include "tak_occupancy.h"
#include "tak_game_sound.h"
#include "tak_battle_config.h"  /* TAK_MAX_PLAYERS */
#include "tak_ai.h"
#include "tak_terrain.h"
#include "tak_fog.h"
#include "tak_pathing.h"
#include "tak_moveinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#  include <strings.h>
#endif

#ifndef TAK_MAX_UNITS
#define TAK_MAX_UNITS 2000
#endif

/* ── State ────────────────────────────────────────────────────────── */

static UnitDef *g_defs       = NULL;
static int      g_def_count  = 0;
static int      g_def_cap    = 0;

static Unit     g_units[TAK_MAX_UNITS];
static int      g_unit_count = 0;
static uint32_t g_next_stable_unit_id = 1;

/* In-flight projectile pool. Sized for ~10 projectiles per unit on a
 * full battlefield — generous but bounded. Reused via alive flag. */
#define TAK_MAX_PROJECTILES 4096
static Projectile g_projectiles[TAK_MAX_PROJECTILES];
static int        g_projectile_count = 0;

static const float TAK_PIXELS_PER_UNIT = 16.0f;

/* ── Spatial bucket grid (legacy gs+0x19f18, 128px cells :220374) ──
 * Rebuilt once per tick; auto-target scans query only nearby buckets
 * instead of every unit (O(n²) at 60Hz breaks the 1000-unit bar). */
#define UGRID_SHIFT 7
#define UGRID_W     128
#define UGRID_MASK  (UGRID_W - 1)
static int16_t g_ugrid_head[UGRID_W * UGRID_W];
static int16_t g_ugrid_next[TAK_MAX_UNITS];

static void ugrid_rebuild(void) {
    memset(g_ugrid_head, 0xFF, sizeof(g_ugrid_head));
    int n = g_unit_count > TAK_MAX_UNITS ? TAK_MAX_UNITS : g_unit_count;
    for (int i = 0; i < n; i++) {
        if (g_units[i].alive != 1) continue;
        int c = ((g_units[i].world_y >> UGRID_SHIFT) & UGRID_MASK) * UGRID_W
              + ((g_units[i].world_x >> UGRID_SHIFT) & UGRID_MASK);
        g_ugrid_next[i] = g_ugrid_head[c];
        g_ugrid_head[c] = (int16_t)i;
    }
}

static int weapon_can_target_unit(const UnitWeapon *wp, const Unit *t);
static int unit_can_see_target(const Unit *u, const Unit *t);
static int unit_players_are_enemies(int a, int b);

/* Nearest visible enemy of u within radius; wp non-NULL adds the
 * weapon targetability filter. Returns handle or -1. */
static int ugrid_nearest_enemy(const Unit *u, int self_idx, int64_t radius,
                               const UnitWeapon *wp) {
    if (radius <= 0) return -1;
    int c0x = (int)((u->world_x - radius) >> UGRID_SHIFT);
    int c1x = (int)((u->world_x + radius) >> UGRID_SHIFT);
    int c0y = (int)((u->world_y - radius) >> UGRID_SHIFT);
    int c1y = (int)((u->world_y + radius) >> UGRID_SHIFT);
    int64_t best_d2 = radius * radius;
    int best = -1;
    for (int cy = c0y; cy <= c1y; cy++) {
        for (int cx = c0x; cx <= c1x; cx++) {
            int c = (cy & UGRID_MASK) * UGRID_W + (cx & UGRID_MASK);
            for (int j = g_ugrid_head[c]; j >= 0; j = g_ugrid_next[j]) {
                const Unit *t = &g_units[j];
                if (j == self_idx || t->alive != 1) continue;
                if (!unit_players_are_enemies(u->player_id, t->player_id))
                    continue;
                if (!unit_can_see_target(u, t)) continue;
                if (wp && !weapon_can_target_unit(wp, t))
                    continue;
                int64_t dx = (int64_t)(t->world_x - u->world_x);
                int64_t dy = (int64_t)(t->world_y - u->world_y);
                int64_t d2 = dx * dx + dy * dy;
                if (d2 < best_d2) { best_d2 = d2; best = j; }
            }
        }
    }
    return best;
}

/* Forward — `apply_killed` is defined further down with the combat
 * state-machine code; the projectile tick needs to call it on hits. */
static void apply_killed(Unit *t, int t_idx);

/* Kill credit — XP to the killer unit plus the victim's mogriumbounty
 * to the killer's mana pool. No self/team credit (legacy :227321). */
static void credit_kill(int shooter_handle, const Unit *victim);
static float build_heading_for_def(const UnitDef *d);

/* Factory build pad (QueryBuildInfo piece -> world spot) and the
 * water-depth window. Both are defined further down, next to the piece
 * transform and move-class helpers they build on. */
static int unit_factory_build_spot(Unit *f, int32_t *out_x, int32_t *out_y);
static int unit_start_script(Unit *u, const char *name,
                             const int32_t *args, int n_args);
static int unit_water_depth_ok(const GameWorld *w, const UnitDef *def,
                               int32_t x, int32_t y);

static int unit_player_team_id(int player_id) {
    const GameWorld *world = World_Get();
    if (!world || player_id < 1 || player_id > TAK_MAX_PLAYERS) return player_id;
    const PlayerSlot *slot = &world->cfg.players[player_id - 1];
    if (slot->kind == TAK_SLOT_CLOSED) return 0;
    return slot->team > 0 ? slot->team : player_id;
}

static int unit_players_are_enemies(int a, int b) {
    int ta = unit_player_team_id(a);
    int tb = unit_player_team_id(b);
    if (ta <= 0 || tb <= 0) return a != b;
    return ta != tb;
}

static int unit_visible_to_local_player(const GameWorld *world,
                                        const Unit *u) {
    if (!u) return 0;
    if (!world || !world->cfg.line_of_sight) return 1;
    if (u->player_id == 1) return 1;
    return Fog_IsVisible(world, u->world_x, u->world_y);
}

int Units_IsVisibleToLocalPlayer(const Unit *u) {
    return unit_visible_to_local_player(World_Get(), u);
}

static int projectile_visible_to_local_player(const GameWorld *world,
                                              const Projectile *p) {
    if (!p) return 0;
    if (!world || !world->cfg.line_of_sight) return 1;
    if (p->player_id == 1) return 1;
    return Fog_IsVisible(world, p->world_x, p->world_y);
}

static uint8_t weapon_visual_kind(const UnitWeapon *wp);
static int weapon_is_melee(const UnitWeapon *wp);
static int weapon_damage_for_category(const UnitWeapon *wp, const char *category);
static void lowercase_into(char *dst, size_t cap, const char *src);
static void proj_model_drop_meshes(void);
static void proj_art_release_textures(void);

static void unit_clear_path(Unit *u) {
    if (!u) return;
    u->path_len = 0;
    u->path_index = 0;
    u->path_failed = 0;
    u->path_goal_x = 0;
    u->path_goal_y = 0;
    u->wp_stall = 0;
    u->wp_best_d2 = 0x7fffffff;
    /* Recycled slots must not inherit a stale hold/blocked state —
     * that would silently freeze the new unit. */
    u->path_pending = 0;
    u->path_wait = 0;
    u->blocked_ticks = 0;
    u->path_replan_cd = 0;
    u->avoid_side = 0;
}

static int unit_def_can_repair(const UnitDef *d) {
    if (!d) return 0;
    if (d->cap_flags & UNIT_CAP_REPAIR) return 1;
    return ((d->cap_flags & UNIT_CAP_BUILDER) != 0 && d->worker_time > 0.0f);
}

static int unit_transport_size(const UnitDef *d) {
    if (!d) return 1;
    if (d->transported_size > 0) return d->transported_size;
    /* Legacy default: footprint area (:163196). */
    if (d->footprint_x > 0 && d->footprint_z > 0)
        return d->footprint_x * d->footprint_z;
    return 1;
}

static int unit_can_carry_target(const Unit *carrier, const Unit *target) {
    if (!carrier || !target) return 0;
    const UnitDef *cd = Units_GetDef(carrier->def_idx);
    const UnitDef *td = Units_GetDef(target->def_idx);
    if (!cd || !td) return 0;
    if (!(cd->cap_flags & UNIT_CAP_TRANSPORT)) return 0;
    if (target->alive != UNIT_ALIVE_ACTIVE) return 0;
    if (target->player_id != carrier->player_id) return 0;
    if (td->cant_be_transported) return 0;
    if (td->can_fly) return 0;                       /* :233637 */
    if (target->under_construction) return 0;
    int count_cap = cd->transport_capacity > 0 ? cd->transport_capacity : 1;
    int size_cap = cd->transport_size_capacity > 0
                 ? cd->transport_size_capacity : count_cap;
    int size = unit_transport_size(td);
    /* Per-unit gate: unit size vs carrier slot size (:233636). */
    if (cd->transport_size > 0 && size > cd->transport_size) return 0;
    if (carrier->cargo_count >= count_cap) return 0;
    if (carrier->cargo_size_used + size > size_cap) return 0;
    return 1;
}

static int unit_load_into_transport(Unit *carrier, int carrier_idx,
                                    Unit *target, int target_idx) {
    if (!unit_can_carry_target(carrier, target)) return 0;
    const UnitDef *td = Units_GetDef(target->def_idx);
    int size = unit_transport_size(td);
    target->alive = UNIT_ALIVE_TRANSPORTED;
    target->carried_by = (int16_t)carrier_idx;
    target->cmd_kind = UNIT_CMD_NONE;
    target->target = -1;
    target->velocity = 0;
    target->world_x = carrier->world_x;
    target->world_y = carrier->world_y;
    unit_clear_path(target);
    carrier->cargo_count++;
    carrier->cargo_size_used += (int16_t)size;
    carrier->cmd_kind = UNIT_CMD_NONE;
    carrier->target = -1;
    unit_clear_path(carrier);
    fprintf(stderr, "Transport: loaded unit %d into %d\n",
            target_idx, carrier_idx);
    return 1;
}

static int unit_drop_site_clear(int cargo_idx, int carrier_idx,
                                int32_t wx, int32_t wy) {
    GameWorld *world = World_Get();
    const Unit *cargo = &g_units[cargo_idx];
    const UnitDef *cd = Units_GetDef(cargo->def_idx);
    int slope = cd ? cd->max_slope : 255;
    if (!Terrain_IsWalkable(world, wx, wy, slope)) return 0;
    int fx = (cd && cd->footprint_x > 0) ? cd->footprint_x : 1;
    int fz = (cd && cd->footprint_z > 0) ? cd->footprint_z : 1;
    int hw = fx * 8;
    int hh = fz * 8;
    int x0 = wx - hw, x1 = wx + hw;
    int y0 = wy - hh, y1 = wy + hh;
    for (int i = 0; i < g_unit_count; i++) {
        if (i == cargo_idx || i == carrier_idx) continue;
        const Unit *u = &g_units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE) continue;
        const UnitDef *ud = Units_GetDef(u->def_idx);
        int uhw = (ud && ud->footprint_x > 0) ? ud->footprint_x * 8 : 16;
        int uhh = (ud && ud->footprint_z > 0) ? ud->footprint_z * 8 : 16;
        int ux0 = u->world_x - uhw, ux1 = u->world_x + uhw;
        int uy0 = u->world_y - uhh, uy1 = u->world_y + uhh;
        if (ux0 < x1 && ux1 > x0 && uy0 < y1 && uy1 > y0) return 0;
    }
    return 1;
}

static int unit_unload_one_from_transport(Unit *carrier, int carrier_idx,
                                          int32_t wx, int32_t wy) {
    if (!carrier || carrier->cargo_count <= 0) return 0;
    int cargo_idx = -1;
    for (int i = 0; i < g_unit_count; i++) {
        if (g_units[i].alive == UNIT_ALIVE_TRANSPORTED &&
            g_units[i].carried_by == carrier_idx) {
            cargo_idx = i;
            break;
        }
    }
    if (cargo_idx < 0) return 0;
    /* Expanding ring search for a clear drop cell (16px steps out to
     * 10 tiles). A fixed offset table stalls the unload whenever the
     * drop area is crowded or the terrain rejects those exact cells. */
    for (int r = 0; r <= 160; r += 16)
    for (int dy = -r; dy <= r; dy += 16)
    for (int dx = -r; dx <= r; dx += 16) {
        /* ring perimeter only — interior was covered by smaller r */
        if (r > 0 && dx > -r && dx < r && dy > -r && dy < r) continue;
        int32_t px = wx + dx;
        int32_t py = wy + dy;
        if (!unit_drop_site_clear(cargo_idx, carrier_idx, px, py)) continue;
        Unit *cargo = &g_units[cargo_idx];
        const UnitDef *cd = Units_GetDef(cargo->def_idx);
        int size = unit_transport_size(cd);
        cargo->alive = UNIT_ALIVE_ACTIVE;
        cargo->carried_by = -1;
        cargo->world_x = px;
        cargo->world_y = py;
        cargo->cmd_kind = UNIT_CMD_NONE;
        cargo->target = -1;
        unit_clear_path(cargo);
        if (carrier->cargo_count > 0) carrier->cargo_count--;
        if (carrier->cargo_size_used >= size) carrier->cargo_size_used -= (int16_t)size;
        else carrier->cargo_size_used = 0;
        fprintf(stderr, "Transport: unloaded unit %d from %d\n",
                cargo_idx, carrier_idx);
        return 1;
    }
    return 0;
}

static int unit_can_see_target(const Unit *viewer, const Unit *target) {
    if (!viewer || !target) return 0;
    GameWorld *world = World_Get();
    if (!world || !world->cfg.line_of_sight) return 1;
    /* Legacy does NOT gate targeting on visibility. PathFind_FindObstacles
     * (:21137-21168) filters on alive / not-under-construction / alliance
     * / category masks / range only — the "visible" bit (0x100) is written
     * by Game_UpdateVisibility (:208607) for the LOCAL player and is read
     * in just two places, the minimap blip draw (:208473) and the AI's
     * strategic map (:20541). Never in a targeting path. Gating here made
     * units refuse to shoot things they were standing next to (arrow
     * towers, knights vs a ghost ship) and, being local-player state,
     * would also desync multiplayer. Strategic AI keeps its own fog
     * check in src/game/ai.c, matching AIBrain_EvaluateMap. */
    (void)world;
    return 1;
}

static uint32_t unit_deterministic_noise(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t x = a * 1664525u + 1013904223u;
    x ^= b + 0x9e3779b9u + (x << 6) + (x >> 2);
    x ^= c + 0x85ebca6bu + (x << 13) + (x >> 7);
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

const Projectile *Units_GetProjectiles(int *out_count) {
    if (out_count) *out_count = g_projectile_count;
    return g_projectiles;
}

/* ── Projectile art caches ────────────────────────────────────────────
 *
 * Legacy resolves a weapon's `model` to a 3DO record and its
 * `weaponart` to a GAF sequence at load time (legacy:250074,
 * legacy:250088), then draws the projectile through the normal model
 * renderer or blits the sequence frame (legacy:246780-246791). We keep
 * the same split: models bake once per (name, team colour) into a
 * UnitMesh and draw batched; sprites decode once into a single strip
 * texture so every projectile of one weapon is a RenderCopy from the
 * same texture and never re-uploads.
 *
 * Both tables are name-keyed and append-only. Slot allocation is
 * simulation-side (no I/O); the pixels/mesh load lazily at draw. */

#define TAK_PROJ_MODEL_MAX  32
#define TAK_PROJ_SPRITE_MAX 96

typedef struct ProjModelArt {
    char      name[32];
    UnitMesh *mesh_per_color[12];
    uint8_t   failed[12];
} ProjModelArt;
static ProjModelArt g_proj_models[TAK_PROJ_MODEL_MAX];
static int          g_proj_model_count = 0;

typedef struct ProjSpriteArt {
    char          file[40];     /* anims basename, no _4444 suffix */
    char          seq[40];      /* sequence name inside the file   */
    int           num_frames;
    int           cell_w, cell_h;
    int          *fw, *fh, *ox, *oy;
    uint32_t     *pixels;       /* decoded strip, kept for re-upload */
    SDL_Texture  *strip;        /* GPU copy, one upload per renderer */
    SDL_Renderer *owner;        /* renderer that owns `strip`        */
    uint32_t      epoch;        /* art epoch `strip` was made in     */
    uint8_t       tried;        /* 1 once a decode was attempted     */
} ProjSpriteArt;
/* A destroyed renderer takes its textures with it, and the next one can
 * land on the same address, so the pointer alone cannot say whether a
 * cached texture is live. Every world teardown bumps this; a strip is
 * only reused when both the renderer and the epoch still match. */
static uint32_t g_proj_art_epoch = 1;
static ProjSpriteArt g_proj_sprites[TAK_PROJ_SPRITE_MAX];
static int           g_proj_sprite_count = 0;

/* Impact effects: explosionclass sprite played once where a shot lands. */
#define TAK_MAX_PROJ_EFFECTS 512
static ProjectileEffect g_proj_effects[TAK_MAX_PROJ_EFFECTS];
static int              g_proj_effect_count = 0;

/* explosions.tdf: each class lists numbered variants, each naming a GAF
 * plus the sequence inside it (legacy binds the class to the weapon at
 * legacy:250135 and spawns it on impact at legacy:245025). */
#define TAK_EXPL_CLASS_MAX   48
#define TAK_EXPL_VARIANT_MAX 8
typedef struct ExplosionClassDef {
    char    name[40];
    int     variant_count;
    int16_t sprite[TAK_EXPL_VARIANT_MAX];
} ExplosionClassDef;
static ExplosionClassDef g_expl_classes[TAK_EXPL_CLASS_MAX];
static int               g_expl_class_count = 0;
static int               g_expl_loaded = 0;

const ProjectileEffect *Units_GetProjectileEffects(int *out_count) {
    if (out_count) *out_count = g_proj_effect_count;
    return g_proj_effects;
}

static int proj_model_index(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < g_proj_model_count; i++) {
        if (tak_stricmp(g_proj_models[i].name, name) == 0) return i;
    }
    if (g_proj_model_count >= TAK_PROJ_MODEL_MAX) return -1;
    ProjModelArt *pm = &g_proj_models[g_proj_model_count];
    memset(pm, 0, sizeof(*pm));
    snprintf(pm->name, sizeof(pm->name), "%s", name);
    return g_proj_model_count++;
}

static int proj_sprite_index(const char *file, const char *seq) {
    if (!file || !*file) return -1;
    if (!seq || !*seq) seq = file;
    for (int i = 0; i < g_proj_sprite_count; i++) {
        if (tak_stricmp(g_proj_sprites[i].file, file) == 0 &&
            tak_stricmp(g_proj_sprites[i].seq, seq) == 0) return i;
    }
    if (g_proj_sprite_count >= TAK_PROJ_SPRITE_MAX) return -1;
    ProjSpriteArt *ps = &g_proj_sprites[g_proj_sprite_count];
    memset(ps, 0, sizeof(*ps));
    snprintf(ps->file, sizeof(ps->file), "%s", file);
    snprintf(ps->seq,  sizeof(ps->seq),  "%s", seq);
    return g_proj_sprite_count++;
}

/* Parse gamedata/explosions/explosions.tdf once. Each [class] holds
 * numbered subsections with `gaf` + `anim`; legacy picks one variant
 * per impact. */
static void load_explosion_classes(void) {
    if (g_expl_loaded) return;
    g_expl_loaded = 1;
    TDFFile *tdf = TDF_Open("gamedata/explosions/explosions.tdf");
    if (!tdf || TDF_Load(tdf) != 0) {
        if (tdf) TDF_Close(tdf);
        fprintf(stderr, "Projectiles: explosions.tdf unavailable\n");
        return;
    }
    /* One shared section cursor, so collect the class names first and
     * only then descend into each. Variants are always numbered. */
    static char names[TAK_EXPL_CLASS_MAX][40];
    int n_names = 0;
    for (const char *cls = TDF_GetFirstSection(tdf);
         cls && n_names < TAK_EXPL_CLASS_MAX;
         cls = TDF_GetNextSection(tdf)) {
        snprintf(names[n_names++], sizeof(names[0]), "%s", cls);
    }
    for (int ni = 0; ni < n_names; ni++) {
        if (TDF_PushSection(tdf, names[ni]) != 0) continue;
        ExplosionClassDef *ec = &g_expl_classes[g_expl_class_count];
        memset(ec, 0, sizeof(*ec));
        snprintf(ec->name, sizeof(ec->name), "%s", names[ni]);
        for (int v = 0; v < TAK_EXPL_VARIANT_MAX; v++) {
            char var_name[8];
            snprintf(var_name, sizeof(var_name), "%d", v);
            if (TDF_PushSection(tdf, var_name) != 0) continue;
            char gaf_lc[40], anim_lc[40];
            lowercase_into(gaf_lc, sizeof(gaf_lc),
                           TDF_ReadString(tdf, "gaf", ""));
            lowercase_into(anim_lc, sizeof(anim_lc),
                           TDF_ReadString(tdf, "anim", ""));
            if (gaf_lc[0]) {
                int si = proj_sprite_index(gaf_lc,
                                           anim_lc[0] ? anim_lc : gaf_lc);
                if (si >= 0) ec->sprite[ec->variant_count++] = (int16_t)si;
            }
            TDF_PopSection(tdf);
        }
        if (ec->variant_count > 0) g_expl_class_count++;
        TDF_PopSection(tdf);
    }
    TDF_Close(tdf);
    fprintf(stderr, "Projectiles: %d explosion classes loaded\n",
            g_expl_class_count);
}

static int explosion_class_index(const char *name) {
    if (!name || !*name) return -1;
    load_explosion_classes();
    for (int i = 0; i < g_expl_class_count; i++) {
        if (tak_stricmp(g_expl_classes[i].name, name) == 0) return i;
    }
    return -1;
}

/* Queue the weapon's explosionclass sprite at the impact point. */
static void spawn_impact_effect(int explosion_idx, int32_t x, int32_t y,
                                int32_t height, uint32_t seed) {
    if (explosion_idx < 0 || explosion_idx >= g_expl_class_count) return;
    const ExplosionClassDef *ec = &g_expl_classes[explosion_idx];
    if (ec->variant_count <= 0) return;
    uint32_t n = unit_deterministic_noise((uint32_t)x, (uint32_t)y, seed);
    int16_t sprite = ec->sprite[n % (uint32_t)ec->variant_count];
    int slot = -1;
    for (int i = 0; i < g_proj_effect_count; i++) {
        if (!g_proj_effects[i].alive) { slot = i; break; }
    }
    if (slot < 0) {
        if (g_proj_effect_count >= TAK_MAX_PROJ_EFFECTS) return;
        slot = g_proj_effect_count++;
    }
    ProjectileEffect *e = &g_proj_effects[slot];
    e->world_x    = x;
    e->world_y    = y;
    e->height     = height;
    e->sprite_idx = sprite;
    e->age_ticks  = 0;
    e->alive      = 1;
}

/* Engine gravity is 0x1fdb in 16.16 per legacy tick (legacy:224904);
 * the sim runs at twice that rate, so the per-tick pull is a quarter. */
static float projectile_gravity_ppt2(float gravity_adjust) {
    const float g_legacy = 8155.0f / 65536.0f;   /* px per legacy tick^2 */
    return g_legacy * 0.25f * gravity_adjust;
}

/* Launch pitch for a gravity weapon. Legacy solves
 * tan(theta) = k -/+ sqrt(k^2 - 2*rise*k/run - 1) with
 * k = v^2 / (g * gravityadjustment * run), taking the low arc unless
 * lobpreferred (legacy:246535). A negative discriminant means the shot
 * cannot reach; legacy then fires flat (legacy:246568). */
static float projectile_launch_pitch(float speed_ppt, float run,
                                     float rise, float grav_ppt2,
                                     int lob_preferred) {
    if (run < 0.001f || grav_ppt2 <= 0.0f) return 0.0f;
    float k = (speed_ppt * speed_ppt) / (grav_ppt2 * run);
    float disc = k * k - (2.0f * rise * k) / run - 1.0f;
    if (disc < 0.0f) return 0.0f;
    float root = sqrtf(disc);
    return atanf(lob_preferred ? (k + root) : (k - root));
}

/* Spawn a new projectile aimed at `target_handle`. Returns -1 if the
 * pool is full. */
static int spawn_projectile(int32_t x, int32_t y,
                             int32_t tx, int32_t ty,
                             float speed_pps,
                             int32_t damage,
                             int32_t area_of_effect,
                             float edge_effectiveness,
                             const UnitWeapon *source_weapon,
                             uint8_t visual_kind,
                             int target_handle,
                             int shooter_handle,
                             uint8_t player_id,
                             uint8_t color_idx)
{
    int slot = -1;
    for (int i = 0; i < g_projectile_count; i++) {
        if (!g_projectiles[i].alive) { slot = i; break; }
    }
    if (slot < 0) {
        if (g_projectile_count >= TAK_MAX_PROJECTILES) return -1;
        slot = g_projectile_count++;
    }
    Projectile *p = &g_projectiles[slot];
    p->world_x = x;
    p->world_y = y;
    float dx = (float)(tx - x);
    float dy = (float)(ty - y);
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) { p->dir_x = 0; p->dir_y = 1; }
    else { p->dir_x = dx / len; p->dir_y = dy / len; }
    p->speed_ppt = speed_pps / 60.0f;
    p->damage = damage;
    p->area_of_effect = area_of_effect;
    if (edge_effectiveness < 0.0f) edge_effectiveness = 0.0f;
    if (edge_effectiveness > 1.0f) edge_effectiveness = 1.0f;
    p->edge_effectiveness = edge_effectiveness;
    p->damage_scale_count = 0;
    /* Pool slots are recycled — a slot that last held a LOS beam would
     * otherwise render every later arrow as lightning and never move it. */
    p->is_beam = 0;
    p->src_x = x;
    p->src_y = y;
    p->hit_sound_class[0] = '\0';
    p->hit_sound[0] = '\0';
    p->dest_x = tx;
    p->dest_y = ty;
    p->friendly_fire = (target_handle < 0);
    if (source_weapon) {
        memcpy(p->hit_sound_class, source_weapon->hit_sound_class,
               sizeof(p->hit_sound_class));
        memcpy(p->hit_sound, source_weapon->hit_sound, sizeof(p->hit_sound));
        int n = source_weapon->damage_scale_count;
        if (n < 0) n = 0;
        if (n > TAK_DAMAGE_CATEGORY_MAX) n = TAK_DAMAGE_CATEGORY_MAX;
        p->damage_scale_count = n;
        if (n > 0) {
            memcpy(p->damage_scales, source_weapon->damage_scales,
                   sizeof(UnitDamageScale) * (size_t)n);
        }
    }
    p->visual_kind = visual_kind;
    p->target = (int16_t)target_handle;

    /* ── Art, arc and orientation ──────────────────────────────────
     * Legacy decomposes the launch velocity into a vertical term plus
     * a heading-rotated horizontal term (legacy:246590) and, for
     * gravity weapons, gets the pitch from the launch solver
     * (legacy:246562). Spinning weapons carry fixed spin rates; the
     * rest point along their velocity. */
    p->art_kind      = UNIT_WEAPON_ART_NONE;
    p->art_idx       = -1;
    p->explosion_idx = -1;
    p->color_idx     = color_idx;
    p->age_ticks     = 0;
    p->vel_up_ppt    = 0.0f;
    p->gravity_ppt2  = 0.0f;
    p->pitch         = 0.0f;
    p->roll          = 0.0f;
    p->spin_pitch    = 0.0f;
    p->spin_heading  = 0.0f;
    p->spin_roll     = 0.0f;
    p->heading       = atan2f(dx, -dy);
    p->sub_x         = 0.0f;
    p->sub_y         = 0.0f;
    const GameWorld *lw = World_Get();
    p->src_height = lw ? Terrain_SampleHeight(lw, x, y) : 0;
    /* height is an absolute world height, same axis as the terrain, so
     * an arc reads correctly over sloping ground. Shots leave the
     * weapon muzzle, not the dirt: without that clearance a downhill
     * shot starts with negative lift and grounds itself on tick one. */
    const float MUZZLE_H = 12.0f;
    p->height = (float)p->src_height + MUZZLE_H;
    /* A flyer fires from where it is drawn. */
    if (shooter_handle >= 0 && shooter_handle < g_unit_count)
        p->height += g_units[shooter_handle].flight_alt;
    if (source_weapon) {
        p->art_kind = source_weapon->art_kind;
        if (source_weapon->art_kind == UNIT_WEAPON_ART_MODEL) {
            p->art_idx = (int16_t)proj_model_index(source_weapon->art_name);
        } else if (source_weapon->art_kind == UNIT_WEAPON_ART_SPRITE) {
            p->art_idx = (int16_t)proj_sprite_index(source_weapon->art_name,
                                                    source_weapon->art_name);
        }
        p->explosion_idx = source_weapon->explosion_idx;
        /* 65536/turn per legacy tick → radians per sim tick. */
        const float SPIN_TO_RAD = (6.28318530718f / 65536.0f) * 0.5f;
        p->spin_pitch   = (float)source_weapon->spin_pitch   * SPIN_TO_RAD;
        p->spin_heading = (float)source_weapon->spin_heading * SPIN_TO_RAD;
        p->spin_roll    = (float)source_weapon->spin_roll    * SPIN_TO_RAD;
        if (source_weapon->is_gravity && p->speed_ppt > 0.0f) {
            p->gravity_ppt2 =
                projectile_gravity_ppt2(source_weapon->gravity_adjust);
            float rise = 0.0f;
            if (lw) rise = (float)Terrain_SampleHeight(lw, tx, ty)
                         + ((target_handle >= 0 && target_handle < g_unit_count)
                            ? g_units[target_handle].flight_alt : 0.0f)
                         - p->height;
            if (source_weapon->dropped) {
                /* Dropped ordnance keeps the carrier's horizontal run
                 * and simply falls (legacy:246794). */
                p->pitch = 0.0f;
            } else {
                p->pitch = projectile_launch_pitch(p->speed_ppt, len, rise,
                                                   p->gravity_ppt2,
                                                   source_weapon->lob_preferred);
                float cp = cosf(p->pitch);
                p->vel_up_ppt = p->speed_ppt * sinf(p->pitch);
                /* The horizontal term shrinks with pitch, which is what
                 * stretches a lobbed shot's flight time. */
                p->speed_ppt  = p->speed_ppt * (cp > 0.05f ? cp : 0.05f);
            }
        }
    }

    /* TTL = 1.5× the time to traverse the initial distance, so a
     * projectile whose target dodges out of the way still despawns. */
    int ttl = (int)(len / (p->speed_ppt > 0 ? p->speed_ppt : 1) * 1.5f);
    if (ttl < 30) ttl = 30;
    if (ttl > 1200) ttl = 1200;   /* slow weapons at long range */
    p->ttl_ticks = (int16_t)ttl;
    p->alive = 1;
    p->player_id = player_id;
    p->shooter = (int16_t)shooter_handle;
    return slot;
}

int Units_ComputeSplashDamage(int base_damage, int area_of_effect,
                              float edge_effectiveness, int64_t dist_sq) {
    if (base_damage <= 0 || area_of_effect <= 0 || dist_sq < 0) return 0;
    int64_t aoe2 = (int64_t)area_of_effect * (int64_t)area_of_effect;
    if (dist_sq > aoe2) return 0;
    if (edge_effectiveness < 0.0f) edge_effectiveness = 0.0f;
    if (edge_effectiveness > 1.0f) edge_effectiveness = 1.0f;

    float dist_frac = 0.0f;
    if (dist_sq > 0) {
        dist_frac = sqrtf((float)dist_sq) / (float)area_of_effect;
        if (dist_frac > 1.0f) dist_frac = 1.0f;
    }
    float scale = edge_effectiveness
                + (1.0f - edge_effectiveness) * (1.0f - dist_frac);
    int damage = (int)((float)base_damage * scale + 0.5f);
    return damage > 0 ? damage : 0;
}

static int projectile_base_damage_for_unit(const Projectile *p,
                                           const Unit *victim) {
    if (!p) return 0;
    UnitWeapon tmp_wp;
    memset(&tmp_wp, 0, sizeof(tmp_wp));
    tmp_wp.damage = p->damage;
    int n = p->damage_scale_count;
    if (n < 0) n = 0;
    if (n > TAK_DAMAGE_CATEGORY_MAX) n = TAK_DAMAGE_CATEGORY_MAX;
    tmp_wp.damage_scale_count = n;
    if (n > 0) {
        memcpy(tmp_wp.damage_scales, p->damage_scales,
               sizeof(UnitDamageScale) * (size_t)n);
    }
    const UnitDef *vd = victim ? Units_GetDef(victim->def_idx) : NULL;
    return weapon_damage_for_category(&tmp_wp,
                                      vd ? vd->damage_category : "");
}

/* Sacred-site income tier: a mana building overlapping a sacredsite
 * feature earns income × tier (1.0/1.5/2.0 — legacy :235838). Off-site
 * we keep ×1.0 (legacy pays 0) until build-on-henge placement lands. */
static float sacred_income_mult(const UnitDef *d, int32_t wx, int32_t wy) {
    const GameWorld *world = World_Get();
    if (!world || !d) return 1.0f;
    if (!(d->mogrium_income_per_sec > 0.0f)) return 1.0f;
    int32_t half_x = (d->footprint_x > 0 ? d->footprint_x : 2) * 8;
    int32_t half_z = (d->footprint_z > 0 ? d->footprint_z : 2) * 8;
    float best = 1.0f;
    for (int i = 0; i < world->feature_count; i++) {
        const FeatureDef *fd =
            Features_GetByIndex(world->features[i].global_idx);
        if (!fd || fd->sacred_site <= 0.0f) continue;
        int32_t fx = world->features[i].tile_x * 16 + fd->footprint_x * 8;
        int32_t fy = world->features[i].tile_z * 16 + fd->footprint_z * 8;
        /* Overlap test: site centre within the building footprint + a
         * one-tile slack (built beside the henge still counts). */
        if (fx >= wx - half_x - 16 && fx <= wx + half_x + 16 &&
            fy >= wy - half_z - 16 && fy <= wy + half_z + 16 &&
            fd->sacred_site > best) {
            best = fd->sacred_site;
        }
    }
    return best;
}

static void credit_kill(int shooter_handle, const Unit *victim) {
    if (!victim) return;
    if (shooter_handle < 0 || shooter_handle >= g_unit_count) return;
    Unit *shooter = &g_units[shooter_handle];
    if (shooter->alive != 1) return;
    if (victim->player_id == shooter->player_id) return;
    const UnitDef *vdef = Units_GetDef(victim->def_idx);
    if (!vdef) return;
    shooter->experience_pts += vdef->kill_xp_value;
    if (vdef->mogrium_bounty > 0.0f) {
        GameWorld *world = World_Get();
        if (world) {
            Economy_EarnBounty(&world->economy, shooter->player_id,
                               vdef->mogrium_bounty);
        }
    }
}

/* Return fire: a damaged unit with no current target engages its
 * attacker (legacy on-hit acquisition). Passive holds; explicit
 * orders (MOVE/BUILD/attack-ground/...) are not hijacked. */
static void unit_on_damaged(Unit *victim, int shooter_handle) {
    if (!victim || victim->alive != 1 || victim->health <= 0) return;
    if (shooter_handle < 0 || shooter_handle >= g_unit_count) return;
    const Unit *shooter = &g_units[shooter_handle];
    if (shooter->alive != 1) return;
    if (!unit_players_are_enemies(victim->player_id, shooter->player_id))
        return;
    if (victim->aggro_mode == UNIT_AGGRO_PASSIVE) return;
    if (victim->target >= 0) return;
    if (victim->cmd_kind != UNIT_CMD_NONE &&
        victim->cmd_kind != UNIT_CMD_PATROL) return;
    const UnitDef *d = Units_GetDef(victim->def_idx);
    if (!d || d->num_weapons <= 0) return;
    victim->target = (int16_t)shooter_handle;
    /* Patrol is a standing order — engage without losing the route. */
    victim->attack_explicit = 0;
    if (victim->cmd_kind != UNIT_CMD_PATROL)
        victim->cmd_kind = UNIT_CMD_ATTACK;
    unit_clear_path(victim);
}

/* Impact audio: soundhitclass routes through the soundclasses hit
 * table ("flesh" variants when striking a unit, "default" fallback);
 * bare soundhit wavs play directly. Legacy: weapon+0xb0/+0xae. */
static void play_projectile_hit_sound(const Projectile *p) {
    const GameWorld *world = World_Get();
    if (!world || !p) return;
    if (p->hit_sound_class[0]) {
        GameSound_WeaponHit(p->hit_sound_class, "flesh", 0x7f,
                            p->world_x, p->world_y,
                            world->cam_x, world->cam_y,
                            world->viewport_w, world->viewport_h);
    } else if (p->hit_sound[0]) {
        GameSound_PlayWorldWav(p->hit_sound, 0x7f, p->world_x, p->world_y,
                               world->cam_x, world->cam_y,
                               world->viewport_w, world->viewport_h);
    }
}

static void apply_projectile_area_damage(const Projectile *p) {
    if (!p) return;
    int aoe = p->area_of_effect;
    if (aoe <= 0) return;
    int64_t aoe2 = (int64_t)aoe * aoe;
    for (int ui = 0; ui < g_unit_count; ui++) {
        Unit *victim = &g_units[ui];
        if (victim->alive != 1) continue;
        if (!p->friendly_fire &&
            !unit_players_are_enemies(victim->player_id, p->player_id)) continue;
        int64_t vx = victim->world_x - p->world_x;
        int64_t vy = victim->world_y - p->world_y;
        int64_t d2 = vx * vx + vy * vy;
        if (d2 > aoe2) continue;
        int base_damage = projectile_base_damage_for_unit(p, victim);
        int damage = Units_ComputeSplashDamage(base_damage, aoe,
                                                p->edge_effectiveness, d2);
        if (damage <= 0) continue;
        victim->health -= damage;
        if (victim->health <= 0) {
            credit_kill(p->shooter, victim);
            apply_killed(victim, ui);
        } else {
            unit_on_damaged(victim, p->shooter);
        }
    }
}

static int64_t point_segment_dist2_i32(int32_t px, int32_t py,
                                       int32_t ax, int32_t ay,
                                       int32_t bx, int32_t by) {
    float abx = (float)(bx - ax);
    float aby = (float)(by - ay);
    float apx = (float)(px - ax);
    float apy = (float)(py - ay);
    float len2 = abx * abx + aby * aby;
    float t = len2 > 0.0001f ? (apx * abx + apy * aby) / len2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float cx = (float)ax + abx * t;
    float cy = (float)ay + aby * t;
    float dx = (float)px - cx;
    float dy = (float)py - cy;
    return (int64_t)(dx * dx + dy * dy + 0.5f);
}

/* Impact: play the hit sound and the weapon's explosionclass sprite
 * where legacy spawns it (legacy:245025). */
static void projectile_impact_fx(const Projectile *p, uint32_t seed) {
    play_projectile_hit_sound(p);
    spawn_impact_effect(p->explosion_idx, p->world_x, p->world_y,
                        (int32_t)p->height, seed);
}

/* Detonate where the shot came down: splash when the weapon has an
 * areaofeffect, else a direct hit on whatever stands there. Legacy
 * picks between the two on the same field (legacy:245029). */
static void projectile_detonate(Projectile *p, int idx) {
    projectile_impact_fx(p, (uint32_t)idx);
    if (p->area_of_effect > 0) {
        apply_projectile_area_damage(p);
    } else {
        for (int ui = 0; ui < g_unit_count; ui++) {
            Unit *v = &g_units[ui];
            if (v->alive != 1) continue;
            int64_t vx = v->world_x - p->world_x;
            int64_t vy = v->world_y - p->world_y;
            if (vx * vx + vy * vy > (int64_t)24 * 24) continue;
            v->health -= projectile_base_damage_for_unit(p, v);
            if (v->health <= 0) {
                credit_kill(p->shooter, v);
                apply_killed(v, ui);
            } else {
                unit_on_damaged(v, p->shooter);
            }
            break;
        }
    }
    p->alive = 0;
}

/* Per-tick: advance projectiles, hit-test against target, apply damage. */
static void tick_projectiles(void) {
    /* Impact effects are pure visuals — hold each for a second, the
     * draw stops once the sequence runs out of frames. */
    for (int i = 0; i < g_proj_effect_count; i++) {
        ProjectileEffect *e = &g_proj_effects[i];
        if (!e->alive) continue;
        if (++e->age_ticks >= 60) e->alive = 0;
    }
    while (g_proj_effect_count > 0 &&
           !g_proj_effects[g_proj_effect_count - 1].alive) {
        g_proj_effect_count--;
    }

    for (int i = 0; i < g_projectile_count; i++) {
        Projectile *p = &g_projectiles[i];
        if (!p->alive) continue;
        if (--p->ttl_ticks <= 0) { p->alive = 0; continue; }
        /* Beams already dealt their damage at fire — hold, don't move. */
        if (p->is_beam) continue;
        p->age_ticks++;
        int32_t old_x = p->world_x;
        int32_t old_y = p->world_y;
        /* Carry the fraction of a pixel each step owes. Rounding the
         * step to a whole pixel instead threw the shot off its line:
         * a cast toward zero cost a shot heading up and left most of
         * its vertical travel, so it flew past a target 500 px away
         * about 50 px to one side and never hit anything. */
        float sx = p->sub_x + p->dir_x * p->speed_ppt;
        float sy = p->sub_y + p->dir_y * p->speed_ppt;
        int32_t step_x = (int32_t)floorf(sx);
        int32_t step_y = (int32_t)floorf(sy);
        p->sub_x = sx - (float)step_x;
        p->sub_y = sy - (float)step_y;
        p->world_x += step_x;
        p->world_y += step_y;
        /* Gravity first, then integrate — the legacy order
         * (legacy:246654-246658). Flat shots simply hug the ground. */
        const GameWorld *gw = World_Get();
        if (p->gravity_ppt2 > 0.0f) {
            p->vel_up_ppt -= p->gravity_ppt2;
            p->height     += p->vel_up_ppt;
        } else if (gw) {
            p->height = (float)Terrain_SampleHeight(gw, p->world_x, p->world_y);
        }
        /* Orientation: spin terms win, otherwise the model points along
         * its velocity vector (legacy:246661-246676). */
        if (p->spin_pitch != 0.0f || p->spin_heading != 0.0f ||
            p->spin_roll != 0.0f) {
            p->pitch   += p->spin_pitch;
            p->heading += p->spin_heading;
            p->roll    += p->spin_roll;
        } else if (p->gravity_ppt2 > 0.0f && p->speed_ppt > 0.0f) {
            p->pitch = atan2f(p->vel_up_ppt, p->speed_ppt);
        }
        /* A lobbed ground shot that fell short still has to go off.
         * Only ground shots: a shot with a live target is governed by
         * the target test below, so a descending arrow aimed downhill
         * is not stopped by the plateau it is leaving. */
        if (p->target < 0 && p->gravity_ppt2 > 0.0f &&
            p->vel_up_ppt < 0.0f && p->age_ticks > 2) {
            float ground = gw
                ? (float)Terrain_SampleHeight(gw, p->world_x, p->world_y)
                : 0.0f;
            if (p->height <= ground) {
                p->height = ground;
                projectile_detonate(p, i);
                continue;
            }
        }
        /* Hit test against target unit. Apply damage on close approach
         * (within 16 px ≈ 1 tile) or if target moved, hit at current
         * world_x/y closest enemy. */
        if (p->target < 0) {
            /* Ground shot: detonate on reaching the aim point. */
            int64_t d2 = point_segment_dist2_i32(p->dest_x, p->dest_y,
                                                 old_x, old_y,
                                                 p->world_x, p->world_y);
            if (d2 <= (int64_t)24 * 24) {
                p->world_x = p->dest_x;
                p->world_y = p->dest_y;
                projectile_detonate(p, i);
            }
            continue;
        }
        if (p->target >= 0 && p->target < g_unit_count) {
            Unit *t = &g_units[p->target];
            if (t->alive == 1) {
                int64_t d2 = point_segment_dist2_i32(t->world_x, t->world_y,
                                                      old_x, old_y,
                                                      p->world_x, p->world_y);
                if (d2 <= (int64_t)24*24) {
                    projectile_impact_fx(p, (uint32_t)i);
                    if (p->area_of_effect > 0) {
                        apply_projectile_area_damage(p);
                        p->alive = 0;
                        continue;
                    }
                    t->health -= projectile_base_damage_for_unit(p, t);
                    if (t->health <= 0) {
                        credit_kill(p->shooter, t);
                        apply_killed(t, p->target);
                    } else {
                        unit_on_damaged(t, p->shooter);
                    }
                    p->alive = 0;
                    continue;
                }
            } else {
                /* Target gone — projectile despawns silently. */
                p->alive = 0;
                continue;
            }
        }
    }
    /* Compact tail of fully-dead projectiles. */
    while (g_projectile_count > 0 &&
           !g_projectiles[g_projectile_count - 1].alive) {
        g_projectile_count--;
    }
}

/* Projection tuning (R3, PHASE_C_3DO.md §5.R3). Tunable via the '-'/'='
 * and '['/']' debug keys in ingame.c.
 *
 * TA_SCALE is not a free parameter: the legacy projector takes the
 * integer part of the 16.16 model vertex as the screen pixel
 * (legacy:197689), so one model unit is one world pixel and the scale
 * is exactly 1/65536. The shipped data agrees. ARAWALL spans 32.0
 * model units on its 2x2 footprint, ARANGATE 224.0 on its 14x4, ARAAT
 * 48.0 on its 3x3, all footprint x 16 px to the pixel. The old
 * hand-tuned 0.000021 drew every model 37% oversized, so walls
 * overhung the cells they were placed on. */
static float g_ta_scale = 1.0f / 65536.0f;
/* Sim-side copy of TA_SCALE: the render tunable must never leak
 * into sim results (spawn spots have to stay deterministic). */
#define UNIT_MODEL_TO_WORLD (1.0f / 65536.0f)
/* Legacy projector: sy = −z − (y >> 1) (legacy:197689) —
 * the camera tilt is exactly 0.5, not tan(30°). The old 0.577 made
 * every model taller than the original and needed per-def y-squash
 * hacks to compensate. */
static float g_tan_tilt = 0.5f;

/* Backface culling. Empirically determined: TAK 3DOs are wound such
 * that our standard CCW-screen-space cross-product test culls the
 * VISIBLE side. Inverting the test (cull when cross > 0 instead of
 * < 0) keeps the right faces. The decomp's literal formula at
 * the legacy reference ~197804 reads "keep when cross >= 0", but with
 * the engine's screen-Y convention that translates to "keep when
 * world-space CCW from front" — which is the opposite sign of our
 * SDL screen-Y-down derivation. So invert defaults to ON. */
static int g_backface_cull_on     = 1;
/* Invert now defaults OFF: the projection carries the legacy
 * handedness mirror (see submit_run), so the literal legacy rule
 * `cross >= 0 → draw` (legacy:197804) selects the correct
 * faces directly. The old invert=1 compensated for the missing mirror
 * — leaving both on double-corrects back to the wrong face set. */
static int g_backface_cull_invert = 0;
static int g_health_bars_on       = 0;   /* Visual Options: Show Damage */

int  Units_GetBackfaceCullOn(void)      { return g_backface_cull_on; }
void Units_SetBackfaceCullOn(int on)    { g_backface_cull_on = on ? 1 : 0; }
int  Units_GetBackfaceCullInvert(void)  { return g_backface_cull_invert; }
int  Units_GetHealthBarsOn(void)        { return g_health_bars_on; }
void Units_ToggleHealthBars(void)       { g_health_bars_on = !g_health_bars_on; }
void  Units_SetHealthBarsOn(int on)      { g_health_bars_on = on ? 1 : 0; }

/* ── Selection state ──────────────────────────────────────────── */
static int g_selection[UNITS_SELECTION_MAX];
static int g_selection_count = 0;

const char *Units_GetSelectedName(void) {
    if (g_selection_count == 0) return NULL;
    int h = g_selection[0];
    if (h < 0 || h >= g_unit_count || g_units[h].alive < 1) return NULL;
    const UnitDef *d = Units_GetDef(g_units[h].def_idx);
    if (!d) return NULL;
    /* FBI's `name` field is the character's display name ("Elsin",
     * "Lokken", "Kirenna", "Thirsha" for monarchs). Fall through
     * to description (faction tag) then unitname as a last resort. */
    if (d->display_name[0]) return d->display_name;
    if (d->description[0])  return d->description;
    return d->unitname;
}

const char *Units_GetSelectedStatus(void) {
    if (g_selection_count == 0) return NULL;
    int h = g_selection[0];
    if (h < 0 || h >= g_unit_count || g_units[h].alive < 1) return NULL;
    const Unit *u = &g_units[h];
    switch (u->cmd_kind) {
        case UNIT_CMD_REPAIR:  return "Repairing";
        case UNIT_CMD_RECLAIM: return "Clearing";
        case UNIT_CMD_LOAD:    return "Loading";
        case UNIT_CMD_UNLOAD:  return "Unloading";
        default: break;
    }
    /* Manual §IV.2 status strings — derived from anim_state. */
    switch (u->anim_state) {
        case UNIT_ANIM_MOVING:    return "Moving";
        case UNIT_ANIM_ATTACKING: return "Engaging target";
        case UNIT_ANIM_BUILDING:  return "Building";
        case UNIT_ANIM_DYING:     return "Dying";
        case UNIT_ANIM_IDLE:
        default:                  return "Standby";
    }
}

const UnitDef *Units_GetSelectedDef(void) {
    if (g_selection_count == 0) return NULL;
    int h = g_selection[0];
    if (h < 0 || h >= g_unit_count || g_units[h].alive < 1) return NULL;
    return Units_GetDef(g_units[h].def_idx);
}

void Units_GetSelectedHealth(int *out_hp, int *out_max) {
    if (out_hp)  *out_hp  = 0;
    if (out_max) *out_max = 0;
    if (g_selection_count == 0) return;
    int h = g_selection[0];
    if (h < 0 || h >= g_unit_count || g_units[h].alive < 1) return;
    if (out_hp)  *out_hp  = g_units[h].health;
    if (out_max) *out_max = g_units[h].max_health;
}

int g_units_get_player(int handle) {
    if (handle < 0 || handle >= g_unit_count) return 0;
    if (g_units[handle].alive != 1) return 0;
    return g_units[handle].player_id;
}

/* Veteran rank = accumulated XP / experiencepoints, capped at 10
 * (legacy cap @0x6188bc; 0 = not yet a veteran). */
int Units_GetVeteranLevel(int handle) {
    if (handle < 0 || handle >= g_unit_count) return 0;
    const Unit *u = &g_units[handle];
    if (u->alive != 1) return 0;
    const UnitDef *d = Units_GetDef(u->def_idx);
    if (!d || d->kill_xp_value <= 0) return 0;
    int lvl = (int)(u->experience_pts / d->kill_xp_value);
    if (lvl < 0) lvl = 0;
    if (lvl > 10) lvl = 10;
    return lvl;
}

void Units_DebugSetVeteranLevel(int handle, int level) {
    if (handle < 0 || handle >= g_unit_count) return;
    Unit *u = &g_units[handle];
    const UnitDef *d = Units_GetDef(u->def_idx);
    if (!d || d->kill_xp_value <= 0) return;
    if (level < 0) level = 0;
    u->experience_pts = level * d->kill_xp_value;
}

int Units_GetSelectedVeteranLevel(void) {
    if (g_selection_count <= 0) return 0;
    return Units_GetVeteranLevel(g_selection[0]);
}

static int unit_terrain_walkable(const GameWorld *w, const UnitDef *def,
                                 int32_t x, int32_t y);
static const MoveClassDef *unit_move_class(const GameWorld *w,
                                           const UnitDef *def);

/* Could this unit legally stand at (x,y)? Terrain/water rules only. */
int Units_CanStandAt(int handle, int32_t world_x, int32_t world_y) {
    if (handle < 0 || handle >= g_unit_count) return 0;
    const UnitDef *d = Units_GetDef(g_units[handle].def_idx);
    if (!d) return 0;
    return unit_terrain_walkable(World_Get(), d, world_x, world_y);
}

int Units_IsUnderConstruction(int handle) {
    if (handle < 0 || handle >= g_unit_count) return 0;
    if (g_units[handle].alive != 1) return 0;
    return g_units[handle].under_construction != 0;
}

int Units_SelectionHasBuilder(void) {
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count) continue;
        const Unit *u = &g_units[h];
        if (u->alive != 1 || u->player_id != 1) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (d && (d->cap_flags & UNIT_CAP_BUILDER) && d->worker_time > 0.0f)
            return 1;
    }
    return 0;
}

/* HUD-side helper. The Unit struct is opaque to ui/hud.c (no header
 * exposes it), so the HUD looks up the selected unit's def via this
 * thin accessor. Returns -1 if the handle isn't alive. */
int g_units_get_def_idx(int handle) {
    if (handle < 0 || handle >= g_unit_count) return -1;
    if (g_units[handle].alive != 1) return -1;
    return (int)g_units[handle].def_idx;
}

int Units_PickAt(int32_t world_x, int32_t world_y, int radius) {
    const GameWorld *world = World_Get();
    /* Pass 1: point-inside-footprint. Centre-distance alone can never
     * pick a large structure (an 8×20 keep spans 320px — clicks on its
     * walls sit far outside any sane radius). Smallest footprint wins
     * so a unit standing on a building picks over the building. */
    int best = -1;
    int64_t best_area = INT64_MAX;
    for (int i = 0; i < g_unit_count; i++) {
        const Unit *u = &g_units[i];
        if (u->alive != 1) continue;
        if (!unit_visible_to_local_player(world, u)) continue;
        /* Units are DRAWN lifted by terrain height (sy = z - y/2,
         * legacy :197689), so hit-test against where the unit actually
         * appears — otherwise clicks miss by the elevation offset. */
        int32_t uy = u->world_y - (int32_t)(((float)Terrain_SampleHeight(
                         world, u->world_x, u->world_y) + u->flight_alt) * g_tan_tilt);
        const UnitDef *d = Units_GetDef(u->def_idx);
        int hw = (d && d->footprint_x > 0) ? d->footprint_x * 8 : 16;
        int hh = (d && d->footprint_z > 0) ? d->footprint_z * 8 : 16;
        if (world_x >= u->world_x - hw && world_x <= u->world_x + hw &&
            world_y >= uy - hh && world_y <= uy + hh) {
            int64_t area = (int64_t)hw * hh;
            if (area < best_area) { best_area = area; best = i; }
        }
    }
    if (best >= 0) return best;

    /* Pass 2: nearest centre within radius (small/mobile units). */
    int64_t best_d2 = (int64_t)radius * radius;
    for (int i = 0; i < g_unit_count; i++) {
        const Unit *u = &g_units[i];
        if (u->alive != 1) continue;
        if (!unit_visible_to_local_player(world, u)) continue;
        int32_t uy2 = u->world_y - (int32_t)(((float)Terrain_SampleHeight(
                          world, u->world_x, u->world_y) + u->flight_alt) * g_tan_tilt);
        int64_t dx = (int64_t)(u->world_x - world_x);
        int64_t dy = (int64_t)(uy2 - world_y);
        int64_t d2 = dx*dx + dy*dy;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    return best;
}

/* Click to inspect. The original shows the same panel for a unit you
 * do not own, portrait, name and health, and simply offers it no
 * orders. Every order path here already checks ownership, so holding
 * an enemy in the selection is safe. */
int Units_SelectForInspect(int handle) {
    if (handle < 0 || handle >= g_unit_count) return 0;
    const Unit *u = &g_units[handle];
    if (u->alive != UNIT_ALIVE_ACTIVE) return 0;
    if (!unit_visible_to_local_player(World_Get(), u)) return 0;
    Units_SelectSingle(handle);
    return 1;
}

void Units_SelectSingle(int handle) {
    g_selection_count = 0;
    if (handle >= 0 && handle < g_unit_count && g_units[handle].alive == 1) {
        g_selection[0] = handle;
        g_selection_count = 1;
    }
}

static int selection_find(int handle) {
    for (int s = 0; s < g_selection_count; s++)
        if (g_selection[s] == handle) return s;
    return -1;
}

void Units_SelectAdd(int handle) {
    if (handle < 0 || handle >= g_unit_count) return;
    if (g_units[handle].alive != 1) return;
    if (g_selection_count >= UNITS_SELECTION_MAX) return;
    if (selection_find(handle) >= 0) return;
    g_selection[g_selection_count++] = handle;
}

void Units_SelectToggle(int handle) {
    int at = selection_find(handle);
    if (at >= 0) {
        g_selection[at] = g_selection[--g_selection_count];
        return;
    }
    Units_SelectAdd(handle);
}

int Units_SelectInRect(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       int additive) {
    if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int32_t t = y0; y0 = y1; y1 = t; }
    if (!additive) g_selection_count = 0;
    for (int i = 0; i < g_unit_count; i++) {
        const Unit *u = &g_units[i];
        if (u->alive != 1) continue;
        if (u->player_id != 1) continue;
        if (u->world_x < x0 || u->world_x > x1) continue;
        if (u->world_y < y0 || u->world_y > y1) continue;
        Units_SelectAdd(i);
    }
    return g_selection_count;
}

/* Control groups: Ctrl+digit stores, digit recalls. Handles are stable
 * for a unit's lifetime, so groups survive between recalls; dead
 * members are dropped at recall time. */
static int g_ctrl_group[10][UNITS_SELECTION_MAX];
static int g_ctrl_group_count[10];

void Units_AssignControlGroup(int group) {
    if (group < 0 || group > 9) return;
    memcpy(g_ctrl_group[group], g_selection,
           (size_t)g_selection_count * sizeof(g_selection[0]));
    g_ctrl_group_count[group] = g_selection_count;
}

int Units_RecallControlGroup(int group) {
    if (group < 0 || group > 9) return g_selection_count;
    g_selection_count = 0;
    int kept = 0;
    for (int s = 0; s < g_ctrl_group_count[group]; s++) {
        int h = g_ctrl_group[group][s];
        if (h < 0 || h >= g_unit_count || g_units[h].alive != 1) continue;
        g_ctrl_group[group][kept++] = h;
        Units_SelectAdd(h);
    }
    g_ctrl_group_count[group] = kept;
    return g_selection_count;
}

const int *Units_GetSelection(int *out_count) {
    if (out_count) *out_count = g_selection_count;
    return g_selection;
}

/* Start the walk cycle unless one is already running. Orders arrive in
 * bursts and each one used to stack another thread on the sixteen-slot
 * engine. Legacy never starts walk at all, its own watcher threads do
 * (docs/notes/2026-09-09-cob-entry-points.md). */
static void unit_kick_walk(Unit *u) {
    if (!u->cob) return;
    if (u->under_construction) return;   /* the MOVE tick kicks it later */
    if (u->walk_thread_slot >= 0 &&
        Cob_IsThreadAlive(u->cob, u->walk_thread_slot)) return;
    u->walk_thread_slot = (int8_t)unit_start_script(u, "walk", NULL, 0);
}

void Units_CommandMoveSelected(int32_t world_x, int32_t world_y) {
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1) continue;
        if (u->player_id != 1) continue;
        /* Move on an immobile production structure sets its rally
         * point (manual: units emerging rally to the Move target). */
        {
            const UnitDef *ud = Units_GetDef(u->def_idx);
            if (ud && ud->max_velocity <= 0.0f &&
                (ud->cap_flags & UNIT_CAP_BUILDER)) {
                Units_FactorySetRally(h, world_x, world_y);
                continue;
            }
        }
        u->cmd_kind = UNIT_CMD_MOVE;
        u->cmd_x    = world_x;
        u->cmd_y    = world_y;
        u->target   = -1;
        u->build_target = -1;   /* detach from any nanoframe */
        unit_clear_path(u);
        /* Kick off the walk script. The per-tick MOVE handler keeps it
         * alive if it terminates mid-move. */
        unit_kick_walk(u);
    }
}

void Units_CommandMoveUnit(int handle, int32_t world_x, int32_t world_y) {
    if (handle < 0 || handle >= g_unit_count) return;
    Unit *u = &g_units[handle];
    if (u->alive != 1) return;
    u->cmd_kind = UNIT_CMD_MOVE;
    u->cmd_x = world_x;
    u->cmd_y = world_y;
    u->target = -1;
    u->build_target = -1;
    unit_clear_path(u);
    unit_kick_walk(u);
}

/* Standing patrol order for one unit (mission-scripted patrols).
 * Anchor = current position, so the unit bounces anchor↔target. */
void Units_CommandPatrolUnit(int handle, int32_t world_x, int32_t world_y) {
    if (handle < 0 || handle >= g_unit_count) return;
    Unit *u = &g_units[handle];
    if (u->alive != 1) return;
    u->cmd_kind = UNIT_CMD_PATROL;
    u->cmd_x = world_x;
    u->cmd_y = world_y;
    u->patrol_x = u->world_x;
    u->patrol_y = u->world_y;
    u->target = -1;
    u->build_target = -1;
    unit_clear_path(u);
    unit_kick_walk(u);
}

void Units_CommandAttackGroundSelected(int32_t world_x, int32_t world_y) {
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1 || u->player_id != 1) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!d || d->num_weapons <= 0) continue;
        u->cmd_kind = UNIT_CMD_ATTACK_GROUND;
        u->cmd_x = world_x;
        u->cmd_y = world_y;
        u->target = -1;
        u->build_target = -1;
        unit_clear_path(u);
    }
}

void Units_CommandPatrolSelected(int32_t world_x, int32_t world_y) {
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1) continue;
        if (u->player_id != 1) continue;
        u->cmd_kind = UNIT_CMD_PATROL;
        u->cmd_x = world_x;
        u->cmd_y = world_y;
        u->patrol_x = u->world_x;
        u->patrol_y = u->world_y;
        u->target = -1;
        u->build_target = -1;
        unit_clear_path(u);
        unit_kick_walk(u);
    }
}

void Units_CommandGuardSelected(int target_handle) {
    if (target_handle < 0 || target_handle >= g_unit_count) return;
    Unit *guarded = &g_units[target_handle];
    if (guarded->alive != 1 || guarded->player_id != 1) return;
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count || h == target_handle) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1) continue;
        if (u->player_id != 1) continue;
        u->cmd_kind = UNIT_CMD_GUARD;
        u->target = (int16_t)target_handle;
        u->cmd_x = guarded->world_x;
        u->cmd_y = guarded->world_y;
        unit_clear_path(u);
    }
}

static void command_attack_unit_ex(int handle, int target_handle, int respect_fog) {
    if (handle < 0 || handle >= g_unit_count) return;
    if (target_handle < 0 || target_handle >= g_unit_count) return;
    Unit *u = &g_units[handle];
    Unit *t = &g_units[target_handle];
    if (u->alive != 1 || t->alive != 1) return;
    if (handle == target_handle) return;
    if (!unit_players_are_enemies(u->player_id, t->player_id)) return;
    if (respect_fog && !unit_can_see_target(u, t)) return;
    u->cmd_kind = UNIT_CMD_ATTACK;
    u->attack_explicit = 1;
    u->target = (int16_t)target_handle;
    unit_clear_path(u);
}

void Units_CommandAttackUnit(int handle, int target_handle) {
    command_attack_unit_ex(handle, target_handle, 1);
}

void Units_CommandAttackUnitScript(int handle, int target_handle) {
    command_attack_unit_ex(handle, target_handle, 0);
}

void Units_SetOwner(int handle, int player_id, int team_color_idx) {
    if (handle < 0 || handle >= g_unit_count) return;
    Unit *u = &g_units[handle];
    if (u->alive != 1) return;
    if (player_id < 0) player_id = 0;
    if (player_id > 10) player_id = 10;
    if (team_color_idx < 0 || team_color_idx > 11) {
        team_color_idx = player_id > 0 ? (player_id - 1) % 12 : 0;
    }
    u->player_id = (uint8_t)player_id;
    u->team_color_idx = (uint8_t)team_color_idx;
    /* A captured gate opens for its new owner: force the next
     * occupancy tick to re-stamp the footprint with the new owner. */
    u->occ_pending = 1;
}

void Units_SetVelocity(int handle, int32_t velocity) {
    if (handle < 0 || handle >= g_unit_count) return;
    Unit *u = &g_units[handle];
    if (u->alive != 1) return;
    u->velocity = velocity;
}

void Units_CommandAttackSelected(int target_handle) {
    if (target_handle < 0 || target_handle >= g_unit_count) return;
    if (g_units[target_handle].alive != 1) return;
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1) continue;
        if (u->player_id != 1) continue;
        if (h == target_handle) continue;  /* don't attack self */
        if (!unit_players_are_enemies(u->player_id,
                                      g_units[target_handle].player_id)) continue;
        if (!unit_can_see_target(u, &g_units[target_handle])) continue;
        u->cmd_kind = UNIT_CMD_ATTACK;
        u->attack_explicit = 1;
        u->target   = (int16_t)target_handle;
        unit_clear_path(u);
    }
}

void Units_CommandRepairSelected(int target_handle) {
    if (target_handle < 0 || target_handle >= g_unit_count) return;
    if (g_units[target_handle].alive != 1) return;
    if (g_units[target_handle].player_id != 1) return;
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count || h == target_handle) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1 || u->player_id != 1) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!unit_def_can_repair(d)) continue;
        if (g_units[target_handle].under_construction) {
            /* Nanoframe: resume construction (legacy HelpBuild). */
            if (d->max_velocity <= 0.0f) continue;
            u->cmd_kind = UNIT_CMD_BUILD;
            u->build_target = (int16_t)target_handle;
            u->target = -1;
            u->cmd_x = g_units[target_handle].world_x;
            u->cmd_y = g_units[target_handle].world_y;
            unit_clear_path(u);
            continue;
        }
        u->cmd_kind = UNIT_CMD_REPAIR;
        u->target = (int16_t)target_handle;
        u->cmd_x = g_units[target_handle].world_x;
        u->cmd_y = g_units[target_handle].world_y;
        unit_clear_path(u);
    }
}

/* Legacy's CLEAR order (type 0xc) only ever resolves onto a map cell:
 * a live feature becomes RECLAIM, a corpse cell RESURRECT, empty ground
 * RECLAIMAREA, and a live unit under the cursor gets no order at all
 * (legacy:187127-187207). We keep the unit-handle form for the
 * scripted/AI callers we already have; the feature form below is the
 * one the sweep cursor uses. */
void Units_CommandReclaimSelected(int target_handle) {
    if (target_handle < 0 || target_handle >= g_unit_count) return;
    if (g_units[target_handle].alive != 1) return;
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count || h == target_handle) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1 || u->player_id != 1) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!d || !(d->cap_flags & UNIT_CAP_RECLAIM)) continue;
        u->cmd_kind = UNIT_CMD_RECLAIM;
        u->target = (int16_t)target_handle;
        u->cmd_x = g_units[target_handle].world_x;
        u->cmd_y = g_units[target_handle].world_y;
        u->reclaim_tile_x = -1;
        u->reclaim_tile_y = -1;
        u->reclaim_accum = 0.0f;
        unit_clear_path(u);
    }
}

int Units_CommandReclaimFeatureSelected(int32_t world_x, int32_t world_y) {
    GameWorld *w = World_Get();
    if (!w) return 0;
    int fi = Features_FindReclaimableAt(w, world_x, world_y);
    if (fi < 0) return 0;
    int32_t cx = world_x, cy = world_y;
    Features_InstanceCentre(w, fi, &cx, &cy);
    int issued = 0;
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1 || u->player_id != 1) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        /* canreclaim gates the order (legacy:187127, cap parse
         * legacy:163041). An immobile unit never reaches the cell. */
        if (!d || !(d->cap_flags & UNIT_CAP_RECLAIM)) continue;
        if (d->max_velocity <= 0.0f) continue;
        u->cmd_kind = UNIT_CMD_RECLAIM;
        u->target = -1;
        u->build_target = -1;
        u->cmd_x = cx;
        u->cmd_y = cy;
        u->reclaim_tile_x = (int16_t)w->features[fi].tile_x;
        u->reclaim_tile_y = (int16_t)w->features[fi].tile_z;
        u->reclaim_accum = 0.0f;
        unit_clear_path(u);
        if (u->cob) {
            int slot = Cob_StartThreadByName(u->cob, "walk", NULL, 0);
            u->walk_thread_slot = (int8_t)slot;
        }
        issued++;
    }
    return issued;
}

/* Re-resolve the feature a unit is clearing. Held by tile, not by
 * array index, because removing any feature compacts world->features.
 * Returns -1 when the feature is gone (someone else cleared it). */
static int unit_reclaim_feature_idx(const Unit *u, const GameWorld *w) {
    if (!w || !w->features) return -1;
    if (u->reclaim_tile_x < 0 || u->reclaim_tile_y < 0) return -1;
    for (int i = 0; i < w->feature_count; i++) {
        if ((int)w->features[i].tile_x == (int)u->reclaim_tile_x &&
            (int)w->features[i].tile_z == (int)u->reclaim_tile_y)
            return i;
    }
    return -1;
}

void Units_CommandLoadSelected(int target_handle) {
    if (target_handle < 0 || target_handle >= g_unit_count) return;
    if (g_units[target_handle].alive != 1) return;
    if (g_units[target_handle].player_id != 1) return;
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count || h == target_handle) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1 || u->player_id != 1) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!d || !((d->cap_flags & UNIT_CAP_LOAD) ||
                    (d->cap_flags & UNIT_CAP_TRANSPORT))) continue;
        u->cmd_kind = UNIT_CMD_LOAD;
        u->target = (int16_t)target_handle;
        u->cmd_x = g_units[target_handle].world_x;
        u->cmd_y = g_units[target_handle].world_y;
        unit_clear_path(u);
    }
}

void Units_CommandUnloadSelected(int32_t world_x, int32_t world_y) {
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1 || u->player_id != 1) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!d || !(d->cap_flags & UNIT_CAP_TRANSPORT)) continue;
        u->cmd_kind = UNIT_CMD_UNLOAD;
        u->target = -1;
        u->cmd_x = world_x;
        u->cmd_y = world_y;
        unit_clear_path(u);
    }
}

void Units_CommandStopSelected(void) {
    /* Mirror legacy STOP_UNITORDER: clear move/attack target, halt
     * velocity, return to idle. The animation state machine in
     * Units_TickCombat will transition the unit back to UNIT_ANIM_IDLE
     * once cmd_kind == NONE and target == -1. */
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1) continue;
        if (u->player_id != 1) continue;
        u->cmd_kind = UNIT_CMD_NONE;
        u->target   = -1;
        u->build_target = -1;
        u->cmd_x    = u->world_x;
        u->cmd_y    = u->world_y;
        u->velocity = 0; u->cur_speed_ppt = 0.0f;
        unit_clear_path(u);
    }
}

void Units_CommandSetAggroSelected(int aggro_mode) {
    if (aggro_mode < UNIT_AGGRO_PASSIVE || aggro_mode > UNIT_AGGRO_OFFENSIVE) return;
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1) continue;
        if (u->player_id != 1) continue;
        u->aggro_mode = (uint8_t)aggro_mode;
        /* Passive units must drop any in-flight auto-target. */
        if (aggro_mode == UNIT_AGGRO_PASSIVE && u->cmd_kind == UNIT_CMD_NONE) {
            u->target = -1;
        }
    }
}

void Units_CommandSetWeaponSlotSelected(int slot) {
    if (slot < 0 || slot > 2) return;
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count) continue;
        Unit *u = &g_units[h];
        if (u->alive != 1) continue;
        if (u->player_id != 1) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!d || slot >= d->num_weapons) continue;  /* skip if unit lacks that weapon */
        u->weapon_slot = (uint8_t)slot;
    }
}

int Units_GetSelectedAggroMode(void) {
    if (g_selection_count == 0) return -1;
    int h = g_selection[0];
    if (h < 0 || h >= g_unit_count || g_units[h].alive != 1) return -1;
    return (int)g_units[h].aggro_mode;
}

int Units_GetSelectedWeaponSlot(void) {
    if (g_selection_count == 0) return -1;
    int h = g_selection[0];
    if (h < 0 || h >= g_unit_count || g_units[h].alive != 1) return -1;
    return (int)g_units[h].weapon_slot;
}

/* ── Building construction ──────────────────────────────────────── */

int Units_ExpandYardmap(const UnitDef *d, uint8_t *out, int max) {
    if (!d || !out || !d->yardmap) return 0;
    int fx = d->footprint_x > 0 ? d->footprint_x : 0;
    int fz = d->footprint_z > 0 ? d->footprint_z : 0;
    int cells = fx * fz;
    if (cells <= 0 || cells > max) return 0;
    /* Parsed once at load by Occ_BuildYardmap (legacy char table,
     * last code repeated to fill the footprint). */
    memcpy(out, d->yardmap, (size_t)cells);
    return cells;
}

/* The sacred site covering a world point, or NULL. */
static const FeatureDef *sacred_feature_at(const GameWorld *world,
                                           int32_t px, int32_t py) {
    if (!world) return NULL;
    for (int i = 0; i < world->feature_count; i++) {
        const FeatureDef *fd =
            Features_GetByIndex(world->features[i].global_idx);
        if (!fd || fd->sacred_site <= 0.0f) continue;
        int fpx = fd->footprint_x > 0 ? fd->footprint_x : 1;
        int fpz = fd->footprint_z > 0 ? fd->footprint_z : 1;
        int32_t fx0 = (int32_t)world->features[i].tile_x * 16;
        int32_t fy0 = (int32_t)world->features[i].tile_z * 16;
        if (px >= fx0 && px < fx0 + fpx * 16 &&
            py >= fy0 && py < fy0 + fpz * 16)
            return fd;
    }
    return NULL;
}

/* Count the yardmap-'S' cells standing on a sacred site and require
 * that count to reach the pad's own cell count, so the whole pad is
 * covered. With no pad under any 'S' cell the target stays at the
 * legacy sentinel and the site can never qualify (legacy:218858-218889). */
static int sacred_site_covered(const GameWorld *world, const uint8_t *yard,
                               int ycells, int fx, int fz,
                               int32_t x0, int32_t y0) {
    int need = 10000;
    int covered = 0;
    for (int cz = 0; cz < fz; cz++) {
        for (int cx = 0; cx < fx; cx++) {
            if (ycells > 0 && !(yard[cz * fx + cx] & TAK_YARD_SACRED))
                continue;
            const FeatureDef *fd =
                sacred_feature_at(world, x0 + cx * 16 + 8, y0 + cz * 16 + 8);
            if (!fd) continue;
            int fpx = fd->footprint_x > 0 ? fd->footprint_x : 1;
            int fpz = fd->footprint_z > 0 ? fd->footprint_z : 1;
            need = fpx * fpz;
            covered++;
        }
    }
    return covered >= need;
}

/* Floor division, so negative map coordinates snap the same way the
 * legacy arithmetic shift does. */
static int32_t floor_div16(int32_t v) {
    return (v >= 0) ? (v / 16) : -(((-v) + 15) / 16);
}

void Units_SnapBuildSite(int def_idx, int32_t *world_x, int32_t *world_y) {
    const UnitDef *d = Units_GetDef(def_idx);
    if (!d || !world_x || !world_y) return;
    int fx = d->footprint_x > 0 ? d->footprint_x : 2;
    int fz = d->footprint_z > 0 ? d->footprint_z : 2;
    /* A building lives on a cell, never between cells: legacy turns
     * the cursor into the footprint's top-left cell (legacy:184168)
     * and reads the centre back as cell*16 + footprint*8
     * (legacy:184216). Without the snap a 2x2 lodestone almost never
     * lands square on a 2x2 pad. */
    *world_x = floor_div16(*world_x - fx * 8 + 8) * 16 + fx * 8;
    *world_y = floor_div16(*world_y - fz * 8 + 8) * 16 + fz * 8;
}

/* The def's water-depth window. The move class owns it when the def
 * names one, else the def's own keys do (legacy:163199-163202). */
static void unit_water_depth_window(const GameWorld *w, const UnitDef *def,
                                    int *out_min, int *out_max) {
    const MoveClassDef *mc = unit_move_class(w, def);
    *out_min = mc ? mc->min_water_depth : (def ? def->min_water_depth : 0);
    *out_max = mc ? mc->max_water_depth : (def ? def->max_water_depth : 0);
}

int Units_IsBuildSiteClear(int def_idx, int32_t wx, int32_t wy) {
    const UnitDef *d = Units_GetDef(def_idx);
    if (!d) return 0;
    /* Judge the cell the build would actually occupy (legacy:184168). */
    Units_SnapBuildSite(def_idx, &wx, &wy);
    int fx = d->footprint_x > 0 ? d->footprint_x : 2;
    int fz = d->footprint_z > 0 ? d->footprint_z : 2;
    /* Footprint half-extents in world pixels (16 px / TA tile). */
    int hw = fx * 8;
    int hh = fz * 8;
    int x0 = wx - hw, x1 = wx + hw;
    int y0 = wy - hh, y1 = wy + hh;
    GameWorld *world = World_Get();
    uint8_t yard[TAK_YARD_MAX_CELLS];
    int ycells = Units_ExpandYardmap(d, yard, TAK_YARD_MAX_CELLS);
    int sea = world ? world->water_height : 0;
    int min_wd = 0, max_wd = 0;
    unit_water_depth_window(world, d, &min_wd, &max_wd);
    /* Legacy's height sentinels: no ground cell leaves min above max
     * and the float line takes over (legacy:218766, :218898). */
    int ground_min = 255, ground_max = 0, water_max = 0;
    for (int sy = y0; sy <= y1; sy += 16) {
        for (int sx = x0; sx <= x1; sx += 16) {
            uint8_t code = 0xff;   /* no yardmap: every test applies */
            if (ycells > 0) {
                int cx = (sx - x0) / 16, cz = (sy - y0) / 16;
                if (cx >= fx) cx = fx - 1;
                if (cz >= fz) cz = fz - 1;
                code = yard[cz * fx + cx];
            }
            /* A sacred cell's code clears the blocking-feature bit, so
             * it is slope-tested only (legacy:218831 vs :218858). */
            if (code & TAK_YARD_SACRED) {
                if (!Terrain_SlopeAllows(world, sx, sy, d->max_slope))
                    return 0;
            } else if (!Terrain_IsWalkable(world, sx, sy, d->max_slope)) {
                return 0;
            }
            if (sea <= 0) continue;
            /* waterheight is raw map units and so is the sample. */
            int h = Terrain_SampleHeight(world, sx, sy);
            if (ycells == 0) {
                /* No yardmap: the depth window applies cell by cell,
                 * which is what keeps a ship off dry land and a land
                 * unit out of deep water (legacy:219149-219156). */
                int depth = sea - h;
                if (depth > max_wd || depth < min_wd) return 0;
            } else {
                if (code & TAK_YARD_LEVEL) {
                    if (h < ground_min) ground_min = h;
                    if (h > ground_max) ground_max = h;
                }
                if ((code & TAK_YARD_WATER) && h > water_max) water_max = h;
            }
        }
    }
    /* Buildings take the same window through their yardmap: ground
     * cells carry the depth test, float cells ('w'/'C'/'Y') must lie
     * below the hull line, which is sea level less the def's waterline
     * when the yard has no ground cell at all (legacy:218890-218911).
     * That is what puts a dock on water and a keep on land. */
    if (ycells > 0 && sea > 0) {
        int level = (ground_min <= ground_max) ? ground_min
                                               : sea - (int)d->waterline;
        if (water_max > level) return 0;
        if (sea - max_wd > ground_min) return 0;
        int hi = ground_max > water_max ? ground_max : water_max;
        if (hi > sea - min_wd) return 0;
    }
    /* Check every alive unit for AABB overlap with the proposed site.
     * Each existing unit reports its OWN footprint so a 2×2 building
     * doesn't collide with a 1×1 archer that's slightly outside the
     * site rect, etc. Mirrors legacy walking the per-tile occupancy
     * grid (legacy:219106) in spirit — we don't yet have the
     * grid itself but unit-vs-unit AABB is functionally equivalent
     * for runtime collisions. */
    for (int i = 0; i < g_unit_count; i++) {
        const Unit *u = &g_units[i];
        if (u->alive != 1) continue;
        const UnitDef *ud = Units_GetDef(u->def_idx);
        int uhw = (ud && ud->footprint_x > 0) ? ud->footprint_x * 8 : 16;
        int uhh = (ud && ud->footprint_z > 0) ? ud->footprint_z * 8 : 16;
        int ux0 = u->world_x - uhw, ux1 = u->world_x + uhw;
        int uy0 = u->world_y - uhh, uy1 = u->world_y + uhh;
        if (ux0 < x1 && ux1 > x0 && uy0 < y1 && uy1 > y0) return 0;
    }
    /* Lodestones (yardmap 'S') only stand on a sacred site, and must
     * cover the whole pad (legacy:218887). */
    if (d->yardmap_sacred &&
        !sacred_site_covered(world, yard, ycells, fx, fz, x0, y0)) {
        return 0;
    }
    return 1;
}

int Units_BeginBuilding(int building_def_idx,
                         int32_t world_x, int32_t world_y) {
    const UnitDef *bd = Units_GetDef(building_def_idx);
    if (!bd) return -1;

    /* Block the build if another unit/building occupies the footprint. */
    if (!Units_IsBuildSiteClear(building_def_idx, world_x, world_y)) {
        fprintf(stderr, "Build: site blocked at (%d, %d)\n", world_x, world_y);
        return -1;
    }

    /* Find the first selected friendly with the BUILDER cap. */
    int builder = -1;
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count) continue;
        const Unit *u = &g_units[h];
        if (u->alive != 1 || u->player_id != 1) continue;
        const UnitDef *ud = Units_GetDef(u->def_idx);
        if (ud && (ud->cap_flags & UNIT_CAP_BUILDER)) { builder = h; break; }
    }
    if (builder < 0) return -1;

    /* Spawn the building at low health (legacy convention: 1 HP,
     * builder feeds it 1.0/workertime of max per tick). */
    int new_handle = Units_Spawn(building_def_idx,
                                  g_units[builder].player_id,
                                  g_units[builder].team_color_idx,
                                  world_x, world_y);
    if (new_handle < 0) return -1;
    Unit *bu = &g_units[new_handle];
    bu->heading = build_heading_for_def(bd);
    bu->health = (bd->max_health > 0) ? 1 : 1;
    bu->under_construction = 1;
    bu->build_hp_accum = 0.0f;
    /* Buildings start passive — they shouldn't auto-target their way
     * out of construction. */
    bu->aggro_mode = UNIT_AGGRO_PASSIVE;

    /* Order the builder to construct the new unit. */
    Unit *u = &g_units[builder];
    u->cmd_kind     = UNIT_CMD_BUILD;
    u->cmd_x        = world_x;
    u->cmd_y        = world_y;
    u->build_target = (int16_t)new_handle;
    u->target       = -1;
    unit_clear_path(u);
    return new_handle;
}

/* ── Build menu / canbuild enumeration ──────────────────────────── */

int Units_BeginBuildingForUnit(int builder_handle,
                               int building_def_idx,
                               int32_t world_x,
                               int32_t world_y) {
    const UnitDef *bd = Units_GetDef(building_def_idx);
    if (!bd) return -1;
    if (builder_handle < 0 || builder_handle >= g_unit_count) return -1;

    Unit *u = &g_units[builder_handle];
    if (u->alive != 1) return -1;
    const UnitDef *ud = Units_GetDef(u->def_idx);
    if (!ud || !(ud->cap_flags & UNIT_CAP_BUILDER)) return -1;

    /* Factory production vs building placement. The legacy order
     * handler (legacy:9342-9456) creates a *mobile* product at
     * the factory's own build spot (COB QueryBuildInfo → Terrain_Find-
     * BuildPlacement → Unit_Create), not at a caller-chosen map site,
     * then runs the factory's StartBuilding script and finally hands
     * the finished unit a move order out of the yard
     * (Mission_AssignMoveTarget). Distinguish the two by the product:
     * mobile units produced by a structure build in-yard; immobile
     * buildings are placement construction at (world_x, world_y). */
    int factory_production =
        (bd->max_velocity > 0.0f && ud->max_velocity <= 0.0f);
    if (factory_production) {
        /* On the build pad, not under the building: QueryBuildInfo
         * names the piece and the piece gives the spot
         * (legacy:9347-9362). Exactly one query per production attempt.
         * Legacy re-queries only when it re-enters the phase after a
         * timed retry (legacy:9374-9382). */
        GameWorld *w = World_Get();
        world_x = u->world_x;
        world_y = u->world_y;
        int placed = 0;
        int32_t sx = 0, sy = 0;
        if (unit_factory_build_spot(u, &sx, &sy) &&
            unit_water_depth_ok(w, bd, sx, sy)) {
            world_x = sx;
            world_y = sy;
            placed = 1;
        }
        /* The PRODUCT's depth window gates the centre fallback too.
         * Legacy runs the placement search with the product def, so a
         * ship never materialises on dry land (legacy:9363-9373).
         * Deviation: legacy stalls and retries, we refuse and log. */
        if (!placed && !unit_water_depth_ok(w, bd, world_x, world_y)) {
            fprintf(stderr, "Build: %s has no water at the build spot\n",
                    bd->unitname);
            return -1;
        }
    } else if (!Units_IsBuildSiteClear(building_def_idx, world_x, world_y)) {
        return -1;
    }

    int new_handle = Units_Spawn(building_def_idx,
                                  u->player_id,
                                  u->team_color_idx,
                                  world_x, world_y);
    if (new_handle < 0) return -1;

    Unit *bu = &g_units[new_handle];
    bu->heading = build_heading_for_def(bd);
    bu->health = (bd->max_health > 0) ? 1 : 1;
    bu->under_construction = 1;
    bu->build_hp_accum = 0.0f;
    bu->aggro_mode = UNIT_AGGRO_PASSIVE;

    u->cmd_kind = UNIT_CMD_BUILD;
    u->cmd_x = world_x;
    u->cmd_y = world_y;
    u->build_target = (int16_t)new_handle;
    u->target = -1;
    unit_clear_path(u);
    return new_handle;
}

/* ── Factory production queue + rally ───────────────────────────── */

int Units_FactoryEnqueue(int factory_handle, int product_def_idx) {
    if (factory_handle < 0 || factory_handle >= g_unit_count) return -1;
    Unit *f = &g_units[factory_handle];
    if (f->alive != 1 || f->under_construction) return -1;
    const UnitDef *fd = Units_GetDef(f->def_idx);
    if (!fd || !(fd->cap_flags & UNIT_CAP_BUILDER)) return -1;
    if (!Units_GetDef(product_def_idx)) return -1;

    /* Idle factory starts right away (in-yard spawn). */
    if (f->cmd_kind != UNIT_CMD_BUILD && f->build_target < 0) {
        return Units_BeginBuildingForUnit(factory_handle, product_def_idx,
                                          f->world_x, f->world_y) >= 0
             ? 0 : -1;
    }
    if (f->prod_queue_len >= UNIT_PROD_QUEUE_MAX) return -1;
    f->prod_queue[f->prod_queue_len++] = (int16_t)product_def_idx;
    return 0;
}

int Units_FactoryCancelCurrent(int factory_handle) {
    if (factory_handle < 0 || factory_handle >= g_unit_count) return -1;
    Unit *f = &g_units[factory_handle];
    if (f->alive != 1) return -1;
    if (f->cmd_kind != UNIT_CMD_BUILD || f->build_target < 0) return -1;
    int bt = f->build_target;
    if (bt >= 0 && bt < g_unit_count && g_units[bt].alive == 1 &&
        g_units[bt].under_construction) {
        /* Refund the mana already fed, proportional to progress —
         * the immediate equivalent of the legacy decay path, which
         * returns buildcost continuously as the abandoned frame
         * decays (legacy:39510-39524). HP grows linearly
         * with spend, so health/max IS the paid fraction. */
        const UnitDef *cd = Units_GetDef(g_units[bt].def_idx);
        GameWorld *cw = World_Get();
        if (cd && cw && cd->build_cost > 0 && g_units[bt].max_health > 0) {
            float paid_frac = (float)g_units[bt].health /
                              (float)g_units[bt].max_health;
            if (paid_frac > 1.0f) paid_frac = 1.0f;
            Economy_EarnF(&cw->economy, g_units[bt].player_id,
                          (float)cd->build_cost * paid_frac);
        }
        g_units[bt].alive = UNIT_ALIVE_DEAD;
        if (g_units[bt].cob) {
            Cob_EngineFree(g_units[bt].cob);
            tak_free(g_units[bt].cob);
            g_units[bt].cob = NULL;
        }
    }
    f->cmd_kind = UNIT_CMD_NONE;
    f->build_target = -1;
    unit_clear_path(f);
    /* Advance to the next queued product, if any. */
    if (f->prod_queue_len > 0) {
        int next_def = f->prod_queue[0];
        for (int i = 1; i < f->prod_queue_len; i++)
            f->prod_queue[i - 1] = f->prod_queue[i];
        f->prod_queue_len--;
        (void)Units_BeginBuildingForUnit(factory_handle, next_def,
                                         f->world_x, f->world_y);
    }
    return 0;
}

int Units_FactoryDequeueDef(int factory_handle, int def_idx) {
    if (factory_handle < 0 || factory_handle >= g_unit_count) return -1;
    Unit *f = &g_units[factory_handle];
    if (f->alive != 1) return -1;
    /* Remove the LAST queued instance first; only then cancel the
     * in-progress one (legacy right-click order). */
    for (int i = f->prod_queue_len - 1; i >= 0; i--) {
        if (f->prod_queue[i] != def_idx) continue;
        for (int j = i + 1; j < f->prod_queue_len; j++)
            f->prod_queue[j - 1] = f->prod_queue[j];
        f->prod_queue_len--;
        return 0;
    }
    if (f->build_target >= 0 && f->build_target < g_unit_count &&
        g_units[f->build_target].under_construction &&
        (int)g_units[f->build_target].def_idx == def_idx) {
        return Units_FactoryCancelCurrent(factory_handle);
    }
    return -1;
}

int Units_FactoryQueuedCountForDef(int factory_handle, int def_idx) {
    if (factory_handle < 0 || factory_handle >= g_unit_count) return 0;
    const Unit *f = &g_units[factory_handle];
    if (f->alive != 1) return 0;
    int n = 0;
    if (f->build_target >= 0 && f->build_target < g_unit_count &&
        g_units[f->build_target].alive == 1 &&
        g_units[f->build_target].under_construction &&
        (int)g_units[f->build_target].def_idx == def_idx) n++;
    for (int i = 0; i < f->prod_queue_len; i++)
        if (f->prod_queue[i] == def_idx) n++;
    return n;
}

int Units_FactoryQueueCount(int factory_handle) {
    if (factory_handle < 0 || factory_handle >= g_unit_count) return 0;
    return g_units[factory_handle].prod_queue_len;
}

void Units_FactorySetRally(int factory_handle,
                           int32_t world_x, int32_t world_y) {
    if (factory_handle < 0 || factory_handle >= g_unit_count) return;
    Unit *f = &g_units[factory_handle];
    f->rally_x = world_x;
    f->rally_y = world_y;
    f->rally_set = 1;
}

int Units_GetFootprintX(int def_idx) {
    const UnitDef *d = Units_GetDef(def_idx);
    return d ? d->footprint_x : 0;
}
int Units_GetFootprintZ(int def_idx) {
    const UnitDef *d = Units_GetDef(def_idx);
    return d ? d->footprint_z : 0;
}

/* Lower-case copy helper (no toupper/tolower platform mismatch). */
static void str_tolower(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap && src[i]) {
        char c = src[i];
        dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        i++;
    }
    dst[i] = '\0';
}

/* Sort helper: smaller priority first. Defs with no priority field
 * default to large priority so they sink to the end. */
typedef struct { int def_idx; int priority; } CanBuildEntry;
static int canbuild_compare(const void *a, const void *b) {
    const CanBuildEntry *x = (const CanBuildEntry *)a;
    const CanBuildEntry *y = (const CanBuildEntry *)b;
    if (x->priority != y->priority) return x->priority - y->priority;
    return x->def_idx - y->def_idx;
}

int Units_GetBuildables(int builder_def_idx, int *out, int max_out) {
    if (!out || max_out <= 0) return 0;
    const UnitDef *bd = Units_GetDef(builder_def_idx);
    if (!bd || !bd->unitname[0]) return 0;

    /* canbuild data is static — cache per def. The uncached path costs
     * a VFS glob + N TDF opens (HPI decompression in the browser) and
     * the AI hits it every replan tick. */
    static int16_t cache_menu[512][64];
    static int8_t  cache_n[512];
    static int     cache_init = 0;
    if (!cache_init) {
        memset(cache_n, -1, sizeof(cache_n));
        cache_init = 1;
    }
    if (builder_def_idx >= 0 && builder_def_idx < 512 &&
        cache_n[builder_def_idx] >= 0) {
        int cn = cache_n[builder_def_idx];
        if (cn > max_out) cn = max_out;
        for (int i = 0; i < cn; i++) out[i] = cache_menu[builder_def_idx][i];
        return cn;
    }

    /* canbuild dirs are keyed by lower-case unitname. */
    char lower[40];
    str_tolower(lower, bd->unitname, sizeof(lower));

    /* Enumerate through the VFS: finds loose files AND HPI-archived
     * entries (the browser build ships HPIs only). Subdirectory globs
     * work since the walk_directory relative-path fix. The loose
     * extraction nests everything under a leading "data/" that the
     * archives' internal tree doesn't have — try both roots. */
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "data/canbuild/%s/*.tdf", lower);

    CanBuildEntry tmp[64];
    int n = 0;

    char **paths = NULL;
    int n_paths = 0;
    if (VFS_ListFiles(pattern, &paths, &n_paths) != 0 || n_paths == 0) {
        if (paths) {
            for (int pi = 0; pi < n_paths; pi++) tak_free(paths[pi]);
            tak_free(paths);
            paths = NULL;
        }
        n_paths = 0;
        snprintf(pattern, sizeof(pattern), "canbuild/%s/*.tdf", lower);
        (void)VFS_ListFiles(pattern, &paths, &n_paths);
    }
    if (paths) {
        for (int pi = 0; pi < n_paths; pi++) {
            const char *rel = paths[pi];
            const char *fname = strrchr(rel, '/');
            fname = fname ? fname + 1 : rel;

            char stem[40];
            size_t bi = 0;
            while (fname[bi] && fname[bi] != '.' && bi + 1 < sizeof(stem)) {
                stem[bi] = fname[bi]; bi++;
            }
            stem[bi] = '\0';
            int def = Units_FindDefByName(stem);
            if (def < 0) { tak_free(paths[pi]); continue; }

            int priority = 99;
            TDFFile *t = TDF_Open(rel);
            if (t) {
                if (TDF_Load(t) == 0 && TDF_PushSection(t, "MENU") == 0) {
                    priority = TDF_ReadInt(t, "Priority", priority);
                    TDF_PopSection(t);
                }
                TDF_Close(t);
            }
            if (n < (int)(sizeof(tmp)/sizeof(tmp[0]))) {
                tmp[n].def_idx = def;
                tmp[n].priority = priority;
                n++;
            }
            tak_free(paths[pi]);
        }
        tak_free(paths);
    }

    /* Log each builder's menu once — this runs on every AI planning
     * tick, and per-tick stderr is ruinous in the browser (Emscripten
     * mirrors it into the DevTools console, which accumulates). */
    static int logged_defs[512];
    if (builder_def_idx >= 0 && builder_def_idx < 512 &&
        !logged_defs[builder_def_idx]) {
        logged_defs[builder_def_idx] = 1;
        fprintf(stderr, "Units_GetBuildables: %s -> %d entries\n", pattern, n);
    }

    qsort(tmp, n, sizeof(tmp[0]), canbuild_compare);

    if (builder_def_idx >= 0 && builder_def_idx < 512) {
        int cn = n > 64 ? 64 : n;
        for (int i = 0; i < cn; i++)
            cache_menu[builder_def_idx][i] = (int16_t)tmp[i].def_idx;
        cache_n[builder_def_idx] = (int8_t)cn;
    }

    if (n > max_out) n = max_out;
    for (int i = 0; i < n; i++) out[i] = tmp[i].def_idx;
    return n;
}
void Units_SetBackfaceCullInvert(int v) { g_backface_cull_invert = v ? 1 : 0; }

/* R6 — team-color palette range. TA's convention is palette indices
 * 16..23 are reserved for team-color slots (the 8-color "TA-style"
 * unit palette). Empirical confirmation in M5 hands-on: spawning the
 * monarch with this range produces a tinted cape that matches the
 * original game's silhouette. Adjust here if TAK uses a different
 * range. */
#define TC_INDEX_LO  0x10   /* 16 */
#define TC_INDEX_HI  0x17   /* 23 */

/* Team-color palette — mirrors battle_setup.c's bs_player_colors
 * (12 entries, Blue / Red / Green / Yellow / Cyan / Magenta /
 * Orange / White / Dark Blue / Dark Red / Dark Green / Grey). The
 * idx into this table travels on the Unit instance as
 * team_color_idx (set at spawn time). */
static const uint8_t g_team_colors[12][3] = {
    {  60, 100, 220 },  /* 0  Blue       (Aramon default) */
    { 210,  50,  50 },  /* 1  Red        (Taros default)  */
    {  60, 180,  80 },  /* 2  Green      (Veruna default) */
    { 220, 200,  80 },  /* 3  Yellow     (Zhon default)   */
    {  60, 200, 220 },  /* 4  Cyan    */
    { 210, 100, 200 },  /* 5  Magenta */
    { 230, 140,  60 },  /* 6  Orange  */
    { 230, 230, 230 },  /* 7  White   */
    {  40,  60, 140 },  /* 8  Dark Blue */
    { 140,  30,  30 },  /* 9  Dark Red  */
    {  40, 110,  50 },  /* 10 Dark Green*/
    { 140, 140, 140 },  /* 11 Grey      */
};

uint32_t Units_GetTeamColorRGBA(int idx) {
    if (idx < 0 || idx > 11) return 0xFFFFFFFFu;
    /* RGBA32 byte order on Windows little-endian: byte0=R, byte1=G,
     * byte2=B, byte3=A. Memory-compatible with SDL_Color's struct. */
    uint32_t r = g_team_colors[idx][0];
    uint32_t g = g_team_colors[idx][1];
    uint32_t b = g_team_colors[idx][2];
    return r | (g << 8) | (b << 16) | (0xFFu << 24);
}

/* Scratch vertex buffers for SDL_RenderGeometryRaw. Persistent across
 * frames so we never per-frame-malloc; grow when the visible-vert
 * count outpaces capacity. Layout is parallel arrays (SoA), one entry
 * per output vertex:
 *   xy[2v..]    pre-projected screen-space float pair
 *   color[v]    packed RGBA8888 per vertex (matches SDL_Color stride)
 *   uv[2v..]    atlas-space UV pair (zeros in M4, real in M5)
 *
 * Why SoA and not interleaved? Because SDL_RenderGeometryRaw takes
 * three independent strided pointers — no repacking needed, the
 * GPU driver reads each stream straight out of these buffers. */
static float    *g_scratch_xy    = NULL;
static uint32_t *g_scratch_color = NULL;
static float    *g_scratch_uv    = NULL;
static float    *g_scratch_wz    = NULL;   /* per-vertex world Z, for depth sort */
/* Per-vertex legacy height key: whole model-space Y units, the same
 * quantity the legacy rasteriser interpolates and depth-tests
 * (legacy:197697). Base offset dropped, only the order matters. */
static float    *g_scratch_hkey  = NULL;
static int       g_scratch_vcap  = 0;
static uint16_t *g_scratch_idx   = NULL;
static int       g_scratch_icap  = 0;

/* Per-triangle sort record. Built fresh per batch each frame, sorted
 * and flattened back into the index stream so one mesh's pieces layer
 * the way legacy's rasteriser resolves them.
 *
 * hkey is the legacy per-pixel height key (base + model y, whole
 * units, legacy:197697) that the rasteriser depth-tests "greater
 * wins" against a parallel byte buffer (legacy:265317, :265360).
 * Under sy = -z - (y >> 1) two surfaces sharing a screen pixel have
 * y = -2(sy + z), so the higher y is simply the nearer one: the key
 * is a depth test in height clothing. We resolve per triangle rather
 * than per pixel, taking each triangle's tallest vertex.
 * key is the authored order used to break ties. */
typedef struct TriSort {
    float    hkey;         /* legacy height key, max over the 3 verts */
    float    key;          /* reverse-node authored order (tri_seq) */
    uint16_t i0, i1, i2;
    uint16_t batch;        /* source batch — used by the single-unit
                            * cross-batch painter to emit draw runs */
} TriSort;
static TriSort *g_scratch_tri    = NULL;
static int      g_scratch_tricap = 0;

/* Per-node world transform scratch (Phase D M3). One slot per node in
 * the mesh; indexed by vert_node_idx to transform a node-local vertex
 * to model-frame. For M3 with identity piece state this is just a
 * cumulative-offset translation; once piece rotation is applied each
 * tick (M4+) the rot matrix becomes non-identity.
 *
 * Sized at submit time to the current mesh's node_count. Reusable
 * across chunks within one Submit pass since all units in a chunk
 * share the same UnitDef and (for M3) identical piece state. */
typedef struct NodeXform {
    float rot[9];     /* 3x3, row-major: [r00 r01 r02 | r10 r11 r12 | r20 r21 r22] */
    float trans[3];   /* world translation in model frame */
    uint8_t hidden;   /* COB HIDE-PIECE flag — skip rendering when set */
    uint8_t _pad[3];
} NodeXform;
static NodeXform *g_scratch_node_xform = NULL;
static int        g_scratch_node_cap   = 0;

/* ── Helpers (kept from pre-M4) ──────────────────────────────────── */

static int stricmp_bounded(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static int ascii_contains_ci(const char *s, const char *needle) {
    if (!s || !needle || !needle[0]) return 0;
    size_t nl = strlen(needle);
    for (const char *p = s; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i]) {
            char a = p[i], b = needle[i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) break;
            i++;
        }
        if (i == nl) return 1;
    }
    return 0;
}

static float build_heading_for_def(const UnitDef *d) {
    /* All structures place facing SOUTH (toward the viewer), like the
     * original. Under the corrected facing convention heading 0 =
     * north, so south = π. The old lodestone-only π special case was
     * compensating for the pre-mirror backwards convention.
     * TODO(parity): honor the FBI `buildangle` field for defs that
     * author a non-default placement orientation. */
    (void)d;
    return 3.14159265358979323846f;
}

static float render_y_scale_for_def(const UnitDef *d) {
    /* Uniform — the legacy projector scales height identically for
     * every model (y>>1, legacy:197689). The old 0.72 lodestone
     * squash compensated for the incorrect 0.577 tilt. */
    (void)d;
    return 1.0f;
}

static void copy_bounded(char *dst, size_t cap, const char *src) {
    if (!src || cap == 0) { if (cap > 0) dst[0] = '\0'; return; }
    size_t n = 0;
    while (n + 1 < cap && src[n] != '\0') { dst[n] = src[n]; n++; }
    dst[n] = '\0';
}

static void lowercase_into(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (; src[n] != '\0' && n + 1 < cap; n++) {
        char c = src[n];
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
        dst[n] = c;
    }
    dst[n] = '\0';
}

static int ensure_def_capacity(void) {
    if (g_def_count < g_def_cap) return 0;
    int new_cap = g_def_cap == 0 ? 64 : g_def_cap * 2;
    UnitDef *bigger = (UnitDef *)tak_malloc((size_t)new_cap * sizeof(UnitDef));
    if (!bigger) return -1;
    if (g_defs) {
        memcpy(bigger, g_defs, (size_t)g_def_count * sizeof(UnitDef));
        tak_free(g_defs);
    }
    g_defs = bigger;
    g_def_cap = new_cap;
    return 0;
}

static int parse_fbi(const char *vfs_path, UnitDef *out) {
    memset(out, 0, sizeof(*out));

    TDFFile *tdf = TDF_Open(vfs_path);
    if (!tdf || TDF_Load(tdf) != 0) {
        if (tdf) TDF_Close(tdf);
        return -1;
    }

    if (TDF_PushSection(tdf, "UNITINFO") != 0) {
        TDF_Close(tdf);
        return -1;
    }

    copy_bounded(out->unitname,    sizeof(out->unitname),
                 TDF_ReadString(tdf, "unitname", ""));
    copy_bounded(out->side,        sizeof(out->side),
                 TDF_ReadString(tdf, "side", ""));
    copy_bounded(out->objectname,  sizeof(out->objectname),
                 TDF_ReadString(tdf, "objectname", ""));
    copy_bounded(out->display_name, sizeof(out->display_name),
                 TDF_ReadString(tdf, "name", ""));
    copy_bounded(out->description, sizeof(out->description),
                 TDF_ReadString(tdf, "description", ""));
    copy_bounded(out->category,    sizeof(out->category),
                 TDF_ReadString(tdf, "category", ""));
    copy_bounded(out->damage_category, sizeof(out->damage_category),
                 TDF_ReadString(tdf, "damagecategory", ""));
    copy_bounded(out->soundcategory, sizeof(out->soundcategory),
                 TDF_ReadString(tdf, "soundcategory", ""));
    out->unitnumber     = TDF_ReadInt(tdf, "unitnumber", 0);
    out->buildtime      = TDF_ReadFloat(tdf, "buildtime", 100.0f);
    if (out->buildtime <= 0.0f) out->buildtime = 100.0f;
    out->build_cost     = (int)(TDF_ReadFloat(tdf, "buildcost", out->buildtime) + 0.5f);
    out->worker_time    = TDF_ReadFloat(tdf, "workertime", 0.0f);
    out->build_distance = TDF_ReadInt(tdf, "builddistance", 0);
    /* Default 666 matches the legacy engine fallback at
     * legacy:162918 (TDF_ReadInt with 0x29a default). */
    out->kill_xp_value  = TDF_ReadInt(tdf, "experiencepoints", 666);
    /* Float, NOT scaled by tick rate (legacy :162910 → def+0x222). */
    out->mogrium_bounty = TDF_ReadFloat(tdf, "mogriumbounty", 0.0f);

    /* Combat / movement fields. FBI uses `maxdamage` for HP (legacy TA
     * name). The legacy loader also reads `radardistance`
     * (legacy:162920) for radar/minimap visibility. */
    out->max_health     = TDF_ReadInt(tdf, "maxdamage", 0);
    out->sight_distance = TDF_ReadInt(tdf, "sightdistance", 0);
    out->radar_distance = TDF_ReadInt(tdf, "radardistance", 0);
    out->can_fly        = TDF_ReadInt(tdf, "canfly", 0);
    out->cruise_alt     = TDF_ReadInt(tdf, "cruisealt", 0);
    out->floater        = TDF_ReadInt(tdf, "floater", 0);
    out->waterline      = TDF_ReadInt(tdf, "waterline", 0);
    out->transport_size = TDF_ReadInt(tdf, "transportsize", 0);
    out->transport_capacity = TDF_ReadInt(tdf, "transportcapacity", 0);
    out->transport_size_capacity = TDF_ReadInt(tdf, "transportsizecapacity", 0);
    out->cant_be_transported = TDF_ReadInt(tdf, "cantbetransported", 0);
    out->transported_size = TDF_ReadInt(tdf, "transportedsize", 0);
    out->transport_distance = TDF_ReadInt(tdf, "transportdistance", 0);
    copy_bounded(out->movement_class, sizeof(out->movement_class),
                 TDF_ReadString(tdf, "movementclass", ""));
    /* Same "absent means no limit" seeding as the move classes
     * (legacy:187363-187374); every land building states its own 0. */
    out->max_water_depth = TDF_ReadInt(tdf, "maxwaterdepth", 10000);
    out->min_water_depth = TDF_ReadInt(tdf, "minwaterdepth", -10000);
    out->bad_max_water_depth = TDF_ReadInt(tdf, "badmaxwaterdepth",
                                           out->max_water_depth);
    out->bad_min_water_depth = TDF_ReadInt(tdf, "badminwaterdepth",
                                           out->min_water_depth);
    out->bad_slope = TDF_ReadInt(tdf, "badslope", 0);
    out->max_water_slope = TDF_ReadInt(tdf, "maxwaterslope", 0);
    out->bad_water_slope = TDF_ReadInt(tdf, "badwaterslope",
                                       out->max_water_slope >> 1);
    out->max_velocity   = TDF_ReadFloat(tdf, "maxvelocity", 0.0f);
    out->activate_when_built = TDF_ReadInt(tdf, "activatewhenbuilt", 0);
    /* Movement law fields — authored per 30 Hz frame (see tak_unit.h). */
    out->acceleration   = TDF_ReadFloat(tdf, "acceleration", 0.0f);
    out->brake_rate     = TDF_ReadFloat(tdf, "brakerate",    0.0f);
    out->turn_rate      = TDF_ReadFloat(tdf, "turnrate",     0.0f);

    /* Capability flags drive per-unit action button visibility.
     * The decomp reads these from the same UNITINFO section as the
     * stat fields; defaulting to 0 (button hidden) is correct for
     * units missing the flag in their FBI. */
    out->cap_flags = 0;
    if (TDF_ReadInt(tdf, "canmove",        0)) out->cap_flags |= UNIT_CAP_MOVE;
    if (TDF_ReadInt(tdf, "canstop",        0)) out->cap_flags |= UNIT_CAP_STOP;
    if (TDF_ReadInt(tdf, "canattack",      0)) out->cap_flags |= UNIT_CAP_ATTACK;
    if (TDF_ReadInt(tdf, "canguard",       0)) out->cap_flags |= UNIT_CAP_GUARD;
    if (TDF_ReadInt(tdf, "canpatrol",      0)) out->cap_flags |= UNIT_CAP_PATROL;
    if (TDF_ReadInt(tdf, "cancloak",       0)) out->cap_flags |= UNIT_CAP_CLOAK;
    if (TDF_ReadInt(tdf, "cantransport",   0)) out->cap_flags |= UNIT_CAP_TRANSPORT;
    if (TDF_ReadInt(tdf, "builder",        0)) out->cap_flags |= UNIT_CAP_BUILDER;
    if (TDF_ReadInt(tdf, "canreclaim",     0)) out->cap_flags |= UNIT_CAP_RECLAIM;
    if (TDF_ReadInt(tdf, "canresurrect",   0)) out->cap_flags |= UNIT_CAP_RESURRECT;
    if (TDF_ReadInt(tdf, "canrepair",      0)) out->cap_flags |= UNIT_CAP_REPAIR;
    if (TDF_ReadInt(tdf, "canload",        0)) out->cap_flags |= UNIT_CAP_LOAD;
    if (TDF_ReadInt(tdf, "weaponswitching",0)) out->cap_flags |= UNIT_CAP_W_SWITCH;
    out->heal_time = TDF_ReadFloat(tdf, "healtime", 0.0f);
    if (out->heal_time < 0.0f) out->heal_time = 0.0f;
    if ((out->cap_flags & UNIT_CAP_BUILDER) && out->worker_time > 0.0f) {
        out->cap_flags |= UNIT_CAP_REPAIR;
    }

    /* Mana economy. FBI fields are integers in legacy units; the
     * legacy loader stores them as floats and scales rate by
     * 1/30 (30Hz sim). We keep capacities as int (always integer
     * pool caps) and rates as float per-second — Economy_Tick does
     * the per-tick division at our 60Hz. Lodestones use mogrium*
     * fields; monarchs use MaxMana / ManaRechargeRate. */
    out->max_mana                = TDF_ReadInt  (tdf, "maxmana",          0);
    out->mana_recharge_per_sec   = TDF_ReadFloat(tdf, "manarechargerate", 0.0f);
    out->mogrium_storage         = TDF_ReadInt  (tdf, "mogriumstorage",   0);
    out->mogrium_income_per_sec  = TDF_ReadFloat(tdf, "mogriumincome",    0.0f);

    /* Building footprint + slope. Used by the build-placement preview
     * to outline the site and color-code valid vs invalid terrain.
     * footprintx/z are in 16-pixel TA tiles. */
    out->footprint_x = TDF_ReadInt(tdf, "footprintx", 0);
    out->footprint_z = TDF_ReadInt(tdf, "footprintz", 0);
    out->max_slope   = TDF_ReadInt(tdf, "maxslope",   255);
    if (out->bad_slope <= 0) out->bad_slope = out->max_slope >> 1;

    /* Occupancy inputs. Only buildings (bmcode 0) carry a real yardmap
     * (legacy:162925, :163208); Occ_BuildYardmap applies the legacy
     * char table and the mobile all-clear fallback. */
    out->bmcode    = TDF_ReadInt(tdf, "bmcode",    0);
    out->is_gate   = TDF_ReadInt(tdf, "gate",      0);
    out->onoffable = TDF_ReadInt(tdf, "onoffable", 0);
    out->yardmap = Occ_BuildYardmap(TDF_ReadString(tdf, "yardmap", ""),
                                    out->bmcode,
                                    out->footprint_x, out->footprint_z);
    out->yardmap_sacred = 0;
    if (out->yardmap) {
        int cells = out->footprint_x * out->footprint_z;
        for (int i = 0; i < cells; i++) {
            if (out->yardmap[i] & TAK_YARD_SACRED) { out->yardmap_sacred = 1; break; }
        }
    }

    TDF_PopSection(tdf);   /* leave UNITINFO */

    /* Parse up to 3 inline [WEAPONn] sections. Each may contain a
     * nested [DAMAGE] subsection holding `default = N`. */
    out->num_weapons = 0;
    for (int wi = 1; wi <= 3 && out->num_weapons < 3; wi++) {
        char section[16];
        snprintf(section, sizeof(section), "WEAPON%d", wi);
        if (TDF_PushSection(tdf, section) != 0) break;

        UnitWeapon *w = &out->weapons[out->num_weapons];
        copy_bounded(w->name, sizeof(w->name),
                     TDF_ReadString(tdf, "name", ""));
        copy_bounded(w->type, sizeof(w->type),
                     TDF_ReadString(tdf, "type", ""));
        copy_bounded(w->damage_type, sizeof(w->damage_type),
                     TDF_ReadString(tdf, "damagetype", ""));
        copy_bounded(w->explosion_class, sizeof(w->explosion_class),
                     TDF_ReadString(tdf, "explosionclass", ""));
        copy_bounded(w->weapon_art, sizeof(w->weapon_art),
                     TDF_ReadString(tdf, "weaponart", ""));
        copy_bounded(w->model, sizeof(w->model),
                     TDF_ReadString(tdf, "model", ""));
        copy_bounded(w->subtype, sizeof(w->subtype),
                     TDF_ReadString(tdf, "subtype", ""));
        copy_bounded(w->hit_sound_class, sizeof(w->hit_sound_class),
                     TDF_ReadString(tdf, "soundhitclass", ""));
        copy_bounded(w->hit_sound, sizeof(w->hit_sound),
                     TDF_ReadString(tdf, "soundhit", ""));
        w->range          = TDF_ReadInt(tdf, "range", 0);
        w->min_range      = TDF_ReadInt(tdf, "minrange", 0);
        w->area_of_effect = TDF_ReadInt(tdf, "areaofeffect", 0);
        w->edge_effectiveness = TDF_ReadFloat(tdf, "edgeeffectiveness", 0.0f);
        if (w->edge_effectiveness < 0.0f) w->edge_effectiveness = 0.0f;
        if (w->edge_effectiveness > 1.0f) w->edge_effectiveness = 1.0f;
        w->burst = TDF_ReadInt(tdf, "burst", 0);
        if (w->burst < 0) w->burst = 0;
        float burst_rate_secs = TDF_ReadFloat(tdf, "burstrate", 0.0f);
        if (burst_rate_secs < 0.0f) burst_rate_secs = 0.0f;
        w->burst_rate_ticks = (int32_t)(burst_rate_secs * 60.0f + 0.5f);
        if (w->burst_rate_ticks <= 0) w->burst_rate_ticks = 1;
        w->spray_angle = TDF_ReadInt(tdf, "sprayangle", 0);
        if (w->spray_angle < 0) w->spray_angle = 0;
        float reload_secs = TDF_ReadFloat(tdf, "reloadtime", 1.0f);
        if (reload_secs < 0.05f) reload_secs = 0.05f;
        w->reload_ticks   = (int32_t)(reload_secs * 60.0f + 0.5f);
        w->mana_per_shot  = TDF_ReadInt(tdf, "manapershot", 0);
        /* `weaponvelocity` is world px/sec — same unit system as
         * `range` (both consumed unscaled; decomp stores raw :249987).
         * The old ×16 made flights 1-4 ticks: shots resolved before a
         * frame rendered, so nothing was ever visible. 0 → hitscan. */
        int wv = TDF_ReadInt(tdf, "weaponvelocity", 0);
        w->velocity_pps   = wv;
        copy_bounded(w->start_sound, sizeof(w->start_sound),
                     TDF_ReadString(tdf, "soundstart", ""));
        /* Line-of-Sight weapons hit instantly and hold a beam effect
         * for emittime (30Hz frames → our 60Hz ticks). */
        w->los_kind = 0;
        if (ascii_contains_ci(w->type, "line of sight")) {
            /* Only these subtypes are beams/cones; every other LOS
             * weapon is a normal flying projectile that happens to
             * travel flat (harpoons, bolts, fireballs, cannonballs). */
            if (ascii_contains_ci(w->subtype, "lightning"))      w->los_kind = 1;
            else if (ascii_contains_ci(w->subtype, "fire") ||
                     ascii_contains_ci(w->subtype, "bluefire") ||
                     ascii_contains_ci(w->subtype, "dieselflame")) w->los_kind = 2;
            else if (ascii_contains_ci(w->subtype, "mindcontrol") ||
                     ascii_contains_ci(w->subtype, "turntostone") ||
                     ascii_contains_ci(w->subtype, "turntofrozen")) w->los_kind = 3;
        }
        w->is_los = (w->los_kind != 0);
        /* emittime is read by the FLAME ctor only (:247444); lightning
         * uses its own short hold (:247096-247133). */
        w->emit_ticks = (w->los_kind == 2)
            ? TDF_ReadInt(tdf, "emittime", 30) * 2
            : 20;
        if (w->emit_ticks < 2) w->emit_ticks = 2;
        {
            static const uint8_t defc[3][3] = {
                { 255, 255, 255 }, { 200, 230, 255 }, { 180, 200, 255 }
            };
            static const char *keys[3] = {
                "innercolor", "middlecolor", "outercolor"
            };
            uint8_t *dst[3] = { w->beam_inner, w->beam_middle, w->beam_outer };
            for (int c = 0; c < 3; c++) {
                int r = defc[c][0], g = defc[c][1], b = defc[c][2];
                const char *s = TDF_ReadString(tdf, keys[c], "");
                if (s && s[0]) sscanf(s, "%d %d %d", &r, &g, &b);
                dst[c][0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
                dst[c][1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
                dst[c][2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            }
        }
        /* Spin terms and the ballistic tuning keys (legacy:250016,
         * legacy:246477). gravityadjustment defaults to 1.0. */
        w->spin_pitch     = TDF_ReadInt(tdf, "spinpitch", 0);
        w->spin_heading   = TDF_ReadInt(tdf, "spinheading", 0);
        w->spin_roll      = TDF_ReadInt(tdf, "spinroll", 0);
        w->gravity_adjust = TDF_ReadFloat(tdf, "gravityadjustment", 1.0f);
        if (!(w->gravity_adjust > 0.0f)) w->gravity_adjust = 1.0f;
        w->lob_preferred  = TDF_ReadInt(tdf, "lobpreferred", 0) ? 1 : 0;
        w->dropped        = ascii_contains_ci(w->subtype, "dropped") ? 1 : 0;
        /* Only `type = Ballistic` gets the gravity behaviour; Guided,
         * Line of Sight and Wandering fly flat (legacy:249725-249983).
         * `subtype = Dropped` is its own legacy behaviour object
         * (legacy:249743) that derives the horizontal run from the fall
         * time instead of solving a launch; until that lands the egg
         * bombs keep flying flat. */
        w->is_gravity     = (ascii_contains_ci(w->type, "ballistic") &&
                             !w->dropped) ? 1 : 0;
        /* Resolve the art once: model 3DO, else weaponart GAF, else the
         * held beam for the lightning/flame LOS subtypes. Retail data
         * never sets both keys on one weapon. */
        w->art_name[0] = '\0';
        if (w->los_kind == 1 || w->los_kind == 2) {
            w->art_kind = UNIT_WEAPON_ART_BEAM;
        } else if (w->model[0]) {
            w->art_kind = UNIT_WEAPON_ART_MODEL;
            lowercase_into(w->art_name, sizeof(w->art_name), w->model);
            /* Some FBIs spell the model with its extension. */
            size_t an = strlen(w->art_name);
            if (an > 4 && strcmp(w->art_name + an - 4, ".3do") == 0)
                w->art_name[an - 4] = '\0';
        } else if (w->weapon_art[0]) {
            w->art_kind = UNIT_WEAPON_ART_SPRITE;
            lowercase_into(w->art_name, sizeof(w->art_name), w->weapon_art);
        } else {
            w->art_kind = UNIT_WEAPON_ART_NONE;
        }
        /* Bind explosionclass to its effect entry once (legacy:250135). */
        w->explosion_idx = (int16_t)explosion_class_index(w->explosion_class);
        w->water_weapon   = TDF_ReadInt(tdf, "waterweapon", 0);
        w->to_air_weapon  = TDF_ReadInt(tdf, "toairweapon", 0);
        w->no_air_weapon  = TDF_ReadInt(tdf, "noairweapon", 0);
        w->no_radar       = TDF_ReadInt(tdf, "noradar", 0);
        /* Per-weapon button icon names — see `legacy:250088+`
         * for the legacy parsing call. These are the basenames of
         * JPEGs in `data/anims/weaponpic/`. */
        copy_bounded(w->icon_up,       sizeof(w->icon_up),
                     TDF_ReadString(tdf, "buttonimageup",       ""));
        copy_bounded(w->icon_down,     sizeof(w->icon_down),
                     TDF_ReadString(tdf, "buttonimagedown",     ""));
        copy_bounded(w->icon_selected, sizeof(w->icon_selected),
                     TDF_ReadString(tdf, "buttonimageselected", ""));
        copy_bounded(w->icon_disabled, sizeof(w->icon_disabled),
                     TDF_ReadString(tdf, "buttonimagedisabled", ""));

        /* Default damage from nested [DAMAGE] section. */
        w->damage = 0;
        w->damage_scale_count = 0;
        if (TDF_PushSection(tdf, "DAMAGE") == 0) {
            w->damage = TDF_ReadInt(tdf, "default", 0);
            const char *key = TDF_GetFirstKey(tdf);
            while (key) {
                if (stricmp_bounded(key, "default") != 0 &&
                    w->damage_scale_count < TAK_DAMAGE_CATEGORY_MAX) {
                    UnitDamageScale *scale =
                        &w->damage_scales[w->damage_scale_count++];
                    copy_bounded(scale->category, sizeof(scale->category), key);
                    scale->scale = TDF_ReadFloat(tdf, key, 1.0f);
                    if (scale->scale < 0.0f) scale->scale = 0.0f;
                }
                key = TDF_GetNextKey(tdf);
            }
            TDF_PopSection(tdf);
        }

        TDF_PopSection(tdf);   /* leave WEAPONn */
        out->num_weapons++;
    }

    TDF_Close(tdf);
    return 0;
}

/* ── Mesh bake: Obj3DFile (tree) → UnitMesh (flat triangle list) ─── */

/* M5 bake is *batched* — triangles get grouped by atlas texture so
 * each batch can render in one draw call. NULL atlas is the flat-color
 * fallback batch (FLAT_COLOR primitives + missing textures, both
 * routed through the primitive's color_idx).
 *
 * Three passes over the tree:
 *   1. discover  — collect unique atlas-texture pointers + per-batch
 *                  vert/index totals
 *   2. layout    — prefix-sum first_vert/first_index per batch
 *   3. emit      — write per-vertex (position, uv, color) into the
 *                  right batch slot, append fan indices
 *
 * Why three passes? Because indices reference vertices in their batch's
 * vertex range, and we don't know the per-batch ranges until pass 1
 * is done. The tree is tiny (≤50 nodes), so triple-walk is irrelevant.
 *
 * UV mapping: 3DO primitives don't store explicit UVs, so we use the
 * standard TA mapping (Spring RTS reference, §3.6):
 *   3-vert: (0,0), (1,0), (1,1)
 *   4-vert: (0,0), (1,0), (1,1), (0,1)   — corners of the texture
 *   N>4:    fan of corners (rare; uses the same anchor pattern)
 * Then atlas-remap each [0..1] local UV to the entry's UV rect inside
 * its atlas: atlas_u = rect.x + local_u * rect.w. */

/* Recursive prim probe (debug diagnostic). Prints every prim with its
 * resolved atlas pointer and UV rect. Used to figure out what specific
 * geometry corresponds to visual artifacts. */
static void zon_probe(const Obj3DNode *n, int depth, int color_idx) {
    char indent[64] = {0};
    int ind = depth * 2;
    if (ind > 60) ind = 60;
    for (int i = 0; i < ind; i++) indent[i] = ' ';
    fprintf(stderr, "%snode='%s' v=%d p=%d offset=(%d,%d,%d)\n",
            indent, n->name, n->num_vertices, n->num_primitives,
            n->offset_x, n->offset_y, n->offset_z);
    for (int p = 0; p < n->num_primitives; p++) {
        const Obj3DPrimitive *pr = &n->primitives[p];
        SDL_FRect uv = {0,0,0,0};
        GPU_Texture *atl = pr->texture_name[0]
            ? TexAtlas_GetByName(pr->texture_name, color_idx, &uv) : NULL;
        fprintf(stderr,
            "%s  prim[%d] nv=%d tex='%s' atlas=%p uv=(%.4f,%.4f,%.4fx%.4f) clr=%u\n",
            indent, p, pr->num_vert_indices,
            pr->texture_name[0] ? pr->texture_name : "(none)",
            (void*)atl,
            (double)uv.x, (double)uv.y,
            (double)uv.w, (double)uv.h,
            (unsigned)pr->color_idx);
    }
    for (Obj3DNode *c = n->first_child; c; c = c->next_sibling) {
        zon_probe(c, depth + 1, color_idx);
    }
}

typedef struct BakeState {
    GPU_Texture *atlas[UNIT_MESH_MAX_BATCHES];
    SDL_FRect    uv_rect[UNIT_MESH_MAX_BATCHES];
    int          vert_count_batch[UNIT_MESH_MAX_BATCHES];
    int          tri_count_batch[UNIT_MESH_MAX_BATCHES];
    int          first_vert[UNIT_MESH_MAX_BATCHES];
    int          first_index[UNIT_MESH_MAX_BATCHES];
    int          v_writer[UNIT_MESH_MAX_BATCHES];
    int          i_writer[UNIT_MESH_MAX_BATCHES];
    int          batch_count;
    const uint32_t *palette;     /* terrain_rgba, color_idx -> RGBA */
    int          team_color_idx; /* 0..11; selects atlas variant */
    /* Authored-order sequence: incremented per emitted triangle in
     * 3DO tree-walk order; written into UnitMesh.tri_seq. */
    uint32_t     seq_counter;
    uint32_t    *tri_seq;
} BakeState;

static int batch_find_or_add(BakeState *bs, GPU_Texture *atlas, const SDL_FRect *uv) {
    for (int i = 0; i < bs->batch_count; i++) {
        if (bs->atlas[i] == atlas) return i;
    }
    if (bs->batch_count >= UNIT_MESH_MAX_BATCHES) return -1;
    int idx = bs->batch_count++;
    bs->atlas[idx] = atlas;
    if (uv) bs->uv_rect[idx] = *uv;
    else    { bs->uv_rect[idx].x = 0; bs->uv_rect[idx].y = 0;
              bs->uv_rect[idx].w = 0; bs->uv_rect[idx].h = 0; }
    return idx;
}

static void prim_local_uv(int k, int nv, float *out_u, float *out_v) {
    static const float corners[4][2] = {
        { 0.0f, 0.0f },
        { 1.0f, 0.0f },
        { 1.0f, 1.0f },
        { 0.0f, 1.0f },
    };
    int idx = (k < 4) ? k : 2;       /* extra verts wrap to (1,1) */
    (void)nv;
    *out_u = corners[idx][0];
    *out_v = corners[idx][1];
}

/* Pass 1: walk tree, collect batches, tally per-batch vert/tri totals.
 * Per-node selection-mesh skip via the node's selection_marker — the
 * decomp confirmed this is per-node, not just root. */
static void bake_discover(const Obj3DNode *n, BakeState *bs) {
    int p_start = (n->selection_marker != 0xFFFFFFFFu) ? 1 : 0;
    for (int p = p_start; p < n->num_primitives; p++) {
        const Obj3DPrimitive *prim = &n->primitives[p];
        int nv = prim->num_vert_indices;
        if (nv < 3) continue;

        GPU_Texture *atlas = NULL;
        SDL_FRect uv = { 0, 0, 0, 0 };
        if (prim->texture_name[0] != '\0') {
            atlas = TexAtlas_GetByName(prim->texture_name, bs->team_color_idx, &uv);
        }
        int b = batch_find_or_add(bs, atlas, &uv);
        if (b < 0) continue;
        bs->vert_count_batch[b] += nv;
        bs->tri_count_batch[b]  += (nv - 2);
    }
    for (Obj3DNode *c = n->first_child; c; c = c->next_sibling) {
        bake_discover(c, bs);
    }
}

/* Pass 3: walk tree, write each prim's data into its batch's slot.
 *
 * Decomp findings (the legacy reference ~197561-197960):
 *   - Original engine does PER-PRIMITIVE flat shading: one face normal,
 *     dotted with light, gives one shade level applied to all the
 *     prim's verts. NO Gouraud, NO per-vertex normals.
 *   - The 3DO node header at +0x0C is a "skip first prim" marker. When
 *     not -1, the first primitive of the node is the selection mesh
 *     and is skipped at render time. Per-NODE, not just root.
 *
 * Step A applies per-prim flat shading. Step B (next) replaces the
 * runtime tc_flag-based team color with per-player atlas variants. */
static void bake_emit(const Obj3DNode *n,
                      float ax, float ay, float az,
                      int parent_node_idx,
                      BakeState *bs,
                      float *positions, float *uvs, uint32_t *colors,
                      uint16_t *indices, uint16_t *vert_node_idx,
                      UnitMeshNode *nodes, int *node_count_inout,
                      float aabb_min[3], float aabb_max[3])
{
    /* Prim counter for THIS node only — the draw key is reversed node
     * index major, authored prim order minor (see the fan loop). */
    uint32_t node_local_seq = 0;

    /* Cumulative parent-chain offset (used for AABB only — vertices
     * themselves are stored in node-local space for Phase D animation). */
    float nx = ax + (float)n->offset_x;
    float ny = ay + (float)n->offset_y;
    float nz = az + (float)n->offset_z;

    /* Assign this node an index in nodes[]. DFS order means parents
     * always precede children, so parent_node_idx < this_node_idx. */
    int this_node_idx;
    if (*node_count_inout >= UNIT_MESH_MAX_NODES) {
        fprintf(stderr, "Mesh_Bake: node count exceeds %d, truncating\n",
                UNIT_MESH_MAX_NODES);
        this_node_idx = -1;
    } else {
        this_node_idx = (*node_count_inout)++;
        UnitMeshNode *out = &nodes[this_node_idx];
        size_t name_len = strlen(n->name);
        if (name_len >= sizeof(out->name)) name_len = sizeof(out->name) - 1;
        memcpy(out->name, n->name, name_len);
        out->name[name_len] = '\0';
        out->parent    = (int16_t)parent_node_idx;
        out->offset[0] = (float)n->offset_x;
        out->offset[1] = (float)n->offset_y;
        out->offset[2] = (float)n->offset_z;
    }

    /* Per-node selection-mesh skip: when selection_marker != -1, the
     * first primitive is the selection mesh and is NOT rendered. */
    int p_start = (n->selection_marker != 0xFFFFFFFFu) ? 1 : 0;

    for (int p = p_start; p < n->num_primitives; p++) {
        const Obj3DPrimitive *prim = &n->primitives[p];
        int nv = prim->num_vert_indices;
        if (nv < 3) continue;

        GPU_Texture *atlas = NULL;
        SDL_FRect uv_rect = { 0, 0, 0, 0 };
        if (prim->texture_name[0] != '\0') {
            atlas = TexAtlas_GetByName(prim->texture_name, bs->team_color_idx, &uv_rect);
        }
        int b = batch_find_or_add(bs, atlas, &uv_rect);
        if (b < 0) continue;

        /* Per-primitive flat shading. Compute the prim's face normal
         * from the first three vertices (consistent with the decomp's
         * 197710-197791 normal pass), dot with a fixed key-light, map
         * to a subtle brightness range. All verts of this prim get the
         * SAME color — flat shading per the original engine. */
        float light_intensity = 1.0f;
        if (nv >= 3) {
            int vi0 = prim->vert_indices[0];
            int vi1 = prim->vert_indices[1];
            int vi2 = prim->vert_indices[2];
            if (vi0 >= 0 && vi0 < n->num_vertices &&
                vi1 >= 0 && vi1 < n->num_vertices &&
                vi2 >= 0 && vi2 < n->num_vertices)
            {
                float ex = n->vertices[vi1].x - n->vertices[vi0].x;
                float ey = n->vertices[vi1].y - n->vertices[vi0].y;
                float ez = n->vertices[vi1].z - n->vertices[vi0].z;
                float fx = n->vertices[vi2].x - n->vertices[vi0].x;
                float fy = n->vertices[vi2].y - n->vertices[vi0].y;
                float fz = n->vertices[vi2].z - n->vertices[vi0].z;
                float nrx = ey * fz - ez * fy;
                float nry = ez * fx - ex * fz;
                float nrz = ex * fy - ey * fx;
                float len = sqrtf(nrx*nrx + nry*nry + nrz*nrz);
                if (len > 1.0f) {
                    nrx /= len; nry /= len; nrz /= len;
                    /* Light from upper-front-left, modest contrast. */
                    const float Lx = 0.4f, Ly = -0.85f, Lz = -0.3f;
                    float dot = nrx * Lx + nry * Ly + nrz * Lz;
                    light_intensity = 0.85f + 0.15f * dot;
                    if (light_intensity < 0.70f) light_intensity = 0.70f;
                    if (light_intensity > 1.0f)  light_intensity = 1.0f;
                }
            }
        }

        /* Build the per-prim color. For textured prims this is a
         * grey-scale modulator applied to the texture sample. For
         * FLAT_COLOR prims it's the artist's palette[color_idx]. */
        uint32_t prim_color;
        if (atlas == NULL && bs->palette) {
            prim_color = bs->palette[prim->color_idx & 0xFF];
            /* Don't dim the artist's chosen flat color via lighting —
             * keep it full intensity so banners and decals stay punchy. */
        } else {
            uint32_t L = (uint32_t)(light_intensity * 255.0f);
            if (L > 255) L = 255;
            prim_color = L | (L << 8) | (L << 16) | (0xFFu << 24);
        }

        int base = bs->v_writer[b];   /* batch-local vertex base */
        for (int k = 0; k < nv; k++) {
            int vi = prim->vert_indices[k];
            /* Node-local vertex position: NO parent-chain offset folded
             * in. The submit path applies the parent chain (and any
             * runtime per-piece transform) at draw time. */
            float lx, ly, lz;
            if (vi < 0 || vi >= n->num_vertices) {
                lx = ly = lz = 0.0f;     /* degenerate fallback at node origin */
            } else {
                lx = n->vertices[vi].x;
                ly = n->vertices[vi].y;
                lz = n->vertices[vi].z;
                /* AABB still computed in unit-folded space for cull. */
                float wx = lx + nx, wy = ly + ny, wz = lz + nz;
                if (wx < aabb_min[0]) aabb_min[0] = wx;
                if (wy < aabb_min[1]) aabb_min[1] = wy;
                if (wz < aabb_min[2]) aabb_min[2] = wz;
                if (wx > aabb_max[0]) aabb_max[0] = wx;
                if (wy > aabb_max[1]) aabb_max[1] = wy;
                if (wz > aabb_max[2]) aabb_max[2] = wz;
            }

            int slot = bs->v_writer[b];
            positions[3 * slot + 0] = lx;
            positions[3 * slot + 1] = ly;
            positions[3 * slot + 2] = lz;
            vert_node_idx[slot] = (uint16_t)((this_node_idx >= 0) ? this_node_idx : 0);
            colors[slot] = prim_color;

            if (atlas) {
                float lu, lv;
                prim_local_uv(k, nv, &lu, &lv);
                /* Inset UV rect by 1.5 texels per side so bilinear
                 * filtering never reaches into adjacent atlas entries.
                 * 0.5 wasn't enough for tiny entries (8×16 etc) where
                 * neighbors had highly-saturated colors that bled into
                 * polygon edges as visible "outlines". 1.5 texels is
                 * a stronger guard that costs ~3 of the entry's texels
                 * along each edge — fine for small entries since the
                 * inner pixels still cover the polygon. */
                const float INSET = 1.5f / 1024.0f;
                float u0 = uv_rect.x + INSET;
                float v0 = uv_rect.y + INSET;
                float uw = uv_rect.w - 2.0f * INSET;
                float vh = uv_rect.h - 2.0f * INSET;
                if (uw < 0.0f) uw = 0.0f;
                if (vh < 0.0f) vh = 0.0f;
                uvs[2 * slot + 0] = u0 + lu * uw;
                uvs[2 * slot + 1] = v0 + lv * vh;
            } else {
                uvs[2 * slot + 0] = 0.0f;
                uvs[2 * slot + 1] = 0.0f;
            }
            bs->v_writer[b]++;
        }

        /* Fan: indices are batch-relative. Legacy rasterizes model
         * NODES in REVERSE table order with prims forward inside a node
         * (Model_RasterizePrims :197658/:197944) — children paint
         * first, parents over them (that's why a cape never shows
         * through the torso). Key = reversed node index major, prim
         * order minor. */
        for (int t = 1; t < nv - 1; t++) {
            if (bs->tri_seq) {
                uint32_t node_key = (uint32_t)(UNIT_MESH_MAX_NODES - 1 -
                    ((this_node_idx >= 0) ? this_node_idx : 0));
                if (node_local_seq > 4095u) node_local_seq = 4095u;
                bs->tri_seq[bs->i_writer[b] / 3] =
                    node_key * 4096u + node_local_seq++;
            }
            indices[bs->i_writer[b]++] = (uint16_t)(base + 0);
            indices[bs->i_writer[b]++] = (uint16_t)(base + t);
            indices[bs->i_writer[b]++] = (uint16_t)(base + t + 1);
        }
    }

    for (Obj3DNode *c = n->first_child; c; c = c->next_sibling) {
        bake_emit(c, nx, ny, nz, this_node_idx, bs,
                  positions, uvs, colors, indices, vert_node_idx,
                  nodes, node_count_inout,
                  aabb_min, aabb_max);
    }
}

static UnitMesh *Mesh_Bake(const Obj3DFile *obj, const uint32_t *palette,
                            int color_idx)
{
    if (!obj || !obj->root) return NULL;

    BakeState bs;
    memset(&bs, 0, sizeof(bs));
    bs.palette = palette;
    bs.team_color_idx = color_idx;

    /* Pass 1 — discover. Per-node selection-mesh skip via marker. */
    bake_discover(obj->root, &bs);
    if (bs.batch_count == 0) return NULL;

    /* Pass 2 — layout. Prefix-sum per-batch totals. */
    int total_v = 0, total_i = 0;
    for (int b = 0; b < bs.batch_count; b++) {
        bs.first_vert[b]  = total_v;
        bs.first_index[b] = total_i;
        bs.v_writer[b]    = bs.first_vert[b];
        bs.i_writer[b]    = bs.first_index[b];
        total_v += bs.vert_count_batch[b];
        total_i += bs.tri_count_batch[b] * 3;
    }
    if (total_v == 0 || total_i == 0) return NULL;
    if (total_v > 65535) {
        fprintf(stderr, "Mesh_Bake: %d verts exceeds uint16 index range\n", total_v);
        return NULL;
    }

    UnitMesh *m = (UnitMesh *)tak_malloc(sizeof(UnitMesh));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));

    m->positions     = (float *)   tak_malloc(sizeof(float)    * 3 * (size_t)total_v);
    m->uvs           = (float *)   tak_malloc(sizeof(float)    * 2 * (size_t)total_v);
    m->colors        = (uint32_t *)tak_malloc(sizeof(uint32_t)     * (size_t)total_v);
    m->indices       = (uint16_t *)tak_malloc(sizeof(uint16_t)     * (size_t)total_i);
    m->vert_node_idx = (uint16_t *)tak_malloc(sizeof(uint16_t)     * (size_t)total_v);
    m->tri_seq       = (uint32_t *)tak_malloc(sizeof(uint32_t) * ((size_t)total_i / 3));
    if (!m->positions || !m->uvs || !m->colors || !m->indices ||
        !m->vert_node_idx || !m->tri_seq) {
        if (m->positions)     tak_free(m->positions);
        if (m->uvs)           tak_free(m->uvs);
        if (m->colors)        tak_free(m->colors);
        if (m->indices)       tak_free(m->indices);
        if (m->vert_node_idx) tak_free(m->vert_node_idx);
        if (m->tri_seq)       tak_free(m->tri_seq);
        tak_free(m);
        return NULL;
    }
    bs.tri_seq = m->tri_seq;
    bs.seq_counter = 0;

    m->aabb_min[0] = m->aabb_min[1] = m->aabb_min[2] = +1e30f;
    m->aabb_max[0] = m->aabb_max[1] = m->aabb_max[2] = -1e30f;
    m->node_count = 0;

    /* Pass 3 — emit. Walks the tree; per-node bookkeeping populates
     * nodes[] and vert_node_idx[]; vertices stored node-local. */
    bake_emit(obj->root, 0.0f, 0.0f, 0.0f, /*parent=*/-1, &bs,
              m->positions, m->uvs, m->colors, m->indices, m->vert_node_idx,
              m->nodes, &m->node_count,
              m->aabb_min, m->aabb_max);

    m->vert_count  = total_v;
    m->tri_count   = total_i / 3;
    m->batch_count = bs.batch_count;
    for (int b = 0; b < bs.batch_count; b++) {
        m->batches[b].atlas_tex   = bs.atlas[b];
        m->batches[b].first_index = bs.first_index[b];
        m->batches[b].index_count = bs.tri_count_batch[b] * 3;
    }
    return m;
}

static void Mesh_Free(UnitMesh *m) {
    if (!m) return;
    if (m->positions)     tak_free(m->positions);
    if (m->uvs)           tak_free(m->uvs);
    if (m->colors)        tak_free(m->colors);
    if (m->indices)       tak_free(m->indices);
    if (m->vert_node_idx) tak_free(m->vert_node_idx);
    if (m->tri_seq)       tak_free(m->tri_seq);
    tak_free(m);
}

/* Lazy-load + bake the mesh for a (UnitDef, color_idx) pair. Each
 * player team color produces a different baked mesh because its batch
 * atlases are per-color. Returns 0 on success. Cached result lives on
 * def->mesh_per_color[color_idx]. */
static int ensure_mesh_baked(UnitDef *def, int color_idx) {
    if (!def) return -1;
    if (color_idx < 0 || color_idx > 11) color_idx = 0;
    if (def->mesh_per_color[color_idx]) return 0;
    if (def->objectname[0] == '\0') return -1;

    char obj_lc[TAK_UNITDEF_OBJ_MAX];
    lowercase_into(obj_lc, sizeof(obj_lc), def->objectname);

    char path[TAK_UNITDEF_OBJ_MAX + 16];
    snprintf(path, sizeof(path), "objects3d/%s.3do", obj_lc);

    Obj3DFile *obj = NULL;
    if (Obj3D_Load(&obj, path) != 0 || !obj) {
        fprintf(stderr, "ensure_mesh_baked: Obj3D_Load failed for %s\n", path);
        return -1;
    }

    /* Faction palette for FLAT_COLOR fallbacks. World should always
     * exist by the time we get here (we're in a skirmish), but
     * tolerate NULL — flat-color verts will end up black. */
    GameWorld *world = World_Get();
    const uint32_t *palette = world ? world->terrain_rgba : NULL;

    /* Diagnostic prim probe: set TAK_PRIM_PROBE=<unitname> to dump a
     * unit's full prim/texture/color resolution at bake time. */
    const char *probe_name = getenv("TAK_PRIM_PROBE");
    if (probe_name && stricmp_bounded(def->unitname, probe_name) == 0) {
        fprintf(stderr, "----- %s prim probe (color=%d) -----\n",
                def->unitname, color_idx);
        zon_probe(obj->root, 0, color_idx);
        fprintf(stderr, "----- end %s probe -----\n", def->unitname);
    }

    UnitMesh *m = Mesh_Bake(obj, palette, color_idx);
    Obj3D_Close(obj);                        /* tree no longer needed */

    if (!m) {
        fprintf(stderr, "ensure_mesh_baked: Mesh_Bake failed for %s\n", def->unitname);
        return -1;
    }

    def->mesh_per_color[color_idx] = m;
    fprintf(stderr,
        "Mesh_Bake: %s color=%d -> %d verts, %d tris, %d batches\n",
        def->unitname, color_idx, m->vert_count, m->tri_count, m->batch_count);
    return 0;
}

/* ── Scratch buffer growth ────────────────────────────────────────── */

static int ensure_scratch(int need_verts, int need_idx) {
    if (need_verts > g_scratch_vcap) {
        int new_cap = g_scratch_vcap ? g_scratch_vcap * 2 : 1024;
        while (new_cap < need_verts) new_cap *= 2;
        float    *xy = (float *)tak_malloc(sizeof(float) * 2 * (size_t)new_cap);
        uint32_t *cl = (uint32_t *)tak_malloc(sizeof(uint32_t) * (size_t)new_cap);
        float    *uv = (float *)tak_malloc(sizeof(float) * 2 * (size_t)new_cap);
        float    *wz = (float *)tak_malloc(sizeof(float) * (size_t)new_cap);
        float    *hk = (float *)tak_malloc(sizeof(float) * (size_t)new_cap);
        if (!xy || !cl || !uv || !wz || !hk) {
            if (xy) tak_free(xy);
            if (cl) tak_free(cl);
            if (uv) tak_free(uv);
            if (wz) tak_free(wz);
            if (hk) tak_free(hk);
            return -1;
        }
        if (g_scratch_xy)    tak_free(g_scratch_xy);
        if (g_scratch_color) tak_free(g_scratch_color);
        if (g_scratch_uv)    tak_free(g_scratch_uv);
        if (g_scratch_wz)    tak_free(g_scratch_wz);
        if (g_scratch_hkey)  tak_free(g_scratch_hkey);
        g_scratch_xy = xy; g_scratch_color = cl; g_scratch_uv = uv; g_scratch_wz = wz;
        g_scratch_hkey = hk;
        g_scratch_vcap = new_cap;
    }
    /* Node xform scratch — sized to the largest mesh seen. */
    if (UNIT_MESH_MAX_NODES > g_scratch_node_cap) {
        if (g_scratch_node_xform) tak_free(g_scratch_node_xform);
        g_scratch_node_xform = (NodeXform *)tak_malloc(sizeof(NodeXform) * UNIT_MESH_MAX_NODES);
        if (!g_scratch_node_xform) return -1;
        g_scratch_node_cap = UNIT_MESH_MAX_NODES;
    }
    if (need_idx > g_scratch_icap) {
        int new_cap = g_scratch_icap ? g_scratch_icap * 2 : 4096;
        while (new_cap < need_idx) new_cap *= 2;
        uint16_t *idx = (uint16_t *)tak_malloc(sizeof(uint16_t) * (size_t)new_cap);
        TriSort *tri = (TriSort *)tak_malloc(sizeof(TriSort) * (size_t)(new_cap / 3 + 1));
        if (!idx || !tri) {
            if (idx) tak_free(idx);
            if (tri) tak_free(tri);
            return -1;
        }
        if (g_scratch_idx) tak_free(g_scratch_idx);
        if (g_scratch_tri) tak_free(g_scratch_tri);
        g_scratch_idx = idx;
        g_scratch_tri = tri;
        g_scratch_icap = new_cap;
        g_scratch_tricap = new_cap / 3 + 1;
    }
    return 0;
}

static void scratch_free(void) {
    if (g_scratch_xy)    { tak_free(g_scratch_xy);    g_scratch_xy    = NULL; }
    if (g_scratch_color) { tak_free(g_scratch_color); g_scratch_color = NULL; }
    if (g_scratch_uv)    { tak_free(g_scratch_uv);    g_scratch_uv    = NULL; }
    if (g_scratch_wz)    { tak_free(g_scratch_wz);    g_scratch_wz    = NULL; }
    if (g_scratch_hkey)  { tak_free(g_scratch_hkey);  g_scratch_hkey  = NULL; }
    if (g_scratch_idx)        { tak_free(g_scratch_idx);        g_scratch_idx        = NULL; }
    if (g_scratch_tri)        { tak_free(g_scratch_tri);        g_scratch_tri        = NULL; }
    if (g_scratch_node_xform) { tak_free(g_scratch_node_xform); g_scratch_node_xform = NULL; }
    g_scratch_vcap = 0;
    g_scratch_icap = 0;
    g_scratch_tricap = 0;
    g_scratch_node_cap = 0;
}

/* Legacy height key for one transformed vertex: the whole-unit part of
 * the model-space Y, exactly what the rasteriser stores per pixel
 * (legacy:197697, `base + (y >> 16)`). Quantising to whole units keeps
 * legacy's byte resolution, so coplanar pieces tie and fall through to
 * the authored node order. */
static float model_height_key(float model_y) {
    return floorf(model_y * (1.0f / 65536.0f));
}

/* Highest of a triangle's three vertex keys. Legacy resolves the same
 * comparison per pixel; the tallest vertex is the closest per-triangle
 * stand-in (legacy:265317). */
static float tri_height_key(const float *hkey, int i0, int i1, int i2) {
    float k = hkey[i0];
    if (hkey[i1] > k) k = hkey[i1];
    if (hkey[i2] > k) k = hkey[i2];
    return k;
}

static int tri_cmp_far_first(const void *a, const void *b) {
    /* Lowest height key first, so the taller (nearer) surface paints
     * over it, the ordering legacy's key buffer produces by rejecting
     * any pixel whose key is not above what is already there
     * (legacy:265317). Equal keys fall back to the authored node
     * order, reverse node table (legacy:197658, :197944). */
    const TriSort *ta = (const TriSort *)a;
    const TriSort *tb = (const TriSort *)b;
    if (ta->hkey < tb->hkey) return -1;
    if (ta->hkey > tb->hkey) return  1;
    if (ta->key  < tb->key)  return -1;
    if (ta->key  > tb->key)  return  1;
    return 0;
}

/* ── Registry API ─────────────────────────────────────────────────── */

int Units_LoadDefs(void) {
    Units_FreeDefs();

    char **paths = NULL;
    int n = 0;
    if (VFS_ListFiles("units/*.fbi", &paths, &n) != 0 || n <= 0) {
        fprintf(stderr, "Units_LoadDefs: no .fbi files found in VFS\n");
        return 0;
    }

    int loaded = 0;
    int skipped = 0;
    for (int i = 0; i < n; i++) {
        if (ensure_def_capacity() != 0) {
            fprintf(stderr, "Units_LoadDefs: OOM growing def array\n");
            return -1;
        }
        UnitDef d;
        if (parse_fbi(paths[i], &d) != 0 || d.unitname[0] == '\0') {
            if (d.yardmap) tak_free(d.yardmap);
            skipped++;
            continue;
        }
        /* Try to load the matching .cob (Phase D). Filename is the
         * lowercased unitname. Walls and similar static structures
         * have stub .cobs (1 script, 0 pieces); that's fine. Units
         * without any .cob still load — they just won't animate. */
        char cob_lc[TAK_UNITDEF_NAME_MAX];
        lowercase_into(cob_lc, sizeof(cob_lc), d.unitname);
        char cob_path[TAK_UNITDEF_NAME_MAX + 16];
        snprintf(cob_path, sizeof(cob_path), "scripts/%s.cob", cob_lc);
        d.cob_script = NULL;
        Cob_Load(&d.cob_script, cob_path);   /* NULL on miss — that's OK */
        g_defs[g_def_count++] = d;
        loaded++;
    }

    fprintf(stderr, "Units_LoadDefs: %d defs loaded (%d skipped)\n",
            loaded, skipped);
    return loaded;
}

void Units_FreeDefs(void) {
    if (g_defs) {
        for (int i = 0; i < g_def_count; i++) {
            for (int c = 0; c < 12; c++) {
                if (g_defs[i].mesh_per_color[c]) {
                    Mesh_Free(g_defs[i].mesh_per_color[c]);
                    g_defs[i].mesh_per_color[c] = NULL;
                }
            }
            if (g_defs[i].cob_script) {
                Cob_Free(g_defs[i].cob_script);
                g_defs[i].cob_script = NULL;
            }
            if (g_defs[i].yardmap) {
                tak_free(g_defs[i].yardmap);
                g_defs[i].yardmap = NULL;
            }
        }
        tak_free(g_defs);
    }
    g_defs      = NULL;
    g_def_count = 0;
    g_def_cap   = 0;
    /* Projectile model meshes reference the same texture atlases. */
    proj_model_drop_meshes();
    scratch_free();
}

int Units_FindDefByName(const char *unitname) {
    if (!unitname || !g_defs) return -1;
    for (int i = 0; i < g_def_count; i++) {
        if (stricmp_bounded(g_defs[i].unitname, unitname) == 0) return i;
    }
    return -1;
}

/* "<prefix> Monarch" word-boundary match: prefix at position 0
 * followed by space/null. Avoids "ARA" matching "ARAA" or "AR". */
static int category_starts_with_side(const char *category, const char *prefix) {
    size_t n = 0;
    while (prefix[n] != '\0') {
        char a = category[n];
        char b = prefix[n];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
        n++;
    }
    char next = category[n];
    return (next == ' ' || next == '\0' || next == '\t');
}

int Units_FindMonarchDef(const char *side_prefix) {
    if (!side_prefix || !g_defs) return -1;

    /* Recon R1 finding (PHASE_C_3DO.md §5.R1): the generic walk silently
     * picks campaign-protected variants on TAR (tarnecr2 < tarnecro
     * alphabetically) and ZON (zonhurt has its own variant mesh).
     * Prefer the skirmish-canonical unitname per side. */
    static const struct {
        const char *prefix;
        const char *canonical_unitname;
    } canonical[] = {
        { "ARA", "ARAKING"  },
        { "TAR", "TARNECRO" },
        { "VER", "VERMAGE"  },
        { "ZON", "ZONHUNT"  },
    };
    for (size_t k = 0; k < sizeof(canonical)/sizeof(canonical[0]); k++) {
        if (stricmp_bounded(side_prefix, canonical[k].prefix) == 0) {
            int idx = Units_FindDefByName(canonical[k].canonical_unitname);
            if (idx >= 0) return idx;
            break;
        }
    }

    for (int i = 0; i < g_def_count; i++) {
        const char *cat = g_defs[i].category;
        if (!category_starts_with_side(cat, side_prefix)) continue;
        size_t n = strlen(side_prefix);
        if (cat[n] != ' ') continue;
        const char *rest = cat + n + 1;
        if (stricmp_bounded(rest, "Monarch") == 0) return i;
    }
    return -1;
}

const UnitDef *Units_GetDef(int idx) {
    if (!g_defs || idx < 0 || idx >= g_def_count) return NULL;
    return &g_defs[idx];
}

int Units_GetDefCount(void) { return g_def_count; }

/* ── Unit occupancy layer ─────────────────────────────────────────────
 *
 * Structures stamp their yardmap into the world occupancy grid so
 * walls, gates and buildings block movement and planning
 * (legacy:217974-218031). Mobile defs never imprint. */

static int unit_def_is_structure(const UnitDef *d) {
    return d && d->yardmap && d->bmcode == 0 &&
           d->footprint_x > 0 && d->footprint_z > 0;
}

static int occ_stamp_for(int handle, TAK_OccStamp *st) {
    if (handle < 0 || handle >= g_unit_count || !st) return 0;
    const Unit *u = &g_units[handle];
    const UnitDef *d = Units_GetDef(u->def_idx);
    if (!unit_def_is_structure(d)) return 0;
    st->fx = d->footprint_x;
    st->fz = d->footprint_z;
    /* Same centre-anchored footprint rect Units_IsBuildSiteClear uses. */
    st->tx0 = Occ_TileOf(u->world_x - d->footprint_x * 8);
    st->ty0 = Occ_TileOf(u->world_y - d->footprint_z * 8);
    st->yard = d->yardmap;
    st->yard_open = u->cob_yard_open;
    st->is_gate = d->is_gate;
    st->handle = handle;
    st->owner = u->player_id;
    return 1;
}

/* Footprint-local mask of tiles a live mobile unit stands on. Built
 * with one linear pass so the per-cell test stays O(1); the imprint
 * yields those cells and retries (legacy:217994-218006), and the
 * refusable yard setter asks the same question (legacy:218984-219042). */
#define OCC_BUSY_MAX 64
static uint8_t g_occ_busy[OCC_BUSY_MAX * OCC_BUSY_MAX];
static int     g_occ_busy_tx0, g_occ_busy_ty0, g_occ_busy_fx, g_occ_busy_fz;

static void unit_occ_fp(const Unit *u, int *fx, int *fz);
static int  unit_is_mobile_occupant(const Unit *u, const UnitDef *d);

static void occ_busy_build(const TAK_OccStamp *st) {
    g_occ_busy_tx0 = st->tx0;
    g_occ_busy_ty0 = st->ty0;
    g_occ_busy_fx  = st->fx > OCC_BUSY_MAX ? 0 : st->fx;
    g_occ_busy_fz  = st->fz > OCC_BUSY_MAX ? 0 : st->fz;
    if (g_occ_busy_fx <= 0 || g_occ_busy_fz <= 0) return;
    memset(g_occ_busy, 0, (size_t)g_occ_busy_fx * g_occ_busy_fz);
    for (int i = 0; i < g_unit_count; i++) {
        const Unit *o = &g_units[i];
        if (i == st->handle) continue;
        const UnitDef *od = Units_GetDef(o->def_idx);
        if (unit_def_is_structure(od)) continue;
        if (!unit_is_mobile_occupant(o, od)) continue;
        int ofx, ofz;
        unit_occ_fp(o, &ofx, &ofz);
        int c0 = Occ_TileOf(o->world_x - ofx * 8) - st->tx0;
        int r0 = Occ_TileOf(o->world_y - ofz * 8) - st->ty0;
        for (int r = r0; r < r0 + ofz; r++) {
            if (r < 0 || r >= g_occ_busy_fz) continue;
            for (int c = c0; c < c0 + ofx; c++) {
                if (c < 0 || c >= g_occ_busy_fx) continue;
                g_occ_busy[r * g_occ_busy_fx + c] = 1;
            }
        }
    }
}

static int occ_busy_lookup(void *user, int tx, int ty) {
    (void)user;
    int col = tx - g_occ_busy_tx0;
    int row = ty - g_occ_busy_ty0;
    if (col < 0 || row < 0 || col >= g_occ_busy_fx || row >= g_occ_busy_fz)
        return 0;
    return g_occ_busy[row * g_occ_busy_fx + col];
}

static void occ_refresh(int handle) {
    GameWorld *w = World_Get();
    TAK_OccStamp st;
    if (!w || !occ_stamp_for(handle, &st)) return;
    Unit *u = &g_units[handle];
    if (!Occ_Ensure(w)) { u->occ_pending = 1; return; }
    occ_busy_build(&st);
    u->occ_pending = Occ_ImprintStamp(w, &st, 1, occ_busy_lookup, NULL)
                   ? 0 : 1;
    u->occ_on = 1;
}

/* ── Mobile footprints ────────────────────────────────────────────────
 *
 * Legacy stamps a moving unit's footprint into the same map cells it
 * uses for buildings (legacy:217933-217971), and the move step refuses
 * a cell another unit holds (legacy:219329-219340). That is what stops
 * units coalescing and what makes a crowd pack instead of stack. */

static const MoveClassDef *unit_move_class(const GameWorld *w,
                                           const UnitDef *def);

static void unit_mobile_footprint(const GameWorld *w, const UnitDef *d,
                                  int *out_fx, int *out_fz) {
    /* Legacy takes the footprint from the resolved move class and only
     * falls back to the def's own (legacy:163193-163195). */
    const MoveClassDef *mc = unit_move_class(w, d);
    int fx = (mc && mc->footprint_x > 0) ? mc->footprint_x
           : (d && d->footprint_x > 0) ? d->footprint_x : 1;
    int fz = (mc && mc->footprint_z > 0) ? mc->footprint_z
           : (d && d->footprint_z > 0) ? d->footprint_z : 1;
    if (fx > 8) fx = 8;
    if (fz > 8) fz = 8;
    *out_fx = fx;
    *out_fz = fz;
}

/* Resolved once per unit: the step test runs per probe and must not
 * re-search the move-class table each time. */
static void unit_cache_footprint(Unit *u) {
    int fx = 1, fz = 1;
    unit_mobile_footprint(World_Get(), Units_GetDef(u->def_idx), &fx, &fz);
    u->occ_fx = (uint8_t)fx;
    u->occ_fz = (uint8_t)fz;
}

static void unit_occ_fp(const Unit *u, int *fx, int *fz) {
    *fx = u->occ_fx ? u->occ_fx : 1;
    *fz = u->occ_fz ? u->occ_fz : 1;
}

static int unit_is_mobile_occupant(const Unit *u, const UnitDef *d) {
    if (!d) return 0;
    if (d->can_fly) return 0;             /* flyers share ground freely */
    return u->alive == UNIT_ALIVE_ACTIVE;
}

static void occ_sync_mobile(int handle) {
    GameWorld *w = World_Get();
    if (!w || !w->occ || handle < 0 || handle >= g_unit_count) return;
    Unit *u = &g_units[handle];
    const UnitDef *d = Units_GetDef(u->def_idx);
    if (unit_def_is_structure(d)) return;   /* structures use occ_refresh */
    int fx, fz;
    unit_occ_fp(u, &fx, &fz);
    if (!unit_is_mobile_occupant(u, d)) {
        if (u->occ_on) {
            Occ_LiftMobile(w, handle, u->occ_tx, u->occ_ty, fx, fz);
            u->occ_on = 0;
        }
        return;
    }
    int tx = Occ_TileOf(u->world_x - fx * 8);
    int ty = Occ_TileOf(u->world_y - fz * 8);
    if (u->occ_on && u->occ_tx == (int16_t)tx && u->occ_ty == (int16_t)ty)
        return;
    Occ_MoveMobile(w, handle, u->player_id, u->occ_on,
                   u->occ_tx, u->occ_ty, tx, ty, fx, fz);
    u->occ_tx = (int16_t)tx;
    u->occ_ty = (int16_t)ty;
    u->occ_on = 1;
}

static void occ_lift(int handle) {
    GameWorld *w = World_Get();
    if (!w || !w->occ || handle < 0 || handle >= g_unit_count) return;
    Unit *u = &g_units[handle];
    TAK_OccStamp st;
    if (occ_stamp_for(handle, &st)) {
        Occ_ImprintStamp(w, &st, 0, NULL, NULL);
    } else if (u->occ_on) {
        int fx, fz;
        unit_occ_fp(u, &fx, &fz);
        Occ_LiftMobile(w, handle, u->occ_tx, u->occ_ty, fx, fz);
    }
    u->occ_pending = 0;
    u->occ_on = 0;
}

/* Refusable SET YARD_OPEN: the legacy setter first asks whether any
 * cell that would block under the NEW state holds another live unit,
 * and silently drops the set when one does. That refusal is what
 * drives the gate script's retry loop (legacy:219046-219059 over
 * :218984-219042). On success the footprint is re-imprinted
 * (legacy:219053-219056). */
int Units_TrySetYardOpen(int handle, int open) {
    if (handle < 0 || handle >= g_unit_count) return 0;
    Unit *u = &g_units[handle];
    open = open ? 1 : 0;
    if (u->cob_yard_open == (uint8_t)open) return 1;
    GameWorld *w = World_Get();
    TAK_OccStamp st;
    if (!w || !Occ_Ensure(w) || !occ_stamp_for(handle, &st)) {
        u->cob_yard_open = (uint8_t)open;
        return 1;
    }
    occ_busy_build(&st);
    if (Occ_AnyBlockingCell(&st, open, occ_busy_lookup, NULL)) return 0;
    Occ_ImprintStamp(w, &st, 0, NULL, NULL);      /* lift the old state */
    u->cob_yard_open = (uint8_t)open;
    st.yard_open = open;
    u->occ_pending = Occ_ImprintStamp(w, &st, 1, occ_busy_lookup, NULL)
                   ? 0 : 1;
    u->occ_on = 1;
    return 1;
}

/* ── Active-array API ─────────────────────────────────────────────── */

void Units_ClearInstances(void) {
    /* Free per-unit COB engines before zeroing metadata. */
    for (int i = 0; i < g_unit_count; i++) {
        if (g_units[i].cob) {
            Cob_EngineFree(g_units[i].cob);
            tak_free(g_units[i].cob);
            g_units[i].cob = NULL;
        }
    }
    Occ_Clear(World_Get());
    /* Teardown runs while the renderer that made the projectile art is
     * still up, so release the strips here and retire the epoch. */
    proj_art_release_textures();
    memset(g_units, 0, sizeof(g_units));
    memset(g_projectiles, 0, sizeof(g_projectiles));
    memset(g_proj_effects, 0, sizeof(g_proj_effects));
    g_unit_count = 0;
    g_projectile_count = 0;
    g_proj_effect_count = 0;
    g_next_stable_unit_id = 1;
}

/* Forward decls for COB host callbacks; bodies are below. */
static int32_t cob_host_get_unit_value(void *user, int param);
static void    cob_host_set_unit_value(void *user, int port, int32_t value);
static int32_t cob_host_play_sound(void *user, const char *sound_name,
                                   int32_t arg);
static int32_t cob_host_call_function(void *user, int fn_id,
                                       int n_args, const int32_t *args);

int Units_Spawn(int def_idx, int player_id, int team_color_idx,
                int32_t world_x, int32_t world_y) {
    if (def_idx < 0 || def_idx >= g_def_count) return -1;
    if (g_unit_count >= TAK_MAX_UNITS) return -1;
    if (team_color_idx < 0 || team_color_idx > 11) team_color_idx = 0;
    /* Ensure the per-color mesh variant is baked. Cheap when cached. */
    ensure_mesh_baked(&g_defs[def_idx], team_color_idx);

    int slot = g_unit_count++;
    Unit *u = &g_units[slot];
    /* Defensive: free any prior engine from a recycled slot. */
    if (u->cob) { Cob_EngineFree(u->cob); tak_free(u->cob); u->cob = NULL; }
    UnitDef *def = &g_defs[def_idx];
    u->stable_id      = g_next_stable_unit_id++;
    if (g_next_stable_unit_id == 0) g_next_stable_unit_id = 1;
    u->world_x        = world_x;
    u->world_y        = world_y;
    u->heading        = 0.0f;
    u->velocity       = 0;
    u->health         = def->max_health > 0 ? def->max_health : 100;
    u->max_health     = u->health;
    u->cmd_x          = 0;
    u->cmd_y          = 0;
    u->patrol_x       = world_x;
    u->patrol_y       = world_y;
    u->target         = -1;
    u->cmd_kind       = UNIT_CMD_NONE;
    u->reclaim_tile_x = -1;
    u->reclaim_tile_y = -1;
    u->reclaim_accum  = 0.0f;
    u->attack_cooldown = 0;
    u->subpixel_x     = 0.0f;
    u->subpixel_y     = 0.0f;
    unit_clear_path(u);
    u->anim_state     = UNIT_ANIM_IDLE;
    u->walk_thread_slot   = -1;
    u->killed_thread_slot = -1;
    u->build_thread_slot  = -1;
    u->move_rate_tier     = -1;   /* unset: first update always reports */
    u->turn_dir_sign      = 0;
    for (int ev = 0; ev < UNIT_SCRIPT_EV_COUNT; ev++) u->script_ev[ev] = 0;
    for (int wi = 0; wi < 3; wi++) {
        u->weapon_state[wi].cooldown_ticks = 0;
        u->weapon_state[wi].burst_ticks = 0;
        u->weapon_state[wi].burst_remaining = 0;
        u->weapon_state[wi].burst_target = -1;
        u->weapon_state[wi].aim_thread_slot = -1;
        u->weapon_state[wi].aim_target = -1;
    }
    u->def_idx        = (uint16_t)def_idx;
    u->player_id      = (uint8_t)player_id;
    u->team_color_idx = (uint8_t)team_color_idx;
    u->alive          = UNIT_ALIVE_ACTIVE;
    /* Default aggression posture is OFFENSIVE — matches legacy
     * (units freshly spawned chase enemies in sight range). */
    u->aggro_mode    = UNIT_AGGRO_OFFENSIVE;
    u->weapon_slot   = 0;            /* Primary weapon by default */
    u->experience_pts = 0;
    u->build_target  = -1;
    u->carried_by = -1;
    u->cargo_count = 0;
    u->cargo_size_used = 0;
    u->under_construction = 0;
    u->build_hp_accum = 0.0f;
    u->cob_yard_open = 0;
    u->cob_bugger_off = 0;
    u->occ_on = 0;
    u->occ_pending = 0;
    u->gate_scan_cd = 0;
    u->gate_hold = 0;
    u->occ_tx = 0;
    u->occ_ty = 0;
    unit_cache_footprint(u);
    /* Face south (toward the viewer) at spawn, like the original.
     * Heading 0 = north under the corrected projection convention. */
    u->heading = 3.14159265f;

    /* Stamp the footprint before the script runs: the legacy building
     * imprint happens the moment the unit exists, nanoframes included
     * (legacy:217974). Mobiles stamp their footprint the same way
     * (legacy:217933-217971). */
    occ_refresh(slot);
    occ_sync_mobile(slot);

    /* Allocate the per-unit COB engine and bind it to this def's
     * script + the mesh's node names. If no script is loaded for the
     * def, the unit just renders statically (no animation). */
    const UnitMesh *m = def->mesh_per_color[team_color_idx];
    if (def->cob_script && m && m->node_count > 0) {
        u->cob = (CobEngine *)tak_malloc(sizeof(CobEngine));
        if (u->cob) {
            /* Build a temporary array of node-name pointers for binding. */
            const char *node_names[UNIT_MESH_MAX_NODES];
            int nc = m->node_count;
            if (nc > UNIT_MESH_MAX_NODES) nc = UNIT_MESH_MAX_NODES;
            for (int i = 0; i < nc; i++) node_names[i] = m->nodes[i].name;
            if (Cob_EngineInit(u->cob, def->cob_script, nc, node_names) != 0) {
                tak_free(u->cob);
                u->cob = NULL;
            } else {
                /* Wire host callbacks so GET-UNIT-VALUE / CALL-FUNCTION
                 * can read this unit's state. */
                Cob_EngineSetHost(u->cob, u,
                                   cob_host_get_unit_value,
                                   cob_host_call_function);
                Cob_EngineSetHostSetter(u->cob, cob_host_set_unit_value);
                Cob_EngineSetHostPlaySound(u->cob, cob_host_play_sound);
                /* Run Create immediately so initial pose (HIDE/TURN-PIECE
                 * etc.) is set before the first frame renders. */
                Cob_StartThreadByName(u->cob, "Create", NULL, 0);
                Cob_RunAllThreads(u->cob);
            }
        }
    }
    return slot;
}

const Unit *Units_GetActive(int *out_count) {
    if (out_count) *out_count = g_unit_count;
    return g_units;
}

/* ── Projection tuning ────────────────────────────────────────────── */

uint32_t Units_GetStableId(int handle) {
    if (handle < 0 || handle >= g_unit_count) return 0;
    if (g_units[handle].alive < 1) return 0;
    return g_units[handle].stable_id;
}

int Units_FindByStableId(uint32_t stable_id) {
    if (stable_id == 0) return -1;
    for (int i = 0; i < g_unit_count; i++) {
        if (g_units[i].alive >= 1 && g_units[i].stable_id == stable_id) {
            return i;
        }
    }
    return -1;
}

float Units_GetTAScale(void)        { return g_ta_scale; }
void  Units_SetTAScale(float s)     { if (s > 0.0f) g_ta_scale = s; }
float Units_GetTanTilt(void)        { return g_tan_tilt; }
void  Units_SetTanTilt(float t)     { g_tan_tilt = t; }

/* ── Debug spawn for M4 ───────────────────────────────────────────── */

void Units_SetHeading(int handle, float heading) {
    if (handle < 0 || handle >= g_unit_count) return;
    if (!g_units[handle].alive) return;
    g_units[handle].heading = heading;
}

void Units_SetHealthPercent(int handle, int pct) {
    Unit *u;
    int max_hp;
    if (handle < 0 || handle >= g_unit_count) return;
    u = &g_units[handle];
    if (u->alive != UNIT_ALIVE_ACTIVE) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    max_hp = u->max_health > 0 ? u->max_health : 1;
    u->health = (max_hp * pct) / 100;
    if (pct > 0 && u->health < 1) u->health = 1;
}

void Units_DebugRotateHead(int32_t delta_units) {
    /* Find each alive unit's "Head" node index, mutate
     * cob.pieces[node].rot[1] (Y-axis = horizontal turn). */
    int rotated = 0;
    for (int i = 0; i < g_unit_count; i++) {
        Unit *u = &g_units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || !u->cob || !u->cob->pieces) continue;
        const UnitDef *def = Units_GetDef(u->def_idx);
        if (!def) continue;
        const UnitMesh *m = def->mesh_per_color[u->team_color_idx];
        if (!m) continue;
        for (int n = 0; n < m->node_count && n < u->cob->piece_count; n++) {
            if (stricmp_bounded(m->nodes[n].name, "Head") == 0) {
                u->cob->pieces[n].rot[1] += delta_units;
                u->cob->pieces[n].rot_target[1] = u->cob->pieces[n].rot[1];
                rotated++;
                break;
            }
        }
    }
    fprintf(stderr, "Units_DebugRotateHead: rotated %d head pieces by %d (= %.2f rad)\n",
            rotated, delta_units, (float)delta_units * 6.2831853f / 65536.0f);
}

/* ── Host callbacks for the COB VM ────────────────────────────────
 *
 * GET-UNIT-VALUE param IDs follow TA/TAK convention. Common values
 * the engine queries:
 *   1: unit world X (pixel position)
 *   2: unit world Y
 *   3: unit world Z (= world_y for our 2D map)
 *   4: heading in COB angular units (65536 = 2π)
 *   5: velocity (signed, units/sec along facing)
 *   6: max speed (placeholder — taken from FBI later)
 *   7: ground height at unit pos
 *   8: build percent (0..100; 100 = done)
 *   9: current health (placeholder)
 *   10: max health (placeholder)
 * Unknown params return 0 (logged once for diagnostic). */

static int g_unit_value_warned[256];
static void warn_unknown_param(int param) {
    if (param < 0 || param >= 256) return;
    if (g_unit_value_warned[param]) return;
    g_unit_value_warned[param] = 1;
    fprintf(stderr, "Cob host: GET-UNIT-VALUE param %d unhandled (returning 0)\n",
            param);
}

static int32_t cob_host_get_unit_value(void *user, int param) {
    const Unit *u = (const Unit *)user;
    if (!u) return 0;
    const UnitDef *def = Units_GetDef(u->def_idx);
    switch (param) {
        case 1:  return u->world_x;
        case 2:  return u->world_y;
        case 3:  return u->world_y;            /* world_z = map-Y in our 2D world */
        case 4:  return (int32_t)(u->heading * 65536.0f / 6.2831853f);
        case 5:  return u->velocity;
        case 6: {
            float max_pps = def ? def->max_velocity * 30.0f : 0.0f;
            return (int32_t)(max_pps + 0.5f);
        }
        case 7: {
            GameWorld *w = World_Get();
            return w ? Terrain_SampleHeight(w, u->world_x, u->world_y) : 0;
        }
        case 8: {
            int max_hp = u->max_health > 0 ? u->max_health : 1;
            int pct = (int)((int64_t)u->health * 100 / max_hp);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            return pct;
        }
        case 9:  return u->health;
        case 10: return u->max_health > 0 ? u->max_health : 1;
        default: warn_unknown_param(param); return 0;
    }
}

/* SET-VALUE host (opcode 0x10082000). Scripts write yard/activation
 * state; GET ports 1/5/18 read it back. */
static int g_set_port_warned[64];
static void cob_host_set_unit_value(void *user, int port, int32_t value) {
    Unit *u = (Unit *)user;
    if (!u) return;
    switch (port) {
        case 1:  u->cob_activation   = (value != 0); return;  /* ACTIVATION */
        case 5:  u->cob_build_stance = (value != 0); return;  /* INBUILDSTANCE */
        case 18: /* YARD_OPEN, refusable and re-imprints (legacy:223375-223378) */
            Units_TrySetYardOpen((int)(u - g_units), value);
            return;
        case 19: /* BUGGER_OFF, script-visible flag (legacy:223379-223381) */
            u->cob_bugger_off = (value != 0);
            return;
        default: break;
    }
    if (port >= 0 && port < 64 && !g_set_port_warned[port]) {
        g_set_port_warned[port] = 1;
        fprintf(stderr, "Cob host: SET-VALUE port %d unhandled (value %d)\n",
                port, value);
    }
}

/* PLAY-SOUND host (opcode 0x10072000). Legacy CobHost_PlaySound
 * (legacy:223761-223781): category = arg & 7, doubling as
 * priority. Categories 0-6 are positional and LOS-gated; chatty
 * categories (<2) only play when the unit is selected. Category 7 is
 * global/UI (2D, bit 5 = loop — looping not yet supported). */
static int32_t cob_host_play_sound(void *user, const char *sound_name,
                                   int32_t arg) {
    const Unit *u = (const Unit *)user;
    if (!u || u->alive != 1 || !sound_name || !sound_name[0]) return 0;
    const GameWorld *world = World_Get();
    int category = arg & 7;
    if (category <= 6) {
        if (!unit_visible_to_local_player(world, u)) return 0;
        if (category < 2) {
            int selected = 0;
            for (int s = 0; s < g_selection_count; s++) {
                if (g_selection[s] >= 0 && &g_units[g_selection[s]] == u) {
                    selected = 1;
                    break;
                }
            }
            if (!selected) return 0;
        }
    }
    if (category == 7) {
        GameSound_PlayUI(sound_name);
        return 1;
    }
    GameSound_PlayWorldWav(sound_name, 0x7f, u->world_x, u->world_y,
                           world ? world->cam_x : 0,
                           world ? world->cam_y : 0,
                           world ? world->viewport_w : 0,
                           world ? world->viewport_h : 0);
    return 1;
}

static int unit_health_percent(const Unit *u) {
    if (!u) return 0;
    int max_hp = u->max_health > 0 ? u->max_health : 1;
    int pct = (int)((int64_t)u->health * 100 / max_hp);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

static int unit_damage_percent(const Unit *u) {
    return 100 - unit_health_percent(u);
}

/* GET port dispatcher (opcodes 0x10042000/0x10043000 pop the port and
 * route through host[0x54]). Port numbers follow the TA/TA:K table
 * (kbot expression.go:303-340, consistent with observed script use):
 *   1 ACTIVATION, 2 STANDINGMOVEORDERS, 3 STANDINGFIREORDERS,
 *   4 HEALTH(%), 5 INBUILDSTANCE, 6 BUSY, 16 GROUND_HEIGHT,
 *   17 BUILD_PERCENT_LEFT, 18 YARD_OPEN, 22 WEAPON_READY,
 *   26 FINISHED_DYING, 27 ORIENTATION, 29 CURRENT_SPEED,
 *   32 VETERAN_LEVEL, 34 ON_ROAD.
 * IDs 0/30/33/46 are TA:K-specific ports kept from empirical corpus
 * behavior until the host[0x54] table is lifted from the legacy reference
 * (Stage 2 of the COB parity audit). */
static int g_call_fn_warned[256];
static int32_t cob_host_call_function(void *user, int fn_id,
                                       int n_args, const int32_t *args)
{
    const Unit *u = (const Unit *)user;
    (void)n_args;
    (void)args;
    if (!u) return 0;
    const UnitDef *def = Units_GetDef(u->def_idx);
    int hp_pct = unit_health_percent(u);
    int damage_pct = 100 - hp_pct;

    switch (fn_id) {
        case 0:
            /* Miscellaneous script/effect callback. HitByWeapon,
             * FireWeapon, AimWeapon and a few factory scripts call it
             * for side effects. The port handles the actual damage,
             * projectile, and build effects elsewhere, so acknowledge
             * success without forcing state changes. */
            return 1;
        case 1:  /* ACTIVATION — unit+0x114 bit 0 (223207) */
            return u->cob_activation;
        case 2:  /* STANDINGMOVEORDERS — 2 bits (223209) */
            return 1;
        case 3:  /* STANDINGFIREORDERS — 2 bits (223211) */
            return u->aggro_mode;
        case 4:  /* HEALTH percent — hp*100/maxdamage (223213) */
            return hp_pct;
        case 5:  /* INBUILDSTANCE — unit+0x12f bit 0 (223215) */
            return u->cob_build_stance;
        case 6:  /* BUSY — unit+0x12f bit 1 (223217) */
            return 0;
        case 7: {
            /* PIECE_XZ — packed (x<<16)|z of a piece's world position
             * (223219). Piece offsets are small; approximate with the
             * unit's own position until piece world transforms are
             * exposed to the host. */
            return (int32_t)(((uint32_t)u->world_x << 16) |
                             ((uint32_t)u->world_y & 0xffff));
        }
        case 8: /* PIECE_Y (223221) — terrain height at the unit */
            return cob_host_get_unit_value((void *)u, 7) << 16;
        case 9: {
            /* UNIT_XZ of the unit handle in args[1] — packed
             * (x_int<<16)|z_int (223224-223233). */
            if (n_args > 1 && args) {
                int h = args[1] & 0xffff;
                int count = 0;
                const Unit *units = Units_GetActive(&count);
                if (h >= 0 && h < count && units[h].alive == UNIT_ALIVE_ACTIVE) {
                    return (int32_t)(((uint32_t)units[h].world_x << 16) |
                                     ((uint32_t)units[h].world_y & 0xffff));
                }
            }
            return 0;
        }
        case 10: {
            /* UNIT_Y of the unit handle in args[1] (223234-223243). */
            if (n_args > 1 && args) {
                int h = args[1] & 0xffff;
                int count = 0;
                const Unit *units = Units_GetActive(&count);
                if (h >= 0 && h < count && units[h].alive == UNIT_ALIVE_ACTIVE) {
                    GameWorld *w = World_Get();
                    if (w) return Terrain_SampleHeight(w, units[h].world_x,
                                                       units[h].world_y) << 16;
                }
            }
            return 0;
        }
        case 12: {
            /* XZ_ATAN — angle to packed (dx<<16)|dz, relative to unit
             * heading (223254-223260). TA angle units. */
            if (n_args > 1 && args) {
                int16_t dx = (int16_t)((uint32_t)args[1] >> 16);
                int16_t dz = (int16_t)((uint32_t)args[1] & 0xffff);
                int32_t ang = (int32_t)(atan2f((float)dx, (float)dz)
                                        * 65536.0f / 6.2831853f);
                int32_t hdg = (int32_t)(u->heading * 65536.0f / 6.2831853f);
                return (hdg - ang) & 0xffff;   /* CCW under LH sense */
            }
            return 0;
        }
        case 13: {
            /* XZ_HYPOT of packed (dx<<16)|dz (223261). */
            if (n_args > 1 && args) {
                int16_t dx = (int16_t)((uint32_t)args[1] >> 16);
                int16_t dz = (int16_t)((uint32_t)args[1] & 0xffff);
                return (int32_t)sqrtf((float)dx * dx + (float)dz * dz) << 16;
            }
            return 0;
        }
        case 14: {
            /* ATAN(args[1], args[2]) in TA angle units (223263-223265). */
            if (n_args > 2 && args) {
                return (int32_t)(atan2f((float)args[1], (float)args[2])
                                 * 65536.0f / 6.2831853f) & 0xffff;
            }
            return 0;
        }
        case 15: {
            /* HYPOT(args[1], args[2]) (223266-223269). */
            if (n_args > 2 && args) {
                float a = (float)args[1], b = (float)args[2];
                return (int32_t)sqrtf(a * a + b * b);
            }
            return 0;
        }
        case 16: /* GROUND_HEIGHT at packed coords — height<<16 (223270) */
            return cob_host_get_unit_value((void *)u, 7) << 16;
        case 17: /* BUILD_PERCENT_LEFT: 100→0 while being built (223278) */
            return u->under_construction ? (100 - hp_pct) : 0;
        case 18: /* YARD_OPEN — unit+0x12f bit 2 (223284) */
            return u->cob_yard_open;
        case 19: /* BUGGER_OFF — unit+0x12f bit 3 (223286) */
            return u->cob_bugger_off;
        case 20: /* ARMORED — unit+0x114 bit 1 (223288) */
        case 21: /* WEAPON_AIM_ABORTED */
            return 0;
        case 24: /* wind direction relative to heading (223290) */
        case 25: /* wind speed (223292) */
            return 0;
        case 22: /* WEAPON_READY */
            if (def && def->num_weapons > 0) {
                int slot = u->weapon_slot;
                if (slot < 0 || slot >= def->num_weapons) slot = 0;
                return u->weapon_state[slot].cooldown_ticks == 0 ? 1 : 0;
            }
            return 0;
        case 23: /* WEAPON_LAUNCH_NOW */
            return 0;
        case 26: /* FINISHED_DYING */
            return (u->anim_state == UNIT_ANIM_DEAD) ? 1 : 0;
        case 27: /* ORIENTATION — heading in TA angle units */
            return (int32_t)(u->heading * 65536.0f / 6.2831853f);
        case 28: /* IN_WATER */
            return 0;
        case 29: /* CURRENT_SPEED */
            return u->velocity >= 0 ? u->velocity : -u->velocity;
        case 30:
            /* Air/ground clearance query used by flight control. */
            return cob_host_get_unit_value((void *)u, 7);
        case 31: /* MAGIC_DEATH */
            return 0;
        case 32: /* VETERAN_LEVEL — raw rank; scripts gate gold pieces on it. */
            return Units_GetVeteranLevel((int)(u - g_units));
        case 33:
            /* Wheel/track movement scalar (TA:K-specific). */
            if (def && def->max_velocity > 0.0f) {
                int max_pps = (int)(def->max_velocity * 30.0f + 0.5f);
                if (max_pps > 0) return (u->velocity * 40) / max_pps;
            }
            return 0;
        case 34: /* ON_ROAD */
            return 0;
        case 46:
            /* Holster state: non-zero when not actively attacking. */
            return (u->anim_state == UNIT_ANIM_ATTACKING) ? 0 : 1;
        default:
            break;
    }
    (void)damage_pct;
    if (fn_id >= 0 && fn_id < 256 && !g_call_fn_warned[fn_id]) {
        g_call_fn_warned[fn_id] = 1;
        fprintf(stderr, "Cob host: CALL-FUNCTION fn=%d nargs=%d unhandled (returning 0)\n",
                fn_id, n_args);
    }
    return 0;
}

int Units_DebugInvokeScript(const char *script_name) {
    if (!script_name) return 0;
    int started = 0;
    for (int i = 0; i < g_unit_count; i++) {
        Unit *u = &g_units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || !u->cob) continue;
        int slot = Cob_StartThreadByName(u->cob, script_name, NULL, 0);
        if (slot >= 0) started++;
    }
    fprintf(stderr, "Units_DebugInvokeScript: '%s' started on %d units\n",
            script_name, started);
    return started;
}

void Units_DebugBumpVelocity(int32_t delta) {
    int n = 0;
    for (int i = 0; i < g_unit_count; i++) {
        Unit *u = &g_units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE) continue;
        u->velocity += delta;
        n++;
    }
    fprintf(stderr, "Units_DebugBumpVelocity: bumped %d units by %d (now first=%d)\n",
            n, delta, (n > 0 ? g_units[0].velocity : 0));
}

int Units_DebugSpawnEnemy(int32_t world_x, int32_t world_y) {
    /* Always spawn within 100 pixels of the first alive P1 unit so
     * auto-targeting always fires regardless of camera position. The
     * caller's (world_x, world_y) is now ignored — kept for API
     * compatibility. */
    (void)world_x; (void)world_y;
    static const char *prefixes[] = { "ARA", "TAR", "VER", "ZON" };
    static const int   colors[]   = { 1, 2, 3, 0 };
    int chosen = 0;
    int32_t origin_x = 0, origin_y = 0;
    int found_p1 = 0;
    for (int i = 0; i < g_unit_count; i++) {
        if (g_units[i].alive == 1 && g_units[i].player_id == 1) {
            const UnitDef *d = Units_GetDef(g_units[i].def_idx);
            if (d) {
                for (int p = 0; p < 4; p++) {
                    if (stricmp_bounded(d->side, prefixes[p]) == 0) {
                        chosen = (p + 1) % 4;
                        break;
                    }
                }
            }
            origin_x = g_units[i].world_x;
            origin_y = g_units[i].world_y;
            found_p1 = 1;
            break;
        }
    }
    if (!found_p1) {
        fprintf(stderr, "Units_DebugSpawnEnemy: no P1 unit found\n");
        return -1;
    }
    int def = Units_FindMonarchDef(prefixes[chosen]);
    if (def < 0) {
        fprintf(stderr, "Units_DebugSpawnEnemy: no monarch def for %s\n",
                prefixes[chosen]);
        return -1;
    }
    /* Place enemy 100 pixels east of P1 — comfortably inside ARAKING's
     * 232-px sight radius and within Lightning's 250-px range. */
    int32_t spawn_x = origin_x + 100;
    int32_t spawn_y = origin_y;
    int handle = Units_Spawn(def, /*player_id=*/2, colors[chosen],
                              spawn_x, spawn_y);
    fprintf(stderr, "Units_DebugSpawnEnemy: %s (P2, tc=%d) at (%d, %d) -> %d\n",
            prefixes[chosen], colors[chosen], spawn_x, spawn_y, handle);
    /* Diagnostic: dump combat-relevant FBI stats so we can verify
     * the parser actually populated them. */
    const UnitDef *d = Units_GetDef(def);
    if (d) {
        fprintf(stderr, "  hp=%d sight=%d radar=%d maxvel=%.2f weapons=%d\n",
                d->max_health, d->sight_distance, d->radar_distance,
                d->max_velocity, d->num_weapons);
        for (int wi = 0; wi < d->num_weapons; wi++) {
            fprintf(stderr, "  weapon[%d] '%s' range=%d reload=%d dmg=%d\n",
                    wi, d->weapons[wi].name, d->weapons[wi].range,
                    d->weapons[wi].reload_ticks, d->weapons[wi].damage);
        }
    }
    return handle;
}

int Units_DebugKillFirst(void) {
    for (int i = 0; i < g_unit_count; i++) {
        Unit *u = &g_units[i];
        if (u->alive != 1 || !u->cob) continue;
        /* Mark the unit as dying. Its threads will be cleared and a
         * fresh "Killed" thread started; despawn happens in
         * Units_TickEngines once that thread (and any it spawns)
         * finishes. */
        Cob_KillAllThreads(u->cob);
        int slot = Cob_StartThreadByName(u->cob, "Killed", NULL, 0);
        u->alive = UNIT_ALIVE_DYING;
        fprintf(stderr, "Units_DebugKillFirst: unit %d killed (Killed thread slot=%d)\n",
                i, slot);
        return i;
    }
    fprintf(stderr, "Units_DebugKillFirst: no alive units\n");
    return -1;
}

static void compose_node_xforms(const UnitMesh *m,
                                const CobPiece *pieces,
                                NodeXform *out);

/* Whether the renderer would skip the named piece on this unit. -1
 * when the unit or the piece is not there. */
int Units_DebugPieceHidden(int handle, const char *piece_name) {
    if (handle < 0 || handle >= g_unit_count || !piece_name) return -1;
    const Unit *u = &g_units[handle];
    if (!u->cob) return -1;
    const UnitDef *def = Units_GetDef(u->def_idx);
    if (!def) return -1;
    const UnitMesh *m = def->mesh_per_color[u->team_color_idx];
    if (!m || m->node_count <= 0) return -1;
    NodeXform *xf = (NodeXform *)tak_malloc(sizeof(NodeXform) *
                                            (size_t)m->node_count);
    if (!xf) return -1;
    compose_node_xforms(m, u->cob->pieces, xf);
    int hidden = -1;
    for (int i = 0; i < m->node_count; i++) {
        if (tak_stricmp(m->nodes[i].name, piece_name) == 0) {
            hidden = xf[i].hidden ? 1 : 0;
            break;
        }
    }
    tak_free(xf);
    return hidden;
}

int Units_DebugScriptEventCount(int handle, UnitScriptEvent ev) {
    if (handle < 0 || handle >= g_unit_count) return -1;
    if ((int)ev < 0 || (int)ev >= UNIT_SCRIPT_EV_COUNT) return -1;
    return (int)g_units[handle].script_ev[ev];
}

/* Test hook: force a unit's posture and drop its current target
 * (Units_CommandSetAggroSelected is player-1 only). */
void Units_DebugSetAggro(int handle, int aggro_mode) {
    if (handle < 0 || handle >= g_unit_count) return;
    Unit *u = &g_units[handle];
    if (u->alive != 1) return;
    u->aggro_mode = (uint8_t)aggro_mode;
    if (aggro_mode == UNIT_AGGRO_PASSIVE) {
        u->target = -1;
        if (u->cmd_kind == UNIT_CMD_ATTACK) u->cmd_kind = UNIT_CMD_NONE;
    }
}

int Units_DebugKillHandle(int handle) {
    if (handle < 0 || handle >= g_unit_count) return -1;
    Unit *u = &g_units[handle];
    if (u->alive != UNIT_ALIVE_ACTIVE || !u->cob) return -1;
    occ_lift(handle);
    Cob_KillAllThreads(u->cob);
    int slot = Cob_StartThreadByName(u->cob, "Killed", NULL, 0);
    u->health = 0;
    u->alive = UNIT_ALIVE_DYING;
    u->cmd_kind = UNIT_CMD_NONE;
    u->target = -1;
    unit_clear_path(u);
    fprintf(stderr, "Units_DebugKillHandle: unit %d killed (Killed thread slot=%d)\n",
            handle, slot);
    return handle;
}

/* ── Combat + animation tick ──────────────────────────────────────
 *
 * Per recon (the legacy reference):
 *   - Mission_CallScript(str_Move_Ground_Formation) is fired when a
 *     move begins (no velocity polling); we emulate by starting the
 *     walk script on MOVING entry.
 *   - Engine fires AimWeapon / FireWeapon per weapon slot when in
 *     range; we mirror that per weapon[] in UnitDef.
 *   - Killed script runs on death; engine despawns once threads done.
 *
 * Animation state machine reconciles desired state (from cmd_kind,
 * target, health) with active scripts each tick. Entry points legacy
 * invokes once per edge are invoked once per edge here. See
 * docs/notes/2026-09-09-cob-entry-points.md for the full table. Only
 * walk is kept alive across a state (its pose cycle returns and we
 * kick a fresh one while still moving), because our host does not yet
 * serve every value the scripts' own watcher threads poll. */

/* Look up a script index by name; returns -1 if not found. Used to
 * cache script entry-points without hitting Cob_FindScript every
 * tick. */
static int find_script_idx(const Unit *u, const char *name) {
    if (!u->cob || !u->cob->script) return -1;
    return Cob_FindScript(u->cob->script, name);
}

/* Entry points legacy invokes once per state edge, mapped to the
 * counters the guard tests read. Anything else counts as untracked. */
static int script_event_for(const char *name) {
    if (tak_stricmp(name, "StartBuilding") == 0)
        return UNIT_SCRIPT_EV_START_BUILDING;
    if (tak_stricmp(name, "StopBuilding") == 0)
        return UNIT_SCRIPT_EV_STOP_BUILDING;
    if (tak_stricmp(name, "StartMoving") == 0)
        return UNIT_SCRIPT_EV_START_MOVING;
    if (tak_stricmp(name, "StopMoving") == 0)
        return UNIT_SCRIPT_EV_STOP_MOVING;
    if (tak_stricmp(name, "Activate") == 0)
        return UNIT_SCRIPT_EV_ACTIVATE;
    if (tak_stricmp(name, "BeginFlight") == 0)
        return UNIT_SCRIPT_EV_BEGIN_FLIGHT;
    if (tak_stricmp(name, "BeginLanding") == 0)
        return UNIT_SCRIPT_EV_BEGIN_LANDING;
    return -1;
}

/* Every engine-driven script start goes through here, so a test can
 * count what the engine asked for. Counted even when the script does
 * not define the entry point, which is the common case for the
 * movement pair. The contract is about the engine, not the data. */
static int unit_start_script(Unit *u, const char *name,
                             const int32_t *args, int n_args) {
    int ev = script_event_for(name);
    if (ev >= 0 && u->script_ev[ev] < 0xffff) u->script_ev[ev]++;
    if (!u->cob) return -1;
    return Cob_StartThreadByName(u->cob, name, args, n_args);
}

/* Ensure a thread is running for the given script; if the previous
 * slot has died, start a fresh thread and update the slot pointer.
 * Only for scripts the engine is entitled to keep alive. See the
 * entry-point table before adding a caller. */
static int ensure_thread(Unit *u, const char *script_name,
                          int8_t *slot_ref,
                          const int32_t *args, int n_args)
{
    if (!u->cob) return -1;
    if (*slot_ref >= 0 && Cob_IsThreadAlive(u->cob, *slot_ref)) {
        return *slot_ref;
    }
    int slot = unit_start_script(u, script_name, args, n_args);
    *slot_ref = (int8_t)slot;
    return slot;
}

/* Killed's severity argument: overkill as a percentage of maximum
 * hitpoints, halved and clamped to 1..100 (legacy:227142). */
static int32_t unit_death_severity(const Unit *u) {
    int hp_max = u->max_health > 0 ? u->max_health : 1;
    int over = u->health < 0 ? -u->health : 0;
    int sev = (over * 100 / hp_max) / 2;
    if (sev < 1) sev = 1;
    if (sev > 100) sev = 100;
    return (int32_t)sev;
}

/* Start a script once and forget it. Legacy queues a thread and moves
 * on for every entry point except the query family, so a script that
 * returns after posing a piece keeps that pose. */
static void invoke_script_once(Unit *u, const char *name,
                                const int32_t *args, int n_args) {
    (void)unit_start_script(u, name, args, n_args);
}

/* Heading and pitch toward the work, the two arguments legacy hands
 * StartBuilding (legacy:9435). No shipped script reads them, but a
 * mobile builder's lean-in pose is what they are for. Structures
 * producing in their own yard get zeros, because the degenerate site
 * heading would swing the whole building. */
static void build_stance_args(const Unit *u, const UnitDef *def,
                              int32_t out[2]) {
    out[0] = 0;
    out[1] = 0;
    if (!def || def->max_velocity <= 0.0f) return;
    int32_t bdx = u->cmd_x - u->world_x;
    int32_t bdy = u->cmd_y - u->world_y;
    if (bdx == 0 && bdy == 0) return;
    int32_t site_ang = (int32_t)(atan2f((float)bdx, -(float)bdy)
                                 * 65536.0f / 6.2831853f);
    int32_t hdg_ang = (int32_t)(u->heading * 65536.0f / 6.2831853f);
    out[0] = (site_ang - hdg_ang) & 0xffff;
}

static void enter_state(Unit *u, UnitAnimState new_state) {
    if (u->anim_state == (uint8_t)new_state) return;
    /* Exit-old: kill scripts that don't belong to the new state. */
    if (u->anim_state == UNIT_ANIM_MOVING && new_state != UNIT_ANIM_MOVING) {
        /* Kingdoms has no StopMoving. MoveRate(0) is the stop signal
         * and update_move_rate emits it. Kept for scripts that do
         * define it. Nothing in the shipped data does. */
        invoke_script_once(u, "StopMoving", NULL, 0);
        /* We started this walk thread, so we end it. Legacy never has
         * to: its walk loops end themselves on the signal MoveRate(0)
         * raises (legacy:184448). Left running it walks in place. */
        if (u->cob && u->walk_thread_slot >= 0)
            Cob_StopThread(u->cob, u->walk_thread_slot);
        u->walk_thread_slot = -1;
    }
    if (u->anim_state == UNIT_ANIM_ATTACKING && new_state != UNIT_ANIM_ATTACKING) {
        for (int wi = 0; wi < 3; wi++) {
            u->weapon_state[wi].aim_thread_slot = -1;
            u->weapon_state[wi].aim_target = -1;
            u->weapon_state[wi].burst_ticks = 0;
            u->weapon_state[wi].burst_remaining = 0;
            u->weapon_state[wi].burst_target = -1;
        }
    }
    if (u->anim_state == UNIT_ANIM_BUILDING && new_state != UNIT_ANIM_BUILDING) {
        /* Falling edge of legacy's per-unit building state bit: one
         * StopBuilding, whether the order completed, was cancelled or
         * was interrupted (legacy:236408, legacy:9290). */
        invoke_script_once(u, "StopBuilding", NULL, 0);
        u->build_thread_slot = -1;
    }
    u->anim_state = (uint8_t)new_state;
    /* Enter-new: spawn the entry-point script if any. */
    switch (new_state) {
        case UNIT_ANIM_MOVING:
            /* The order may already have kicked the walk; ensure_thread
             * keeps a live slot rather than starting a second one. */
            invoke_script_once(u, "StartMoving", NULL, 0);
            ensure_thread(u, "walk", &u->walk_thread_slot, NULL, 0);
            break;
        case UNIT_ANIM_BUILDING: {
            /* Rising edge of the building state bit: exactly one
             * StartBuilding for the whole construction (legacy:9435).
             * Never re-invoked while building. A script that raises a
             * tool and returns must keep the pose, and one that loops
             * its own animation is already looping. */
            const UnitDef *bdef = Units_GetDef(u->def_idx);
            int32_t build_args[2];
            build_stance_args(u, bdef, build_args);
            u->build_thread_slot = (int8_t)unit_start_script(
                u, "StartBuilding", build_args, 2);
            break;
        }
        case UNIT_ANIM_DYING: {
            /* Killed(severity, corpse type, flags). Legacy derives
             * severity from overkill against maximum hitpoints and
             * clamps it to 1..100 (legacy:227142, legacy:227241).
             * Death scripts branch on it. */
            int32_t args[3] = { unit_death_severity(u), 0, 0 };
            u->killed_thread_slot = (int8_t)unit_start_script(
                u, "Killed", args, 3);
            break;
        }
        default: break;
    }
}

/* Legacy caches the movement rate tier and invokes MoveRate only when
 * it changes (legacy:184448). Kingdoms has no StartMoving/StopMoving:
 * scripts branch on rate > 0 to swap between the moving and idle pose,
 * so tier 0 is the stop signal. */
static void update_move_rate(Unit *u, const UnitDef *def) {
    int tier = 0;
    if (u->anim_state == UNIT_ANIM_MOVING && u->cur_speed_ppt > 0.0f) {
        /* FBI maxvelocity is px per 30Hz frame, so halve for 60Hz. */
        float top = (def && def->max_velocity > 0.0f)
                  ? def->max_velocity * 30.0f / 60.0f : 0.0f;
        tier = (top > 0.0f && u->cur_speed_ppt >= top * 0.5f) ? 2 : 1;
    }
    if (tier == u->move_rate_tier) return;
    u->move_rate_tier = (int8_t)tier;
    int32_t args[1] = { tier };
    invoke_script_once(u, "MoveRate", args, 1);
}

/* TurnDirection(sign) fires only when the sign of the turn changes
 * (legacy:183171). Scripts stash the sign and lean the walk cycle. */
static void update_turn_direction(Unit *u, float prev_heading) {
    float d = u->heading - prev_heading;
    while (d >  3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    int sign = (d > 0.0005f) ? 1 : (d < -0.0005f) ? -1 : 0;
    if (sign == u->turn_dir_sign) return;
    u->turn_dir_sign = (int8_t)sign;
    int32_t args[1] = { sign };
    invoke_script_once(u, "TurnDirection", args, 1);
}

static void apply_killed(Unit *t, int t_idx) {
    if (t->alive != 1) return;
    /* Stop blocking the moment it dies; the corpse feature takes over
     * through the terrain feature path (legacy:218300-218326). */
    occ_lift(t_idx);
    /* Undo any economic contribution this unit was providing (cap +
     * regen) before flipping it dead. We only credited the pool if
     * the unit was past construction — in-progress buildings never
     * contributed, so skip them. Mirrors how legacy retracts the
     * lodestone's mogriumincome when it's destroyed. */
    if (!t->under_construction) {
        const UnitDef *td = Units_GetDef(t->def_idx);
        if (td) {
            /* A caster's maxmana is its own reserve, not pool storage. */
            int32_t cap_delta = td->mogrium_storage;
            float   regen_delta = td->mogrium_income_per_sec
                                  * sacred_income_mult(td, t->world_x,
                                                       t->world_y);
            if (cap_delta != 0 || regen_delta != 0.0f) {
                GameWorld *wgw = World_Get();
                if (wgw) {
                    Economy_AdjustCaps(&wgw->economy, t->player_id,
                                       -cap_delta, -regen_delta);
                }
            }
        }
    }
    if (t->cob) {
        Cob_KillAllThreads(t->cob);
        t->killed_thread_slot = -1;
        t->alive = 2;
        enter_state(t, UNIT_ANIM_DYING);
    } else {
        t->alive = 0;
    }
    fprintf(stderr, "Units: unit %d killed (HP=%d)\n", t_idx, t->health);
}

static int unit_path_goal_changed(const Unit *u, int32_t gx, int32_t gy) {
    if (!u) return 1;
    if (u->path_len == 0 && !u->path_failed)
        return u->path_goal_x != gx || u->path_goal_y != gy;
    /* Chase tolerance: a pursued target moves every tick — replanning
     * on every wiggle ran full A* per chaser per tick (87% of the
     * browser CPU trace). Old waypoints still lead toward a goal that
     * drifted <48px; the exhausted-path replan re-aims periodically. */
    int32_t dx = u->path_goal_x - gx;
    int32_t dy = u->path_goal_y - gy;
    return (int64_t)dx * dx + (int64_t)dy * dy > 48 * 48;
}

static const MoveClassDef *unit_move_class(const GameWorld *w,
                                           const UnitDef *def) {
    if (!w || !def || !def->movement_class[0]) return NULL;
    return TAK_MoveInfo_Find(&w->moveinfo, def->movement_class);
}

static int unit_effective_max_slope(const UnitDef *def,
                                    const MoveClassDef *move_class) {
    if (move_class && move_class->max_slope > 0) return move_class->max_slope;
    return def ? def->max_slope : 0;
}

/* Water-depth window (legacy:219155-219157): the cell's depth must sit
 * inside the move class's [minwaterdepth, maxwaterdepth]. Land units
 * cannot wade past the maximum, boats need at least the minimum. This
 * is a hard rule with no escape hatch: it is the only thing keeping a
 * boat off dry land. Flyers are exempt through canfly. */
static int unit_water_depth_ok(const GameWorld *w, const UnitDef *def,
                         int32_t x, int32_t y) {
    if (def && def->can_fly) return 1;
    if (!w || w->water_height <= 0) return 1;
    int min_wd = 0, max_wd = 0;
    unit_water_depth_window(w, def, &min_wd, &max_wd);
    /* waterheight (sidedata) and Terrain_SampleHeight share raw
     * heightmap units, so the difference is the depth directly. */
    int depth = w->water_height - Terrain_SampleHeight(w, x, y);
    if (depth < 0) depth = 0;
    if (depth > max_wd) return 0;
    if (min_wd > 0 && depth < min_wd) return 0;
    return 1;
}

static int unit_terrain_walkable(const GameWorld *w,
                                 const UnitDef *def,
                                 int32_t x,
                                 int32_t y) {
    /* Flyers ignore terrain entirely (legacy canfly). */
    if (def && def->can_fly) return 1;
    const MoveClassDef *mc = unit_move_class(w, def);
    if (!Terrain_IsWalkable(w, x, y, unit_effective_max_slope(def, mc)))
        return 0;
    return unit_water_depth_ok(w, def, x, y);
}

/* Dynamic occupancy under the mover: the legacy move step walks the
 * destination footprint and refuses any cell another live unit holds
 * (legacy:219329-219340). The unit's own cells never block, and an own
 * closed gate stays walkable because the cost classifier reclassifies
 * it as passable for its owner (legacy:21986-22032). */
static int occ_step_blocked(const GameWorld *w, const Unit *u, int handle,
                            int32_t x, int32_t y) {
    if (!w || !w->occ) return 0;
    const UnitDef *d = Units_GetDef(u->def_idx);
    if (d && d->can_fly) return 0;
    int fx, fz;
    unit_occ_fp(u, &fx, &fz);
    int tx0 = Occ_TileOf(x - fx * 8);
    int ty0 = Occ_TileOf(y - fz * 8);
    for (int row = 0; row < fz; row++) {
        for (int col = 0; col < fx; col++) {
            if (Occ_QueryTile(w, tx0 + col, ty0 + row,
                              u->player_id, handle + 1) == 1) {
                return 1;
            }
        }
    }
    return 0;
}

extern double g_path_plan_calls;
static int g_path_budget_this_tick = 8;
static void unit_replan_path(Unit *u, const UnitDef *def,
                             const GameWorld *w,
                             int32_t gx, int32_t gy) {
    if (!u || !w || !def) return;
    /* Global per-tick A* budget bounds cost regardless of army size.
     * On denial the request is PENDING: the unit keeps its old path or
     * holds — it must never beeline at the goal, or it walks into the
     * first cliff and grinds there. */
    if (g_path_budget_this_tick <= 0) {
        u->path_pending = 1;
        if (u->path_wait < 255) u->path_wait++;
        return;
    }
    g_path_budget_this_tick--;
    u->path_pending = 0;
    u->path_wait = 0;
    g_path_plan_calls += 1.0;
    TAK_Path path;
    const MoveClassDef *mc = unit_move_class(w, def);
    /* Plan as this unit's owner: own closed gates are routed through
     * and opened on arrival (legacy:21986-22032). */
    int n = TAK_PathPlanForMoveClass(w, u->world_x, u->world_y, gx, gy,
                                     mc, def->max_slope, u->player_id, &path);
    u->path_goal_x = gx;
    u->path_goal_y = gy;
    u->path_len = 0;
    u->path_index = 0;
    u->path_failed = 0;
    if (n <= 0) return;
    if (n > UNIT_PATH_MAX_WAYPOINTS) n = UNIT_PATH_MAX_WAYPOINTS;
    for (int i = 0; i < n; i++) {
        u->path_x[i] = path.x[i];
        u->path_y[i] = path.y[i];
    }
    u->path_len = (uint8_t)n;
    /* Fresh route: clear the stall watchdog, or the first far waypoint
     * of a new long order looks like "no progress" and gets discarded. */
    u->wp_stall = 0;
    u->wp_best_d2 = 0x7fffffff;
}

/* What the follower wants the mover to do this tick. Returned
 * explicitly so walk_tick cannot silently ignore a HOLD (it used to
 * infer intent from path_len and discarded the hold entirely). */
typedef enum {
    NAV_STEER = 0,   /* move toward *out (waypoint or final goal) */
    NAV_HOLD         /* stand still this tick — NOT arrived */
} NavAction;

static NavAction unit_next_path_target(Unit *u, const UnitDef *def,
                                  const GameWorld *w,
                                  int32_t final_x, int32_t final_y,
                                  int32_t *out_x, int32_t *out_y) {
    *out_x = final_x;
    *out_y = final_y;
    if (!u || !def || !w) return NAV_STEER;
    if (def->can_fly) return NAV_STEER;   /* flyers go straight — no A* */
    if (unit_path_goal_changed(u, final_x, final_y)) {
        unit_replan_path(u, def, w, final_x, final_y);
        if (u->path_len == 0 && !u->path_pending) u->path_failed = 1;
    } else if (u->path_failed && u->path_len == 0) {
        /* No route from here — retry periodically. The unit keeps
         * moving meanwhile (below): sliding along the obstruction
         * usually reaches a cell A* can plan from. */
        if (u->path_replan_cd > 0) {
            u->path_replan_cd--;
        } else {
            u->path_replan_cd = (int16_t)(30 + (u->stable_id & 15));
            unit_replan_path(u, def, w, final_x, final_y);
            if (u->path_len == 0 && !u->path_pending) u->path_failed = 1;
        }
    }
    /* Plan queued but not run yet: hold briefly rather than walking
     * blindly at the goal. BOUNDED — a starved budget (many units
     * ordered at once) must never freeze a unit permanently, so after
     * ~0.4s we advance on direct steering while the plan is pending. */
    if (u->path_pending && u->path_len == 0 && u->path_wait <= 24) {
        *out_x = u->world_x;
        *out_y = u->world_y;
        return NAV_HOLD;
    }
    /* A* found nothing: steer straight at the goal and let the local
     * avoidance fan slide along whatever is in the way. Freezing here
     * was why units parked against terrain and never went around. */
    if (u->path_failed && u->path_len == 0) {
        *out_x = final_x;
        *out_y = final_y;
        return NAV_STEER;
    }
    while (u->path_index < u->path_len) {
        int32_t wx = u->path_x[u->path_index];
        int32_t wy = u->path_y[u->path_index];
        int64_t dx = (int64_t)wx - u->world_x;
        int64_t dy = (int64_t)wy - u->world_y;
        int64_t d2 = dx * dx + dy * dy;
        if (d2 > 256) {
            /* Waypoint watchdog: a unit that stops closing on its next
             * waypoint (local pocket, obstacle it keeps sliding around)
             * skips it rather than circling there forever. */
            int32_t d2c = d2 > 0x7fffffff ? 0x7fffffff : (int32_t)d2;
            if (d2c < u->wp_best_d2 - 64) {
                u->wp_best_d2 = d2c;
                u->wp_stall = 0;
            } else if (++u->wp_stall > 90) {
                /* Not closing on this waypoint for 1.5s: the stale route
                 * leads into a pocket. Drop it and replan from where we
                 * actually stand — crawling through the remaining dead
                 * waypoints could take minutes. */
                u->wp_stall = 0;
                u->wp_best_d2 = 0x7fffffff;
                u->path_len = 0;
                u->path_index = 0;
                u->path_failed = 1;
                u->path_replan_cd = 0;
                *out_x = final_x;
                *out_y = final_y;
                return NAV_STEER;
            }
            *out_x = wx;
            *out_y = wy;
            return NAV_STEER;
        }
        u->path_index++;
        u->wp_stall = 0;
        u->wp_best_d2 = 0x7fffffff;
    }
    if (u->path_len > 0) {
        int64_t dx = (int64_t)final_x - u->world_x;
        int64_t dy = (int64_t)final_y - u->world_y;
        if (dx * dx + dy * dy > 64) {
            /* Cooldown + jitter: without it a converged crowd re-runs
             * full A* per unit per 60Hz tick (the 104ms-frame lockup
             * in the browser perf trace). */
            if (u->path_replan_cd > 0) {
                u->path_replan_cd--;
                *out_x = u->world_x;
                *out_y = u->world_y;
                return NAV_STEER;
            }
            u->path_replan_cd = (int16_t)(30 + (u->stable_id & 15));
            unit_replan_path(u, def, w, final_x, final_y);
            if (u->path_len > 0) {
                *out_x = u->path_x[0];
                *out_y = u->path_y[0];
                return NAV_STEER;
            }
        }
    }
    return NAV_STEER;
}

/* How close to the order point a unit must be before it accepts being
 * blocked by another unit as "arrived". Roughly two footprints, so a
 * squad packs around the point instead of orbiting it. */
#define UNIT_CROWD_ARRIVE_PX 96

/* Walk one tick of position integration toward (gx, gy); returns 1
 * if arrived (within 8 px), else 0. Sub-pixel accumulator is on
 * Unit so slow units actually translate. */
static int walk_tick(Unit *u, const UnitDef *def, int32_t gx, int32_t gy) {
    GameWorld *w = World_Get();
    int self_h = (int)(u - g_units);
    /* Already standing on someone's footprint (spawned in a factory
     * yard, or a structure claimed the cell): every candidate step
     * would fail the same test, so ignore occupancy until it is out.
     * Same escape hatch the terrain predicate uses below. */
    int occ_escape = occ_step_blocked(w, u, self_h, u->world_x, u->world_y);
    int32_t step_gx = gx;
    int32_t step_gy = gy;
    NavAction nav = unit_next_path_target(u, def, w, gx, gy,
                                          &step_gx, &step_gy);
    if (nav == NAV_HOLD) {
        /* Plan queued: stand still WITHOUT reporting arrival, else a
         * held MOVE order would complete at the unit's own feet. */
        u->velocity = 0;
        u->cur_speed_ppt = 0.0f;
        return 0;
    }
    gx = step_gx;
    gy = step_gy;
    int64_t dx = (int64_t)(gx - u->world_x);
    int64_t dy = (int64_t)(gy - u->world_y);
    int64_t d2 = dx*dx + dy*dy;
    if (d2 < 64) {
        if (u->path_len > 0) {
            if (u->path_index + 1 < u->path_len) {
                u->path_index++;
                return 0;
            }
            int64_t fdx = (int64_t)u->path_goal_x - u->world_x;
            int64_t fdy = (int64_t)u->path_goal_y - u->world_y;
            if (fdx * fdx + fdy * fdy > 64) {
                unit_clear_path(u);
                return 0;
            }
        }
        return 1;
    }
    /* FBI maxvelocity is 16.16 world-pixels per 30Hz frame (legacy
     * stores it raw at def+0x162, :162833) — px/sec = value * 30.
     * The old *16 ran every unit at ~53% of legacy speed. */
    float speed_pps = def->max_velocity * 30.0f;
    float max_ppt = speed_pps / 60.0f;
    if (max_ppt > 0.0f) {
        float dist = sqrtf((float)dx * dx + (float)dy * dy);
        if (dist > 0.001f) {
            float dir_x = (float)dx / dist;
            float dir_y = (float)dy / dist;
            if (w) {
                /* Obstacle hugging, legacy-style: try straight first,
                 * then widening deviations, and take the FIRST walkable
                 * one — scoring the whole fan made units flip left/right
                 * on alternate ticks and stand still. The chosen side is
                 * remembered so the unit keeps sliding the same way
                 * instead of oscillating in a pocket. */
                float best_x = dir_x, best_y = dir_y;
                int8_t side = u->avoid_side >= 0 ? 1 : -1;
                const float mags[] = { 0.0f, 0.3f, 0.7f, 1.2f, 1.6f, 2.2f };
                float probes[11];
                int nprobe = 0;
                probes[nprobe++] = 0.0f;
                for (int m = 1; m < (int)(sizeof(mags)/sizeof(mags[0])); m++) {
                    probes[nprobe++] = mags[m] * (float)side;
                    probes[nprobe++] = -mags[m] * (float)side;
                }
                int chosen = -1;
                int h0 = Terrain_SampleHeight(w, u->world_x, u->world_y);
                (void)h0;
                for (int pi = 0; pi < nprobe; pi++) {
                    float ca = cosf(probes[pi]), sa = sinf(probes[pi]);
                    float px = dir_x * ca - dir_y * sa;
                    float py = dir_x * sa + dir_y * ca;
                    /* Probe the full step the unit would take plus a
                     * look-ahead, so it commits to a clear lane. */
                    int32_t nx = u->world_x + (int32_t)(px * 24.0f);
                    int32_t ny = u->world_y + (int32_t)(py * 24.0f);
                    if (!unit_terrain_walkable(w, def, nx, ny)) continue;
                    /* The avoidance fan now has a real obstacle signal:
                     * it slides around whoever is standing there. */
                    if (!occ_escape &&
                        occ_step_blocked(w, u, self_h, nx, ny)) continue;
                    best_x = px;
                    best_y = py;
                    chosen = pi;
                    break;
                }
                if (chosen <= 0) {
                    u->avoid_side = 0;          /* straight: forget the side */
                } else if (probes[chosen] != 0.0f) {
                    u->avoid_side = probes[chosen] > 0.0f ? 1 : -1;
                }
                dir_x = best_x;
                dir_y = best_y;
            }

            /* ── Turn-rate clamp ──────────────────────────────────
             * FBI turnrate is TA angle units (65536 = full circle)
             * per 30 Hz frame; halve for our 60 Hz ticks. Zero or
             * missing turnrate turns instantly (buildings, tests). */
            float des_heading = atan2f(dir_x, -dir_y);
            if (def->turn_rate > 0.0f) {
                float turn_ppt = def->turn_rate
                               * (6.2831853f / 65536.0f) * 0.5f;
                float delta = des_heading - u->heading;
                while (delta >  3.14159265f) delta -= 6.2831853f;
                while (delta < -3.14159265f) delta += 6.2831853f;
                if (delta >  turn_ppt) delta =  turn_ppt;
                if (delta < -turn_ppt) delta = -turn_ppt;
                u->heading += delta;
                while (u->heading >  3.14159265f) u->heading -= 6.2831853f;
                while (u->heading < -3.14159265f) u->heading += 6.2831853f;
                /* Travel along the clamped heading, not the desired
                 * one — wide turns arc like the original. */
                dir_x = sinf(u->heading);
                dir_y = -cosf(u->heading);
            } else {
                u->heading = des_heading;
            }

            /* ── Bang-bang speed integrator ───────────────────────
             * FBI acceleration/brakerate are velocity units per 30 Hz
             * frame; the ratio to maxvelocity gives frames-to-max,
             * preserved at 60 Hz by halving the per-tick delta.
             * Brake when the remaining distance to the *command* goal
             * drops inside the kinematic stopping distance v²/(2b).
             * Zero-authored fields keep the old constant-speed law. */
            float accel_ppt = (def->acceleration > 0.0f &&
                               def->max_velocity > 0.0f)
                ? max_ppt * (def->acceleration / def->max_velocity) * 0.5f
                : max_ppt;
            float brake_ppt = (def->brake_rate > 0.0f &&
                               def->max_velocity > 0.0f)
                ? max_ppt * (def->brake_rate / def->max_velocity) * 0.5f
                : max_ppt;
            float goal_dist = dist;
            if (u->path_len > 0) {
                float gdx = (float)(u->path_goal_x - u->world_x);
                float gdy = (float)(u->path_goal_y - u->world_y);
                goal_dist = sqrtf(gdx * gdx + gdy * gdy);
            }
            float v = u->cur_speed_ppt;
            float stop_dist = (brake_ppt > 0.0f)
                            ? (v * v) / (2.0f * brake_ppt) : 0.0f;
            if (goal_dist <= stop_dist) {
                v -= brake_ppt;
            } else {
                v += accel_ppt;
            }
            if (v > max_ppt) v = max_ppt;
            /* Floor keeps units closing the last few pixels instead of
             * stalling under an overshooting brake estimate. */
            float min_ppt = max_ppt * 0.15f;
            if (v < min_ppt) v = min_ppt;
            u->cur_speed_ppt = v;

            float fx = u->subpixel_x + dir_x * v;
            float fy = u->subpixel_y + dir_y * v;
            int32_t mx = (int32_t)floorf(fx);
            int32_t my = (int32_t)floorf(fy);
            int32_t nx = u->world_x + mx;
            int32_t ny = u->world_y + my;
            /* Escape hatch: a unit already standing on illegal ground
             * (mission placement, factory exit, a nudge from the
             * avoidance fan) must be allowed to step OUT — otherwise
             * every candidate fails the same predicate and it is
             * immobilised forever. "Don't enter illegal ground" and
             * "leave illegal ground" cannot share one test. */
            int escaping = (w && !unit_terrain_walkable(w, def,
                                                        u->world_x,
                                                        u->world_y));
            /* The escape hatch relaxes slope and features so a unit can
             * leave illegal ground, but it must never relax the water
             * window: that window is the only thing keeping a boat off
             * dry land (legacy:219155-219157). */
            int terrain_stop = w && (!unit_water_depth_ok(w, def, nx, ny) ||
                                     (!escaping &&
                                      !unit_terrain_walkable(w, def, nx, ny)));
            int occ_stop = (w && !occ_escape &&
                            occ_step_blocked(w, u, self_h, nx, ny));
            if (terrain_stop || occ_stop) {
                /* Blocked step: stop for this tick but KEEP the route.
                 * Wiping the path here meant any unit that brushed
                 * terrain lost its plan, re-planned into the same
                 * obstacle and ground there forever. Only after
                 * repeated blocks do we invalidate so the periodic
                 * replan can find a way around. */
                u->subpixel_x = 0.0f;
                u->subpixel_y = 0.0f;
                u->velocity = 0; u->cur_speed_ppt = 0.0f;
                if (u->blocked_ticks < 255) u->blocked_ticks++;
                if (occ_stop && !terrain_stop) {
                    /* Another unit is in the way. Legacy adds a retry
                     * delay and keeps the route rather than dropping it
                     * (legacy:184687-184694). A squad converging on one
                     * point settles where it stands and packs, instead
                     * of shuffling around the leader forever. */
                    if (goal_dist <= (float)UNIT_CROWD_ARRIVE_PX) return 1;
                    if (u->blocked_ticks > 120) {
                        u->blocked_ticks = 0;
                        u->path_len = 0;
                        u->path_index = 0;
                        u->path_failed = 1;
                        u->path_replan_cd = 0;
                    }
                    return 0;
                }
                if (u->blocked_ticks > 12) {
                    u->blocked_ticks = 0;
                    u->path_len = 0;
                    u->path_index = 0;
                    u->path_failed = 1;
                    u->path_replan_cd = 0;   /* replan next tick */
                }
                return 0;
            }
            /* Only a step that actually moved clears the block count:
             * a sub-pixel step alternating with a blocked one kept the
             * count at zero and the unit pinned for good. */
            if (mx != 0 || my != 0) u->blocked_ticks = 0;
            u->world_x   = nx;
            u->world_y   = ny;
            u->subpixel_x = fx - (float)mx;
            u->subpixel_y = fy - (float)my;
            u->velocity = (int32_t)(v * 60.0f);
            /* Re-stamp immediately so units later in this tick see the
             * cell as taken (legacy re-imprints on the move itself). */
            occ_sync_mobile(self_h);
        }
    }
    return 0;
}

static int weapon_name_has(const UnitWeapon *wp, const char *needle) {
    if (!wp || !needle) return 0;
    char name[32];
    char nd[32];
    size_t i = 0;
    for (; i + 1 < sizeof(name) && wp->name[i]; i++) {
        char c = wp->name[i];
        name[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    name[i] = '\0';
    for (i = 0; i + 1 < sizeof(nd) && needle[i]; i++) {
        char c = needle[i];
        nd[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    nd[i] = '\0';
    return strstr(name, nd) != NULL;
}

static int weapon_is_melee(const UnitWeapon *wp) {
    if (!wp) return 0;
    if (weapon_name_has(wp, "sword") || weapon_name_has(wp, "axe") ||
        weapon_name_has(wp, "club") || weapon_name_has(wp, "fist") ||
        weapon_name_has(wp, "claw") || weapon_name_has(wp, "bite") ||
        weapon_name_has(wp, "melee")) return 1;
    return (wp->velocity_pps == 0 && wp->mana_per_shot == 0 && wp->range > 64);
}

static int weapon_damage_for_category(const UnitWeapon *wp, const char *category) {
    if (!wp) return 0;
    if (!category || !category[0]) return wp->damage;
    for (int i = 0; i < wp->damage_scale_count && i < TAK_DAMAGE_CATEGORY_MAX; i++) {
        if (stricmp_bounded(wp->damage_scales[i].category, category) == 0) {
            float scaled = (float)wp->damage * wp->damage_scales[i].scale;
            if (scaled <= 0.0f) return 0;
            return (int)(scaled + 0.5f);
        }
    }
    return wp->damage;
}

int Units_ComputeWeaponDamageForCategory(const UnitWeapon *wp,
                                         const char *category) {
    return weapon_damage_for_category(wp, category);
}

static uint8_t weapon_visual_kind(const UnitWeapon *wp) {
    if (!wp) return UNIT_PROJECTILE_VIS_GENERIC;
    if (ascii_contains_ci(wp->type, "remote")) return UNIT_PROJECTILE_VIS_REMOTE;
    if (wp->mana_per_shot > 0 ||
        ascii_contains_ci(wp->type, "line of sight") ||
        ascii_contains_ci(wp->damage_type, "paralyzer") ||
        ascii_contains_ci(wp->subtype, "stone") ||
        ascii_contains_ci(wp->subtype, "lightning") ||
        ascii_contains_ci(wp->explosion_class, "shock") ||
        ascii_contains_ci(wp->explosion_class, "lightning") ||
        ascii_contains_ci(wp->weapon_art, "fireball") ||
        ascii_contains_ci(wp->weapon_art, "ice") ||
        ascii_contains_ci(wp->weapon_art, "magic")) {
        return UNIT_PROJECTILE_VIS_MAGIC;
    }
    if (ascii_contains_ci(wp->model, "arrow") ||
        ascii_contains_ci(wp->weapon_art, "arrow") ||
        weapon_name_has(wp, "arrow")) {
        return UNIT_PROJECTILE_VIS_ARROW;
    }
    if (ascii_contains_ci(wp->weapon_art, "cann") ||
        ascii_contains_ci(wp->explosion_class, "explosion") ||
        weapon_name_has(wp, "cannon")) {
        return UNIT_PROJECTILE_VIS_CANNON;
    }
    return UNIT_PROJECTILE_VIS_GENERIC;
}

int Units_GetWeaponVisualKind(int def_idx, int weapon_slot) {
    const UnitDef *def = Units_GetDef(def_idx);
    if (!def || weapon_slot < 0 || weapon_slot >= def->num_weapons) return -1;
    return (int)weapon_visual_kind(&def->weapons[weapon_slot]);
}

int Units_GetWeaponArtKind(int def_idx, int weapon_slot,
                           char *out_name, int out_cap) {
    if (out_name && out_cap > 0) out_name[0] = '\0';
    const UnitDef *def = Units_GetDef(def_idx);
    if (!def || weapon_slot < 0 || weapon_slot >= def->num_weapons) return -1;
    const UnitWeapon *wp = &def->weapons[weapon_slot];
    if (out_name && out_cap > 0) {
        snprintf(out_name, (size_t)out_cap, "%s", wp->art_name);
    }
    return (int)wp->art_kind;
}

static int weapon_effective_range(const UnitWeapon *wp) {
    if (!wp) return 0;
    int range = wp->range;
    if (weapon_is_melee(wp) && range > 40) range = 40;
    return range;
}

static int weapon_min_range(const UnitWeapon *wp) {
    if (!wp || weapon_is_melee(wp)) return 0;
    return wp->min_range > 0 ? wp->min_range : 0;
}

/* Squared distance from a unit to what it must reach on its target.
 * The original measures to a point on a structure's footprint, not
 * its centre (legacy:235291 Unit_GetTargetPosition), which is how a
 * short weapon closes on a 5x14 keep. Mobile targets keep centre
 * distance. */
static int64_t unit_reach_d2(const Unit *u, const Unit *t) {
    const UnitDef *td = Units_GetDef(t->def_idx);
    int64_t dx = (int64_t)t->world_x - u->world_x;
    int64_t dy = (int64_t)t->world_y - u->world_y;
    if (td && td->max_velocity <= 0.0f &&
        td->footprint_x > 0 && td->footprint_z > 0) {
        int64_t hx = (int64_t)td->footprint_x * 8;
        int64_t hz = (int64_t)td->footprint_z * 8;
        int64_t ax = dx < 0 ? -dx : dx;
        int64_t ay = dy < 0 ? -dy : dy;
        ax = ax > hx ? ax - hx : 0;
        ay = ay > hz ? ay - hz : 0;
        return ax * ax + ay * ay;
    }
    return dx * dx + dy * dy;
}

/* Air is a state, not a type. The original tests the target's current
 * movement mode (legacy:249578-249586): noairweapon refuses a unit
 * that is airborne, toairweapon refuses one that is not. A flyer on
 * the ground, or a ghost ship resting on the water, is a ground
 * target for a galley's cannon. */
static int weapon_can_target_unit(const UnitWeapon *wp, const Unit *t) {
    if (!wp || !t) return 1;
    if (t->flying && wp->no_air_weapon) return 0;
    if (!t->flying && wp->to_air_weapon) return 0;
    return 1;
}

static int ai_difficulty_for_player(int player_id) {
    const GameWorld *world = World_Get();
    if (!world || player_id <= 0 || player_id > TAK_MAX_PLAYERS) return 1;
    const PlayerSlot *slot = &world->cfg.players[player_id - 1];
    if (slot->kind != TAK_SLOT_AI) return 1;
    return TAK_AI_ClampDifficulty(slot->ai_difficulty);
}

static int resolve_weapon_script_name(const Unit *u, const char *base,
                                      int slot, char *out, size_t out_sz) {
    if (!u || !u->cob || !base || !out || out_sz == 0) return 0;
    snprintf(out, out_sz, "%s%d", base, slot + 1);
    if (find_script_idx(u, out) >= 0) return 1;
    snprintf(out, out_sz, "%s", base);
    return find_script_idx(u, out) >= 0;
}

/* FireWeapon(weapon index): one argument, one thread per shot
 * (legacy:249415). Scripts read the index to pick a muzzle. */
static void start_fire_script(Unit *u, int slot) {
    if (!u->cob) return;
    char name[32];
    if (!resolve_weapon_script_name(u, "FireWeapon", slot, name, sizeof(name)))
        return;
    int32_t args[1] = { (int32_t)slot };
    Cob_StartThreadByName(u->cob, name, args, 1);
}

/* aim_key names what is being aimed at so a change restarts the aim:
 * a unit handle, or -2 for the ground point of an attack-ground order.
 * The original aims at a ground point exactly like a unit; only the
 * target of the shot differs. */
static int weapon_aim_ready(Unit *u, int slot, int aim_key,
                            int32_t aim_x, int32_t aim_y,
                            UnitWeaponState *ws) {
    if (!u || !ws || !u->cob) return 1;

    char name[32];
    if (!resolve_weapon_script_name(u, "AimWeapon", slot, name, sizeof(name))) {
        return 1;
    }

    /* AimWeapon(heading, pitch, weapon index): angles toward the TARGET
     * relative to the unit's facing, then the slot (legacy:249312 passes
     * three). Scripts write the third straight into their active-weapon
     * value, so a missing one poses the wrong arm. Passing the unit's
     * own absolute heading made aim loops chase a nonsense angle. */
    int32_t rel_heading = 0;
    {
        int32_t adx = aim_x - u->world_x;
        int32_t ady = aim_y - u->world_y;
        if (adx != 0 || ady != 0) {
            int32_t aim_ang = (int32_t)(atan2f((float)adx, -(float)ady)
                                        * 65536.0f / 6.2831853f);
            int32_t hdg_ang = (int32_t)(u->heading
                                        * 65536.0f / 6.2831853f);
            /* CCW delta (hdg - aim): +y turns rotate -z toward +x =
             * unit's LEFT under the corrected LH sense. Half a turn is
             * built into the engine's aim angle: every turret script in
             * the shipped data subtracts 0x8000 before turning its
             * piece, so without it here a tower faces away from what it
             * shoots. Emit signed shortest-way about the corrected
             * centre. Scripts do signed math on the arg. */
            rel_heading = ((hdg_ang - aim_ang) & 0xffff) - 0x8000;
        }
    }
    int32_t args[3] = { rel_heading, 0, (int32_t)slot };

    if (ws->aim_target != aim_key) {
        ws->aim_thread_slot = -1;
        ws->aim_ticks = 0;
        ws->aim_target = (int16_t)aim_key;
    }

    if (ws->aim_thread_slot < 0) {
        int started = Cob_StartThreadByName(u->cob, name, args, 3);
        if (started < 0) {
            ws->aim_thread_slot = -1;
            ws->aim_target = -1;
            ws->aim_ticks = 0;
            return 1;
        }
        ws->aim_thread_slot = (int8_t)started;
        ws->aim_ticks = 0;
        return 0;
    }

    if (Cob_IsThreadAlive(u->cob, ws->aim_thread_slot)) {
        ws->aim_ticks++;
        if (ws->aim_ticks > 120) {
            Cob_StopThread(u->cob, ws->aim_thread_slot);
            ws->aim_thread_slot = -1;
            ws->aim_target = -1;
            ws->aim_ticks = 0;
            return 1;
        }
        return 0;
    }

    int32_t ret = 1;
    int has_ret = Cob_GetThreadReturn(u->cob, ws->aim_thread_slot, &ret);
    ws->aim_thread_slot = -1;
    ws->aim_target = -1;
    ws->aim_ticks = 0;
    return !has_ret || ret != 0;
}

static void fire_weapon_shot(Unit *u, int shooter_idx, int slot,
                             const UnitWeapon *wp, int target_handle,
                             int burst_ordinal, int run_fire_script) {
    if (!u || !wp) return;
    if (target_handle < 0 || target_handle >= g_unit_count) return;
    Unit *t = &g_units[target_handle];
    if (t->alive != UNIT_ALIVE_ACTIVE) return;

    if (run_fire_script) {
        start_fire_script(u, slot);
    }

    if (weapon_is_melee(wp)) {
        const UnitDef *td = Units_GetDef(t->def_idx);
        int damage = weapon_damage_for_category(
            wp, td ? td->damage_category : "");
        t->health -= damage;
        if (t->health <= 0) {
            credit_kill(shooter_idx, t);
            apply_killed(t, target_handle);
            if (u->target == target_handle) {
                u->target = -1;
                /* Don't clear a standing PATROL — only a manual attack. */
                if (u->cmd_kind == UNIT_CMD_ATTACK)
                    u->cmd_kind = UNIT_CMD_NONE;
                unit_clear_path(u);
            }
        } else {
            unit_on_damaged(t, shooter_idx);
        }
        return;
    }

    const GameWorld *sw = World_Get();
    if (wp->start_sound[0]) {
        GameSound_PlayWorldWav(wp->start_sound, 0x7f, u->world_x, u->world_y,
                               sw ? sw->cam_x : 0, sw ? sw->cam_y : 0,
                               sw ? sw->viewport_w : 0,
                               sw ? sw->viewport_h : 0);
    }

    /* Line-of-Sight: instant ray — damage lands now, the pool entry
     * only holds the beam visual for emittime (legacy :249725). */
    if (wp->los_kind == 1 || wp->los_kind == 2) {
        int bslot = spawn_projectile(u->world_x, u->world_y,
                                     t->world_x, t->world_y, 1.0f,
                                     wp->damage, wp->area_of_effect,
                                     wp->edge_effectiveness, wp,
                                     weapon_visual_kind(wp),
                                     target_handle, shooter_idx,
                                     u->player_id, u->team_color_idx);
        /* Damage must land even when the pool is full — a beam that
         * found no free slot still hits, it just draws nothing. */
        if (bslot < 0) {
            const UnitDef *td = Units_GetDef(t->def_idx);
            int dmg = weapon_damage_for_category(
                wp, td ? td->damage_category : "");
            t->health -= dmg;
            if (t->health <= 0) {
                credit_kill(shooter_idx, t);
                apply_killed(t, target_handle);
                if (u->target == target_handle) {
                    u->target = -1;
                    if (u->cmd_kind == UNIT_CMD_ATTACK)
                        u->cmd_kind = UNIT_CMD_NONE;
                    unit_clear_path(u);
                }
            } else {
                unit_on_damaged(t, shooter_idx);
            }
            return;
        }
        Projectile *b = &g_projectiles[bslot];
        b->is_beam   = 1;
        b->src_x     = u->world_x;
        b->src_y     = u->world_y;
        b->world_x   = t->world_x;
        b->world_y   = t->world_y;
        b->dest_x    = t->world_x;
        b->dest_y    = t->world_y;
        b->speed_ppt = 0.0f;
        b->ttl_ticks = (int16_t)wp->emit_ticks;
        if (sw) b->height = (float)Terrain_SampleHeight(sw, b->world_x,
                                                        b->world_y);
        memcpy(b->beam_rgb[0], wp->beam_inner,  3);
        memcpy(b->beam_rgb[1], wp->beam_middle, 3);
        memcpy(b->beam_rgb[2], wp->beam_outer,  3);
        /* LOS hits still play the weapon's explosionclass at the
         * strike point (legacy:245025). */
        projectile_impact_fx(b, (uint32_t)bslot);
        if (b->area_of_effect > 0) {
            apply_projectile_area_damage(b);
        } else {
            t->health -= projectile_base_damage_for_unit(b, t);
            if (t->health <= 0) {
                credit_kill(shooter_idx, t);
                apply_killed(t, target_handle);
                if (u->target == target_handle) {
                    u->target = -1;
                    /* Don't cancel a standing PATROL on a kill. */
                    if (u->cmd_kind == UNIT_CMD_ATTACK)
                        u->cmd_kind = UNIT_CMD_NONE;
                    unit_clear_path(u);
                }
            } else {
                unit_on_damaged(t, shooter_idx);
            }
        }
        return;
    }

    float speed = (float)wp->velocity_pps;
    if (speed <= 0.0f) speed = 720.0f;

    int32_t tx = t->world_x;
    int32_t ty = t->world_y;
    if (wp->spray_angle > 0) {
        float dx = (float)(tx - u->world_x);
        float dy = (float)(ty - u->world_y);
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 0.001f) {
            uint32_t n = unit_deterministic_noise(u->stable_id,
                                                  (uint32_t)t->stable_id,
                                                  (uint32_t)(slot * 17 + burst_ordinal));
            float unit = ((float)(n & 0xffffu) / 65535.0f) * 2.0f - 1.0f;
            float radians = ((float)wp->spray_angle / 65536.0f) * 6.2831853f;
            float offset = tanf(radians * 0.5f) * len * unit;
            float nx = -dy / len;
            float ny =  dx / len;
            tx += (int32_t)(nx * offset);
            ty += (int32_t)(ny * offset);
        }
    }

    spawn_projectile(u->world_x, u->world_y,
                      tx, ty,
                      speed,
                      wp->damage,
                      wp->area_of_effect,
                      wp->edge_effectiveness,
                      wp,
                      weapon_visual_kind(wp),
                      target_handle,
                      shooter_idx,
                      u->player_id, u->team_color_idx);
}

/* Ground shot: projectile flies to (cmd_x, cmd_y) with no unit target
 * — splash there damages everything (friendly fire, legacy). */
static void fire_ground_shot(Unit *u, int shooter_idx, int slot,
                             const UnitWeapon *wp) {
    if (!u || !wp) return;
    start_fire_script(u, slot);
    const GameWorld *sw = World_Get();
    if (wp->start_sound[0]) {
        GameSound_PlayWorldWav(wp->start_sound, 0x7f, u->world_x, u->world_y,
                               sw ? sw->cam_x : 0, sw ? sw->cam_y : 0,
                               sw ? sw->viewport_w : 0,
                               sw ? sw->viewport_h : 0);
    }
    float speed = (float)wp->velocity_pps;
    if (speed <= 0.0f) speed = 720.0f;
    int slot_idx = spawn_projectile(u->world_x, u->world_y,
                                    u->cmd_x, u->cmd_y,
                                    (wp->los_kind==1||wp->los_kind==2) ? 1.0f : speed,
                                    wp->damage, wp->area_of_effect,
                                    wp->edge_effectiveness, wp,
                                    weapon_visual_kind(wp), -1, shooter_idx,
                                    u->player_id, u->team_color_idx);
    if (slot_idx < 0 || (wp->los_kind != 1 && wp->los_kind != 2)) return;
    /* LOS ground shot: detonate at the aim point immediately, hold beam. */
    Projectile *b = &g_projectiles[slot_idx];
    b->is_beam   = 1;
    b->src_x     = u->world_x;
    b->src_y     = u->world_y;
    b->world_x   = u->cmd_x;
    b->world_y   = u->cmd_y;
    b->speed_ppt = 0.0f;
    b->ttl_ticks = (int16_t)wp->emit_ticks;
    if (sw) b->height = (float)Terrain_SampleHeight(sw, b->world_x, b->world_y);
    memcpy(b->beam_rgb[0], wp->beam_inner,  3);
    memcpy(b->beam_rgb[1], wp->beam_middle, 3);
    memcpy(b->beam_rgb[2], wp->beam_outer,  3);
    projectile_impact_fx(b, (uint32_t)slot_idx);
    if (b->area_of_effect > 0) apply_projectile_area_damage(b);
}

static void tick_weapon_burst(Unit *u, int shooter_idx, int slot,
                              const UnitDef *def, UnitWeaponState *ws) {
    if (!u || !def || !ws) return;
    if (slot < 0 || slot >= def->num_weapons) return;
    if (ws->burst_remaining <= 0) return;
    if (ws->burst_ticks > 0) {
        ws->burst_ticks--;
        return;
    }
    int target = ws->burst_target;
    if (target < 0 || target >= g_unit_count ||
        g_units[target].alive != UNIT_ALIVE_ACTIVE ||
        !unit_can_see_target(u, &g_units[target])) {
        ws->burst_remaining = 0;
        ws->burst_target = -1;
        return;
    }
    const UnitWeapon *wp = &def->weapons[slot];
    fire_weapon_shot(u, shooter_idx, slot, wp, target,
                     ws->burst_remaining, 1);
    ws->burst_remaining--;
    if (ws->burst_remaining > 0) {
        ws->burst_ticks = wp->burst_rate_ticks > 0 ? wp->burst_rate_ticks : 1;
    } else {
        ws->burst_target = -1;
    }
}

/* Ground contact state for setSFXoccupy (legacy:185079-185112): a flyer
 * is always 5, a ground mover is 4 wading, 1 on land, 2 sitting at its
 * waterline. Sent only when it changes. */
static int unit_sfx_occupy_state(const Unit *u, const UnitDef *def,
                                 const GameWorld *w) {
    (void)def;
    if (u->flying) return 5;
    if (!w || w->water_height <= 0) return 1;
    int h = Terrain_SampleHeight(w, u->world_x, u->world_y);
    int sea = w->water_height;
    if (sea > h) return 4;
    int st = (h - sea > -5) ? 1 : 4;
    if (def->waterline + h == sea) st = 2;
    return st;
}

/* Flight state machine. The original starts a flyer's mission with
 * BeginFlight and ends it with BeginLanding (legacy:24117, :24302);
 * the script's own watcher runs the wing loop while the occupy state
 * says airborne. The climb is part of the mover's 3D step toward the
 * cruise height (legacy:190500, :241337), so it runs at walking speed. */
/* Airborne is a state the unit has to enter, and only through the
 * flight pair: BeginFlight flips the mission's mode from ground to air
 * (legacy:24117-24131) and only the landing path ever sets the ground
 * mode it flips from (legacy:24387). A unit whose script carries no
 * BeginFlight never runs that state machine and so never reads as
 * airborne. Seventeen of the twenty-two units with canfly have the
 * pair. The five without are the ghost ship, the two Taros priests,
 * the Veruna ball and the bird: they hover at their cruise height and
 * stay surface targets, which is why a war galley's cannon, a weapon
 * flagged noairweapon, can shell a ghost ship. */
static int unit_def_has_flight(const UnitDef *def) {
    return def && def->cob_script &&
           Cob_FindScript(def->cob_script, "BeginFlight") >= 0;
}

static void flight_tick(Unit *u, const UnitDef *def, const GameWorld *w) {
    if (!def->can_fly) return;
    float target;
    if (!unit_def_has_flight(def)) {
        u->flying = 0;
        target = (float)def->cruise_alt;   /* hovers, never takes off */
    } else {
        /* Takeoff follows the original's missions: moving, and an
         * ordered attack, lift the unit at once (legacy:25786). One
         * that picked its own target stays down while it can already
         * reach it (legacy:26770-26803). Once up it stays up until it
         * runs out of orders and targets. */
        int lift = (u->anim_state == UNIT_ANIM_MOVING) ||
                   (u->cmd_kind != UNIT_CMD_NONE &&
                    (u->cmd_kind != UNIT_CMD_ATTACK || u->attack_explicit));
        int idle = u->anim_state == UNIT_ANIM_IDLE &&
                   u->cmd_kind == UNIT_CMD_NONE && u->target < 0;
        if (!u->flying && lift) {
            u->flying = 1;
            unit_start_script(u, "BeginFlight", NULL, 0);
        } else if (u->flying && idle) {
            u->flying = 0;
            unit_start_script(u, "BeginLanding", NULL, 0);
        }
        target = u->flying ? (float)def->cruise_alt : 0.0f;
    }
    int occ = unit_sfx_occupy_state(u, def, w);
    if (u->sfx_occupy != occ) {
        int32_t arg = occ;
        unit_start_script(u, "setSFXoccupy", &arg, 1);
        u->sfx_occupy = (uint8_t)occ;
    }
    float step = def->max_velocity * 30.0f / 60.0f;
    if (step < 0.5f) step = 0.5f;
    if (u->flight_alt < target) {
        u->flight_alt += step;
        if (u->flight_alt > target) u->flight_alt = target;
    } else if (u->flight_alt > target) {
        u->flight_alt -= step;
        if (u->flight_alt < target) u->flight_alt = target;
    }
}

/* A caster's reserve: a {value, max} pair the original tops up by
 * manarechargerate every frame once the unit is built (legacy:8709,
 * Resource_Add at legacy:8661 caps at the max), and every mana-costing
 * shot draws from it (legacy:17214, legacy:245908). A unit starts full. */
static void caster_mana_tick(Unit *u, const UnitDef *def) {
    if (def->max_mana <= 0) return;
    if (u->mana_max != (float)def->max_mana) {
        u->mana_max = (float)def->max_mana;
        u->mana = u->mana_max;
        return;
    }
    u->mana += def->mana_recharge_per_sec / 60.0f;
    if (u->mana > u->mana_max) u->mana = u->mana_max;
}

int Units_GetMana(int handle, float *out_cur, float *out_max) {
    if (handle < 0 || handle >= g_unit_count) return 0;
    const Unit *u = &g_units[handle];
    if (u->mana_max <= 0.0f) return 0;
    if (out_cur) *out_cur = u->mana;
    if (out_max) *out_max = u->mana_max;
    return 1;
}

void Units_DebugSetMana(int handle, float value) {
    if (handle < 0 || handle >= g_unit_count) return;
    Unit *u = &g_units[handle];
    if (value < 0.0f) value = 0.0f;
    if (value > u->mana_max) value = u->mana_max;
    u->mana = value;
}

static void Units_TickCombat(void) {
    const GameWorld *flight_world = World_Get();
    for (int i = 0; i < g_unit_count; i++) {
        Unit *u = &g_units[i];
        if (u->alive == UNIT_ALIVE_TRANSPORTED) {
            if (u->carried_by >= 0 && u->carried_by < g_unit_count &&
                g_units[u->carried_by].alive == UNIT_ALIVE_ACTIVE) {
                u->world_x = g_units[u->carried_by].world_x;
                u->world_y = g_units[u->carried_by].world_y;
            } else {
                u->alive = UNIT_ALIVE_ACTIVE;
                u->carried_by = -1;
            }
            continue;
        }
        if (u->alive != UNIT_ALIVE_ACTIVE && u->alive != UNIT_ALIVE_DYING) continue;
        const UnitDef *def = Units_GetDef(u->def_idx);
        if (!def) continue;
        /* Not built yet: the order is kept but nothing acts on it until
         * getbuilt runs, so a product cannot be walked off the pad. */
        if (u->under_construction) continue;
        if (u->alive == UNIT_ALIVE_ACTIVE) {
            flight_tick(u, def, flight_world);
            caster_mana_tick(u, def);
        }

        /* Per-weapon cooldown decrements every tick regardless of state. */
        for (int w = 0; w < def->num_weapons; w++) {
            if (u->weapon_state[w].cooldown_ticks > 0) {
                u->weapon_state[w].cooldown_ticks--;
            }
            tick_weapon_burst(u, i, w, def, &u->weapon_state[w]);
        }
        /* Legacy single-weapon cooldown for backward compat (unused
         * once multi-weapon is fully wired). */
        if (u->attack_cooldown > 0) u->attack_cooldown--;

        /* Validate target. Clear if dead, out-of-range, or friendly
         * (auto-acquire only fires on enemy player_ids). */
        if (u->target >= 0) {
            int target_allowed = 1;
            if (u->cmd_kind == UNIT_CMD_ATTACK &&
                u->target >= 0 && u->target < g_unit_count && def->num_weapons > 0) {
                int slot = u->weapon_slot;
                if (slot < 0 || slot >= def->num_weapons) slot = 0;
                target_allowed = weapon_can_target_unit(&def->weapons[slot],
                                                        &g_units[u->target]);
            }
            int friendly_target =
                (u->target < g_unit_count &&
                 !unit_players_are_enemies(u->player_id,
                                           g_units[u->target].player_id));
            int bad_relation =
                ((u->cmd_kind == UNIT_CMD_ATTACK && friendly_target) ||
                 (u->cmd_kind == UNIT_CMD_GUARD && !friendly_target) ||
                 (u->cmd_kind == UNIT_CMD_REPAIR && !friendly_target) ||
                 (u->cmd_kind == UNIT_CMD_LOAD && !friendly_target));
            if (u->target >= g_unit_count || g_units[u->target].alive != 1
                || bad_relation || !target_allowed
                || !unit_can_see_target(u, &g_units[u->target]))
            {
                u->target = -1;
                if (u->cmd_kind == UNIT_CMD_ATTACK ||
                    u->cmd_kind == UNIT_CMD_GUARD ||
                    u->cmd_kind == UNIT_CMD_REPAIR ||
                    u->cmd_kind == UNIT_CMD_RECLAIM ||
                    u->cmd_kind == UNIT_CMD_LOAD) {
                    u->cmd_kind = UNIT_CMD_NONE;
                    unit_clear_path(u);
                }
            }
        }

        /* Auto-acquire when idle (no manual command + no target). Per
         * recon, TAK fires `Unit_FindTargetById` searches within sight
         * range and returns nearest enemy. */
        /* Auto-targeting respects per-unit aggression posture
         * (the legacy reference ~9063 + 151409):
         *   PASSIVE:    never auto-target — only fire on manual order
         *   DEFENSIVE:  auto-acquire only inside the *weapon range*
         *               (no chasing — defend in place)
         *   OFFENSIVE:  acquire anything inside sight_distance
         *               (will chase out of weapon range to engage)
         * Builders/lodestones with no weapons skip auto-target entirely. */
        /* Patrollers fight en route: they may acquire, but keep their
         * standing PATROL order so the route resumes after the kill. */
        if (u->target < 0 &&
            (u->cmd_kind == UNIT_CMD_NONE ||
             u->cmd_kind == UNIT_CMD_PATROL) &&
            def->num_weapons > 0 &&
            u->aggro_mode != UNIT_AGGRO_PASSIVE &&
            def->sight_distance > 0)
        {
            /* Offensive scan covers at least weapon reach: towers have
             * sight 250 vs range 500, so a sight-only radius meant they
             * never engaged anything they could actually hit. */
            int64_t wrange = (int64_t)weapon_effective_range(&def->weapons[0]);
            int64_t scan_radius = (u->aggro_mode == UNIT_AGGRO_DEFENSIVE)
                ? wrange
                : ((int64_t)def->sight_distance > wrange
                       ? (int64_t)def->sight_distance : wrange);
            if (scan_radius > 0) {
                int best_i = ugrid_nearest_enemy(u, i, scan_radius,
                                                 &def->weapons[0]);
                if (best_i >= 0) {
                    u->target = (int16_t)best_i;
                    u->attack_explicit = 0;
                    if (u->cmd_kind != UNIT_CMD_PATROL)
                        u->cmd_kind = UNIT_CMD_ATTACK;
                    unit_clear_path(u);
                }
            }

            /* Basic AI pursuit: enemy units (player_id != 1) with no
             * target in sight range walk toward the nearest player
             * unit. Mirrors legacy AI behaviour at high level — the
             * full AIBrain (legacy:15241+) reads weight/limit
             * scripts from data/ai/ and makes priority-driven decisions
             * (build, attack, defend); this is a placeholder until
             * that lands. Keeps enemy monarchs from idling forever. */
            if (u->target < 0 && u->cmd_kind == UNIT_CMD_NONE &&
                u->player_id != 1 && def->max_velocity > 0.0f)
            {
                int weapon_range = (def->num_weapons > 0)
                    ? weapon_effective_range(&def->weapons[0]) : 0;
                int scan_radius = TAK_AI_PursuitRadius(
                    def->sight_distance, weapon_range,
                    ai_difficulty_for_player(u->player_id));
                if (scan_radius > 0) {
                    int nearest_i = ugrid_nearest_enemy(u, i, scan_radius,
                                                        NULL);
                    if (nearest_i >= 0) {
                        u->cmd_kind = UNIT_CMD_MOVE;
                        u->cmd_x = g_units[nearest_i].world_x;
                        u->cmd_y = g_units[nearest_i].world_y;
                        unit_clear_path(u);
                    }
                }
            }
        }

        /* Determine desired state. Order of precedence:
         *   target acquired + in-range → ATTACKING
         *   target acquired + out-of-range → MOVING (toward target)
         *   manual MOVE command → MOVING
         *   else → IDLE */
        UnitAnimState desired = UNIT_ANIM_IDLE;
        int32_t  goal_x = u->world_x, goal_y = u->world_y;
        int      target_in_range = 0;
        int64_t  target_d2 = 0;
        float    heading_at_entry = u->heading;

        if (u->cmd_kind == UNIT_CMD_GUARD && u->target >= 0) {
            Unit *t = &g_units[u->target];
            int64_t dx = (int64_t)(t->world_x - u->world_x);
            int64_t dy = (int64_t)(t->world_y - u->world_y);
            target_d2 = dx*dx + dy*dy;
            if (target_d2 > (int64_t)96 * 96) {
                desired = UNIT_ANIM_MOVING;
                goal_x = t->world_x;
                goal_y = t->world_y;
                u->cmd_x = goal_x;
                u->cmd_y = goal_y;
            }
            /* Immobile units never swing their base — legacy aims the
             * turret/bowmen PIECES via the COB AimWeapon script while
             * the structure itself stays put. */
            if ((dx != 0 || dy != 0) && def->max_velocity > 0.0f)
                u->heading = atan2f((float)dx, -(float)dy);
        } else if (u->cmd_kind == UNIT_CMD_RECLAIM && u->target < 0 &&
                   u->reclaim_tile_x >= 0) {
            /* Clearing a map feature: walk to the cell, then hold in the
             * work state. Legacy's reclaim order approaches, sets the
             * unit's work state and counts a duration down before the
             * feature record goes (legacy:32288-32320, 32366-32371). */
            GameWorld *rw = World_Get();
            int fi = unit_reclaim_feature_idx(u, rw);
            if (fi < 0) {
                u->cmd_kind = UNIT_CMD_NONE;
                u->reclaim_tile_x = -1;
                u->reclaim_tile_y = -1;
                unit_clear_path(u);
            } else {
                int32_t fx = u->cmd_x, fy = u->cmd_y;
                Features_InstanceCentre(rw, fi, &fx, &fy);
                u->cmd_x = fx;
                u->cmd_y = fy;
                const FeatureDef *ffd =
                    Features_GetByIndex(rw->features[fi].global_idx);
                int fp = 1;
                if (ffd) {
                    fp = ffd->footprint_x > ffd->footprint_z
                       ? ffd->footprint_x : ffd->footprint_z;
                    if (fp < 1) fp = 1;
                }
                /* Reach = FBI builddistance plus half the footprint, so a
                 * big rock is worked from its edge like a build site. */
                int reach = def->build_distance > 0 ? def->build_distance : 48;
                int work_range = reach + fp * 8;
                int64_t dx = (int64_t)(fx - u->world_x);
                int64_t dy = (int64_t)(fy - u->world_y);
                target_d2 = dx*dx + dy*dy;
                if (target_d2 > (int64_t)work_range * work_range) {
                    desired = UNIT_ANIM_MOVING;
                    goal_x = fx;
                    goal_y = fy;
                } else {
                    desired = UNIT_ANIM_BUILDING;
                }
                if ((dx != 0 || dy != 0) && def->max_velocity > 0.0f)
                    u->heading = atan2f((float)dx, -(float)dy);
            }
        } else if ((u->cmd_kind == UNIT_CMD_REPAIR ||
                    u->cmd_kind == UNIT_CMD_RECLAIM ||
                    u->cmd_kind == UNIT_CMD_LOAD) &&
                   u->target >= 0) {
            Unit *t = &g_units[u->target];
            int64_t dx = (int64_t)(t->world_x - u->world_x);
            int64_t dy = (int64_t)(t->world_y - u->world_y);
            target_d2 = dx*dx + dy*dy;
            /* Repair and reclaim are builder work, so the reach is the
             * FBI builddistance (ARAPRIES 75), same as UNIT_CMD_BUILD. */
            int work_range = 48;
            if (u->cmd_kind == UNIT_CMD_LOAD) {
                if (def->transport_distance > 0)
                    work_range = def->transport_distance;
            } else if (def->build_distance > 0) {
                work_range = def->build_distance;
            }
            /* Measured to the target's footprint, not its centre: now
             * that units and buildings hold their cells, a repairer can
             * never stand on a big target's middle. It is the same stand-back
             * the UNIT_CMD_BUILD path already applies. */
            {
                int tfx, tfz;
                unit_occ_fp(t, &tfx, &tfz);
                const UnitDef *td = Units_GetDef(t->def_idx);
                if (td && td->footprint_x > tfx) tfx = td->footprint_x;
                if (td && td->footprint_z > tfz) tfz = td->footprint_z;
                work_range += (int)(8.0f * sqrtf((float)(tfx * tfx +
                                                         tfz * tfz)));
            }
            if (target_d2 > (int64_t)work_range * work_range) {
                desired = UNIT_ANIM_MOVING;
                goal_x = t->world_x;
                goal_y = t->world_y;
                u->cmd_x = goal_x;
                u->cmd_y = goal_y;
            } else if (u->cmd_kind == UNIT_CMD_REPAIR ||
                       u->cmd_kind == UNIT_CMD_RECLAIM) {
                desired = UNIT_ANIM_BUILDING;
            } else if (u->cmd_kind == UNIT_CMD_LOAD) {
                unit_load_into_transport(u, i, t, u->target);
            }
            /* Immobile units never swing their base — legacy aims the
             * turret/bowmen PIECES via the COB AimWeapon script while
             * the structure itself stays put. */
            if ((dx != 0 || dy != 0) && def->max_velocity > 0.0f)
                u->heading = atan2f((float)dx, -(float)dy);
        } else if (u->target >= 0) {
            Unit *t = &g_units[u->target];
            int64_t dx = (int64_t)(t->world_x - u->world_x);
            int64_t dy = (int64_t)(t->world_y - u->world_y);
            target_d2 = unit_reach_d2(u, t);
            /* Range check uses the unit's ACTIVE weapon slot — a caster
             * switched to its long-range Special weapon should engage
             * targets sooner than with its short-range Primary. */
            int range_slot = u->weapon_slot;
            if (range_slot < 0 || range_slot >= def->num_weapons) range_slot = 0;
            int range = (def->num_weapons > 0)
                ? weapon_effective_range(&def->weapons[range_slot]) : 0;
            int min_range = (def->num_weapons > 0)
                ? weapon_min_range(&def->weapons[range_slot]) : 0;
            int64_t range_d2 = (int64_t)range * range;
            int64_t min_range_d2 = (int64_t)min_range * min_range;
            if (range > 0 && target_d2 <= range_d2 &&
                (min_range <= 0 || target_d2 >= min_range_d2)) {
                target_in_range = 1;
                desired = UNIT_ANIM_ATTACKING;
            } else {
                desired = UNIT_ANIM_MOVING;
                goal_x = t->world_x;
                goal_y = t->world_y;
            }
            /* Face the target only once in reach. On the approach the
             * walker owns the heading: forcing it here sent a unit
             * along the target bearing instead of its waypoint, so it
             * ground against whatever stood in between and never
             * arrived. Immobile units never swing their base; their
             * pieces aim through the COB AimWeapon script. */
            if (target_in_range && (dx != 0 || dy != 0) &&
                def->max_velocity > 0.0f)
                u->heading = atan2f((float)dx, -(float)dy);
        } else if (u->cmd_kind == UNIT_CMD_ATTACK_GROUND &&
                   def->num_weapons > 0) {
            /* Fire at a map point until a new order (legacy attack-
             * ground; friendly-fire splash). In range → ATTACKING with
             * target -1; the fire branch aims at cmd_x/cmd_y. */
            int64_t dx = (int64_t)(u->cmd_x - u->world_x);
            int64_t dy = (int64_t)(u->cmd_y - u->world_y);
            target_d2 = dx*dx + dy*dy;
            int range_slot = u->weapon_slot;
            if (range_slot < 0 || range_slot >= def->num_weapons) range_slot = 0;
            int range = weapon_effective_range(&def->weapons[range_slot]);
            if (range > 0 && target_d2 <= (int64_t)range * range) {
                target_in_range = 1;
                desired = UNIT_ANIM_ATTACKING;
            } else {
                desired = UNIT_ANIM_MOVING;
                goal_x = u->cmd_x;
                goal_y = u->cmd_y;
            }
            if (target_in_range && (dx != 0 || dy != 0) &&
                def->max_velocity > 0.0f)
                u->heading = atan2f((float)dx, -(float)dy);
        } else if (u->cmd_kind == UNIT_CMD_MOVE) {
            desired = UNIT_ANIM_MOVING;
            goal_x = u->cmd_x;
            goal_y = u->cmd_y;
        } else if (u->cmd_kind == UNIT_CMD_PATROL) {
            desired = UNIT_ANIM_MOVING;
            goal_x = u->cmd_x;
            goal_y = u->cmd_y;
        } else if (u->cmd_kind == UNIT_CMD_UNLOAD) {
            desired = UNIT_ANIM_MOVING;
            goal_x = u->cmd_x;
            goal_y = u->cmd_y;
        } else if (u->cmd_kind == UNIT_CMD_BUILD) {
            /* Builder en route to / working on a building site. While
             * still walking, MOVING; once we're at the footprint edge
             * (NOT the centre — the builder must stand outside the
             * building, not on top of it) and the target is still
             * under construction, switch to BUILDING so the COB plays
             * the build loop. Stand-back distance = half the footprint
             * diagonal in world pixels + a small buffer so the builder
             * actually clears the building's bounding box. */
            int target_alive_inprog = 0;
            int stand_back_px = 16;  /* default 1-tile margin */
            if (u->build_target >= 0 && u->build_target < g_unit_count) {
                const Unit *bt = &g_units[u->build_target];
                target_alive_inprog = (bt->alive == 1 && bt->under_construction);
                const UnitDef *btd = bt->alive ? Units_GetDef(bt->def_idx) : NULL;
                if (btd) {
                    int fx = btd->footprint_x > 0 ? btd->footprint_x : 1;
                    int fz = btd->footprint_z > 0 ? btd->footprint_z : 1;
                    int half_diag = (int)(8.0f * sqrtf((float)(fx*fx + fz*fz)));
                    stand_back_px = half_diag + 12;
                }
            }
            /* Walk toward the building centre, but stop at the stand-
             * back radius. Compute a goal point that is `stand_back_px`
             * shy of the centre along the builder→site vector. If the
             * builder is already inside that radius, treat as arrived
             * and stop in place. */
            int32_t cx = u->cmd_x, cy = u->cmd_y;
            int64_t vx = (int64_t)(cx - u->world_x);
            int64_t vy = (int64_t)(cy - u->world_y);
            int64_t d2 = vx*vx + vy*vy;
            /* Builders work at arm's length: FBI `builddistance`
             * (ARABUILD 75, ARAKING 100) is the legacy reach, so the
             * builder stops there instead of walking onto the site. */
            int reach = def->build_distance > 0 ? def->build_distance : 32;
            int work_radius = stand_back_px + reach;
            int64_t r2 = (int64_t)work_radius * work_radius;
            int arrived_at_site = (d2 <= r2);
            if (!arrived_at_site) {
                float d = sqrtf((float)d2);
                float scale = (d - (float)stand_back_px) / d;
                goal_x = u->world_x + (int32_t)((float)vx * scale);
                goal_y = u->world_y + (int32_t)((float)vy * scale);
                /* Face the build site even before arrival — looks
                 * cleaner than walking sideways into it. */
                if (vx != 0 || vy != 0) {
                    u->heading = atan2f((float)vx, -(float)vy);
                }
            } else {
                goal_x = u->world_x;
                goal_y = u->world_y;
                /* Face the building so the build animation plays
                 * facing the work, not whatever heading we approached
                 * from. */
                if (vx != 0 || vy != 0) {
                    u->heading = atan2f((float)vx, -(float)vy);
                }
            }
            if (arrived_at_site && target_alive_inprog) {
                desired = UNIT_ANIM_BUILDING;
            } else {
                desired = UNIT_ANIM_MOVING;
            }
        }

        enter_state(u, desired);

        /* Per-state per-tick work. */
        switch (u->anim_state) {
            case UNIT_ANIM_MOVING: {
                /* Keep walk thread alive — many scripts loop once and
                 * RETURN; restart while still moving. */
                ensure_thread(u, "walk", &u->walk_thread_slot, NULL, 0);
                int arrived = walk_tick(u, def, goal_x, goal_y);
                if (arrived) {
                    u->velocity = 0; u->cur_speed_ppt = 0.0f;
                    if (u->cmd_kind == UNIT_CMD_MOVE) {
                        u->cmd_kind = UNIT_CMD_NONE;
                        unit_clear_path(u);
                    } else if (u->cmd_kind == UNIT_CMD_UNLOAD) {
                        /* One unload order empties the hold: the
                         * carrier keeps setting units down at the
                         * point until it has none left or nowhere to
                         * put them. */
                        int dropped = unit_unload_one_from_transport(
                            u, i, u->cmd_x, u->cmd_y);
                        if (!dropped || u->cargo_count <= 0) {
                            u->cmd_kind = UNIT_CMD_NONE;
                            unit_clear_path(u);
                        }
                    } else if (u->cmd_kind == UNIT_CMD_PATROL &&
                               u->target < 0) {
                        /* Swap legs only on arrival at the waypoint. A
                         * patroller chasing an acquired enemy walks to
                         * the enemy instead, and reaching it must not
                         * reverse the route (legacy keeps the standing
                         * patrol order untouched while the sub-order
                         * runs, legacy:11565). */
                        int32_t next_x = u->patrol_x;
                        int32_t next_y = u->patrol_y;
                        u->patrol_x = u->cmd_x;
                        u->patrol_y = u->cmd_y;
                        u->cmd_x = next_x;
                        u->cmd_y = next_y;
                        unit_clear_path(u);
                    }
                    /* Don't clear target — combat will pick up on next
                     * tick when we transition to ATTACKING in range.
                     * Build-target HP feeding now happens in the
                     * UNIT_ANIM_BUILDING state instead of here. */
                }
            } break;

            case UNIT_ANIM_BUILDING: {
                /* Builder is anchored at the build site holding the
                 * pose StartBuilding set. The legacy loader stores
                 * target buildtime and builder workertime as floats
                 * (legacy:162897, 162906), so construction
                 * needs a fractional accumulator instead of integer
                 * HP division. */
                u->velocity = 0; u->cur_speed_ppt = 0.0f;
                /* No script invocation here. Legacy's construction tick
                 * only moves hitpoints. StartBuilding already ran once
                 * on the state edge (legacy:9435) and StopBuilding runs
                 * once when the state falls. Re-invoking it every tick
                 * is what made Elsin's sword swing on a loop. */
                if (u->cmd_kind == UNIT_CMD_RECLAIM && u->target < 0 &&
                    u->reclaim_tile_x >= 0) {
                    /* Feature sweep. Legacy counts a stored frame count
                     * down and only then drops the feature record
                     * (legacy:32318-32320, 32366-32371). We spend the
                     * feature's hit points at the builder's workertime
                     * per tick and pay its `energy` back as mana pro
                     * rata (field parse legacy:127332). */
                    GameWorld *rw = World_Get();
                    int fi = unit_reclaim_feature_idx(u, rw);
                    const FeatureDef *ffd = (fi >= 0)
                        ? Features_GetByIndex(rw->features[fi].global_idx)
                        : NULL;
                    if (!ffd) {
                        u->cmd_kind = UNIT_CMD_NONE;
                        u->reclaim_tile_x = -1;
                        u->reclaim_tile_y = -1;
                        unit_clear_path(u);
                        break;
                    }
                    float hp_max = (ffd->damage > 0)
                                 ? (float)ffd->damage : 1.0f;
                    float worker = (def && def->worker_time > 0.0f)
                                 ? def->worker_time : 1.0f;
                    float prev = u->reclaim_accum;
                    u->reclaim_accum += worker;
                    if (u->reclaim_accum > hp_max) u->reclaim_accum = hp_max;
                    if (ffd->energy > 0.0f) {
                        float paid = ffd->energy *
                                     (u->reclaim_accum - prev) / hp_max;
                        if (paid > 0.0f && rw)
                            Economy_EarnF(&rw->economy, u->player_id, paid);
                    }
                    if (u->reclaim_accum >= hp_max) {
                        Features_RemoveInstance(rw, fi);
                        u->cmd_kind = UNIT_CMD_NONE;
                        u->reclaim_tile_x = -1;
                        u->reclaim_tile_y = -1;
                        u->reclaim_accum = 0.0f;
                        unit_clear_path(u);
                    }
                    break;
                }
                if (u->cmd_kind == UNIT_CMD_REPAIR ||
                    u->cmd_kind == UNIT_CMD_RECLAIM) {
                    if (u->target < 0 || u->target >= g_unit_count) {
                        u->cmd_kind = UNIT_CMD_NONE;
                        unit_clear_path(u);
                        break;
                    }
                    Unit *rt = &g_units[u->target];
                    if (rt->alive != 1) {
                        u->cmd_kind = UNIT_CMD_NONE;
                        u->target = -1;
                        unit_clear_path(u);
                        break;
                    }
                    const UnitDef *rtd = Units_GetDef(rt->def_idx);
                    int hp_max = rt->max_health > 0 ? rt->max_health : 1;
                    float worker = (def && def->worker_time > 0.0f)
                                 ? def->worker_time : 1.0f;
                    if (u->cmd_kind == UNIT_CMD_REPAIR) {
                        float heal_time = (rtd && rtd->heal_time > 0.0f)
                                        ? rtd->heal_time : 1.0f;
                        float hp_per_tick_f =
                            ((float)hp_max * worker) / (heal_time * 60.0f);
                        /* Healing is paid for. The original charges
                         * the target's cost spread over its build
                         * time, takes whatever the treasury holds and
                         * heals proportionally slower when it is short
                         * (legacy:39546-39562). The rate of repair
                         * comes from healtime, the price from
                         * buildtime and buildcost. */
                        if (rtd && rtd->build_cost > 0 &&
                            rtd->buildtime > 0.0f) {
                            float cost_per_tick_f =
                                ((float)rtd->build_cost * worker) /
                                (rtd->buildtime * 60.0f);
                            GameWorld *hgw = World_Get();
                            if (cost_per_tick_f > 0.0f && hgw) {
                                float paid = Economy_SpendAvailable(
                                    &hgw->economy, u->player_id,
                                    cost_per_tick_f);
                                float scale = paid / cost_per_tick_f;
                                if (scale <= 0.0f) break;
                                if (scale > 1.0f) scale = 1.0f;
                                hp_per_tick_f *= scale;
                            }
                        }
                        rt->build_hp_accum += hp_per_tick_f;
                        int hp_per_tick = (int)floorf(rt->build_hp_accum);
                        if (hp_per_tick > 0) {
                            rt->build_hp_accum -= (float)hp_per_tick;
                            rt->health += hp_per_tick;
                            if (rt->health > hp_max) rt->health = hp_max;
                        }
                        if (rt->health >= hp_max) {
                            u->cmd_kind = UNIT_CMD_NONE;
                            u->target = -1;
                            unit_clear_path(u);
                        }
                    } else {
                        float reclaim_time = (rtd && rtd->buildtime > 0.0f)
                                           ? rtd->buildtime : 100.0f;
                        float hp_per_tick_f =
                            ((float)hp_max * worker) / (reclaim_time * 60.0f);
                        rt->build_hp_accum += hp_per_tick_f;
                        int hp_per_tick = (int)floorf(rt->build_hp_accum);
                        if (hp_per_tick > 0) {
                            rt->build_hp_accum -= (float)hp_per_tick;
                            rt->health -= hp_per_tick;
                        }
                        /* Pay back pro rata on the FRACTIONAL hit points
                         * removed. The integer form truncated to zero
                         * whenever buildcost < maxhp, so most reclaims
                         * returned no mana at all. */
                        if (rtd && rtd->build_cost > 0) {
                            float earned = ((float)rtd->build_cost
                                            * hp_per_tick_f) / (float)hp_max;
                            if (earned > 0.0f) {
                                GameWorld *wgw = World_Get();
                                if (wgw) Economy_EarnF(&wgw->economy,
                                                       u->player_id,
                                                       earned);
                            }
                        }
                        if (rt->health <= 0) {
                            apply_killed(rt, u->target);
                            u->cmd_kind = UNIT_CMD_NONE;
                            u->target = -1;
                            unit_clear_path(u);
                        }
                    }
                    break;
                }
                if (u->build_target < 0 || u->build_target >= g_unit_count) {
                    u->cmd_kind = UNIT_CMD_NONE;
                    unit_clear_path(u);
                    break;
                }
                Unit *bt = &g_units[u->build_target];
                if (bt->alive != 1 || !bt->under_construction) {
                    /* Target gone or already complete — drop the cmd
                     * so we exit BUILDING next tick (StopBuilding fires). */
                    u->cmd_kind = UNIT_CMD_NONE;
                    u->build_target = -1;
                    unit_clear_path(u);
                    break;
                }
                const UnitDef *btd = Units_GetDef(bt->def_idx);
                int hp_max = bt->max_health > 0 ? bt->max_health : 1;
                float buildtime = (btd && btd->buildtime > 0.0f)
                                ? btd->buildtime : 100.0f;
                float worker = (def && def->worker_time > 0.0f)
                             ? def->worker_time : 1.0f;
                float progress_scale = 1.0f;
                /* A worker is standing here, so the frame is not
                 * abandoned even when the treasury is empty. The
                 * original pays what it can, scales the tick's
                 * progress by that share and leaves the frame alone
                 * (legacy:39483-39496), which is why you can always
                 * start another building. It just goes slowly. */
                bt->nano_idle_ticks = 0;
                if (btd && btd->build_cost > 0) {
                    GameWorld *wgw = World_Get();
                    float cost_per_tick_f =
                        ((float)btd->build_cost * worker) /
                        (buildtime * 60.0f);
                    if (cost_per_tick_f > 0.0f && wgw) {
                        float paid = Economy_SpendAvailable(&wgw->economy,
                                                            u->player_id,
                                                            cost_per_tick_f);
                        progress_scale = paid / cost_per_tick_f;
                        if (progress_scale <= 0.0f) break;
                        if (progress_scale > 1.0f) progress_scale = 1.0f;
                    }
                }
                float hp_per_tick_f = (((float)hp_max * worker) /
                                      (buildtime * 60.0f)) * progress_scale;
                bt->build_hp_accum += hp_per_tick_f;
                int hp_per_tick = (int)floorf(bt->build_hp_accum);
                if (hp_per_tick > 0) {
                    bt->build_hp_accum -= (float)hp_per_tick;
                    bt->health += hp_per_tick;
                }
                if (bt->health >= hp_max) {
                    bt->health = hp_max;
                    bt->under_construction = 0;
                    bt->aggro_mode = UNIT_AGGRO_OFFENSIVE;
                    if (btd) {
                        GameWorld *wgw = World_Get();
                        if (wgw) {
                            int32_t cap_delta = btd->mogrium_storage;
                            float   regen_delta = btd->mogrium_income_per_sec
                                                  * sacred_income_mult(
                                                        btd, bt->world_x,
                                                        bt->world_y);
                            if (cap_delta != 0 || regen_delta != 0.0f) {
                                Economy_AdjustCaps(&wgw->economy,
                                                   bt->player_id,
                                                   cap_delta, regen_delta);
                                fprintf(stderr,
                                    "Build: %s economy +cap=%d +regen=%.1f/s for P%d\n",
                                    btd->unitname, cap_delta,
                                    regen_delta, bt->player_id);
                            }
                        }
                    }
                    fprintf(stderr,
                        "Build: %s complete (handle=%d)\n",
                        btd ? btd->unitname : "?",
                        u->build_target);
                    /* Factory exit: a mobile product finished in-yard
                     * must clear the factory footprint. Legacy hands
                     * the fresh unit a move order right after getbuilt
                     * (Mission_AssignMoveTarget, legacy:9430).
                     * Rally point wins when set (manual: units emerging
                     * from the structure rally to its Move point);
                     * otherwise the first walkable point just outside
                     * the footprint. */
                    if (btd && btd->max_velocity > 0.0f &&
                        def && def->max_velocity <= 0.0f) {
                        GameWorld *wgw = World_Get();
                        if (u->rally_set) {
                            bt->cmd_kind = UNIT_CMD_MOVE;
                            bt->cmd_x = u->rally_x;
                            bt->cmd_y = u->rally_y;
                            bt->target = -1;
                            unit_clear_path(bt);
                        } else {
                            int fx = def->footprint_x > 0 ? def->footprint_x : 2;
                            int fz = def->footprint_z > 0 ? def->footprint_z : 2;
                            int exit_px = (fx > fz ? fx : fz) * 8 + 24;
                            /* Off the pad first: keep going the way the
                             * pad already points, so the product steps
                             * clear of the doors instead of turning
                             * back through the yard. */
                            int32_t px = bt->world_x - u->world_x;
                            int32_t py = bt->world_y - u->world_y;
                            if (px != 0 || py != 0) {
                                float plen = sqrtf((float)px * (float)px +
                                                   (float)py * (float)py);
                                int32_t ex = bt->world_x +
                                    (int32_t)((float)px * 48.0f / plen);
                                int32_t ey = bt->world_y +
                                    (int32_t)((float)py * 48.0f / plen);
                                if (unit_terrain_walkable(wgw, btd, ex, ey)) {
                                    bt->cmd_kind = UNIT_CMD_MOVE;
                                    bt->cmd_x = ex;
                                    bt->cmd_y = ey;
                                    bt->target = -1;
                                    unit_clear_path(bt);
                                }
                            }
                            static const int exit_dirs[4][2] = {
                                { 0, 1 }, { 1, 0 }, { -1, 0 }, { 0, -1 }
                            };
                            for (int e = 0;
                                 e < 4 && bt->cmd_kind != UNIT_CMD_MOVE; e++) {
                                int32_t ex = u->world_x + exit_dirs[e][0] * exit_px;
                                int32_t ey = u->world_y + exit_dirs[e][1] * exit_px;
                                if (!unit_terrain_walkable(wgw, btd, ex, ey))
                                    continue;
                                bt->cmd_kind = UNIT_CMD_MOVE;
                                bt->cmd_x = ex;
                                bt->cmd_y = ey;
                                bt->target = -1;
                                unit_clear_path(bt);
                                break;
                            }
                        }
                    }
                    u->cmd_kind = UNIT_CMD_NONE;
                    u->build_target = -1;
                    /* Production queue advance (manual: the structure
                     * builds each queued unit in turn). */
                    if (u->prod_queue_len > 0 &&
                        def && def->max_velocity <= 0.0f) {
                        int next_def = u->prod_queue[0];
                        for (int q = 1; q < u->prod_queue_len; q++)
                            u->prod_queue[q - 1] = u->prod_queue[q];
                        u->prod_queue_len--;
                        (void)Units_BeginBuildingForUnit(i, next_def,
                                                         u->world_x,
                                                         u->world_y);
                    }
                    /* Falling out of BUILDING next tick triggers
                     * StopBuilding via enter_state's exit path. */
                }
            } break;

            case UNIT_ANIM_ATTACKING: {
                u->velocity = 0; u->cur_speed_ppt = 0.0f;
                /* Multi-weapon: each slot fires independently when its
                 * own cooldown is ready. For now we only have target
                 * info for slot 0 (range tracking); future Phase G
                 * extends per-weapon target acquisition. */
                if (target_in_range && def->num_weapons > 0) {
                    /* Fire whichever weapon the user picked via the
                     * Primary/Secondary/Special button row. Slot is
                     * clamped to [0, num_weapons) so a unit with only
                     * one weapon still works regardless of weapon_slot. */
                    int slot = u->weapon_slot;
                    if (slot < 0 || slot >= def->num_weapons) slot = 0;
                    UnitWeaponState *ws = &u->weapon_state[slot];
                    int ground = (u->cmd_kind == UNIT_CMD_ATTACK_GROUND);
                    /* Attack-ground aims at the clicked point the same
                     * way a unit target is aimed at. */
                    int aim_key = ground ? -2 : u->target;
                    int32_t aim_x = u->world_x, aim_y = u->world_y;
                    if (ground) {
                        aim_x = u->cmd_x; aim_y = u->cmd_y;
                    } else if (u->target >= 0 && u->target < g_unit_count) {
                        aim_x = g_units[u->target].world_x;
                        aim_y = g_units[u->target].world_y;
                    }
                    if (ws->cooldown_ticks == 0 &&
                        weapon_aim_ready(u, slot, aim_key, aim_x, aim_y, ws)) {
                        const UnitWeapon *wp = &def->weapons[slot];

                        /* A mana-costing shot draws from the caster's
                         * own reserve and waits while it is short
                         * (legacy:17214, legacy:245908). Every shipped
                         * unit with such a weapon carries maxmana; one
                         * without falls back to the player pool. */
                        int may_fire = 1;
                        if (wp->mana_per_shot > 0) {
                            if (def->max_mana > 0) {
                                if (u->mana < (float)wp->mana_per_shot) may_fire = 0;
                                else u->mana -= (float)wp->mana_per_shot;
                            } else {
                                GameWorld *w = World_Get();
                                if (!w || !Economy_TrySpend(&w->economy,
                                                             u->player_id,
                                                             wp->mana_per_shot)) {
                                    may_fire = 0;
                                }
                            }
                        }

                        if (may_fire) {
                            ws->cooldown_ticks = wp->reload_ticks;
                            if (ground) {
                                fire_ground_shot(u, i, slot, wp);
                                break;
                            }
                            fire_weapon_shot(u, i, slot, wp, u->target, 0, 1);
                            int burst_count = wp->burst > 1 ? wp->burst : 1;
                            if (burst_count > 1 && u->target >= 0) {
                                ws->burst_remaining = (int16_t)(burst_count - 1);
                                ws->burst_target = u->target;
                                ws->burst_ticks = wp->burst_rate_ticks > 0
                                                ? wp->burst_rate_ticks : 1;
                            }
                        } else {
                            /* Out of mana — short retry delay so we
                             * don't churn TrySpend every tick. */
                            ws->cooldown_ticks = 30;
                        }
                    }
                }
            } break;

            case UNIT_ANIM_IDLE:
            case UNIT_ANIM_DYING:
            case UNIT_ANIM_DEAD:
            default:
                u->velocity = 0; u->cur_speed_ppt = 0.0f;
                break;
        }
        /* Locomotion signals, both edge-triggered on a cached value the
         * way legacy caches them (legacy:184448, legacy:183171). */
        update_move_rate(u, def);
        update_turn_direction(u, heading_at_entry);
        (void)target_d2;
    }
}

/* Abandoned-nanoframe decay (legacy :9629-9657 + :39510-39524):
 * a frame no builder has fed for a 10s grace decays at HALF the
 * nominal build rate, refunding buildcost mana continuously and
 * proportionally; at zero HP the frame vanishes. */
static void tick_nanoframe_decay(void) {
    GameWorld *world = World_Get();
    for (int i = 0; i < g_unit_count; i++) {
        Unit *u = &g_units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE || !u->under_construction) continue;
        if (u->nano_idle_ticks < 30000) u->nano_idle_ticks++;
        /* 10s grace: legacy 300 frames at 30Hz (:9634) = 600 at our 60Hz. */
        if (u->nano_idle_ticks <= 600) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!d) continue;
        float buildtime = d->buildtime > 0.0f ? d->buildtime : 100.0f;
        int hp_max = u->max_health > 0 ? u->max_health : 1;
        /* The original's abandoned frame gives back half a build
         * unit per frame, at 30 frames a second (legacy:9643 calls
         * the tick with one frame, legacy:39534 halves it). That is
         * fifteen build units a second, thirty times what the old
         * per second reading of the same number produced, which is
         * why a dropped frame never visibly lost anything. */
        float frac_per_tick = 0.5f / (buildtime * 2.0f);
        if (world && d->build_cost > 0) {
            Economy_EarnF(&world->economy, u->player_id,
                          (float)d->build_cost * frac_per_tick);
        }
        u->build_hp_accum -= (float)hp_max * frac_per_tick;
        while (u->build_hp_accum <= -1.0f && u->health > 0) {
            u->build_hp_accum += 1.0f;
            u->health -= 1;
        }
        if (u->health <= 0) {
            apply_killed(u, i);
        }
    }
}

/* Engine-phase timing for the perf probe: 0 combat, 1 projectiles,
 * 2 cob, 3 misc; [4] counts A* calls. */
double g_eng_prof_ms[4];
double g_path_plan_calls;

static double eng_now_ms(void) {
    return (double)SDL_GetPerformanceCounter() * 1000.0 /
           (double)SDL_GetPerformanceFrequency();
}

/* ── Gates ────────────────────────────────────────────────────────────
 *
 * A gate opens for its owner's traffic and closes behind it. The legacy
 * brain tick runs this scan per gate-flagged unit (legacy:19003-19011 →
 * :15260-15361) and drives the COB Activate / Deactivate threads on the
 * activation edge (legacy:236399-236406). Scans are staggered; each one
 * only visits the spatial buckets covering the gate's own ring. */
#define GATE_SCAN_TICKS 20
/* A manual order has to outlive the door animation at both ends (the
 * script sleeps 3s closing and 3s opening) or the scan undoes it before
 * the player sees anything. Legacy re-asserts on the next brain tick;
 * holding the player's order is a deliberate deviation. */
#define GATE_HOLD_TICKS 600

static int gate_wants_open(const Unit *g, const UnitDef *gd, int self) {
    const GameWorld *w = World_Get();
    /* Footprint plus a one-tile border (legacy:15286-15330). */
    int tx0 = Occ_TileOf(g->world_x - gd->footprint_x * 8) - 1;
    int ty0 = Occ_TileOf(g->world_y - gd->footprint_z * 8) - 1;
    int tx1 = tx0 + gd->footprint_x + 1;
    int ty1 = ty0 + gd->footprint_z + 1;
    int c0x = (tx0 * TAK_OCC_TILE_PX) >> UGRID_SHIFT;
    int c1x = (((tx1 + 1) * TAK_OCC_TILE_PX) - 1) >> UGRID_SHIFT;
    int c0y = (ty0 * TAK_OCC_TILE_PX) >> UGRID_SHIFT;
    int c1y = (((ty1 + 1) * TAK_OCC_TILE_PX) - 1) >> UGRID_SHIFT;
    for (int cy = c0y; cy <= c1y; cy++) {
        for (int cx = c0x; cx <= c1x; cx++) {
            int c = (cy & UGRID_MASK) * UGRID_W + (cx & UGRID_MASK);
            for (int j = g_ugrid_head[c]; j >= 0; j = g_ugrid_next[j]) {
                const Unit *o = &g_units[j];
                if (j == self || o->alive != UNIT_ALIVE_ACTIVE) continue;
                if (o->player_id != g->player_id) continue;  /* legacy:15300 */
                const UnitDef *od = Units_GetDef(o->def_idx);
                if (!od || od->max_velocity <= 0.0f) continue; /* legacy:15301 */
                int otx = Occ_TileOf(o->world_x);
                int oty = Occ_TileOf(o->world_y);
                if (otx < tx0 || otx > tx1 || oty < ty0 || oty > ty1) continue;
                /* Moving traffic opens it (legacy:15302); anyone parked
                 * on a gate cell holds it open so it never closes on top
                 * of them (legacy:15307-15323). */
                if (o->velocity != 0) return 1;
                if (Occ_IsGateTile(w, otx, oty)) return 1;
            }
        }
    }
    return 0;
}

static void gate_issue_order(Unit *u, int open) {
    /* The ACTIVATE / DEACTIVATE order flips the activation bit, and the
     * edge is what runs the COB thread (legacy:236399-236406). */
    if (u->cob_activation == (uint8_t)(open ? 1 : 0)) return;
    u->cob_activation = (uint8_t)(open ? 1 : 0);
    unit_start_script(u, open ? "Activate" : "Deactivate", NULL, 0);
}

/* Per-tick occupancy pass: keep every mobile's footprint stamp current,
 * retry a yielded structure imprint, then run the gate scan. The mobile
 * half early-outs on the tile origin, so an idle army costs a compare
 * per unit. */
static void tick_occupancy(void) {
    GameWorld *w = World_Get();
    if (!w) return;
    for (int i = 0; i < g_unit_count; i++) {
        Unit *u = &g_units[i];
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!unit_def_is_structure(d)) { occ_sync_mobile(i); continue; }
        if (u->alive != UNIT_ALIVE_ACTIVE) continue;
        if (u->gate_hold > 0) u->gate_hold--;
        if (u->gate_scan_cd > 0) { u->gate_scan_cd--; continue; }
        u->gate_scan_cd = (int16_t)(GATE_SCAN_TICKS + (i & 7));
        /* A yielded cell is claimed as soon as its occupant leaves
         * (legacy re-dirties the unit and re-imprints, :217924). */
        if (!u->occ_on || u->occ_pending) occ_refresh(i);
        if (!d->is_gate || u->under_construction || u->gate_hold > 0) continue;
        gate_issue_order(u, gate_wants_open(u, d, i));
    }
}

int Units_GateState(int handle) {
    if (handle < 0 || handle >= g_unit_count) return -1;
    const Unit *u = &g_units[handle];
    if (u->alive != UNIT_ALIVE_ACTIVE) return -1;
    const UnitDef *d = Units_GetDef(u->def_idx);
    /* The order buttons exist for onoffable defs (legacy:150430-150436);
     * this API is scoped to gates. */
    if (!d || !d->is_gate || !d->onoffable) return -1;
    return u->cob_activation ? 1 : 0;
}

void Units_SetGateOpen(int handle, int open) {
    if (Units_GateState(handle) < 0) return;
    Unit *u = &g_units[handle];
    gate_issue_order(u, open);
    /* Legacy re-evaluates on the next brain tick, which would undo a
     * player's order immediately. Hold it long enough to be visible. */
    u->gate_hold = GATE_HOLD_TICKS;
}

int Units_SelectedGateState(void) {
    for (int s = 0; s < g_selection_count; s++) {
        int st = Units_GateState(g_selection[s]);
        if (st >= 0) return st;
    }
    return -1;
}

void Units_ToggleSelectedGate(void) {
    for (int s = 0; s < g_selection_count; s++) {
        int st = Units_GateState(g_selection[s]);
        if (st < 0) continue;
        Units_SetGateOpen(g_selection[s], !st);
    }
}

void Units_TickEngines(void) {
    /* Sprint 1: per-tick simulation step.
     *   1. Combat: auto-target + damage + movement.
     *   2. Projectiles: advance + hit-test.
     *   3. COB: animator + script threads (per unit). */
    ugrid_rebuild();
    /* Plans per tick. A* is a byte-array walk now (passability bitmap),
     * so this is cheap; keep it generous enough that a large squad
     * order does not queue for long. */
    g_path_budget_this_tick = 16;
    double e0 = eng_now_ms();
    Units_TickCombat();
    double e1 = eng_now_ms();
    tick_projectiles();
    double e2 = eng_now_ms();
    g_eng_prof_ms[0] += e1 - e0;
    g_eng_prof_ms[1] += e2 - e1;
    tick_nanoframe_decay();
    tick_occupancy();
    double e3 = eng_now_ms();

    for (int i = 0; i < g_unit_count; i++) {
        Unit *u = &g_units[i];
        if ((u->alive != UNIT_ALIVE_ACTIVE && u->alive != UNIT_ALIVE_DYING) || !u->cob) continue;
        /* activatewhenbuilt: the moment the unit is complete, legacy
         * raises the activation state bit, and that rising edge runs
         * Activate once (legacy:236402). Lodestones raise their crystal
         * here. The latch is the engine's own invocation count, NOT the
         * ACTIVATION port value, which scripts write themselves. Using
         * the port meant a script that cleared it got Activate restarted
         * every tick. Covers direct spawns and construction alike. */
        if (!u->under_construction && u->alive == UNIT_ALIVE_ACTIVE &&
            u->script_ev[UNIT_SCRIPT_EV_ACTIVATE] == 0) {
            const UnitDef *adef = Units_GetDef(u->def_idx);
            if (adef && adef->activate_when_built) {
                u->cob_activation = 1;
                unit_start_script(u, "Activate", NULL, 0);
            }
        }
        Cob_AnimatePieces(u->cob);
        Cob_RunAllThreads(u->cob);
        if (u->alive == UNIT_ALIVE_DYING && Cob_AliveThreadCount(u->cob) == 0) {
            /* Killed sequence completed — despawn. */
            Cob_EngineFree(u->cob);
            tak_free(u->cob);
            u->cob = NULL;
            u->alive = UNIT_ALIVE_DEAD;
            fprintf(stderr, "Units_TickEngines: unit %d despawned (Killed done)\n", i);
        }
    }
    double e4 = eng_now_ms();
    g_eng_prof_ms[2] += e4 - e3;
    g_eng_prof_ms[3] += e3 - e2;
}

void Units_DebugRotateAll(float delta_rad) {
    for (int i = 0; i < g_unit_count; i++) {
        if (g_units[i].alive == UNIT_ALIVE_ACTIVE) g_units[i].heading += delta_rad;
    }
}

void Units_DropAllMeshCaches(void) {
    proj_model_drop_meshes();
    if (!g_defs) return;
    for (int i = 0; i < g_def_count; i++) {
        for (int c = 0; c < 12; c++) {
            if (g_defs[i].mesh_per_color[c]) {
                Mesh_Free(g_defs[i].mesh_per_color[c]);
                g_defs[i].mesh_per_color[c] = NULL;
            }
        }
    }
}

int Units_BakeMonarchMeshes(void) {
    /* Pre-bake each monarch with its faction-default color. Other
     * colors will lazy-bake on first spawn. */
    static const struct { const char *name; int color_idx; } monarchs[] = {
        { "ARAKING",  0 },   /* Blue (Aramon)  */
        { "TARNECRO", 1 },   /* Red  (Taros)   */
        { "VERMAGE",  2 },   /* Green (Veruna) */
        { "ZONHUNT",  3 },   /* Yellow (Zhon)  */
    };
    int baked = 0;
    for (size_t i = 0; i < sizeof(monarchs)/sizeof(monarchs[0]); i++) {
        int idx = Units_FindDefByName(monarchs[i].name);
        if (idx < 0) continue;
        if (ensure_mesh_baked(&g_defs[idx], monarchs[i].color_idx) == 0) baked++;
    }
    fprintf(stderr, "Units_BakeMonarchMeshes: %d/4 monarch meshes pre-baked\n", baked);
    return baked;
}

int Units_DebugSpawnGrid(int n, const char *side,
                         int player_id, int color_idx,
                         int32_t cx, int32_t cy, int pitch)
{
    if (n <= 0) return 0;
    int def_idx = Units_FindMonarchDef(side);
    if (def_idx < 0) {
        fprintf(stderr, "Units_DebugSpawnGrid: no monarch def for side %s\n",
                side ? side : "(null)");
        return 0;
    }
    if (color_idx < 0 || color_idx > 11) color_idx = 0;
    if (pitch < 16) pitch = 16;
    if (ensure_mesh_baked(&g_defs[def_idx], color_idx) != 0) {
        fprintf(stderr, "Units_DebugSpawnGrid: mesh bake failed\n");
        return 0;
    }

    Units_ClearInstances();

    /* Smallest square that fits N points. */
    int side_count = 1;
    while (side_count * side_count < n) side_count++;
    int half = side_count / 2;

    int spawned = 0;
    for (int row = 0; row < side_count && spawned < n; row++) {
        for (int col = 0; col < side_count && spawned < n; col++) {
            int32_t wx = cx + (int32_t)(col - half) * pitch;
            int32_t wy = cy + (int32_t)(row - half) * pitch;
            int h = Units_Spawn(def_idx, player_id, color_idx, wx, wy);
            if (h < 0) goto done;   /* active array full */
            spawned++;
        }
    }
done:
    fprintf(stderr, "Units_DebugSpawnGrid: %d/%d %s monarchs spawned (%dx%d grid, pitch=%d)\n",
            spawned, n, side, side_count, side_count, pitch);
    return spawned;
}

int Units_DebugSpawnMonarch(const char *side, int32_t world_x, int32_t world_y) {
    int def_idx = Units_FindMonarchDef(side);
    if (def_idx < 0) {
        fprintf(stderr, "Units_DebugSpawnMonarch: no monarch def for side %s\n",
                side ? side : "(null)");
        return -1;
    }
    /* Side-default team color for debug spawns: ARA=0 (Blue),
     * TAR=1 (Red), VER=2 (Green), ZON=3 (Yellow). Matches the
     * faction's "kingdom colors" from battle_setup. */
    int tc = 0;
    if (side) {
        if      (stricmp_bounded(side, "TAR") == 0) tc = 1;
        else if (stricmp_bounded(side, "VER") == 0) tc = 2;
        else if (stricmp_bounded(side, "ZON") == 0) tc = 3;
    }
    /* Bake the per-color variant for THIS player. */
    if (ensure_mesh_baked(&g_defs[def_idx], tc) != 0) {
        return -1;
    }
    int handle = Units_Spawn(def_idx, 1, tc, world_x, world_y);
    if (handle < 0) {
        fprintf(stderr, "Units_DebugSpawnMonarch: spawn failed (array full?)\n");
        return -1;
    }
    fprintf(stderr,
        "Units_DebugSpawnMonarch: %s at (%d, %d), tc=%d, handle=%d\n",
        g_defs[def_idx].unitname, world_x, world_y, tc, handle);
    return 0;
}

/* ── The 3D submit path (M5 textured + M7 heading/Y-sort/team) ──── *
 *
 * Per unit:
 *   1. Transform every mesh vertex once: model → world → screen.
 *      Heading rotation around Y happens in this step (rotates the
 *      mx/mz pair; my passes through unchanged because Y is the
 *      rotation axis).
 *   2. Per-vertex color: if tc_flag is set, output the unit's team
 *      color; otherwise the mesh's baked color (white for textured
 *      prims, palette lookup for FLAT_COLOR).
 *   3. For each batch in the unit's mesh, submit one draw call with
 *      the batch's atlas texture and the batch's slice of indices.
 *
 * Y-sort: units are drawn in ascending order of world_y (depth on
 * map). Painter's algorithm — a unit further down the screen draws
 * after (and on top of) a unit further up. Within one mesh, the
 * 3DO tree-walk order gives correct body-before-limb layering for
 * free, so we don't need depth sorting *inside* a unit. */

/* Draw order for the active array. Filled fresh each frame from
 * alive units, sorted ascending by world_y. Insertion sort is fine
 * because the typical N is tiny (M5: 1-4; M8 stress: 500). */
static int g_draw_order[TAK_MAX_UNITS];

static int build_draw_order(const struct GameWorld *world) {
    /* Frustum cull pass: only keep units whose footprint is inside the
     * viewport (expanded by a margin for the unit's projected size).
     *
     * The margin needs to swallow:
     *   - half the unit's world-space footprint width (~40 px at the
     *     typical TA_SCALE),
     *   - the model's height projected through the camera tilt
     *     (~world_height * tan_tilt ≈ 50 px for a monarch).
     *
     * 256 px is comfortably bigger than both, so a unit "just off
     * screen" won't pop in until it's safely off-camera. The cost of
     * being too generous is only that we transform a few extra
     * non-visible units; being too tight pops units in/out at the edge.
     *
     * Real-game payoff: a 16k×16k map with 250 deployed units typically
     * shows 30-100 in the viewport — this drops the work in Submit by
     * 2-8x without changing anything else. For the M8 stress grid
     * (everyone visible) it's a no-op. */
    const int32_t margin = 256;
    const int32_t left   = world->cam_x - margin;
    const int32_t right  = world->cam_x + world->viewport_w + margin;
    const int32_t top    = world->cam_y - margin;
    const int32_t bottom = world->cam_y + world->viewport_h + margin;

    int n = 0;
    for (int i = 0; i < g_unit_count; i++) {
        const Unit *u = &g_units[i];
        if (u->alive != UNIT_ALIVE_ACTIVE && u->alive != UNIT_ALIVE_DYING) continue;
        /* Legacy: nanoframes below 50% are NOT drawn at all (:197310);
         * only the construction sparklies mark the site. */
        if (u->under_construction && u->max_health > 0 &&
            u->health * 2 < u->max_health) continue;
        if (!unit_visible_to_local_player(world, u)) continue;
        if (u->world_x < left || u->world_x > right) continue;
        if (u->world_y < top  || u->world_y > bottom) continue;
        g_draw_order[n++] = i;
    }
    /* Insertion sort by world_y (smaller world_y → drawn first → at
     * the back). Stable: ties keep their spawn-order layering. */
    for (int i = 1; i < n; i++) {
        int key = g_draw_order[i];
        int32_t key_y = g_units[key].world_y;
        int j = i - 1;
        while (j >= 0 && g_units[g_draw_order[j]].world_y > key_y) {
            g_draw_order[j + 1] = g_draw_order[j];
            j--;
        }
        g_draw_order[j + 1] = key;
    }
    return n;
}

/* Compose per-node world transforms for a mesh, given uniform per-piece
 * state (Phase D M3 — extends to per-unit divergent state in M5).
 * Walks nodes in DFS order so parents always precede children. The
 * world transform is parent_xform × T(node.offset) × T(piece.pos) × R(piece.rot).
 * For all-identity piece state this collapses to pure translation by
 * cumulative parent offsets — recovering Phase C's flat-fold positions.
 *
 * pieces may be NULL, in which case all-identity is assumed. Handles
 * up to UNIT_MESH_MAX_NODES nodes; out must have that capacity. */
static void compose_node_xforms(const UnitMesh *m,
                                 const CobPiece *pieces,
                                 NodeXform *out)
{
    /* Fixed-point CobPiece angles are 1/65536-th of a "tau" angular
     * unit. For M3 with all zeros we never enter the rotation path
     * but keep the math available for M4+. */
    /* COB/3DO model frame is LEFT-handed (+x=model left, +y=up,
     * +z=back — LegUL/LegUR offsets, cape z in arasword.3do). lr[]
     * uses right-handed Euler formulas and the model->world map has
     * det = -1, so rotations conjugate as R(A·n, -θ): negate the
     * angle scale or every TURN plays backwards (BOS oracle table in
     * docs/digs). MOVE x and z flip as well, see the piece loop. */
    const float ANGLE_TO_RAD = -6.28318530718f / 65536.0f;
    /* COB linear values share the raw 3DO fixed-point space of the
     * mesh offsets (move [-0.6] compiles to -98304; Hip y=1894644):
     * scale 1, NOT 1/65536 (which made all move anims invisible). */
    const float COB_POS_TO_MODEL = 1.0f;

    for (int i = 0; i < m->node_count; i++) {
        const UnitMeshNode *n = &m->nodes[i];
        NodeXform *x = &out[i];
        x->hidden = 0;

        /* Local rotation from this piece's COB rot[3]. Y-axis rot is
         * the most common; X and Z are rarer. For M3 ship-it (all
         * zeros), this is identity. */
        float lrx = 0.0f, lry = 0.0f, lrz = 0.0f;
        float lpx = 0.0f, lpy = 0.0f, lpz = 0.0f;
        if (pieces) {
            lrx = pieces[i].rot[0] * ANGLE_TO_RAD;
            lry = pieces[i].rot[1] * ANGLE_TO_RAD;
            lrz = pieces[i].rot[2] * ANGLE_TO_RAD;
            /* The original turns every 3DO vertex and offset half a
             * turn about y at load (x and z negated,
             * legacy:270790-270815) and adds script positions to those
             * flipped offsets raw (legacy:198927-198930). Our mesh keeps
             * the authored frame, so a MOVE lands at (-x, y, -z). */
            lpx = -pieces[i].pos[0] * COB_POS_TO_MODEL;
            lpy =  pieces[i].pos[1] * COB_POS_TO_MODEL;
            lpz = -pieces[i].pos[2] * COB_POS_TO_MODEL;
            if (pieces[i].hidden) x->hidden = 1;
        } else {
            /* No COB engine bound (e.g. placement-ghost preview). Apply
             * the same naming convention Cob_EngineInit uses so the
             * `*_off` / `*_dead` alternate-state pieces are hidden by
             * default. Otherwise the ghost preview shows the on-state
             * + off-state meshes overlapped — same upside-down look
             * we just fixed for live-spawned units. */
            const char *nm = n->name;
            if (nm) {
                size_t L = 0; while (nm[L]) L++;
                if (L >= 4) {
                    const char *t4 = nm + L - 4;
                    if ((t4[0] == '_' || t4[0] == '-') &&
                        (t4[1] == 'o' || t4[1] == 'O') &&
                        (t4[2] == 'f' || t4[2] == 'F') &&
                        (t4[3] == 'f' || t4[3] == 'F')) x->hidden = 1;
                }
                if (!x->hidden && L >= 5) {
                    const char *t5 = nm + L - 5;
                    if ((t5[0] == '_' || t5[0] == '-') &&
                        (t5[1] == 'd' || t5[1] == 'D') &&
                        (t5[2] == 'e' || t5[2] == 'E') &&
                        (t5[3] == 'a' || t5[3] == 'A') &&
                        (t5[4] == 'd' || t5[4] == 'D')) x->hidden = 1;
                }
            }
        }
        const float cx = cosf(lrx), sx = sinf(lrx);
        const float cy = cosf(lry), sy = sinf(lry);
        const float cz = cosf(lrz), sz = sinf(lrz);

        /* Local rot matrix R = Ry x Rx x Rz: the original's builder
         * turns a piece about z first, then x, then y
         * (legacy:364077-364160). Identity for all angles zero. */
        float lr[9];
        lr[0] = cy*cz + sy*sx*sz;  lr[1] = sy*sx*cz - cy*sz;  lr[2] = sy*cx;
        lr[3] = cx*sz;             lr[4] = cx*cz;             lr[5] = -sx;
        lr[6] = cy*sx*sz - sy*cz;  lr[7] = sy*sz + cy*sx*cz;  lr[8] = cy*cx;

        /* Local translation = node static offset + piece anim translation. */
        const float lt0 = n->offset[0] + lpx;
        const float lt1 = n->offset[1] + lpy;
        const float lt2 = n->offset[2] + lpz;

        if (n->parent < 0) {
            /* Root: world = local. */
            for (int k = 0; k < 9; k++) x->rot[k] = lr[k];
            x->trans[0] = lt0;
            x->trans[1] = lt1;
            x->trans[2] = lt2;
        } else {
            /* Compose: world = parent.world × local.
             *   x.rot   = p.rot × lr
             *   x.trans = p.trans + p.rot × (lt) */
            const NodeXform *p = &out[n->parent];
            x->rot[0] = p->rot[0]*lr[0] + p->rot[1]*lr[3] + p->rot[2]*lr[6];
            x->rot[1] = p->rot[0]*lr[1] + p->rot[1]*lr[4] + p->rot[2]*lr[7];
            x->rot[2] = p->rot[0]*lr[2] + p->rot[1]*lr[5] + p->rot[2]*lr[8];
            x->rot[3] = p->rot[3]*lr[0] + p->rot[4]*lr[3] + p->rot[5]*lr[6];
            x->rot[4] = p->rot[3]*lr[1] + p->rot[4]*lr[4] + p->rot[5]*lr[7];
            x->rot[5] = p->rot[3]*lr[2] + p->rot[4]*lr[5] + p->rot[5]*lr[8];
            x->rot[6] = p->rot[6]*lr[0] + p->rot[7]*lr[3] + p->rot[8]*lr[6];
            x->rot[7] = p->rot[6]*lr[1] + p->rot[7]*lr[4] + p->rot[8]*lr[7];
            x->rot[8] = p->rot[6]*lr[2] + p->rot[7]*lr[5] + p->rot[8]*lr[8];
            x->trans[0] = p->trans[0] + p->rot[0]*lt0 + p->rot[1]*lt1 + p->rot[2]*lt2;
            x->trans[1] = p->trans[1] + p->rot[3]*lt0 + p->rot[4]*lt1 + p->rot[5]*lt2;
            x->trans[2] = p->trans[2] + p->rot[6]*lt0 + p->rot[7]*lt1 + p->rot[8]*lt2;
            /* Hiding a piece hides that piece alone. The original's
             * HIDE calls a per piece visibility hook (legacy:306415)
             * and the shipped models lean on that: a stronghold's
             * veteran arm ArmLR5 is a child of ArmLR, and its
             * StatusControl hides ArmLR while showing ArmLR5. Same
             * for ArmLL5, Wheel5 and Cannon10. Passing the flag down
             * the tree left the crew and the gun with nothing to draw
             * the moment a tower earned a veteran level, which is why
             * the people in strongholds and towers vanished. */
        }
    }
}

/* Factory build pad: run the yard's QueryBuildInfo script, take the
 * piece index it writes to its out-arg and turn that piece into a world
 * spot (legacy:9347-9362). Returns 0 when there is no usable pad (no
 * engine, no script, no mesh, unbound piece) and the caller falls back
 * to the yard centre, exactly as a piece of -1 does in legacy.
 *
 * Running the script is part of the contract, not a side effect: a
 * stateful QueryBuildInfo (the Veruna yard toggles a static and turns
 * its dock piece) advances one step per production attempt. */
static int unit_factory_build_spot(Unit *f, int32_t *out_x, int32_t *out_y) {
    if (!f || !f->cob || !f->cob->script || !out_x || !out_y) return 0;
    const UnitDef *fd = Units_GetDef(f->def_idx);
    int c = f->team_color_idx;
    if (c < 0 || c > 11) c = 0;
    const UnitMesh *m = fd ? fd->mesh_per_color[c] : NULL;
    if (!m || m->node_count <= 0) return 0;
    /* Four args, out-arg seeded to -1 so a script that never writes it
     * leaves no pad at all (legacy:9345-9350). */
    int32_t qa[4] = { -1, 0, 0, 0 };
    if (Cob_RunScriptSync(f->cob, "QueryBuildInfo", qa, 4) != 0) return 0;
    if (qa[0] < 0 || qa[0] >= (int32_t)f->cob->script->num_pieces) return 0;
    int node = f->cob->piece_to_node ? f->cob->piece_to_node[qa[0]] : -1;
    if (node < 0 || node >= m->node_count) return 0;

    NodeXform *xf = (NodeXform *)tak_malloc(sizeof(NodeXform) *
                                            (size_t)m->node_count);
    if (!xf) return 0;
    compose_node_xforms(m, f->cob->pieces, xf);
    float mx = xf[node].trans[0], mz = xf[node].trans[2];
    tak_free(xf);

    /* Model frame -> world, the same heading rotation and handedness
     * mirror submit_run applies to vertices. Legacy composes the same
     * chain and negates z before adding the unit position
     * (legacy:185790-185859). */
    float ch = cosf(f->heading), sh = sinf(f->heading);
    float rx = -(ch * mx + sh * mz);
    float rz = -(sh * mx - ch * mz);
    *out_x = f->world_x + (int32_t)lroundf(rx * UNIT_MODEL_TO_WORLD);
    *out_y = f->world_y + (int32_t)lroundf(rz * UNIT_MODEL_TO_WORLD);
    return 1;
}

int Units_DebugPieceWorldHeading(int handle, const char *piece_name,
                                 float *out_heading) {
    if (handle < 0 || handle >= g_unit_count || !piece_name || !out_heading)
        return 0;
    Unit *u = &g_units[handle];
    const UnitDef *d = Units_GetDef(u->def_idx);
    const UnitMesh *m = d ? d->mesh_per_color[u->team_color_idx] : NULL;
    if (!m || !u->cob) return 0;
    int node = -1;
    for (int n = 0; n < m->node_count; n++) {
        if (stricmp_bounded(m->nodes[n].name, piece_name) == 0) { node = n; break; }
    }
    if (node < 0) return 0;

    NodeXform *xf = (NodeXform *)tak_malloc(sizeof(NodeXform) *
                                            (size_t)m->node_count);
    if (!xf) return 0;
    compose_node_xforms(m, u->cob->pieces, xf);
    /* Authored forward is -z in the model frame (+z = back). */
    float mx = -xf[node].rot[2];
    float mz = -xf[node].rot[8];
    tak_free(xf);
    /* Same model->world map the submit path applies to vertices. */
    float ch = cosf(u->heading), sh = sinf(u->heading);
    float wx = -(ch * mx + sh * mz);
    float wy = -(sh * mx - ch * mz);
    if (wx == 0.0f && wy == 0.0f) return 0;
    *out_heading = atan2f(wx, -wy);
    return 1;
}

static int debug_find_node(const UnitMesh *m, const char *piece_name) {
    for (int n = 0; n < m->node_count; n++) {
        if (stricmp_bounded(m->nodes[n].name, piece_name) == 0) return n;
    }
    return -1;
}

int Units_DebugPieceWorldOffset(int handle, const char *piece_name,
                                float *out_origin, float *out_centroid) {
    if (handle < 0 || handle >= g_unit_count || !piece_name ||
        !out_origin || !out_centroid) return 0;
    Unit *u = &g_units[handle];
    const UnitDef *d = Units_GetDef(u->def_idx);
    const UnitMesh *m = d ? d->mesh_per_color[u->team_color_idx] : NULL;
    if (!m || !u->cob) return 0;
    int node = debug_find_node(m, piece_name);
    if (node < 0) return 0;

    NodeXform *xf = (NodeXform *)tak_malloc(sizeof(NodeXform) *
                                            (size_t)m->node_count);
    if (!xf) return 0;
    compose_node_xforms(m, u->cob->pieces, xf);
    const NodeXform *x = &xf[node];
    float o[3] = { x->trans[0], x->trans[1], x->trans[2] };
    float c[3] = { 0.0f, 0.0f, 0.0f };
    int nv = 0;
    for (int v = 0; v < m->vert_count; v++) {
        if (m->vert_node_idx[v] != node) continue;
        const float lx = m->positions[3 * v + 0];
        const float ly = m->positions[3 * v + 1];
        const float lz = m->positions[3 * v + 2];
        c[0] += x->rot[0]*lx + x->rot[1]*ly + x->rot[2]*lz + x->trans[0];
        c[1] += x->rot[3]*lx + x->rot[4]*ly + x->rot[5]*lz + x->trans[1];
        c[2] += x->rot[6]*lx + x->rot[7]*ly + x->rot[8]*lz + x->trans[2];
        nv++;
    }
    tak_free(xf);
    if (nv > 0) {
        c[0] /= (float)nv; c[1] /= (float)nv; c[2] /= (float)nv;
    } else {
        c[0] = o[0]; c[1] = o[1]; c[2] = o[2];
    }
    /* Same model->world map the submit path applies to vertices. */
    const float ch = cosf(u->heading), sh = sinf(u->heading);
    out_origin[0]   = -(ch * o[0] + sh * o[2]) * UNIT_MODEL_TO_WORLD;
    out_origin[1]   = o[1] * UNIT_MODEL_TO_WORLD;
    out_origin[2]   = -(sh * o[0] - ch * o[2]) * UNIT_MODEL_TO_WORLD;
    out_centroid[0] = -(ch * c[0] + sh * c[2]) * UNIT_MODEL_TO_WORLD;
    out_centroid[1] = c[1] * UNIT_MODEL_TO_WORLD;
    out_centroid[2] = -(sh * c[0] - ch * c[2]) * UNIT_MODEL_TO_WORLD;
    return 1;
}

int Units_DebugSetPieceRot(int handle, const char *piece_name,
                           int32_t rx, int32_t ry, int32_t rz) {
    if (handle < 0 || handle >= g_unit_count || !piece_name) return 0;
    Unit *u = &g_units[handle];
    const UnitDef *d = Units_GetDef(u->def_idx);
    const UnitMesh *m = d ? d->mesh_per_color[u->team_color_idx] : NULL;
    if (!m || !u->cob) return 0;
    int node = debug_find_node(m, piece_name);
    if (node < 0) return 0;
    u->cob->pieces[node].rot[0] = rx;
    u->cob->pieces[node].rot[1] = ry;
    u->cob->pieces[node].rot[2] = rz;
    return 1;
}

int Units_FactoryBuildSpot(int factory_handle,
                           int32_t *out_x, int32_t *out_y) {
    if (factory_handle < 0 || factory_handle >= g_unit_count) return 0;
    Unit *f = &g_units[factory_handle];
    if (f->alive != 1) return 0;
    return unit_factory_build_spot(f, out_x, out_y);
}

/* Transform one unit's mesh into the scratch buffers at vertex offset
 * `v_off`: per-piece COB transform, heading rotation, projection, plus
 * the per-vertex height key the draw order needs. */
static void transform_unit_verts(const UnitMesh *m, const struct GameWorld *world,
                                 const Unit *u, float y_scale, int v_off)
{
    const float cam_x = (float)world->cam_x;
    const float cam_y = (float)world->cam_y;
    const float ta    = g_ta_scale;
    const float tilt  = g_tan_tilt;
    const float ux = (float)u->world_x;
    const float uz = (float)u->world_y;
    const float uh = (float)Terrain_SampleHeight(world, u->world_x, u->world_y)
                   + u->flight_alt;   /* airborne units draw at their height */
    const float ch = cosf(u->heading);
    const float sh = sinf(u->heading);
    const int   V  = m->vert_count;

    /* Construction fade: while under_construction, the building starts
     * barely visible and becomes opaque as its HP fills toward max.
     * Legacy ramp: alpha 0 to 255 over the 50%..100% span
     * (legacy:197310-197322); sub-50% is culled upstream. */
    uint32_t alpha_mul = 255;
    if (u->under_construction && u->max_health > 0) {
        float t = (float)u->health / (float)u->max_health;
        float a = (t - 0.5f) * 2.0f;
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        alpha_mul = (uint32_t)(a * 255.0f);
    }

    /* Per-piece transforms for THIS unit. Animation gives each unit its
     * own piece state, so this is recomputed per unit. */
    compose_node_xforms(m, u->cob ? u->cob->pieces : NULL, g_scratch_node_xform);

    for (int v = 0; v < V; v++) {
        const NodeXform *x = &g_scratch_node_xform[m->vert_node_idx[v]];
        const int i = v_off + v;
        /* A piece HIDE'd by a COB script (verlode's Create() hides
         * VerLode_off) collapses onto one point, so its triangles have
         * zero area and the rasteriser skips them. */
        if (x->hidden) {
            g_scratch_xy[2 * i + 0] = 0.0f;
            g_scratch_xy[2 * i + 1] = 0.0f;
            g_scratch_wz[i]         = 0.0f;
            g_scratch_hkey[i]       = 0.0f;
            g_scratch_color[i]      = 0;
            g_scratch_uv[2 * i + 0] = 0.0f;
            g_scratch_uv[2 * i + 1] = 0.0f;
            continue;
        }
        const float lx = m->positions[3 * v + 0];
        const float ly = m->positions[3 * v + 1];
        const float lz = m->positions[3 * v + 2];
        const float mx = x->rot[0]*lx + x->rot[1]*ly + x->rot[2]*lz + x->trans[0];
        const float my = (x->rot[3]*lx + x->rot[4]*ly + x->rot[5]*lz + x->trans[1]) * y_scale;
        const float mz = x->rot[6]*lx + x->rot[7]*ly + x->rot[8]*lz + x->trans[2];

        /* Heading rotation around Y + the top-down handedness mirror.
         * The legacy projector carries exactly one mirror (it negates z
         * building screen verts, legacy:197681). Without it every
         * triangle's screen winding flips and the legacy backface rule
         * (cross >= 0 draws, legacy:197804) keeps the wrong face set.
         * 3DO models are authored front toward -z, so the map carries
         * an extra 180 degree yaw: forward lands on (sin h, -cos h),
         * matching walk_tick, and units face their motion. */
        const float rx = -(ch * mx + sh * mz);
        const float rz = -(sh * mx - ch * mz);
        const float wx = ux + rx * ta;
        const float wy =      my * ta + uh;
        const float wz = uz + rz * ta;
        g_scratch_xy[2 * i + 0] = wx - cam_x;
        g_scratch_xy[2 * i + 1] = wz - cam_y - wy * tilt;
        g_scratch_wz[i]         = wz;
        g_scratch_hkey[i]       = model_height_key(my);
        /* Colour is RGBA8888 little-endian (A in the high byte). */
        uint32_t c = m->colors[v];
        if (alpha_mul != 255) {
            uint32_t a = (c >> 24) & 0xFFu;
            a = (a * alpha_mul) / 255u;
            c = (c & 0x00FFFFFFu) | (a << 24);
        }
        g_scratch_color[i] = c;
        g_scratch_uv[2 * i + 0] = m->uvs[2 * v + 0];
        g_scratch_uv[2 * i + 1] = m->uvs[2 * v + 1];
    }
}

/* Build the per-triangle sort list for ONE transformed unit sitting at
 * vertex offset `v_off` in the scratch buffers. Walks every batch, drops
 * back-facing tris by the legacy rule (cross >= 0 draws, legacy:197804),
 * and stamps each survivor with its height key and authored order.
 * Returns the number kept. The caller sorts and emits. */
static int build_unit_tri_list(const UnitMesh *m, int v_off) {
    int kept = 0;
    for (int b = 0; b < m->batch_count; b++) {
        const UnitMeshBatch *batch = &m->batches[b];
        const int n_tri = batch->index_count / 3;
        const int tri0  = batch->first_index / 3;
        const uint16_t *src_idx = m->indices + batch->first_index;
        for (int t = 0; t < n_tri; t++) {
            const int i0 = v_off + src_idx[t * 3 + 0];
            const int i1 = v_off + src_idx[t * 3 + 1];
            const int i2 = v_off + src_idx[t * 3 + 2];
            if (g_backface_cull_on) {
                const float sx0 = g_scratch_xy[2*i0+0], sy0 = g_scratch_xy[2*i0+1];
                const float sx1 = g_scratch_xy[2*i1+0], sy1 = g_scratch_xy[2*i1+1];
                const float sx2 = g_scratch_xy[2*i2+0], sy2 = g_scratch_xy[2*i2+1];
                const float cross = (sy0 - sy1) * (sx2 - sx1)
                                  - (sy2 - sy1) * (sx0 - sx1);
                if (g_backface_cull_invert) {
                    if (cross > 0.0f) continue;
                } else {
                    if (cross < 0.0f) continue;
                }
            }
            g_scratch_tri[kept].i0    = (uint16_t)i0;
            g_scratch_tri[kept].i1    = (uint16_t)i1;
            g_scratch_tri[kept].i2    = (uint16_t)i2;
            g_scratch_tri[kept].batch = (uint16_t)b;
            g_scratch_tri[kept].hkey  = tri_height_key(g_scratch_hkey, i0, i1, i2);
            g_scratch_tri[kept].key   =
                (float)(m->tri_seq ? m->tri_seq[tri0 + t] : (uint32_t)(tri0 + t));
            kept++;
        }
    }
    return kept;
}

/* Submit a coalesced run: a contiguous slice of g_draw_order in which
 * every unit shares the same (def_idx, color_idx) and therefore the
 * same baked UnitMesh. Builds a merged vertex buffer for the whole run
 * and emits ONE draw call per atlas batch — instead of one per unit
 * per batch as the naive path would. For a 2000-monarch stress test
 * this drops from ~12k draw calls to ~6 chunked × ~6 batches = ~36.
 *
 * uint16 indices cap each draw call at 65535 verts, so we sub-chunk
 * the run if N × vert_count exceeds the budget.
 *
 * Per-tri qsort is dropped in the merged path: across thousands of
 * tris from many units it becomes a real cost, and the ordering
 * mostly matters within one unit's mesh — which the bake's natural
 * primitive order already preserves. Per-batch z-order is computed
 * from the first unit's transformed verts (cheap, mostly correct;
 * occasional wrong layering on differently-oriented capes is
 * imperceptible at gameplay scale). */
static void submit_run(TAK_Platform *plat, const struct GameWorld *world,
                       uint16_t def_idx, uint8_t color_idx,
                       const int *unit_indices, int n_units)
{
    if (n_units <= 0) return;
    const UnitDef *def = Units_GetDef(def_idx);
    if (!def) return;
    if (color_idx > 11) color_idx = 0;
    const UnitMesh *m = def->mesh_per_color[color_idx];
    if (!m || m->vert_count == 0) return;

    const int V = m->vert_count;
    int max_units_per_chunk = 65000 / V;
    if (max_units_per_chunk < 1) max_units_per_chunk = 1;

    const float cam_x = (float)world->cam_x;
    const float cam_y = (float)world->cam_y;
    const float ta    = g_ta_scale;
    const float tilt  = g_tan_tilt;
    const float y_scale = render_y_scale_for_def(def);

    for (int chunk_start = 0; chunk_start < n_units; chunk_start += max_units_per_chunk) {
        int chunk_n = n_units - chunk_start;
        if (chunk_n > max_units_per_chunk) chunk_n = max_units_per_chunk;
        const int total_verts   = chunk_n * V;
        const int max_total_idx = chunk_n * m->tri_count * 3;

        if (ensure_scratch(total_verts, max_total_idx) != 0) continue;

        /* ── Transform every unit's verts into the merged buffer. ── */
        for (int ci = 0; ci < chunk_n; ci++) {
            transform_unit_verts(m, world, &g_units[unit_indices[chunk_start + ci]],
                                 y_scale, ci * V);
        }

        /* ── Per-batch z-order from the first unit's transformed verts. ── */
        float batch_centroid[UNIT_MESH_MAX_BATCHES];
        int   batch_order[UNIT_MESH_MAX_BATCHES];
        for (int b = 0; b < m->batch_count; b++) {
            batch_order[b] = b;
            const UnitMeshBatch *bb = &m->batches[b];
            if (bb->index_count == 0) { batch_centroid[b] = 0.0f; continue; }
            float sum = 0.0f;
            const uint16_t *idx = m->indices + bb->first_index;
            for (int k = 0; k < bb->index_count; k++) {
                sum += g_scratch_wz[idx[k]];   /* first unit at offset 0 */
            }
            batch_centroid[b] = sum / (float)bb->index_count;
        }
        for (int i = 1; i < m->batch_count; i++) {
            int   key_b = batch_order[i];
            float key_c = batch_centroid[key_b];
            int j = i - 1;
            while (j >= 0 && batch_centroid[batch_order[j]] > key_c) {
                batch_order[j + 1] = batch_order[j];
                j--;
            }
            batch_order[j + 1] = key_b;
        }

        /* ── Single unit: one ordered pass over every batch. ───────
         *
         * The legacy rasteriser walks nodes in reverse table order
         * (legacy:197658, :197944) with prims forward inside a node,
         * and lets a per-pixel height-key depth test decide who
         * survives where two pieces land on the same pixel
         * (legacy:265317). Our bake groups tris by texture batch, so
         * we rebuild one list across batches, order it by that key
         * with the authored order breaking ties, and emit contiguous
         * same-batch runs. Only for single-unit runs: the merged
         * stress-grid path keeps the cheap per-batch order. */
        if (chunk_n == 1) {
            int kept = build_unit_tri_list(m, 0);
            if (kept > 1) {
                qsort(g_scratch_tri, kept, sizeof(TriSort),
                      tri_cmp_far_first);
            }
            /* Emit contiguous same-batch runs as individual draws. */
            int s = 0;
            while (s < kept) {
                int e = s + 1;
                while (e < kept &&
                       g_scratch_tri[e].batch == g_scratch_tri[s].batch) e++;
                int w = 0;
                for (int t = s; t < e; t++) {
                    g_scratch_idx[w++] = g_scratch_tri[t].i0;
                    g_scratch_idx[w++] = g_scratch_tri[t].i1;
                    g_scratch_idx[w++] = g_scratch_tri[t].i2;
                }
                GPU_DrawGeometryRaw(plat,
                    m->batches[g_scratch_tri[s].batch].atlas_tex,
                    g_scratch_xy, g_scratch_color, g_scratch_uv,
                    total_verts,
                    g_scratch_idx, w);
                s = e;
            }
            continue;   /* next chunk */
        }

        /* ── For each batch, merge across all units, submit one draw.
         *
         * Per-unit tri sort by depth (within this batch) — needed so
         * pieces that animate (cape, tail) keep correct overdraw order
         * relative to the rest of the body. Cross-unit ordering comes
         * from the prior Y-sort of g_draw_order; we don't sort across
         * units in a chunk, only within each unit's tri block. */
        for (int bo = 0; bo < m->batch_count; bo++) {
            int b = batch_order[bo];
            const UnitMeshBatch *batch = &m->batches[b];
            if (batch->index_count == 0) continue;
            const int n_tri = batch->index_count / 3;
            const uint16_t *src_idx = m->indices + batch->first_index;

            int kept = 0;
            for (int ci = 0; ci < chunk_n; ci++) {
                const int v_off = ci * V;
                /* Build sort entries for this unit's surviving tris. */
                int unit_kept_start = kept;
                for (int t = 0; t < n_tri; t++) {
                    uint16_t i0 = src_idx[t * 3 + 0];
                    uint16_t i1 = src_idx[t * 3 + 1];
                    uint16_t i2 = src_idx[t * 3 + 2];

                    if (g_backface_cull_on) {
                        const float sx0 = g_scratch_xy[2*(v_off+i0)+0], sy0 = g_scratch_xy[2*(v_off+i0)+1];
                        const float sx1 = g_scratch_xy[2*(v_off+i1)+0], sy1 = g_scratch_xy[2*(v_off+i1)+1];
                        const float sx2 = g_scratch_xy[2*(v_off+i2)+0], sy2 = g_scratch_xy[2*(v_off+i2)+1];
                        const float cross = (sy0 - sy1) * (sx2 - sx1)
                                          - (sy2 - sy1) * (sx0 - sx1);
                        if (g_backface_cull_invert) {
                            if (cross > 0.0f) continue;
                        } else {
                            if (cross < 0.0f) continue;
                        }
                    }

                    g_scratch_tri[kept].i0  = (uint16_t)(v_off + i0);
                    g_scratch_tri[kept].i1  = (uint16_t)(v_off + i1);
                    g_scratch_tri[kept].i2  = (uint16_t)(v_off + i2);
                    /* Height key then authored order, same rule as the
                     * single-unit path (legacy:265317, :197658). */
                    g_scratch_tri[kept].hkey =
                        tri_height_key(g_scratch_hkey, v_off + i0,
                                       v_off + i1, v_off + i2);
                    g_scratch_tri[kept].key =
                        (float)(m->tri_seq
                                    ? m->tri_seq[batch->first_index / 3 + t]
                                    : (uint32_t)t);
                    kept++;
                }
                /* Sort just this unit's slice (back-to-front: smaller
                 * wz drawn first). N is small (~25 tris per unit per
                 * batch) so qsort is cheap. */
                int unit_kept_count = kept - unit_kept_start;
                if (unit_kept_count > 1) {
                    qsort(&g_scratch_tri[unit_kept_start], unit_kept_count,
                          sizeof(TriSort), tri_cmp_far_first);
                }
            }
            if (kept == 0) continue;

            /* Flatten sorted tri list back into the index stream. */
            for (int t = 0; t < kept; t++) {
                g_scratch_idx[t * 3 + 0] = g_scratch_tri[t].i0;
                g_scratch_idx[t * 3 + 1] = g_scratch_tri[t].i1;
                g_scratch_idx[t * 3 + 2] = g_scratch_tri[t].i2;
            }

            GPU_DrawGeometryRaw(plat, batch->atlas_tex,
                g_scratch_xy, g_scratch_color, g_scratch_uv,
                total_verts,
                g_scratch_idx, kept * 3);
        }
    }
}

/* Resolve one live unit to its baked mesh, then run the shared submit
 * transform into the scratch buffers. Shared by the two debug hooks
 * below so they measure exactly what the frame draws. */
static const UnitMesh *debug_transform_unit(int handle,
                                            const struct GameWorld *world)
{
    if (handle < 0 || handle >= g_unit_count || !world) return NULL;
    const Unit *u = &g_units[handle];
    if (u->alive != UNIT_ALIVE_ACTIVE && u->alive != UNIT_ALIVE_DYING) return NULL;
    const UnitDef *def = Units_GetDef(u->def_idx);
    if (!def) return NULL;
    int color_idx = u->team_color_idx;
    if (color_idx < 0 || color_idx > 11) color_idx = 0;
    const UnitMesh *m = def->mesh_per_color[color_idx];
    if (!m || m->vert_count == 0) return NULL;
    if (ensure_scratch(m->vert_count, m->tri_count * 3) != 0) return NULL;
    transform_unit_verts(m, world, u, render_y_scale_for_def(def), 0);
    return m;
}

/* Screen-space AABB of one live unit, through the submit transform.
 * Test hook: pins model proportions (legacy:197689, sx = x,
 * sy = -z - (y >> 1)). Returns 0 on success. */
int Units_DebugProjectedBounds(int handle, const struct GameWorld *world,
                               float out_min[2], float out_max[2])
{
    const UnitMesh *m = debug_transform_unit(handle, world);
    if (!m) return -1;

    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    int seen = 0;
    for (int v = 0; v < m->vert_count; v++) {
        if (g_scratch_node_xform[m->vert_node_idx[v]].hidden) continue;
        const float sx = g_scratch_xy[2 * v + 0];
        const float sy = g_scratch_xy[2 * v + 1];
        if (sx < min_x) min_x = sx;
        if (sx > max_x) max_x = sx;
        if (sy < min_y) min_y = sy;
        if (sy > max_y) max_y = sy;
        seen = 1;
    }
    if (!seen) return -1;
    if (out_min) { out_min[0] = min_x; out_min[1] = min_y; }
    if (out_max) { out_max[0] = max_x; out_max[1] = max_y; }
    return 0;
}

/* Submit order for one live unit: fills out_nodes with the owning node
 * of each triangle, and out_keys (optional) with its height key, in the
 * order the single-unit path hands them to the GPU. Test hook for piece
 * layering. Returns the triangle count. */
int Units_DebugSubmitOrder(int handle, const struct GameWorld *world,
                           uint16_t *out_nodes, float *out_keys, int max_tris)
{
    const UnitMesh *m = debug_transform_unit(handle, world);
    if (!m || !out_nodes || max_tris <= 0) return -1;

    int kept = build_unit_tri_list(m, 0);
    if (kept > 1) qsort(g_scratch_tri, kept, sizeof(TriSort), tri_cmp_far_first);
    if (kept > max_tris) kept = max_tris;
    for (int t = 0; t < kept; t++) {
        out_nodes[t] = m->vert_node_idx[g_scratch_tri[t].i0];
        if (out_keys) out_keys[t] = g_scratch_tri[t].hkey;
    }
    return kept;
}

/* Persistent "preview unit" CobEngine — one per Units_RenderBuildGhost
 * caller, re-initialised when the requested def changes. Mirrors the
 * legacy Render_DrawBuildPreview (the legacy reference ~227585) which
 * calls Unit_Create() on a real unit instance for the preview. We
 * give the ghost a real COB engine with Create() run, so the render
 * pipeline matches a live-spawned unit exactly (HIDE-PIECE, alternate-
 * piece toggles, all the side effects of Create() take effect). */
static CobEngine *g_ghost_cob = NULL;
static int        g_ghost_cob_def_idx = -1;
static int        g_ghost_cob_color   = -1;

/* The preview's Create runs against a host that answers the way the
 * finished building would at rest. The original builds its preview
 * as a real unit, so the script sees a real orientation: arakeep's
 * Create turns the build pad by 32768 minus ORIENTATION, which is
 * nothing for a building facing south and a half turn for a host
 * that answers zero. That half turn put the barracks' pad at the
 * back of the preview. */
static int32_t g_ghost_orientation = 0;   /* port 27, TA angle units */
static int32_t ghost_host_query_zero(void *user, int param) {
    (void)user; (void)param;
    return 0;
}
/* GET-UNIT-VALUE arrives through the call hook with the port as the
 * function id (legacy:306675), so the answers live here. */
static int32_t ghost_host_call(void *user, int fn_id,
                               int n_args, const int32_t *args) {
    (void)user; (void)n_args; (void)args;
    switch (fn_id) {
        case 4:  return 100;                  /* HEALTH, finished */
        case 27: return g_ghost_orientation;  /* ORIENTATION */
        default: return 0;
    }
}

/* Let Create's own motion finish: a piece told to move or turn at a
 * speed gets there over ticks, and the preview is never ticked once
 * it is drawn. The rest pose can also arrive late, from a script
 * Create starts that sleeps between its moves (a stronghold's crew
 * settles over a couple of seconds), so this always runs five
 * seconds of ticks, then carries on while anything still moves, up
 * to ten. Spinning pieces, the flags, never settle and do not
 * count. */
static void ghost_settle(CobEngine *e) {
    for (int t = 0; t < 600; t++) {
        Cob_RunAllThreads(e);
        Cob_AnimatePieces(e);
        int moving = 0;
        for (int i = 0; i < e->piece_count && !moving; i++) {
            const CobPiece *p = &e->pieces[i];
            for (int a = 0; a < 3; a++) {
                if (p->pos_speed[a] != 0) moving = 1;
                if (p->rot_speed[a] != 0 &&
                    p->rot_target[a] != COB_ROT_SPINNING) moving = 1;
            }
        }
        if (!moving && t >= 300) return;
    }
}

static void ghost_release_cob(void) {
    if (g_ghost_cob) {
        Cob_EngineFree(g_ghost_cob);
        tak_free(g_ghost_cob);
        g_ghost_cob = NULL;
    }
    g_ghost_cob_def_idx = -1;
    g_ghost_cob_color   = -1;
}

static CobEngine *ghost_ensure_cob(UnitDef *def, int def_idx, int color_idx,
                                    const UnitMesh *m) {
    /* Re-init when def or color changes (color affects which mesh's
     * node names we bind against — same texture-swap logic as live). */
    if (g_ghost_cob_def_idx == def_idx && g_ghost_cob_color == color_idx
        && g_ghost_cob) {
        return g_ghost_cob;
    }
    ghost_release_cob();
    if (!def->cob_script || !m || m->node_count <= 0) return NULL;
    g_ghost_cob = (CobEngine *)tak_malloc(sizeof(CobEngine));
    if (!g_ghost_cob) return NULL;
    const char *node_names[UNIT_MESH_MAX_NODES];
    int nc = m->node_count;
    if (nc > UNIT_MESH_MAX_NODES) nc = UNIT_MESH_MAX_NODES;
    for (int i = 0; i < nc; i++) node_names[i] = m->nodes[i].name;
    if (Cob_EngineInit(g_ghost_cob, def->cob_script, nc, node_names) != 0) {
        tak_free(g_ghost_cob);
        g_ghost_cob = NULL;
        return NULL;
    }
    /* Run Create() once with a fresh-unit host: every GET port reads 0
     * (inactive, not building, zero speed…). Without a host the VM's
     * fallback returns 1, which drives Create() down active-state
     * branches — lodestone previews then show the parked/flipped
     * alternate pieces ("upside-down" ghosts). */
    g_ghost_orientation =
        (int32_t)(build_heading_for_def(def) * 65536.0f / 6.2831853f);
    Cob_EngineSetHost(g_ghost_cob, NULL,
                      ghost_host_query_zero, ghost_host_call);
    Cob_StartThreadByName(g_ghost_cob, "Create", NULL, 0);
    Cob_RunAllThreads(g_ghost_cob);
    ghost_settle(g_ghost_cob);
    /* Create() and nothing else, which is exactly the pose a finished
     * building holds. Running Activate here froze factories mid-open,
     * so the preview showed a raised build pad the real building does
     * not have. */
    g_ghost_cob_def_idx = def_idx;
    g_ghost_cob_color   = color_idx;
    return g_ghost_cob;
}

static int mesh_node_by_name(const UnitMesh *m, const char *name) {
    for (int i = 0; m && i < m->node_count; i++) {
        if (tak_stricmp(m->nodes[i].name, name) == 0) return i;
    }
    return -1;
}

int Units_DebugGhostPieceState(int def_idx, int color_idx,
                               const char *piece_name,
                               int32_t out_rot[3], int32_t out_pos[3]) {
    UnitDef *def = (UnitDef *)Units_GetDef(def_idx);
    if (!def || !piece_name) return 0;
    if (color_idx < 0 || color_idx > 11) color_idx = 0;
    if (!def->mesh_per_color[color_idx] &&
        ensure_mesh_baked(def, color_idx) != 0) return 0;
    const UnitMesh *m = def->mesh_per_color[color_idx];
    CobEngine *g = ghost_ensure_cob(def, def_idx, color_idx, m);
    int node = mesh_node_by_name(m, piece_name);
    if (!g || node < 0 || node >= g->piece_count) return 0;
    for (int a = 0; a < 3; a++) {
        if (out_rot) out_rot[a] = g->pieces[node].rot[a];
        if (out_pos) out_pos[a] = g->pieces[node].pos[a];
    }
    return 1;
}

int Units_DebugPieceState(int handle, const char *piece_name,
                          int32_t out_rot[3], int32_t out_pos[3]) {
    if (handle < 0 || handle >= g_unit_count || !piece_name) return 0;
    const Unit *u = &g_units[handle];
    const UnitDef *def = Units_GetDef(u->def_idx);
    const UnitMesh *m = def ? def->mesh_per_color[u->team_color_idx] : NULL;
    int node = mesh_node_by_name(m, piece_name);
    if (!u->cob || node < 0 || node >= u->cob->piece_count) return 0;
    for (int a = 0; a < 3; a++) {
        if (out_rot) out_rot[a] = u->cob->pieces[node].rot[a];
        if (out_pos) out_pos[a] = u->cob->pieces[node].pos[a];
    }
    return 1;
}

int Units_DebugPieceCount(int handle) {
    if (handle < 0 || handle >= g_unit_count) return 0;
    const Unit *u = &g_units[handle];
    return u->cob ? u->cob->piece_count : 0;
}

int Units_DebugSnapshotPieces(int handle, int32_t *out_rot, int32_t *out_pos,
                              int cap) {
    if (handle < 0 || handle >= g_unit_count) return 0;
    const Unit *u = &g_units[handle];
    if (!u->cob) return 0;
    int n = u->cob->piece_count;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {
        for (int a = 0; a < 3; a++) {
            if (out_rot) out_rot[i * 3 + a] = u->cob->pieces[i].rot[a];
            if (out_pos) out_pos[i * 3 + a] = u->cob->pieces[i].pos[a];
        }
    }
    return n;
}

/* Does the placement preview of this unit's kind hold the pose the
 * unit itself holds? Compares every piece's visibility, and the
 * position and angle of every piece at rest. A piece the live unit
 * animates on its own, a waving flag, a turning gear, a fidgeting
 * crewman, is skipped: it is in motion now, or it has moved since the
 * earlier sample the caller passes in. Returns 1 for a match, 0 with
 * the first difference described, -1 when there is nothing to
 * compare. */
int Units_DebugGhostMatchesUnit(int handle,
                                const int32_t *rot_before,
                                const int32_t *pos_before,
                                char *why, size_t cap) {
    if (why && cap) why[0] = 0;
    if (handle < 0 || handle >= g_unit_count) return -1;
    const Unit *u = &g_units[handle];
    UnitDef *def = (UnitDef *)Units_GetDef(u->def_idx);
    const UnitMesh *m = def ? def->mesh_per_color[u->team_color_idx] : NULL;
    if (!def || !m || !u->cob) return -1;
    CobEngine *g = ghost_ensure_cob(def, (int)u->def_idx,
                                    u->team_color_idx, m);
    if (!g || g->piece_count != u->cob->piece_count) return -1;
    for (int i = 0; i < g->piece_count; i++) {
        const CobPiece *a = &u->cob->pieces[i];
        const CobPiece *b = &g->pieces[i];
        const char *nm = (i < m->node_count) ? m->nodes[i].name : "?";
        if (a->hidden != b->hidden) {
            if (why) snprintf(why, cap, "%s: %s hidden %d, preview %d",
                              def->unitname, nm, a->hidden, b->hidden);
            return 0;
        }
        for (int ax = 0; ax < 3; ax++) {
            int moved = 0;
            if (rot_before && rot_before[i * 3 + ax] != a->rot[ax]) moved = 1;
            if (pos_before && pos_before[i * 3 + ax] != a->pos[ax]) moved = 1;
            if (moved) continue;
            if (a->pos_speed[ax] == 0) {
                int32_t d = a->pos[ax] - b->pos[ax];
                if (d < 0) d = -d;
                if (d > 4096) {
                    if (why) snprintf(why, cap, "%s: %s pos[%d] %d, preview %d",
                                      def->unitname, nm, ax, a->pos[ax], b->pos[ax]);
                    return 0;
                }
            }
            if (a->rot_target[ax] == COB_ROT_SPINNING ||
                b->rot_target[ax] == COB_ROT_SPINNING) continue;
            if (a->rot_speed[ax] != 0) continue;
            int32_t d = (a->rot[ax] - b->rot[ax]) & 0xffff;
            if (d > 32768) d = 65536 - d;
            if (d > 1024) {
                if (why) snprintf(why, cap, "%s: %s rot[%d] %d, preview %d",
                                  def->unitname, nm, ax, a->rot[ax], b->rot[ax]);
                return 0;
            }
        }
    }
    return 1;
}

void Units_RenderBuildGhost(TAK_Platform *plat,
                              const struct GameWorld *world,
                              int def_idx, int color_idx,
                              int32_t world_x, int32_t world_y,
                              uint8_t alpha255, int valid) {
    if (!plat || !plat->renderer || !world) return;
    UnitDef *def = (UnitDef *)Units_GetDef(def_idx);
    if (!def) return;
    if (color_idx < 0 || color_idx > 11) color_idx = 0;
    if (!def->mesh_per_color[color_idx]) {
        if (ensure_mesh_baked(def, color_idx) != 0) return;
    }
    const UnitMesh *m = def->mesh_per_color[color_idx];
    if (!m || m->vert_count == 0) return;

    const int V = m->vert_count;
    if (ensure_scratch(V, m->tri_count * 3) != 0) return;

    /* Draw the preview on the cell the build will occupy, with the
     * terrain lift a live unit gets, so the ghost and the finished
     * building sit on exactly the same pixels (legacy:184168). */
    Units_SnapBuildSite(def_idx, &world_x, &world_y);
    const float cam_x = (float)world->cam_x;
    const float cam_y = (float)world->cam_y;
    const float ta    = g_ta_scale;
    const float tilt  = g_tan_tilt;
    const float ux = (float)world_x;
    const float uz = (float)world_y;
    const float uh = (float)Terrain_SampleHeight(world, world_x, world_y);
    const float heading = build_heading_for_def(def);
    const float ch = cosf(heading), sh = sinf(heading);
    const float y_scale = render_y_scale_for_def(def);

    /* Use the ghost's persistent COB engine — same way live units
     * have one. Without a real engine the per-piece state is missing
     * the side effects of Create() (HIDE-PIECE on alternate meshes,
     * TURN-PIECE rest-pose, etc.) and the mesh renders wrong. */
    CobEngine *gcob = ghost_ensure_cob(def, def_idx, color_idx, m);
    const CobPiece *gpieces = gcob ? gcob->pieces : NULL;
    compose_node_xforms(m, gpieces, g_scratch_node_xform);

    /* Per-vertex tint: green-ish if valid, red-ish if blocked, plus
     * the requested alpha. Multiply with each vertex's authored color
     * so team-color logos still read. */
    const uint32_t tint_r = valid ?  60 : 200;
    const uint32_t tint_g = valid ? 220 :  60;
    const uint32_t tint_b = valid ?  90 :  60;

    for (int v = 0; v < V; v++) {
        const uint16_t node = m->vert_node_idx[v];
        const NodeXform *x  = &g_scratch_node_xform[node];
        if (x->hidden) {
            g_scratch_xy[2 * v + 0] = 0.0f;
            g_scratch_xy[2 * v + 1] = 0.0f;
            g_scratch_wz[v]         = 0.0f;
            g_scratch_hkey[v]       = 0.0f;
            g_scratch_color[v]      = 0;
            g_scratch_uv[2 * v + 0] = 0.0f;
            g_scratch_uv[2 * v + 1] = 0.0f;
            continue;
        }
        const float lx = m->positions[3 * v + 0];
        const float ly = m->positions[3 * v + 1];
        const float lz = m->positions[3 * v + 2];
        const float mx = x->rot[0]*lx + x->rot[1]*ly + x->rot[2]*lz + x->trans[0];
        const float my = (x->rot[3]*lx + x->rot[4]*ly + x->rot[5]*lz + x->trans[1]) * y_scale;
        const float mz = x->rot[6]*lx + x->rot[7]*ly + x->rot[8]*lz + x->trans[2];
        /* Same heading rotation + handedness mirror + 180° model yaw
         * as the live-unit path (see submit_run). */
        const float rx = -(ch * mx + sh * mz);
        const float rz = -(sh * mx - ch * mz);
        const float wx = ux + rx * ta;
        const float wy = my * ta + uh;
        const float wz = uz + rz * ta;
        g_scratch_xy[2 * v + 0] = wx - cam_x;
        g_scratch_xy[2 * v + 1] = wz - cam_y - wy * tilt;
        g_scratch_wz[v]         = wz;
        g_scratch_hkey[v]       = model_height_key(my);
        /* Blend authored vertex colour with the tint and the ghost
         * alpha. RGB = mix(authored, tint, 0.5); A = authored.A * alpha255. */
        uint32_t c = m->colors[v];
        uint32_t cr = (c >>  0) & 0xFFu;
        uint32_t cg = (c >>  8) & 0xFFu;
        uint32_t cb = (c >> 16) & 0xFFu;
        uint32_t ca = (c >> 24) & 0xFFu;
        cr = (cr + tint_r) >> 1;
        cg = (cg + tint_g) >> 1;
        cb = (cb + tint_b) >> 1;
        ca = (ca * alpha255) / 255u;
        g_scratch_color[v] = cr | (cg << 8) | (cb << 16) | (ca << 24);
        g_scratch_uv[2 * v + 0] = m->uvs[2 * v + 0];
        g_scratch_uv[2 * v + 1] = m->uvs[2 * v + 1];
    }

    /* Submit one draw per atlas batch. Apply the SAME per-tri pipeline
     * the live submit_run uses: backface cull (drops back-facing tris)
     * + back-to-front depth sort (so the kept tris draw in correct
     * overdraw order). Without sort, two-sided billboard quads can
     * still look wrong because the kept-half of the first quad draws
     * over the kept-half of the second when they should be the other
     * way. Per-batch z-order also matters: draw the batch whose tris
     * are farthest from the camera first. */
    int   batch_order[UNIT_MESH_MAX_BATCHES];
    float batch_centroid[UNIT_MESH_MAX_BATCHES];
    for (int b = 0; b < m->batch_count; b++) {
        batch_order[b] = b;
        const UnitMeshBatch *bb = &m->batches[b];
        if (bb->index_count == 0) { batch_centroid[b] = 0.0f; continue; }
        float sum = 0.0f;
        const uint16_t *idx = m->indices + bb->first_index;
        for (int k = 0; k < bb->index_count; k++) sum += g_scratch_wz[idx[k]];
        batch_centroid[b] = sum / (float)bb->index_count;
    }
    /* Insertion sort batches by centroid Z (smaller wz drawn last/on top). */
    for (int i = 1; i < m->batch_count; i++) {
        int   key_b = batch_order[i];
        float key_c = batch_centroid[key_b];
        int j = i - 1;
        while (j >= 0 && batch_centroid[batch_order[j]] > key_c) {
            batch_order[j + 1] = batch_order[j];
            j--;
        }
        batch_order[j + 1] = key_b;
    }

    for (int bo = 0; bo < m->batch_count; bo++) {
        int b = batch_order[bo];
        const UnitMeshBatch *batch = &m->batches[b];
        if (batch->index_count == 0) continue;
        const int n_tri = batch->index_count / 3;
        const uint16_t *src_idx = m->indices + batch->first_index;
        int kept = 0;
        for (int t = 0; t < n_tri; t++) {
            uint16_t i0 = src_idx[t * 3 + 0];
            uint16_t i1 = src_idx[t * 3 + 1];
            uint16_t i2 = src_idx[t * 3 + 2];
            if (g_backface_cull_on) {
                const float sx0 = g_scratch_xy[2*i0+0], sy0 = g_scratch_xy[2*i0+1];
                const float sx1 = g_scratch_xy[2*i1+0], sy1 = g_scratch_xy[2*i1+1];
                const float sx2 = g_scratch_xy[2*i2+0], sy2 = g_scratch_xy[2*i2+1];
                const float cross = (sy0 - sy1) * (sx2 - sx1)
                                  - (sy2 - sy1) * (sx0 - sx1);
                if (g_backface_cull_invert) {
                    if (cross > 0.0f) continue;
                } else {
                    if (cross < 0.0f) continue;
                }
            }
            g_scratch_tri[kept].i0 = i0;
            g_scratch_tri[kept].i1 = i1;
            g_scratch_tri[kept].i2 = i2;
            /* Same height key then authored order as the live path. */
            g_scratch_tri[kept].hkey = tri_height_key(g_scratch_hkey, i0, i1, i2);
            g_scratch_tri[kept].key =
                (float)(m->tri_seq ? m->tri_seq[batch->first_index / 3 + t]
                                   : (uint32_t)t);
            kept++;
        }
        if (kept == 0) continue;
        if (kept > 1) qsort(g_scratch_tri, kept, sizeof(TriSort), tri_cmp_far_first);
        for (int t = 0; t < kept; t++) {
            g_scratch_idx[t * 3 + 0] = g_scratch_tri[t].i0;
            g_scratch_idx[t * 3 + 1] = g_scratch_tri[t].i1;
            g_scratch_idx[t * 3 + 2] = g_scratch_tri[t].i2;
        }
        GPU_DrawGeometryRaw(plat, batch->atlas_tex,
            g_scratch_xy, g_scratch_color, g_scratch_uv,
            V,
            g_scratch_idx, kept * 3);
    }
}

/* Walk Y-sorted draw order, group consecutive units sharing
 * (def_idx, color_idx) into runs, submit each run as one coalesced
 * batch. Mixed-faction crowds get many tiny runs (no batching gain
 * but no loss either); homogeneous fleets — like the M8 stress
 * grid — collapse to a single run and one set of draw calls. */
static void Units_Submit(TAK_Platform *plat, const struct GameWorld *world) {
    if (!plat || !plat->renderer || !world) return;

    int n = build_draw_order(world);
    if (n == 0) return;

    int run_start = 0;
    while (run_start < n) {
        const Unit *u0 = &g_units[g_draw_order[run_start]];
        int run_end = run_start + 1;
        while (run_end < n) {
            const Unit *uk = &g_units[g_draw_order[run_end]];
            if (uk->def_idx        != u0->def_idx)        break;
            if (uk->team_color_idx != u0->team_color_idx) break;
            run_end++;
        }
        submit_run(plat, world, u0->def_idx, u0->team_color_idx,
                   g_draw_order + run_start, run_end - run_start);
        run_start = run_end;
    }
}

/* Draw a health bar in screen coords above each alive unit. Bar is
 * green at full HP, transitioning through yellow to red as damage
 * accumulates. Hidden when health is full (no visual clutter on
 * undamaged units). Toggle with `~`. */
/* Damage bar (legacy:210837-210915): drawn only with the Visual
 * Options setting on, and only for the local player's units unless
 * cheat codes are allowed; nothing under 1 HP. A 32x5 quad centred
 * 10 px below the unit's screen origin, lit idx = hp*30/max pixels
 * from x=1: red under 10, yellow to 19, green from 20. */
static int unit_health_bar_rect(const struct GameWorld *world, const Unit *u,
                                SDL_Rect *out) {
    if (!g_health_bars_on || !world || !u || u->alive != 1) return 0;
    if (u->health < 1) return 0;
    if (u->player_id != 1 && !world->cfg.power_codes) return 0;
    if (!unit_visible_to_local_player(world, u)) return 0;
    float sx = (float)(u->world_x - world->cam_x);
    float sy = (float)(u->world_y - world->cam_y)
             - ((float)Terrain_SampleHeight(world, u->world_x, u->world_y)
                + u->flight_alt) * g_tan_tilt;
    out->x = (int)sx - 16;
    out->y = (int)sy + 10 - 2;
    out->w = 32;
    out->h = 5;
    return 1;
}

int Units_DebugHealthBarRect(int handle, SDL_Rect *out) {
    if (handle < 0 || handle >= g_unit_count || !out) return 0;
    return unit_health_bar_rect(World_Get(), &g_units[handle], out);
}

static void render_health_bars(const struct GameWorld *world, TAK_Platform *plat) {
    if (!plat || !plat->renderer || !g_health_bars_on) return;
    SDL_Renderer *r = plat->renderer;
    for (int i = 0; i < g_unit_count; i++) {
        const Unit *u = &g_units[i];
        SDL_Rect bar;
        if (!unit_health_bar_rect(world, u, &bar)) continue;
        if (bar.x + bar.w < 0 || bar.x > world->viewport_w) continue;
        if (bar.y + bar.h < 0 || bar.y > world->viewport_h) continue;
        int max_hp = u->max_health > 0 ? u->max_health : 1;
        int idx = (int)((int64_t)u->health * 30 / max_hp);
        if (idx < 0) idx = 0;
        if (idx > 30) idx = 30;
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderFillRect(r, &bar);
        if (idx > 0) {
            SDL_Rect lit = { bar.x + 1, bar.y, idx, bar.h };
            if (idx < 10)      SDL_SetRenderDrawColor(r, 255, 0, 0, 255);
            else if (idx < 20) SDL_SetRenderDrawColor(r, 255, 255, 0, 255);
            else               SDL_SetRenderDrawColor(r, 0, 255, 0, 255);
            SDL_RenderFillRect(r, &lit);
        }
    }
}

/* Draw a dashed selection ring at each selected unit's foot.
 * Decomp pattern (the legacy reference ~227938): 8 line segments
 * forming the visible portion of an oval — the gaps between them
 * give the characteristic dashed look. Colour = unit's team_color_idx
 * so a yellow Aramon unit gets a yellow ring, blue Zhon gets blue,
 * matching the legacy screenshots. */
static void render_selection_rings(const struct GameWorld *world, TAK_Platform *plat) {
    if (g_selection_count == 0) return;
    SDL_Renderer *r = plat->renderer;
    if (!r) return;

    const float tilt = g_tan_tilt;
    for (int s = 0; s < g_selection_count; s++) {
        int h = g_selection[s];
        if (h < 0 || h >= g_unit_count || g_units[h].alive != 1) continue;
        const Unit *u = &g_units[h];
        if (!unit_visible_to_local_player(world, u)) continue;
        const UnitDef *def = Units_GetDef(u->def_idx);
        /* Tight ring just past the unit's footprint. Reference shots
         * (image #77 etc) show the ring sitting close to the feet —
         * roughly the unit's silhouette half-width plus a few pixels. */
        float radius_x = 20.0f;
        if (def) {
            const UnitMesh *m = def->mesh_per_color[u->team_color_idx];
            if (m) {
                float ax = m->aabb_max[0] - m->aabb_min[0];
                float az = m->aabb_max[2] - m->aabb_min[2];
                float wider = ax > az ? ax : az;
                radius_x = wider * 0.5f * g_ta_scale + 3.0f;
            }
        }
        const float radius_y = radius_x * tilt;

        /* Ring colour is tied to the unit's HEALTH percentage:
         *   ≥50% → green, ≥25% → yellow, else red.
         * Same convention as the floating per-unit health bar; gives
         * the player at-a-glance status of every selected unit. */
        int max_hp = u->max_health > 0 ? u->max_health : 1;
        int pct    = u->health * 100 / max_hp;
        if (pct >= 50) {
            SDL_SetRenderDrawColor(r,  60, 220,  60, 255);   /* green */
        } else if (pct >= 25) {
            SDL_SetRenderDrawColor(r, 230, 200,  40, 255);   /* yellow */
        } else {
            SDL_SetRenderDrawColor(r, 230,  40,  30, 255);   /* red */
        }

        float fx = (float)(u->world_x - world->cam_x);
        float fy = (float)(u->world_y - world->cam_y)
                 - ((float)Terrain_SampleHeight(world, u->world_x, u->world_y)
                    + u->flight_alt) * tilt;

        /* Eight dashes = 8 short polyline arcs, each spanning 1/16
         * of the circle, with 1/16 gaps between. Gives the dashed-oval
         * silhouette TAK's renderer produces. */
        /* Draw each dash three times with tiny offsets to fake a
         * thicker (~3 px) line — SDL2's renderer doesn't expose
         * line width otherwise. */
        #define DASHES   8
        #define DASH_SEG 4
        static const float thick_dx[3] = { 0.0f, 0.0f,  1.0f };
        static const float thick_dy[3] = { 0.0f, 1.0f,  0.0f };
        for (int d = 0; d < DASHES; d++) {
            float a0 = ((float)d / (float)DASHES) * 6.2831853f;
            float a1 = a0 + (6.2831853f / (float)(DASHES * 2));
            for (int pass = 0; pass < 3; pass++) {
                SDL_FPoint pts[DASH_SEG + 1];
                for (int i = 0; i <= DASH_SEG; i++) {
                    float t = (float)i / (float)DASH_SEG;
                    float a = a0 + (a1 - a0) * t;
                    pts[i].x = fx + cosf(a) * radius_x + thick_dx[pass];
                    pts[i].y = fy + sinf(a) * radius_y + thick_dy[pass];
                }
                SDL_RenderDrawLinesF(r, pts, DASH_SEG + 1);
            }
        }
        #undef DASHES
        #undef DASH_SEG
    }
}

static uint32_t g_construct_anim_tick;   /* defined below; beam flicker seed */

/* ── Projectile art: 3DO models ───────────────────────────────────────
 *
 * Legacy hands the weapon's model to the ordinary model renderer with
 * the projectile's own heading/pitch/roll and the owner's team colour
 * (legacy:246780). We bake the same way units do, then draw one merged
 * vertex run per (model, colour) so a sky full of arrows costs a
 * handful of draw calls rather than one per shot. */
/* Projectile meshes hold pointers into the texture atlases, so they
 * die with them. Names and slot indices stay put so weapons keep their
 * resolved art index across a world reload. */
static void proj_model_drop_meshes(void) {
    for (int i = 0; i < g_proj_model_count; i++) {
        for (int c = 0; c < 12; c++) {
            if (g_proj_models[i].mesh_per_color[c]) {
                Mesh_Free(g_proj_models[i].mesh_per_color[c]);
                g_proj_models[i].mesh_per_color[c] = NULL;
            }
            g_proj_models[i].failed[c] = 0;
        }
    }
}

static const UnitMesh *proj_model_mesh(int model_idx, int color_idx) {
    if (model_idx < 0 || model_idx >= g_proj_model_count) return NULL;
    if (color_idx < 0 || color_idx > 11) color_idx = 0;
    ProjModelArt *pm = &g_proj_models[model_idx];
    if (pm->mesh_per_color[color_idx]) return pm->mesh_per_color[color_idx];
    if (pm->failed[color_idx]) return NULL;

    char path[64];
    snprintf(path, sizeof(path), "objects3d/%s.3do", pm->name);
    Obj3DFile *obj = NULL;
    if (Obj3D_Load(&obj, path) != 0 || !obj) {
        pm->failed[color_idx] = 1;
        fprintf(stderr, "Projectiles: no model %s\n", path);
        return NULL;
    }
    GameWorld *world = World_Get();
    const uint32_t *palette = world ? world->terrain_rgba : NULL;
    UnitMesh *m = Mesh_Bake(obj, palette, color_idx);
    Obj3D_Close(obj);
    if (!m) { pm->failed[color_idx] = 1; return NULL; }
    pm->mesh_per_color[color_idx] = m;
    fprintf(stderr, "Projectiles: %s color=%d -> %d verts, %d tris\n",
            pm->name, color_idx, m->vert_count, m->tri_count);
    return m;
}

/* Scratch lists for the model grouping pass. */
static int g_proj_draw[TAK_MAX_PROJECTILES];
static int g_proj_key [TAK_MAX_PROJECTILES];
static int g_proj_run [TAK_MAX_PROJECTILES];

/* One merged run: every projectile here shares a mesh, so the whole
 * group goes out as one vertex buffer per atlas batch. */
static void submit_projectile_run(TAK_Platform *plat,
                                  const struct GameWorld *world,
                                  int model_idx, int color_idx,
                                  const int *idx_list, int n)
{
    const UnitMesh *m = proj_model_mesh(model_idx, color_idx);
    if (!m || m->vert_count == 0) return;
    const int V = m->vert_count;
    int per_chunk = 65000 / V;
    if (per_chunk < 1) per_chunk = 1;

    /* Projectiles carry no script state, so the node transforms are the
     * mesh's static offsets and are the same for every one of them.
     * ensure_scratch first: on a frame with no units drawn nothing else
     * has allocated the node buffer yet. */
    if (ensure_scratch(V, m->tri_count * 3) != 0) return;
    compose_node_xforms(m, NULL, g_scratch_node_xform);

    const float cam_x = (float)world->cam_x;
    const float cam_y = (float)world->cam_y;
    const float ta    = g_ta_scale;
    const float tilt  = g_tan_tilt;

    for (int start = 0; start < n; start += per_chunk) {
        int cn = n - start;
        if (cn > per_chunk) cn = per_chunk;
        const int total_verts = cn * V;
        if (ensure_scratch(total_verts, cn * m->tri_count * 3) != 0) continue;

        for (int ci = 0; ci < cn; ci++) {
            const Projectile *p = &g_projectiles[idx_list[start + ci]];
            const float ux = (float)p->world_x;
            const float uz = (float)p->world_y;
            const float uh = p->height;
            const float ch = cosf(p->heading), sh = sinf(p->heading);
            const float cp = cosf(p->pitch),   sp = sinf(p->pitch);
            const float cr = cosf(p->roll),    sr = sinf(p->roll);
            const int v_off = ci * V;
            for (int v = 0; v < V; v++) {
                const uint16_t node = m->vert_node_idx[v];
                const NodeXform *x  = &g_scratch_node_xform[node];
                const int i = v_off + v;
                if (x->hidden) {
                    g_scratch_xy[2*i+0] = 0.0f;
                    g_scratch_xy[2*i+1] = 0.0f;
                    g_scratch_wz[i]     = 0.0f;
                    g_scratch_color[i]  = 0;
                    g_scratch_uv[2*i+0] = 0.0f;
                    g_scratch_uv[2*i+1] = 0.0f;
                    continue;
                }
                const float lx = m->positions[3*v+0];
                const float ly = m->positions[3*v+1];
                const float lz = m->positions[3*v+2];
                const float mx = x->rot[0]*lx + x->rot[1]*ly + x->rot[2]*lz + x->trans[0];
                const float my = x->rot[3]*lx + x->rot[4]*ly + x->rot[5]*lz + x->trans[1];
                const float mz = x->rot[6]*lx + x->rot[7]*ly + x->rot[8]*lz + x->trans[2];
                /* Roll about the model's forward axis, then pitch the
                 * nose, then the same yaw + handedness mirror the unit
                 * path uses (see submit_run). */
                const float ax = cr*mx - sr*my;
                const float ay = sr*mx + cr*my;
                const float by = cp*ay - sp*mz;
                const float bz = sp*ay + cp*mz;
                const float rx = -(ch * ax + sh * bz);
                const float rz = -(sh * ax - ch * bz);
                const float wx = ux + rx * ta;
                const float wy =      by * ta + uh;
                const float wz = uz + rz * ta;
                g_scratch_xy[2*i+0] = wx - cam_x;
                g_scratch_xy[2*i+1] = wz - cam_y - wy * tilt;
                g_scratch_wz[i]     = wz;
                g_scratch_color[i]  = m->colors[v];
                g_scratch_uv[2*i+0] = m->uvs[2*v+0];
                g_scratch_uv[2*i+1] = m->uvs[2*v+1];
            }
        }

        for (int b = 0; b < m->batch_count; b++) {
            const UnitMeshBatch *batch = &m->batches[b];
            if (batch->index_count == 0) continue;
            const int n_tri = batch->index_count / 3;
            const uint16_t *src = m->indices + batch->first_index;
            int w = 0;
            for (int ci = 0; ci < cn; ci++) {
                const int v_off = ci * V;
                for (int t = 0; t < n_tri; t++) {
                    uint16_t i0 = (uint16_t)(v_off + src[t*3+0]);
                    uint16_t i1 = (uint16_t)(v_off + src[t*3+1]);
                    uint16_t i2 = (uint16_t)(v_off + src[t*3+2]);
                    if (g_backface_cull_on) {
                        const float sx0 = g_scratch_xy[2*i0+0], sy0 = g_scratch_xy[2*i0+1];
                        const float sx1 = g_scratch_xy[2*i1+0], sy1 = g_scratch_xy[2*i1+1];
                        const float sx2 = g_scratch_xy[2*i2+0], sy2 = g_scratch_xy[2*i2+1];
                        const float cross = (sy0 - sy1) * (sx2 - sx1)
                                          - (sy2 - sy1) * (sx0 - sx1);
                        if (g_backface_cull_invert) { if (cross > 0.0f) continue; }
                        else                        { if (cross < 0.0f) continue; }
                    }
                    g_scratch_idx[w++] = i0;
                    g_scratch_idx[w++] = i1;
                    g_scratch_idx[w++] = i2;
                }
            }
            if (w == 0) continue;
            GPU_DrawGeometryRaw(plat, batch->atlas_tex,
                                g_scratch_xy, g_scratch_color, g_scratch_uv,
                                total_verts, g_scratch_idx, w);
        }
    }
}

static void submit_projectile_models(TAK_Platform *plat,
                                     const struct GameWorld *world) {
    if (!plat || !world) return;
    const int32_t margin = 256;
    const int32_t left   = world->cam_x - margin;
    const int32_t right  = world->cam_x + world->viewport_w + margin;
    const int32_t top    = world->cam_y - margin;
    const int32_t bottom = world->cam_y + world->viewport_h + margin;

    int n = 0;
    for (int i = 0; i < g_projectile_count; i++) {
        const Projectile *p = &g_projectiles[i];
        if (!p->alive || p->is_beam) continue;
        if (p->art_kind != UNIT_WEAPON_ART_MODEL || p->art_idx < 0) continue;
        if (!projectile_visible_to_local_player(world, p)) continue;
        if (p->world_x < left || p->world_x > right) continue;
        if (p->world_y < top  || p->world_y > bottom) continue;
        g_proj_draw[n] = i;
        g_proj_key[n]  = p->art_idx * 12 + (p->color_idx > 11 ? 0 : p->color_idx);
        n++;
    }
    /* Distinct (model, colour) pairs on screen are few, so gathering
     * each in one sweep beats sorting the whole list. */
    int done = 0;
    while (done < n) {
        int key = -1;
        for (int i = 0; i < n; i++) {
            if (g_proj_key[i] >= 0) { key = g_proj_key[i]; break; }
        }
        if (key < 0) break;
        int run = 0;
        for (int i = 0; i < n; i++) {
            if (g_proj_key[i] != key) continue;
            g_proj_run[run++] = g_proj_draw[i];
            g_proj_key[i] = -1;
            done++;
        }
        submit_projectile_run(plat, world, key / 12, key % 12, g_proj_run, run);
    }
}

/* ── Projectile art: weaponart / explosion GAF sequences ──────────────
 *
 * All frames of a sequence go into one texture, uploaded once, so every
 * projectile sharing a weapon draws from the same texture and SDL can
 * batch them. Nothing re-uploads per frame. */
static int proj_sprite_ensure(SDL_Renderer *r, int sprite_idx) {
    if (sprite_idx < 0 || sprite_idx >= g_proj_sprite_count) return -1;
    ProjSpriteArt *ps = &g_proj_sprites[sprite_idx];
    /* A texture belongs to the renderer that made it and dies with it,
     * so on a renderer swap drop the handle and re-upload from the
     * decoded strip we keep. */
    if (ps->strip && (ps->owner != r || ps->epoch != g_proj_art_epoch)) {
        ps->strip = NULL;
        ps->owner = NULL;
    }
    if (ps->strip) return 0;

    if (!ps->tried) {
        ps->tried = 1;
        /* Truecolor TAF first (retail ships every weapon sprite as
         * one), then a paletted GAF for mod data. */
        char path[128];
        GAFFile *gaf = NULL;
        int is_taf = 1;
        snprintf(path, sizeof(path), "data/anims/%s_4444.taf", ps->file);
        if (GAF_Open(&gaf, path) != 0 || !gaf) {
            snprintf(path, sizeof(path), "data/anims/%s_1555.taf", ps->file);
            if (GAF_Open(&gaf, path) != 0 || !gaf) {
                is_taf = 0;
                snprintf(path, sizeof(path), "data/anims/%s.gaf", ps->file);
                if (GAF_Open(&gaf, path) != 0 || !gaf) {
                    fprintf(stderr, "Projectiles: no art for %s\n", ps->file);
                    return -1;
                }
            }
        }
        int seq_off = GAF_FindSequence(gaf, ps->seq);
        if (seq_off < 0) seq_off = GAF_FindSequence(gaf, ps->file);
        if (seq_off < 0 && gaf->num_entries > 0) {
            /* Single-sequence files sometimes name the entry oddly. */
            seq_off = (int)(*(const uint32_t *)(gaf->data + 12));
        }
        if (seq_off < 0) { GAF_Close(gaf); return -1; }
        int nf = *(const uint16_t *)(gaf->data + seq_off);
        if (nf <= 0) { GAF_Close(gaf); return -1; }

        const GameWorld *world = World_Get();
        const uint32_t *pal = world ? world->features_rgba : NULL;
        uint32_t **pix = (uint32_t **)tak_malloc(sizeof(uint32_t *) * (size_t)nf);
        ps->fw = (int *)tak_malloc(sizeof(int) * (size_t)nf);
        ps->fh = (int *)tak_malloc(sizeof(int) * (size_t)nf);
        ps->ox = (int *)tak_malloc(sizeof(int) * (size_t)nf);
        ps->oy = (int *)tak_malloc(sizeof(int) * (size_t)nf);
        if (!pix || !ps->fw || !ps->fh || !ps->ox || !ps->oy) {
            if (pix) tak_free(pix);
            GAF_Close(gaf);
            return -1;
        }
        int cw = 1, chh = 1;
        for (int f = 0; f < nf; f++) {
            FrameHeader *fh = NULL;
            pix[f] = NULL;
            ps->fw[f] = ps->fh[f] = ps->ox[f] = ps->oy[f] = 0;
            if (GAF_GetFrameInfo(gaf, (uint32_t)seq_off, f, &fh) != 0 || !fh)
                continue;
            pix[f] = is_taf ? TAF_DecodeFrameRGBA(gaf, fh)
                            : GAF_DecodeFrameRGBA(gaf, fh, pal);
            ps->fw[f] = fh->width;
            ps->fh[f] = fh->height;
            ps->ox[f] = fh->offset_x;
            ps->oy[f] = fh->offset_y;
            if (ps->fw[f] > cw)  cw  = ps->fw[f];
            if (ps->fh[f] > chh) chh = ps->fh[f];
        }
        /* Pack every frame into one strip: all shots of a weapon then
         * draw from a single texture and batch together. */
        size_t px = (size_t)cw * (size_t)nf * (size_t)chh;
        ps->pixels = (uint32_t *)tak_malloc(sizeof(uint32_t) * px);
        if (ps->pixels) {
            memset(ps->pixels, 0, sizeof(uint32_t) * px);
            for (int f = 0; f < nf; f++) {
                if (!pix[f] || ps->fw[f] <= 0 || ps->fh[f] <= 0) continue;
                for (int row = 0; row < ps->fh[f]; row++) {
                    memcpy(ps->pixels + (size_t)row * (size_t)cw * nf
                                      + (size_t)f * cw,
                           pix[f] + (size_t)row * ps->fw[f],
                           sizeof(uint32_t) * (size_t)ps->fw[f]);
                }
            }
            ps->num_frames = nf;
            ps->cell_w     = cw;
            ps->cell_h     = chh;
        }
        for (int f = 0; f < nf; f++) if (pix[f]) tak_free(pix[f]);
        tak_free(pix);
        GAF_Close(gaf);
        fprintf(stderr, "Projectiles: %s/%s art loaded (%d frames, %dx%d)\n",
                ps->file, ps->seq, nf, cw, chh);
    }
    if (!ps->pixels || ps->num_frames <= 0) return -1;

    SDL_Texture *strip = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC,
                                           ps->cell_w * ps->num_frames,
                                           ps->cell_h);
    if (!strip) return -1;
    SDL_SetTextureBlendMode(strip, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(strip, NULL, ps->pixels,
                      ps->cell_w * ps->num_frames * 4);
    ps->strip = strip;
    ps->owner = r;
    ps->epoch = g_proj_art_epoch;
    return 0;
}

/* Drop every cached strip texture and retire the epoch. Called from
 * Units_ClearInstances, which runs during world teardown with the
 * owning renderer still alive; anything that somehow outlives its
 * renderer is abandoned by the epoch instead of being touched. */
static void proj_art_release_textures(void) {
    for (int i = 0; i < g_proj_sprite_count; i++) {
        ProjSpriteArt *ps = &g_proj_sprites[i];
        if (ps->strip && ps->epoch == g_proj_art_epoch) {
            SDL_DestroyTexture(ps->strip);
        }
        ps->strip = NULL;
        ps->owner = NULL;
    }
    g_proj_art_epoch++;
}

static void blit_proj_sprite(SDL_Renderer *r, int sprite_idx, int frame,
                             int sx, int sy) {
    if (sprite_idx < 0 || sprite_idx >= g_proj_sprite_count) return;
    const ProjSpriteArt *ps = &g_proj_sprites[sprite_idx];
    if (!ps->strip || ps->num_frames <= 0) return;
    if (frame < 0 || frame >= ps->num_frames) return;
    if (ps->fw[frame] <= 0 || ps->fh[frame] <= 0) return;
    SDL_Rect src = { frame * ps->cell_w, 0, ps->fw[frame], ps->fh[frame] };
    SDL_Rect dst = { sx - ps->ox[frame], sy - ps->oy[frame],
                     ps->fw[frame], ps->fh[frame] };
    SDL_RenderCopy(r, ps->strip, &src, &dst);
}

/* weaponart projectiles. Legacy steps the sequence once per engine
 * update (legacy:246685), which is every second sim tick for us. */
static void render_projectile_sprites(const struct GameWorld *world,
                                      TAK_Platform *plat) {
    if (!plat || !plat->renderer) return;
    SDL_Renderer *r = plat->renderer;
    for (int i = 0; i < g_projectile_count; i++) {
        const Projectile *p = &g_projectiles[i];
        if (!p->alive || p->is_beam) continue;
        if (p->art_kind != UNIT_WEAPON_ART_SPRITE || p->art_idx < 0) continue;
        if (!projectile_visible_to_local_player(world, p)) continue;
        if (proj_sprite_ensure(r, p->art_idx) != 0) continue;
        const ProjSpriteArt *ps = &g_proj_sprites[p->art_idx];
        int frame = ps->num_frames > 1
            ? (int)((p->age_ticks / 2) % (uint16_t)ps->num_frames) : 0;
        int sx = p->world_x - world->cam_x;
        int sy = p->world_y - world->cam_y - (int)(p->height * g_tan_tilt);
        blit_proj_sprite(r, p->art_idx, frame, sx, sy);
    }
}

/* explosionclass impact sprites (legacy:245025). */
static void render_projectile_effects(const struct GameWorld *world,
                                      TAK_Platform *plat) {
    if (!plat || !plat->renderer) return;
    SDL_Renderer *r = plat->renderer;
    for (int i = 0; i < g_proj_effect_count; i++) {
        const ProjectileEffect *e = &g_proj_effects[i];
        if (!e->alive) continue;
        if (world->cfg.line_of_sight &&
            Fog_IsVisible(world, e->world_x, e->world_y) == 0) continue;
        if (proj_sprite_ensure(r, e->sprite_idx) != 0) continue;
        const ProjSpriteArt *ps = &g_proj_sprites[e->sprite_idx];
        int frame = e->age_ticks / 2;
        if (frame >= ps->num_frames) continue;   /* played out */
        int sx = e->world_x - world->cam_x;
        int sy = e->world_y - world->cam_y
               - (int)((float)e->height * g_tan_tilt);
        blit_proj_sprite(r, e->sprite_idx, frame, sx, sy);
    }
}

/* Draw the projectiles that have no art of their own: beams, and the
 * fallback bright disc for weapons with neither `model` nor
 * `weaponart` (melee-adjacent and Remote Effect entries). */

static void render_projectiles(const struct GameWorld *world,
                                TAK_Platform *plat) {
    if (!plat || !plat->renderer) return;
    SDL_Renderer *r = plat->renderer;
    SDL_BlendMode prev_blend;
    SDL_GetRenderDrawBlendMode(r, &prev_blend);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < g_projectile_count; i++) {
        const Projectile *p = &g_projectiles[i];
        if (!p->alive) continue;
        /* Model and sprite projectiles are drawn by their own passes. */
        if (!p->is_beam && (p->art_kind == UNIT_WEAPON_ART_MODEL ||
                            p->art_kind == UNIT_WEAPON_ART_SPRITE)) continue;
        if (!projectile_visible_to_local_player(world, p)) continue;
        int sx = p->world_x - world->cam_x;
        int sy = p->world_y - world->cam_y
               - (int)((float)Terrain_SampleHeight(world, p->world_x, p->world_y)
                       * g_tan_tilt);
        /* A beam ends on an airborne target where it is drawn. */
        if (p->is_beam && p->target >= 0 && p->target < g_unit_count)
            sy -= (int)(g_units[p->target].flight_alt * g_tan_tilt);
        if (p->is_beam) {
            /* Jagged src→dest ray, three passes outer→inner. Jitter is
             * reseeded per rendered frame for the flicker. */
            int ax = p->src_x - world->cam_x;
            int ay = p->src_y - world->cam_y
                   - (int)((float)Terrain_SampleHeight(world, p->src_x, p->src_y)
                           * g_tan_tilt);
            float dx = (float)(sx - ax), dy = (float)(sy - ay);
            float len = sqrtf(dx * dx + dy * dy);
            float nx = len > 0.001f ? -dy / len : 0.0f;
            float ny = len > 0.001f ?  dx / len : 0.0f;
            #define BEAM_SEG 10
            int px[BEAM_SEG + 1], py[BEAM_SEG + 1];
            const int SEG = BEAM_SEG;
            for (int s = 0; s <= SEG; s++) {
                float t = (float)s / (float)SEG;
                float jit = 0.0f;
                if (s > 0 && s < SEG) {
                    uint32_t n = unit_deterministic_noise(
                        (uint32_t)i, (uint32_t)g_construct_anim_tick,
                        (uint32_t)s);
                    jit = ((float)(n & 0xffffu) / 65535.0f) * 12.0f - 6.0f;
                }
                px[s] = ax + (int)(dx * t + nx * jit);
                py[s] = ay + (int)(dy * t + ny * jit);
            }
            static const int pass_off[3] = { 2, 1, 0 };
            static const int pass_alpha[3] = { 90, 170, 255 };
            for (int pass = 0; pass < 3; pass++) {
                int ci = 2 - pass;   /* outer, middle, inner */
                SDL_SetRenderDrawColor(r, p->beam_rgb[ci][0],
                                       p->beam_rgb[ci][1],
                                       p->beam_rgb[ci][2],
                                       (uint8_t)pass_alpha[pass]);
                int off = pass_off[pass];
                for (int o = -off; o <= off; o++) {
                    for (int s = 0; s < SEG; s++) {
                        SDL_RenderDrawLine(r, px[s] + o, py[s],
                                           px[s + 1] + o, py[s + 1]);
                    }
                }
            }
            continue;
        }
        uint8_t hr = 255, hg = 220, hb = 90;
        uint8_t cr = 255, cg = 180, cb = 40;
        int halo_max = 14;
        int core_half = 4;
        if (p->visual_kind == UNIT_PROJECTILE_VIS_ARROW) {
            hr = 230; hg = 230; hb = 210;
            cr = 245; cg = 245; cb = 225;
            halo_max = 8;
            core_half = 2;
        } else if (p->visual_kind == UNIT_PROJECTILE_VIS_CANNON) {
            hr = 255; hg = 130; hb = 60;
            cr = 245; cg = 90; cb = 35;
            halo_max = 16;
            core_half = 5;
        } else if (p->visual_kind == UNIT_PROJECTILE_VIS_MAGIC) {
            hr = 110; hg = 190; hb = 255;
            cr = 80; cg = 235; cb = 255;
            halo_max = 18;
            core_half = 5;
        } else if (p->visual_kind == UNIT_PROJECTILE_VIS_REMOTE) {
            hr = 190; hg = 110; hb = 255;
            cr = 230; cg = 120; cb = 255;
            halo_max = 20;
            core_half = 6;
        }
        SDL_SetRenderDrawColor(r, hr, hg, hb, 80);
        for (int rsz = halo_max; rsz >= 6; rsz -= 4) {
            SDL_Rect halo = { sx - rsz, sy - rsz, rsz*2, rsz*2 };
            SDL_RenderFillRect(r, &halo);
        }
        SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
        SDL_Rect core = { sx - core_half, sy - core_half,
                          core_half * 2, core_half * 2 };
        SDL_RenderFillRect(r, &core);
        SDL_SetRenderDrawColor(r, 255, 240, 180, 255);
        SDL_Rect spark = { sx - 1, sy - 1, 2, 2 };
        SDL_RenderFillRect(r, &spark);
    }
    SDL_SetRenderDrawBlendMode(r, prev_blend);
}

/* Construction-effect sprite cache, one per faction prefix. The
 * legacy engine reads `buildsparklygaf` from sidedata.tdf — Aramon =
 * `aramonbuild_4444.taf`, Taros = `tarosbuild_4444.taf`, Veruna =
 * `verunabuild_4444.taf`, Zhon = `zhonbuild_4444.taf`. Loaded lazily
 * on first need; rendered at the building's foot while it's under
 * construction (Unit.under_construction flag, mirrors legacy bit
 * 0x200000 at unit+0x5a per legacy:179316). */
typedef struct ConstructFX {
    char       prefix[8];          /* "ARA" / "TAR" / "VER" / "ZON" */
    GAFFile   *gaf;
    int        num_frames;
    uint32_t **frame_pixels;       /* num_frames pointers           */
    int       *frame_w;
    int       *frame_h;
    int       *frame_off_x;
    int       *frame_off_y;
} ConstructFX;
static ConstructFX g_construct_fx[4];
static int          g_construct_fx_count = 0;
static uint32_t     g_construct_anim_tick = 0;

static const char *fx_taf_for_prefix(const char *prefix) {
    if (!prefix) return NULL;
    if (strcmp(prefix, "ARA") == 0) return "data/anims/aramonbuild_4444.taf";
    if (strcmp(prefix, "TAR") == 0) return "data/anims/tarosbuild_4444.taf";
    if (strcmp(prefix, "VER") == 0) return "data/anims/verunabuild_4444.taf";
    if (strcmp(prefix, "ZON") == 0) return "data/anims/zhonbuild_4444.taf";
    return NULL;
}

static ConstructFX *load_construct_fx(const char *prefix) {
    /* Cache hit. */
    for (int i = 0; i < g_construct_fx_count; i++) {
        if (strcmp(g_construct_fx[i].prefix, prefix) == 0) {
            return &g_construct_fx[i];
        }
    }
    if (g_construct_fx_count >= 4) return NULL;
    const char *path = fx_taf_for_prefix(prefix);
    if (!path) return NULL;

    GAFFile *gaf = NULL;
    if (GAF_Open(&gaf, path) != 0 || !gaf) {
        fprintf(stderr, "ConstructFX: failed to open %s\n", path);
        return NULL;
    }
    if (gaf->num_entries < 1) { GAF_Close(gaf); return NULL; }

    /* Decode every frame of entry 0 into RGBA buffers we can keep. */
    uint32_t entry_off = *(const uint32_t *)(gaf->data + 12);
    uint16_t num_frames = *(const uint16_t *)(gaf->data + entry_off);
    if (num_frames == 0) { GAF_Close(gaf); return NULL; }

    ConstructFX *fx = &g_construct_fx[g_construct_fx_count++];
    memset(fx, 0, sizeof(*fx));
    snprintf(fx->prefix, sizeof(fx->prefix), "%s", prefix);
    fx->gaf = gaf;
    fx->num_frames = num_frames;
    fx->frame_pixels = (uint32_t **)tak_malloc(sizeof(uint32_t *) * num_frames);
    fx->frame_w      = (int *)tak_malloc(sizeof(int) * num_frames);
    fx->frame_h      = (int *)tak_malloc(sizeof(int) * num_frames);
    fx->frame_off_x  = (int *)tak_malloc(sizeof(int) * num_frames);
    fx->frame_off_y  = (int *)tak_malloc(sizeof(int) * num_frames);
    for (int f = 0; f < num_frames; f++) {
        FrameHeader *fh = NULL;
        if (GAF_GetFrameInfo(gaf, entry_off, f, &fh) != 0 || !fh) {
            fx->frame_pixels[f] = NULL;
            continue;
        }
        /* TAF format = truecolor 1555 / 4444. The 4444 file is comp=4. */
        fx->frame_pixels[f] = TAF_DecodeFrameRGBA(gaf, fh);
        fx->frame_w[f]     = fh->width;
        fx->frame_h[f]     = fh->height;
        fx->frame_off_x[f] = fh->offset_x;
        fx->frame_off_y[f] = fh->offset_y;
    }
    fprintf(stderr, "ConstructFX: %s loaded (%d frames)\n", path, num_frames);
    return fx;
}

static void blit_construct_frame(SDL_Renderer *r,
                                   const ConstructFX *fx,
                                   int frame_idx, int sx, int sy) {
    uint32_t *pix = fx->frame_pixels[frame_idx];
    if (!pix) return;
    int w = fx->frame_w[frame_idx];
    int h = fx->frame_h[frame_idx];
    int dx = sx - fx->frame_off_x[frame_idx];
    int dy = sy - fx->frame_off_y[frame_idx];
    SDL_Texture *tex = SDL_CreateTexture(r,
        SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex) return;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(tex, NULL, pix, w * 4);
    SDL_Rect dst = { dx, dy, w, h };
    SDL_RenderCopy(r, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

static void render_construction_effects(const struct GameWorld *world,
                                         TAK_Platform *plat) {
    if (!plat || !plat->renderer) return;
    g_construct_anim_tick++;
    SDL_Renderer *r = plat->renderer;
    for (int i = 0; i < g_unit_count; i++) {
        Unit *u = &g_units[i];
        if (u->alive != 1 || !u->under_construction) continue;
        if (!unit_visible_to_local_player(world, u)) continue;
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!d) continue;
        ConstructFX *fx = load_construct_fx(d->side);
        if (fx == NULL) continue;
        if (fx->num_frames <= 0) continue;
        int frame_idx = (int)((g_construct_anim_tick / 4) % fx->num_frames);
        int sx = u->world_x - world->cam_x;
        int sy = u->world_y - world->cam_y;
        blit_construct_frame(r, fx, frame_idx, sx, sy);
    }
}

/* ── Map-feature sprite cache ─────────────────────────────────────────
 *
 * Each FeatureDef references a GAF (filename) + sequence (seqname). On
 * first sight we open the GAF, decode every frame of the named sequence
 * into RGBA buffers using the kingdom palette (world->terrain_rgba —
 * the same one Build_PaletteRGBATable produced for the active map's
 * kingdom). Animated features cycle frames on the same global tick the
 * construct effect uses; static ones (animating=0) just hold frame 0.
 *
 * Cache key is (filename, seqname). Same filename, different seqname is
 * a separate entry — we open the GAF once but each sequence is its own
 * frame array. Capacity 256 handles all veruna / aramon / taros / zhon
 * sprites a single map could plausibly hit. */
typedef struct FeatureSprite {
    char       filename[40];
    char       seqname[40];
    GAFFile   *gaf;
    int        num_frames;
    uint32_t **frame_pixels;
    int       *frame_w;
    int       *frame_h;
    int       *frame_off_x;
    int       *frame_off_y;
    int        loaded_ok;          /* 1 if frames[*] valid; 0 = load failed, don't retry */
} FeatureSprite;
static FeatureSprite g_feat_sprites[256];
static int           g_feat_sprite_count = 0;

static FeatureSprite *load_feature_sprite(const char *filename,
                                           const char *seqname,
                                           const uint32_t *rgba_table) {
    if (!filename || !*filename || !seqname || !*seqname) return NULL;
    /* Cache. */
    for (int i = 0; i < g_feat_sprite_count; i++) {
        if (tak_stricmp(g_feat_sprites[i].filename, filename) == 0 &&
            tak_stricmp(g_feat_sprites[i].seqname, seqname) == 0) {
            return g_feat_sprites[i].loaded_ok ? &g_feat_sprites[i] : NULL;
        }
    }
    if (g_feat_sprite_count >= (int)(sizeof(g_feat_sprites)/sizeof(g_feat_sprites[0]))) {
        return NULL;
    }
    FeatureSprite *fs = &g_feat_sprites[g_feat_sprite_count++];
    memset(fs, 0, sizeof(*fs));
    snprintf(fs->filename, sizeof(fs->filename), "%s", filename);
    snprintf(fs->seqname,  sizeof(fs->seqname),  "%s", seqname);

    char gaf_path[128];
    snprintf(gaf_path, sizeof(gaf_path), "data/anims/%s.gaf", filename);
    GAFFile *gaf = NULL;
    if (GAF_Open(&gaf, gaf_path) != 0 || !gaf) {
        fprintf(stderr, "FeatureSprite: failed to open %s\n", gaf_path);
        return NULL;
    }
    int seq_off = GAF_FindSequence(gaf, seqname);
    if (seq_off < 0) {
        fprintf(stderr, "FeatureSprite: %s has no sequence %s\n",
                gaf_path, seqname);
        GAF_Close(gaf);
        return NULL;
    }
    uint16_t num_frames = *(const uint16_t *)(gaf->data + seq_off);
    if (num_frames == 0) { GAF_Close(gaf); return NULL; }

    fs->gaf          = gaf;
    fs->num_frames   = num_frames;
    fs->frame_pixels = (uint32_t **)tak_malloc(sizeof(uint32_t *) * num_frames);
    fs->frame_w      = (int *)tak_malloc(sizeof(int) * num_frames);
    fs->frame_h      = (int *)tak_malloc(sizeof(int) * num_frames);
    fs->frame_off_x  = (int *)tak_malloc(sizeof(int) * num_frames);
    fs->frame_off_y  = (int *)tak_malloc(sizeof(int) * num_frames);
    if (!fs->frame_pixels || !fs->frame_w || !fs->frame_h ||
        !fs->frame_off_x || !fs->frame_off_y) {
        GAF_Close(gaf);
        return NULL;
    }
    for (int f = 0; f < num_frames; f++) {
        FrameHeader *fh = NULL;
        if (GAF_GetFrameInfo(gaf, (uint32_t)seq_off, f, &fh) != 0 || !fh) {
            fs->frame_pixels[f] = NULL;
            continue;
        }
        fs->frame_pixels[f] = GAF_DecodeFrameRGBA(gaf, fh, rgba_table);
        fs->frame_w[f]      = fh->width;
        fs->frame_h[f]      = fh->height;
        fs->frame_off_x[f]  = fh->offset_x;
        fs->frame_off_y[f]  = fh->offset_y;
    }
    fs->loaded_ok = 1;
    fprintf(stderr, "FeatureSprite: %s/%s loaded (%d frames, frame0 %dx%d off=%d,%d)\n",
            filename, seqname, num_frames,
            fs->frame_w[0], fs->frame_h[0],
            fs->frame_off_x[0], fs->frame_off_y[0]);
    return fs;
}

static void blit_feature_frame(SDL_Renderer *r,
                                const FeatureSprite *fs,
                                int frame_idx, int sx, int sy) {
    uint32_t *pix = fs->frame_pixels[frame_idx];
    if (!pix) return;
    int w  = fs->frame_w[frame_idx];
    int h  = fs->frame_h[frame_idx];
    int dx = sx - fs->frame_off_x[frame_idx];
    int dy = sy - fs->frame_off_y[frame_idx];
    SDL_Texture *tex = SDL_CreateTexture(r,
        SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex) return;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(tex, NULL, pix, w * 4);
    SDL_Rect dst = { dx, dy, w, h };
    SDL_RenderCopy(r, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

static void render_features(const struct GameWorld *world, TAK_Platform *plat) {
    if (!world || !plat || !plat->renderer) return;
    if (!world->features || world->feature_count <= 0) return;
    SDL_Renderer *r = plat->renderer;
    for (int i = 0; i < world->feature_count; i++) {
        const FeatureDef *fd =
            Features_GetByIndex(world->features[i].global_idx);

        /* Anchor the sprite at the centre of the feature's footprint
         * (in tile space). The sprite's authored offset_x/_y is the
         * hotspot relative to that anchor — usually the foot-centre,
         * which is exactly what a top-down RTS expects so the sprite
         * sits on the cell that its TNT entry occupies. Tile = 16 wpx
         * to match the rest of the engine. */
        int fp_x = (fd && fd->footprint_x > 0) ? fd->footprint_x : 1;
        int fp_z = (fd && fd->footprint_z > 0) ? fd->footprint_z : 1;
        int wx = world->features[i].tile_x * 16 + fp_x * 8;
        int wy = world->features[i].tile_z * 16 + fp_z * 8;
        int sx = wx - world->cam_x;
        /* Features lift with the ground the same way units do: legacy
         * subtracts the mean of the cell's four corner heights over 2
         * (legacy:211150-211153). Without it a sacred pad sits lower
         * on screen than the lodestone standing on it. */
        int sy = wy - world->cam_y
               - (int)((float)Terrain_SampleHeight(world, wx, wy) * g_tan_tilt);
        if (sx < -128 || sy < -128 ||
            sx > world->viewport_w + 128 ||
            sy > world->viewport_h + 128) continue;
        if (Fog_StateAt(world, wx, wy) == TAK_FOG_UNEXPLORED) continue;

        if (!fd) {
            /* Unresolved feature: red dot so we can see which TNT cells
             * have local IDs that didn't map to the global registry. */
            SDL_SetRenderDrawColor(r, 220, 50, 50, 255);
            SDL_Rect dot = { sx - 3, sy - 3, 6, 6 };
            SDL_RenderFillRect(r, &dot);
            continue;
        }

        FeatureSprite *fs = load_feature_sprite(fd->filename, fd->seqname,
                                                 world->features_rgba);
        if (!fs || fs->num_frames <= 0) {
            /* Resolved but sprite load failed: yellow dot. */
            uint8_t cr = 140, cg = 140, cb = 140;
            if (tak_stricmp(fd->category, "mana") == 0) { cr=90; cg=230; cb=255; }
            else if (tak_stricmp(fd->category, "rocks") == 0) { cr=200; cg=140; cb=80; }
            else if (tak_stricmp(fd->category, "trees") == 0) { cr=80; cg=180; cb=80; }
            SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
            SDL_Rect dot = { sx - 4, sy - 4, 8, 8 };
            SDL_RenderFillRect(r, &dot);
            continue;
        }
        int frame_idx = fs->num_frames > 1
            ? (int)((g_construct_anim_tick / 4) % fs->num_frames)
            : 0;
        blit_feature_frame(r, fs, frame_idx, sx, sy);
    }
}

void Units_Render(const struct GameWorld *world, TAK_Platform *plat) {
    if (!world || !plat) return;
    /* Selection ring goes UNDER unit meshes — they draw on top of
     * the ring where they overlap, so only the ground-visible part
     * of the ring shows around the unit's feet. */
    render_selection_rings(world, plat);
    /* Map features (lodestones, rocks, trees) parsed from the TNT
     * feature_layer at map load. Drawn BEFORE units so unit meshes
     * occlude any feature their feet stand on, matching legacy
     * Y-sort approximations. */
    render_features(world, plat);
    Units_Submit(plat, world);
    /* Projectile models ride the same batched geometry path as units
     * (legacy draws them through the model renderer, legacy:246780). */
    submit_projectile_models(plat, world);
    if (g_health_bars_on) {
        render_health_bars(world, plat);
    }
    render_projectile_sprites(world, plat);
    render_projectiles(world, plat);
    render_projectile_effects(world, plat);
    render_construction_effects(world, plat);
}
