/*
 * test_unit_stall.c — soak harness for issue #60.
 *
 * It tries hard to wedge units, then watches every unit that holds an
 * order and reports the ones that stop making progress. The engine is
 * not touched: the stall clock lives here and reads only the public
 * unit fields, so the numbers it prints describe the engine as it
 * stands.
 *
 * A stall candidate on a tick is a unit with an order, alive, mobile,
 * not a flyer, whose goal is not yet reached. For each candidate the
 * harness keeps one monotone progress measure per order kind and one
 * order-level clock. The clock resets when the measure improves on its
 * best value so far and stops entirely for the legitimate waits.
 *
 * Rungs at 150, 300, 480, 720 and 1200 ticks (2.5, 5, 8, 12 and 20 s at
 * 60 Hz, half those numbers in the reference's 30 Hz frames).
 */

#include "test_framework.h"
#include "tak_hpi.h"
#include "tak_ui.h"
#include "tak_platform.h"
#include "tak_gameloop.h"
#include "tak_battle_config.h"
#include "tak_loading.h"
#include "tak_ingame.h"
#include "tak_world.h"
#include "tak_unit.h"
#include "tak_pathing.h"
#include "tak_occupancy.h"
#include "tak_terrain.h"
#include "tak_moveinfo.h"
#include "tak_economy.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#define SOAK_MAX_UNITS 2000

static const char *g_test_filter = NULL;

#define RUN_SOAK(name) \
    do { if (!g_test_filter || strstr(#name, g_test_filter)) RUN(name); } while (0)

/* ── platform + vfs ──────────────────────────────────────────────── */

static int setup_platform(TAK_Platform *p) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SKIP (SDL init failed: %s) ", SDL_GetError());
        return -1;
    }
    memset(p, 0, sizeof(*p));
    p->window = SDL_CreateWindow("tak-soak", SDL_WINDOWPOS_CENTERED,
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

/* Load a map. with_ai 0 puts both seats on human, so nothing but the
 * harness gives orders and the one-army victory rule never fires. */
static int soak_boot(TAK_Platform *platform, const char *map,
                     const char *kingdom, int with_ai, GameWorld **out) {
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    strncpy(cfg.map_name, map, sizeof(cfg.map_name) - 1);
    cfg.line_of_sight = 0;
    cfg.map_revealed = 1;
    if (!with_ai) cfg.players[1].kind = TAK_SLOT_HUMAN;
    if (World_BeginLoad(platform, &cfg, map, kingdom) != 0) return -1;
    if (Loading_Init(platform) != 0) return -1;
    int next = GAMESTATE_GAME_LOADING;
    for (int i = 0; i < 3000 && next == GAMESTATE_GAME_LOADING; i++)
        next = Loading_Tick(platform, 1.0f / 60.0f);
    if (next != GAMESTATE_IN_GAME) return -1;
    *out = World_Get();
    return *out ? 0 : -1;
}

static void soak_unload(TAK_Platform *platform) {
    Loading_Shutdown();
    World_End(platform);
}

/* ── the stall clock ─────────────────────────────────────────────── */

#define SOAK_R1 150
#define SOAK_R2 300
#define SOAK_R3 480
#define SOAK_R4 720
#define SOAK_R5 1200

typedef struct SoakOrderKey {
    int16_t  cmd_kind;
    int16_t  target;
    int32_t  cmd_x, cmd_y;
    int32_t  patrol_x, patrol_y;
    int16_t  build_target;
    uint32_t stable_id;
} SoakOrderKey;

/* Why the unit was not getting anywhere, counted per tick of the
 * clock: the cause is what a fix has to answer. */
typedef struct SoakCause {
    int32_t t_terrain;   /* a step refused by ground it cannot enter */
    int32_t t_unit;      /* a step refused by another unit or a structure */
    int32_t t_blocked;   /* refused, and the cell ahead reads as clear */
    int32_t t_noroute;   /* A* found nothing from here */
    int32_t t_pending;   /* waiting on the plan budget */
    int32_t t_walking;   /* nothing refused it, it just never closes */
    int32_t t_still;     /* it did not move at all this tick */
    int32_t replans;     /* route dropped and planned again */
} SoakCause;

typedef struct SoakUnitObs {
    SoakOrderKey key;
    int      have;
    int64_t  best_dist;    /* lower is better */
    double   best_work;    /* higher is better */
    int32_t  clock;
    uint8_t  rung;
    int32_t  order_start_tick;
    int32_t  anchor_x, anchor_y;   /* where the net displacement is measured from */
    int32_t  last_x, last_y;
    int      last_path_len;
    /* Where the unit was on each of the last five 30 tick marks, so a
     * tick can ask how far it has actually got in the last 150. */
    int32_t  hist_x[5], hist_y[5];
    int      hist_n;
    int64_t  travelled;    /* pixels covered while the clock ran */
    SoakCause cause;
} SoakUnitObs;

typedef struct SoakOffender {
    int32_t  ticks;        /* longest clock reached */
    int      handle;
    int      cmd_kind;
    int32_t  x, y;
    int32_t  goal_x, goal_y;
    int      fx, fz;       /* footprint in tiles */
    int      open_7x7;     /* walkable tiles in the 7x7 tile window */
    int      occupied_7x7; /* of those, tiles a unit or structure holds */
    int      route_left;   /* waypoints A* still finds from here, -1 none */
    int      escaped;      /* ended the run free to act */
    int64_t  travelled;    /* pixels covered while the clock ran */
    int      net150;       /* pixels of ground made in the last 150 ticks */
    SoakCause cause;
    char     name[24];
    char     scen[48];
} SoakOffender;

#define SOAK_OFF_MAX 16

typedef struct SoakCounters {
    long long candidate_ticks;
    long      stalls_detected;
    long      r2, r3, r4, r5;
    long      recovered;
    long long stall_ticks_total;
    long      longest;
    int       last_handle, last_tick, last_rung;
    long      orders_seen;
    /* Ticks where the measure did not improve but the unit was still
     * covering ground: not closing, but not helpless either. */
    long long noprog_ticks;
    SoakCause total_cause;   /* every stalled tick, across every unit */
    SoakOffender off[SOAK_OFF_MAX];
    int       off_count;
} SoakCounters;

static SoakUnitObs  g_obs[SOAK_MAX_UNITS];
static SoakCounters g_ctr;
static const char  *g_scen = "?";
static int          g_tick = 0;

static void soak_reset(const char *scen) {
    memset(g_obs, 0, sizeof(g_obs));
    memset(&g_ctr, 0, sizeof(g_ctr));
    g_scen = scen;
    g_tick = 0;
}

static int key_same(const SoakOrderKey *a, const SoakOrderKey *b) {
    return a->cmd_kind == b->cmd_kind && a->target == b->target &&
           a->cmd_x == b->cmd_x && a->cmd_y == b->cmd_y &&
           a->patrol_x == b->patrol_x && a->patrol_y == b->patrol_y &&
           a->build_target == b->build_target &&
           a->stable_id == b->stable_id;
}

/* Where the order really ends, the same rule the mover uses: a goal the
 * planner had to shift away from is served by the route's last point. */
static void soak_effective_goal(const Unit *u, int32_t gx, int32_t gy,
                                int32_t *ex, int32_t *ey) {
    *ex = gx; *ey = gy;
    if (u->path_len == 0) return;
    int32_t lx = u->path_x[u->path_len - 1];
    int32_t ly = u->path_y[u->path_len - 1];
    int64_t dx = (int64_t)lx - gx, dy = (int64_t)ly - gy;
    if (dx * dx + dy * dy > 32 * 32) { *ex = lx; *ey = ly; }
}

static int64_t soak_seg(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    double dx = (double)bx - ax, dy = (double)by - ay;
    return (int64_t)(sqrt(dx * dx + dy * dy) + 0.5);
}

/* How far the unit still has to walk: to the waypoint it is heading
 * for, then along the rest of the route, then from the route's end to
 * the goal. In pixels, so it falls as the unit advances and rises when
 * a new plan picks a longer way round. The distance to the current
 * waypoint alone is not a measure an order can be judged on: it jumps
 * up every time a waypoint is passed, and a best-so-far over the whole
 * order could then never be beaten again. */
static int64_t soak_walk_measure(const Unit *u, int32_t gx, int32_t gy) {
    int32_t cx = u->world_x, cy = u->world_y;
    int64_t d = 0;
    for (int i = u->path_index; i < u->path_len; i++) {
        d += soak_seg(cx, cy, u->path_x[i], u->path_y[i]);
        cx = u->path_x[i];
        cy = u->path_y[i];
    }
    int32_t ex, ey;
    soak_effective_goal(u, gx, gy, &ex, &ey);
    return d + soak_seg(cx, cy, ex, ey);
}

static int64_t soak_d2(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    int64_t dx = (int64_t)ax - bx, dy = (int64_t)ay - by;
    return dx * dx + dy * dy;
}

static int soak_max_weapon_range(const UnitDef *d) {
    int r = 0;
    for (int i = 0; i < d->num_weapons; i++)
        if ((int)d->weapons[i].range > r) r = (int)d->weapons[i].range;
    return r;
}

typedef struct SoakMeas {
    int      excluded;     /* the clock does not run this tick */
    int      arrived;      /* the order's goal is reached */
    int64_t  dist;
    double   work;
    int32_t  goal_x, goal_y;
} SoakMeas;

/* One tick's reading for a unit. Everything the invariant calls a
 * legitimate wait comes back excluded. */
static void soak_measure(const GameWorld *w, const Unit *units, int n,
                         int h, SoakMeas *m) {
    const Unit *u = &units[h];
    memset(m, 0, sizeof(*m));
    m->excluded = 1;
    m->dist = -1;
    if (u->cmd_kind == UNIT_CMD_NONE) return;
    if (u->alive != UNIT_ALIVE_ACTIVE) return;      /* dead, dying, aboard */
    if (u->under_construction) return;
    const UnitDef *d = Units_GetDef(u->def_idx);
    if (!d) return;
    if (d->max_velocity <= 0.0f) return;            /* every structure */
    if (d->can_fly) return;                         /* flyers never run A* */
    if (u->path_pending && u->path_len == 0 && u->path_wait <= 24) return;
    if (u->prod_queue_len > 0) return;
    if (u->xfer_ticks > 0 || u->unload_stage > 0) return;  /* transfer stage */

    m->goal_x = u->cmd_x;
    m->goal_y = u->cmd_y;

    switch (u->cmd_kind) {
    case UNIT_CMD_MOVE:
    case UNIT_CMD_PATROL:
    case UNIT_CMD_ATTACK_GROUND:
    case UNIT_CMD_UNLOAD: {
        int32_t ex, ey;
        soak_effective_goal(u, u->cmd_x, u->cmd_y, &ex, &ey);
        if (soak_d2(u->world_x, u->world_y, ex, ey) <= (int64_t)96 * 96) {
            m->arrived = 1;
            return;
        }
        m->dist = soak_walk_measure(u, u->cmd_x, u->cmd_y);
        m->excluded = 0;
        return;
    }
    case UNIT_CMD_ATTACK:
    case UNIT_CMD_GUARD:
    case UNIT_CMD_REPAIR:
    case UNIT_CMD_LOAD:
    case UNIT_CMD_BOARD: {
        int t = u->target;
        if (t < 0 || t >= n || units[t].alive == UNIT_ALIVE_DEAD) return;
        int32_t tx = units[t].world_x, ty = units[t].world_y;
        int64_t gd = soak_d2(u->world_x, u->world_y, tx, ty);
        if (u->cmd_kind == UNIT_CMD_ATTACK) {
            /* Firing is the order being served. */
            if (u->anim_state == UNIT_ANIM_ATTACKING) { m->arrived = 1; return; }
            int rng = soak_max_weapon_range(d);
            if (rng > 0 && gd <= (int64_t)rng * rng) { m->arrived = 1; return; }
        } else if (gd <= (int64_t)96 * 96) {
            m->arrived = 1;
            return;
        }
        m->goal_x = tx; m->goal_y = ty;
        m->dist = soak_walk_measure(u, tx, ty);
        m->excluded = 0;
        return;
    }
    case UNIT_CMD_BUILD: {
        int bt = u->build_target;
        int reach = d->build_distance > 0 ? d->build_distance : 48;
        if (bt >= 0 && bt < n && units[bt].alive != UNIT_ALIVE_DEAD) {
            int64_t gd = soak_d2(u->world_x, u->world_y,
                                 units[bt].world_x, units[bt].world_y);
            m->goal_x = units[bt].world_x;
            m->goal_y = units[bt].world_y;
            int work_range = reach + 96;
            {
                /* Big keeps are 5x14 tiles: the reach is to the
                 * footprint edge, not to its centre. */
                const UnitDef *td = Units_GetDef(units[bt].def_idx);
                if (td && td->footprint_x > 0 && td->footprint_z > 0) {
                    int fx = td->footprint_x * 16, fz = td->footprint_z * 16;
                    work_range += (int)(sqrt((double)fx * fx +
                                             (double)fz * fz) / 2.0);
                }
            }
            if (gd <= (int64_t)work_range * work_range) {
                /* At work: the frame's health rising is the progress. */
                m->work = (double)units[bt].health + (double)u->build_hp_accum;
                m->dist = -1;
                m->excluded = 0;
                return;
            }
            m->dist = soak_walk_measure(u, m->goal_x, m->goal_y);
            m->excluded = 0;
            return;
        }
        int32_t ex, ey;
        soak_effective_goal(u, u->cmd_x, u->cmd_y, &ex, &ey);
        if (soak_d2(u->world_x, u->world_y, ex, ey) <= (int64_t)96 * 96) {
            m->arrived = 1;
            return;
        }
        m->dist = soak_walk_measure(u, u->cmd_x, u->cmd_y);
        m->excluded = 0;
        return;
    }
    case UNIT_CMD_RECLAIM:
    case UNIT_CMD_RESURRECT: {
        int32_t gx = u->cmd_x, gy = u->cmd_y;
        if (u->reclaim_tile_x >= 0) {
            gx = (int32_t)u->reclaim_tile_x * 16 + 8;
            gy = (int32_t)u->reclaim_tile_y * 16 + 8;
        } else if (u->target >= 0 && u->target < n) {
            gx = units[u->target].world_x;
            gy = units[u->target].world_y;
        }
        m->goal_x = gx; m->goal_y = gy;
        int reach = (d->build_distance > 0 ? d->build_distance : 48) + 96;
        if (soak_d2(u->world_x, u->world_y, gx, gy) <= (int64_t)reach * reach) {
            m->work = (double)u->reclaim_accum - (double)u->raise_left;
            m->dist = -1;
            m->excluded = 0;
            return;
        }
        m->dist = soak_walk_measure(u, gx, gy);
        m->excluded = 0;
        return;
    }
    default:
        return;
    }
}

/* ── map probes ──────────────────────────────────────────────────── */

static const MoveClassDef *soak_mc(const GameWorld *w, const UnitDef *d) {
    return d->movement_class[0]
        ? TAK_MoveInfo_Find(&w->moveinfo, d->movement_class) : NULL;
}

static int soak_open(const GameWorld *w, const MoveClassDef *mc, int slope,
                     int tx, int ty) {
    if (tx < 0 || ty < 0) return 0;
    return TAK_PathClearanceAt(w, mc, slope, tx, ty) >= 1;
}

/* Terrain around a jammed unit, for the report. */
static void soak_neighbourhood(const GameWorld *w, const Unit *u,
                               const UnitDef *d, int *open, int *occupied) {
    const MoveClassDef *mc = soak_mc(w, d);
    int tx = u->world_x / 16, ty = u->world_y / 16;
    int o = 0, q = 0;
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            int cx = tx + dx, cy = ty + dy;
            if (cx < 0 || cy < 0) continue;
            if (!soak_open(w, mc, d->max_slope, cx, cy)) continue;
            o++;
            if (Occ_QueryTile(w, cx, cy, (int)u->player_id, 0) == 1) q++;
        }
    }
    *open = o;
    *occupied = q;
}

/* What refused this unit's step on a tick the clock was running. */
static void soak_note_cause(const GameWorld *w, const Unit *u, int h,
                            const UnitDef *d, const SoakUnitObs *ob,
                            SoakCause *c) {
    memset(c, 0, sizeof(*c));
    if (u->world_x == ob->last_x && u->world_y == ob->last_y) c->t_still++;
    if (u->path_pending) c->t_pending++;
    if (u->path_failed && u->path_len == 0) c->t_noroute++;
    if (ob->last_path_len > 0 && u->path_len == 0) c->replans++;
    if (u->route_flags & (UNIT_ROUTE_BLOCKED | UNIT_ROUTE_BLOCKED_HARD)) {
        /* One step ahead along the heading: what stands there. */
        int32_t px = u->world_x + (int32_t)(sinf(u->heading) * 18.0f);
        int32_t py = u->world_y - (int32_t)(cosf(u->heading) * 18.0f);
        int tx = px / 16, ty = py / 16;
        if (!soak_open(w, soak_mc(w, d), d->max_slope, tx, ty)) c->t_terrain++;
        else if (Occ_QueryTile(w, tx, ty, (int)u->player_id, h + 1) == 1) c->t_unit++;
        else c->t_blocked++;
    } else {
        c->t_walking++;
    }
}

static void soak_add_cause(SoakCause *a, const SoakCause *b) {
    a->t_terrain += b->t_terrain;
    a->t_unit    += b->t_unit;
    a->t_blocked += b->t_blocked;
    a->t_noroute += b->t_noroute;
    a->t_pending += b->t_pending;
    a->t_walking += b->t_walking;
    a->t_still   += b->t_still;
    a->replans   += b->replans;
}

static void soak_record_offender(const GameWorld *w, const Unit *units, int n,
                                 int h, int32_t ticks, int32_t gx, int32_t gy) {
    const Unit *u = &units[h];
    const UnitDef *d = Units_GetDef(u->def_idx);
    if (!d) return;
    /* One row per unit: keep the worst episode it had. */
    for (int i = 0; i < g_ctr.off_count; i++) {
        if (g_ctr.off[i].handle != h) continue;
        if (ticks <= g_ctr.off[i].ticks) return;
        g_ctr.off_count--;
        memmove(&g_ctr.off[i], &g_ctr.off[i + 1],
                (size_t)(g_ctr.off_count - i) * sizeof(SoakOffender));
        break;
    }
    int slot = g_ctr.off_count;
    if (slot >= SOAK_OFF_MAX) {
        int worst = 0;
        for (int i = 1; i < SOAK_OFF_MAX; i++)
            if (g_ctr.off[i].ticks < g_ctr.off[worst].ticks) worst = i;
        if (g_ctr.off[worst].ticks >= ticks) return;
        slot = worst;
    } else {
        g_ctr.off_count++;
    }
    SoakOffender *o = &g_ctr.off[slot];
    memset(o, 0, sizeof(*o));
    o->ticks = ticks;
    o->handle = h;
    o->cmd_kind = u->cmd_kind;
    o->x = u->world_x; o->y = u->world_y;
    o->goal_x = gx; o->goal_y = gy;
    o->fx = u->occ_fx; o->fz = u->occ_fz;
    soak_neighbourhood(w, u, d, &o->open_7x7, &o->occupied_7x7);
    {
        TAK_Path p;
        int r = TAK_PathPlanForMoveClass(w, u->world_x, u->world_y, gx, gy,
                                         soak_mc(w, d), d->max_slope,
                                         (int)u->player_id, &p);
        o->route_left = r > 0 ? r : -1;
    }
    o->cause = g_obs[h].cause;
    strncpy(o->name, d->unitname, sizeof(o->name) - 1);
    strncpy(o->scen, g_scen, sizeof(o->scen) - 1);
}

/* One sample. Call once per simulation tick. */
static void soak_sample(const GameWorld *w) {
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    if (n > SOAK_MAX_UNITS) n = SOAK_MAX_UNITS;
    for (int h = 0; h < n; h++) {
        const Unit *u = &units[h];
        SoakUnitObs *ob = &g_obs[h];
        SoakOrderKey k;
        memset(&k, 0, sizeof(k));
        k.cmd_kind = u->cmd_kind;
        k.target = u->target;
        k.cmd_x = u->cmd_x; k.cmd_y = u->cmd_y;
        k.patrol_x = u->patrol_x; k.patrol_y = u->patrol_y;
        k.build_target = u->build_target;
        k.stable_id = u->stable_id;
        if (!ob->have || !key_same(&ob->key, &k)) {
            if (k.cmd_kind != UNIT_CMD_NONE) g_ctr.orders_seen++;
            memset(ob, 0, sizeof(*ob));
            ob->have = 1;
            ob->key = k;
            ob->best_dist = INT64_MAX;
            ob->best_work = -1e30;
            ob->order_start_tick = g_tick;
            ob->anchor_x = u->world_x;
            ob->anchor_y = u->world_y;
        }
        /* A position mark every 30 ticks: five of them span the 150
         * ticks the first rung is judged over. */
        if ((g_tick % 30) == 0) {
            if (ob->hist_n < 5) {
                ob->hist_x[ob->hist_n] = u->world_x;
                ob->hist_y[ob->hist_n] = u->world_y;
                ob->hist_n++;
            } else {
                memmove(ob->hist_x, ob->hist_x + 1, 4 * sizeof(int32_t));
                memmove(ob->hist_y, ob->hist_y + 1, 4 * sizeof(int32_t));
                ob->hist_x[4] = u->world_x;
                ob->hist_y[4] = u->world_y;
            }
        }
        int net150 = ob->hist_n > 0
            ? (int)soak_seg(u->world_x, u->world_y, ob->hist_x[0], ob->hist_y[0])
            : 0;
        SoakMeas m;
        soak_measure(w, units, n, h, &m);
        if (m.excluded || m.arrived) {
            ob->anchor_x = u->world_x;
            ob->anchor_y = u->world_y;
            ob->last_x = u->world_x;
            ob->last_y = u->world_y;
            ob->last_path_len = u->path_len;
            /* Legitimately waiting or served. A unit that got there
             * after stalling has recovered. */
            if (m.arrived && ob->rung > 0) { g_ctr.recovered++; ob->rung = 0; }
            ob->clock = 0;
            continue;
        }
        g_ctr.candidate_ticks++;
        int improved = 0;
        if (m.dist >= 0) {
            /* Two pixels of real ground closed: below that a unit is
             * not getting anywhere. */
            if (m.dist < ob->best_dist - 2) {
                ob->best_dist = m.dist;
                improved = 1;
            }
        } else if (m.work > ob->best_work) {
            ob->best_work = m.work;
            improved = 1;
        }
        if (!improved &&
            soak_d2(u->world_x, u->world_y, ob->anchor_x, ob->anchor_y) >=
            (int64_t)48 * 48) {
            /* Real ground covered since the last check. A unit that is
             * moving is not stuck, even when a new plan made the route
             * ahead longer than the best it had. */
            ob->anchor_x = u->world_x;
            ob->anchor_y = u->world_y;
            improved = 1;
        }
        if (improved) {
            ob->last_x = u->world_x;
            ob->last_y = u->world_y;
            ob->last_path_len = u->path_len;
            if (ob->rung > 0) { g_ctr.recovered++; ob->rung = 0; }
            ob->clock = 0;
            continue;
        }
        /* Not closing on the goal, but still covering real ground: the
         * long way round, or a circle. That is a route quality
         * complaint, not a unit that can do nothing, and the owner's
         * bar is the second one. Counted apart so it stays visible. */
        if (net150 >= 48) {
            g_ctr.noprog_ticks++;
            ob->last_x = u->world_x;
            ob->last_y = u->world_y;
            ob->last_path_len = u->path_len;
            if (ob->rung > 0) { g_ctr.recovered++; ob->rung = 0; }
            ob->clock = 0;
            continue;
        }
        ob->travelled += soak_seg(ob->last_x, ob->last_y,
                                  u->world_x, u->world_y);
        {
            SoakCause tick;
            soak_note_cause(w, u, h, Units_GetDef(u->def_idx), ob, &tick);
            soak_add_cause(&ob->cause, &tick);
            soak_add_cause(&g_ctr.total_cause, &tick);
        }
        ob->last_x = u->world_x;
        ob->last_y = u->world_y;
        ob->last_path_len = u->path_len;
        ob->clock++;
        g_ctr.stall_ticks_total++;
        if (ob->clock > g_ctr.longest) g_ctr.longest = ob->clock;
        if (ob->clock == SOAK_R1) {
            g_ctr.stalls_detected++;
            ob->rung = 1;
            g_ctr.last_handle = h;
            g_ctr.last_tick = g_tick;
            g_ctr.last_rung = 1;
        } else if (ob->clock == SOAK_R2) { g_ctr.r2++; ob->rung = 2; g_ctr.last_rung = 2; }
        else if (ob->clock == SOAK_R3) { g_ctr.r3++; ob->rung = 3; g_ctr.last_rung = 3; }
        else if (ob->clock == SOAK_R4) { g_ctr.r4++; ob->rung = 4; g_ctr.last_rung = 4; }
        else if (ob->clock == SOAK_R5) { g_ctr.r5++; ob->rung = 5; g_ctr.last_rung = 5; }
        if (ob->clock >= SOAK_R2 && (ob->clock % 60) == 0) {
            soak_record_offender(w, units, n, h, ob->clock, m.goal_x, m.goal_y);
            for (int i = 0; i < g_ctr.off_count; i++) {
                if (g_ctr.off[i].handle != h) continue;
                g_ctr.off[i].travelled = ob->travelled;
                g_ctr.off[i].net150 = net150;
            }
        }
    }
    g_tick++;
}

/* Run ticks, sampling each one. */
static void soak_run(const GameWorld *w, int ticks) {
    for (int i = 0; i < ticks; i++) {
        InGame_DebugRunSimTicks(1);
        soak_sample(w);
    }
}

static const char *cmd_name(int k) {
    switch (k) {
    case UNIT_CMD_MOVE: return "MOVE";
    case UNIT_CMD_ATTACK: return "ATTACK";
    case UNIT_CMD_BUILD: return "BUILD";
    case UNIT_CMD_PATROL: return "PATROL";
    case UNIT_CMD_GUARD: return "GUARD";
    case UNIT_CMD_REPAIR: return "REPAIR";
    case UNIT_CMD_RECLAIM: return "RECLAIM";
    case UNIT_CMD_LOAD: return "LOAD";
    case UNIT_CMD_UNLOAD: return "UNLOAD";
    case UNIT_CMD_ATTACK_GROUND: return "ATKGND";
    case UNIT_CMD_RESURRECT: return "RESURRECT";
    case UNIT_CMD_BOARD: return "BOARD";
    default: return "NONE";
    }
}

/* Did each offender end the run able to act: no order left, or an order
 * it is making progress on again. */
static void soak_mark_escapes(void) {
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    for (int i = 0; i < g_ctr.off_count; i++) {
        int h = g_ctr.off[i].handle;
        if (h < 0 || h >= n) continue;
        g_ctr.off[i].escaped = (units[h].cmd_kind == UNIT_CMD_NONE) ||
                               (g_obs[h].clock < SOAK_R1);
    }
}

/* ── Issue #60 as one assertion ────────────────────────────────────
 *
 * "A unit must never sit stuck against a wall or another unit for
 * good, and it must detect the stall and take another route to the
 * same destination." The soak's clock already counts exactly the
 * ticks that rule is about: it runs only while a unit has a live
 * order, is not legitimately waiting, is not closing on its goal and
 * has covered less than 48 px of ground in the last 150 ticks. So the
 * rule is the longest that clock ever reached, over every scenario
 * this process ran, plus nobody left holding at the end.
 *
 * The bound is the mover's own ladder with room to spare: it
 * escalates every UNIT_NO_PROGRESS_TICKS (240) and has four rungs
 * before it gives an order up, so a unit that is going to recover has
 * done so inside 1200 ticks and one that is not has had its order
 * ended. */
#define SOAK_SIXTY 1500

static long g_sixty_worst;          /* ticks */
static long g_sixty_r5;             /* runs past the soak's own top rung */
static long g_sixty_held;           /* offenders still held at the end */
static long g_sixty_scenarios;
static char g_sixty_where[96];

static void soak_sixty_take(void) {
    g_sixty_scenarios++;
    g_sixty_r5 += g_ctr.r5;
    if (g_ctr.longest > g_sixty_worst) {
        g_sixty_worst = g_ctr.longest;
        snprintf(g_sixty_where, sizeof(g_sixty_where), "%s", g_scen);
    }
    for (int i = 0; i < g_ctr.off_count; i++)
        if (!g_ctr.off[i].escaped) g_sixty_held++;
}

/* 1 when every scenario so far kept the rule. Prints why when not. */
static int soak_sixty_ok(void) {
    if (g_sixty_scenarios <= 0) {
        printf("\n    #60: no scenario ran, nothing was checked\n");
        return 0;
    }
    if (g_sixty_worst <= SOAK_SIXTY && g_sixty_held == 0) return 1;
    printf("\n    #60 BROKEN over %ld scenario(s): worst %ld ticks with a "
           "live order and no ground covered (%s), %ld past the soak's top "
           "rung, %ld unit(s) still held when the run ended\n",
           g_sixty_scenarios, g_sixty_worst, g_sixty_where,
           g_sixty_r5, g_sixty_held);
    return 0;
}

static void soak_report(void) {
    /* Sanity: the simulation really advanced. A battle that reached
     * its end screen stops ticking, and every counter would then
     * read zero for the wrong reason. */
    const GameWorld *w = World_Get();
    int sim_ticks = w ? w->skirmish_elapsed_ticks +
                        w->mission_elapsed_ticks : -1;
    printf("\n    [%s] ticks=%d sim_ticks=%d cand_ticks=%lld orders=%ld\n",
           g_scen, g_tick, sim_ticks,
           (long long)g_ctr.candidate_ticks, g_ctr.orders_seen);
    if (w && w->skirmish_stats_open)
        printf("      WARNING: the battle ended, the sim stopped early\n");
    printf("      stalls=%ld r2=%ld r3=%ld r4=%ld r5(>=1200t)=%ld "
           "recovered=%ld longest=%ld stall_ticks=%lld\n",
           g_ctr.stalls_detected, g_ctr.r2, g_ctr.r3, g_ctr.r4, g_ctr.r5,
           g_ctr.recovered, g_ctr.longest, (long long)g_ctr.stall_ticks_total);
    printf("      not-closing-but-moving ticks=%lld\n",
           (long long)g_ctr.noprog_ticks);
    {
        const SoakCause *c = &g_ctr.total_cause;
        printf("      why (stalled ticks): terrain=%d unit=%d refused-clear=%d "
               "no-route=%d pending=%d walking=%d still=%d replans=%d\n",
               c->t_terrain, c->t_unit, c->t_blocked, c->t_noroute,
               c->t_pending, c->t_walking, c->t_still, c->replans);
    }
    for (int i = 0; i < g_ctr.off_count; i++) {
        SoakOffender *o = &g_ctr.off[i];
        printf("      worst h=%-4d %-12s %-7s %5dt at %6d,%-6d goal %6d,%-6d "
               "fp=%dx%d open7=%2d occ7=%2d route=%3d %s\n",
               o->handle, o->name, cmd_name(o->cmd_kind), o->ticks,
               o->x, o->y, o->goal_x, o->goal_y, o->fx, o->fz,
               o->open_7x7, o->occupied_7x7, o->route_left,
               o->escaped ? "free-at-end" : "STILL-HELD");
        printf("           moved %lld px while stalled, %d px of ground in "
               "the last 150 ticks\n", (long long)o->travelled, o->net150);
        printf("           why: terrain=%d unit=%d refused-clear=%d no-route=%d "
               "pending=%d walking=%d still=%d replans=%d\n",
               o->cause.t_terrain, o->cause.t_unit, o->cause.t_blocked,
               o->cause.t_noroute, o->cause.t_pending, o->cause.t_walking,
               o->cause.t_still, o->cause.replans);
    }
    soak_sixty_take();
    fflush(stdout);
}

static uint32_t g_rng = 0x12345678u;
static uint32_t soak_rand(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

/* A walkable spot for this def, searched outward from (cx, cy). */
static int soak_find_open(const GameWorld *w, const UnitDef *d,
                          int32_t cx, int32_t cy, int need,
                          int32_t *ox, int32_t *oy) {
    const MoveClassDef *mc = soak_mc(w, d);
    int tw = w->map_pixels_w / 16, th = w->map_pixels_h / 16;
    int tx0 = cx / 16, ty0 = cy / 16;
    for (int r = 0; r < 140; r++) {
        int per = r == 0 ? 1 : 8 * r;
        for (int k = 0; k < per; k++) {
            int tx, ty;
            if (r == 0) { tx = tx0; ty = ty0; }
            else if (k < 2 * r)      { tx = tx0 - r + k;              ty = ty0 - r; }
            else if (k < 4 * r)      { tx = tx0 + r;                  ty = ty0 - r + (k - 2 * r); }
            else if (k < 6 * r)      { tx = tx0 + r - (k - 4 * r);    ty = ty0 + r; }
            else                     { tx = tx0 - r;                  ty = ty0 + r - (k - 6 * r); }
            if (tx < 2 || ty < 2 || tx >= tw - 2 || ty >= th - 2) continue;
            if (TAK_PathClearanceAt(w, mc, d->max_slope, tx, ty) >= need) {
                *ox = tx * 16 + 8; *oy = ty * 16 + 8;
                return 1;
            }
        }
    }
    return 0;
}

/* How many tiles wide the open ground is through (tx, ty) across the
 * axis (dx, dy), looking at most `cap` tiles each way. */
static int soak_width(const GameWorld *w, const MoveClassDef *mc, int slope,
                      int tx, int ty, int dx, int dy, int cap) {
    int n = 1, i;
    for (i = 1; i <= cap; i++) {
        if (!soak_open(w, mc, slope, tx + dx * i, ty + dy * i)) break;
        n++;
    }
    for (i = 1; i <= cap; i++) {
        if (!soak_open(w, mc, slope, tx - dx * i, ty - dy * i)) break;
        n++;
    }
    return n;
}

/* A neck of land: walkable ground at most `max_w` tiles across, with
 * open ground four tiles out along the corridor both ways. Water on one
 * side and rock on the other is the shape the owner screenshotted. */
static int soak_find_neck(const GameWorld *w, const UnitDef *d, int max_w,
                          int skip, int32_t *ax, int32_t *ay,
                          int32_t *bx, int32_t *by) {
    const MoveClassDef *mc = soak_mc(w, d);
    int tw = w->map_pixels_w / 16, th = w->map_pixels_h / 16;
    int seen = 0;
    for (int ty = 8; ty < th - 8; ty++) {
        for (int tx = 8; tx < tw - 8; tx++) {
            if (!soak_open(w, mc, d->max_slope, tx, ty)) continue;
            int wv = soak_width(w, mc, d->max_slope, tx, ty, 0, 1, max_w + 1);
            int wh = soak_width(w, mc, d->max_slope, tx, ty, 1, 0, max_w + 1);
            int dx, dy;
            if (wv <= max_w && wh > max_w)      { dx = 1; dy = 0; }
            else if (wh <= max_w && wv > max_w) { dx = 0; dy = 1; }
            else continue;
            int ok = 1;
            for (int i = 1; i <= 4; i++) {
                if (!soak_open(w, mc, d->max_slope, tx + dx * i, ty + dy * i) ||
                    !soak_open(w, mc, d->max_slope, tx - dx * i, ty - dy * i)) {
                    ok = 0;
                    break;
                }
            }
            if (!ok) continue;
            if (seen++ < skip) continue;
            *ax = (tx - 5 * dx) * 16 + 8; *ay = (ty - 5 * dy) * 16 + 8;
            *bx = (tx + 5 * dx) * 16 + 8; *by = (ty + 5 * dy) * 16 + 8;
            return 1;
        }
    }
    return 0;
}

/* Wall off open ground, leaving one gap a single unit wide, and hand
 * back the two sides. Maps whose own ground has no neck still get the
 * wedge, and the built one is the same shape every run. */
static int soak_build_neck(const GameWorld *w, const UnitDef *sd,
                           int wall_def, int32_t cx, int32_t cy,
                           int32_t *ax, int32_t *ay,
                           int32_t *bx, int32_t *by) {
    int32_t ox, oy;
    if (wall_def < 0) return 0;
    if (!soak_find_open(w, sd, cx, cy, 8, &ox, &oy)) return 0;
    int any = 0;
    for (int k = -6; k <= 6; k++) {
        if (k == 0) continue;                 /* the gap */
        if (Units_Spawn(wall_def, 1, 0, ox + k * 32, oy) >= 0) any = 1;
    }
    if (!any) return 0;
    *ax = ox + 16; *ay = oy - 6 * 32;
    *bx = ox + 16; *by = oy + 6 * 32;
    return 1;
}

/* A tile this def cannot stand on at all: water or cliff. */
static int soak_find_blocked(const GameWorld *w, const UnitDef *d,
                             int32_t cx, int32_t cy, int32_t *ox, int32_t *oy) {
    const MoveClassDef *mc = soak_mc(w, d);
    int tw = w->map_pixels_w / 16, th = w->map_pixels_h / 16;
    static const int dir[8][2] = {
        {1,0},{0,1},{-1,0},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}
    };
    for (int r = 4; r < 220; r += 2) {
        for (int k = 0; k < 8; k++) {
            int tx = cx / 16 + dir[k][0] * r, ty = cy / 16 + dir[k][1] * r;
            if (tx < 1 || ty < 1 || tx >= tw - 1 || ty >= th - 1) continue;
            if (TAK_PathClearanceAt(w, mc, d->max_slope, tx, ty) == 0) {
                *ox = tx * 16 + 8; *oy = ty * 16 + 8;
                return 1;
            }
        }
    }
    return 0;
}

/* A walkable tile with no route from (sx, sy): an island or a sealed
 * pocket, an order that can never be served. */
static int soak_find_unreachable(const GameWorld *w, const UnitDef *d,
                                 int32_t sx, int32_t sy,
                                 int32_t *ox, int32_t *oy) {
    const MoveClassDef *mc = soak_mc(w, d);
    int tw = w->map_pixels_w / 16, th = w->map_pixels_h / 16;
    TAK_Path p;
    for (int tries = 0; tries < 800; tries++) {
        int tx = 3 + (int)(soak_rand() % (uint32_t)(tw - 6));
        int ty = 3 + (int)(soak_rand() % (uint32_t)(th - 6));
        if (TAK_PathClearanceAt(w, mc, d->max_slope, tx, ty) < 1) continue;
        int32_t gx = tx * 16 + 8, gy = ty * 16 + 8;
        if (soak_d2(sx, sy, gx, gy) < (int64_t)600 * 600) continue;
        if (TAK_PathPlanForMoveClass(w, sx, sy, gx, gy, mc, d->max_slope,
                                     1, &p) <= 0) {
            *ox = gx; *oy = gy;
            return 1;
        }
    }
    return 0;
}

static void soak_passive_all(void) {
    int n = 0;
    const Unit *u = Units_GetActive(&n);
    for (int i = 0; i < n; i++)
        if (u[i].alive == UNIT_ALIVE_ACTIVE)
            Units_DebugSetAggro(i, UNIT_AGGRO_PASSIVE);
}

/* The biggest-footprint mobile land def the registry has. */
static int soak_big_mobile_def(const GameWorld *w) {
    int best = -1, best_fp = 1;
    for (int i = 0; i < Units_GetDefCount(); i++) {
        const UnitDef *d = Units_GetDef(i);
        if (!d || d->max_velocity <= 0.0f || d->can_fly) continue;
        const MoveClassDef *mc = soak_mc(w, d);
        int fp = mc ? mc->footprint_x : 1;
        if (fp > best_fp) { best_fp = fp; best = i; }
    }
    return best;
}

/* ── scenarios ───────────────────────────────────────────────────── */

/* Plain moves across open ground. Nothing should stall. */
TEST(soak_open_ground_control) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    if (soak_boot(&platform, "Two Castles", "aramon", 0, &world) != 0) {
        printf("SKIP (map load) ");
        goto out;
    }
    {
    int sword = Units_FindDefByName("ARASWORD");
    if (sword < 0) { printf("SKIP (no unit defs) "); goto out; }
    const UnitDef *sd = Units_GetDef(sword);
    const MoveClassDef *mc = soak_mc(world, sd);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    if (n <= 0) { printf("SKIP (no spawned units) "); goto out; }
    int32_t bx = units[0].world_x, by = units[0].world_y;
    g_rng = 0xA11CEu;
    soak_reset("open-ground control");
    int made = 0, hs[16];
    int32_t gx[16], gy[16];
    for (int i = 0; i < 16; i++) {
        int32_t sx, sy;
        if (!soak_find_open(world, sd, bx + ((i % 4) - 2) * 96,
                            by + ((i / 4) - 2) * 96, 2, &sx, &sy)) continue;
        int h = Units_Spawn(sword, 1, 0, sx, sy);
        if (h < 0) continue;
        /* A goal A* already says is reachable: this scenario is about
         * open ground, not about pockets. */
        TAK_Path p;
        int32_t tx = sx + (((int)(soak_rand() % 3)) - 1) * 700;
        int32_t ty = sy + (((int)(soak_rand() % 3)) - 1) * 700;
        int pn = TAK_PathPlanForMoveClass(world, sx, sy, tx, ty, mc,
                                          sd->max_slope, 1, &p);
        if (pn <= 0) { Units_DebugKillHandle(h); continue; }
        hs[made] = h;
        gx[made] = p.x[pn - 1];
        gy[made] = p.y[pn - 1];
        made++;
    }
    soak_passive_all();
    for (int i = 0; i < made; i++) Units_CommandMoveUnit(hs[i], gx[i], gy[i]);
    soak_run(world, 2400);
    soak_mark_escapes();
    printf("(%d units) ", made);
    soak_report();
    }
out:
    soak_unload(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* A crowd funnelled through a one-cell gap, on several shipped maps. */
static int soak_gap_on(TAK_Platform *platform, const char *map,
                       const char *kingdom) {
    GameWorld *world = NULL;
    char tag[48];
    snprintf(tag, sizeof(tag), "%s one-cell gap", map);
    if (soak_boot(platform, map, kingdom, 0, &world) != 0) {
        printf("\n    [%s] SKIP (map load)", tag);
        return 0;
    }
    int sword = Units_FindDefByName("ARASWORD");
    const UnitDef *sd = sword >= 0 ? Units_GetDef(sword) : NULL;
    if (!sd) { soak_unload(platform); return 0; }
    int32_t ax, ay, bx, by;
    int wall = Units_FindDefByName("ARAWALL");
    int built_neck = 0;
    if (!soak_find_neck(world, sd, 1, 0, &ax, &ay, &bx, &by)) {
        int n0 = 0;
        const Unit *u0 = Units_GetActive(&n0);
        if (n0 <= 0 || !soak_build_neck(world, sd, wall,
                                        u0[0].world_x + 700,
                                        u0[0].world_y + 700,
                                        &ax, &ay, &bx, &by)) {
            printf("\n    [%s] NO SITE: no neck on the map and none could be built", tag);
            soak_unload(platform);
            return 0;
        }
        built_neck = 1;
    }
    printf("\n    [%s] %s neck", tag, built_neck ? "built" : "map own");
    soak_reset(tag);
    int made = 0, hs[24];
    for (int i = 0; i < 24; i++) {
        int32_t sx, sy;
        if (!soak_find_open(world, sd, ax + ((i % 5) - 2) * 20,
                            ay + ((i / 5) - 2) * 20, 1, &sx, &sy)) continue;
        int h = Units_Spawn(sword, 1, 0, sx, sy);
        if (h >= 0) hs[made++] = h;
    }
    soak_passive_all();
    for (int i = 0; i < made; i++) Units_CommandMoveUnit(hs[i], bx, by);
    soak_run(world, 3600);
    soak_mark_escapes();
    printf("\n    (%d units through one cell at %d,%d) ", made, ax, ay);
    soak_report();
    soak_unload(platform);
    return 1;
}

TEST(soak_one_cell_gap_crowd) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    int ran = 0;
    ran += soak_gap_on(&platform, "CASTLE", "aramon");
    ran += soak_gap_on(&platform, "Two Castles", "aramon");
    ran += soak_gap_on(&platform, "Athri Cay", "aramon");
    ran += soak_gap_on(&platform, "Angvir's Maze", "aramon");
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
    /* A wedge test that quietly found nowhere to run reads as a
     * pass, which is worse than no test at all. */
    ASSERT(ran > 0);
    ASSERT(soak_sixty_ok());
}

/* A narrow strip of land between water and rock: a column each way,
 * meeting head on in the middle. */
TEST(soak_narrow_strip) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    static const char *maps[3] = { "CASTLE", "Athri Cay", "Sea Dragon Spine" };
    int ran = 0;
    for (int mi = 0; mi < 3; mi++) {
        GameWorld *world = NULL;
        char tag[48];
        snprintf(tag, sizeof(tag), "%s narrow strip", maps[mi]);
        if (soak_boot(&platform, maps[mi], "aramon", 0, &world) != 0) {
            printf("\n    [%s] SKIP (map load)", tag);
            continue;
        }
        int sword = Units_FindDefByName("ARASWORD");
        const UnitDef *sd = sword >= 0 ? Units_GetDef(sword) : NULL;
        int32_t ax, ay, bx, by;
        if (!sd) { soak_unload(&platform); continue; }
        int built_neck = 0;
        if (!soak_find_neck(world, sd, 2, 3, &ax, &ay, &bx, &by)) {
            int n0 = 0;
            const Unit *u0 = Units_GetActive(&n0);
            int wall = Units_FindDefByName("ARAWALL");
            if (n0 <= 0 || !soak_build_neck(world, sd, wall,
                                            u0[0].world_x - 700,
                                            u0[0].world_y + 700,
                                            &ax, &ay, &bx, &by)) {
                printf("\n    [%s] NO SITE: no strip on the map and none could be built", tag);
                soak_unload(&platform);
                continue;
            }
            built_neck = 1;
        }
        ran++;
        printf("\n    [%s] %s strip", tag, built_neck ? "built" : "map own");
        soak_reset(tag);
        int made = 0, hs[16];
        int32_t gx[16], gy[16];
        for (int i = 0; i < 8; i++) {
            int32_t sx, sy;
            if (!soak_find_open(world, sd, ax, ay, 1, &sx, &sy)) break;
            int h = Units_Spawn(sword, 1, 0, sx + (i % 3) * 18, sy + (i / 3) * 18);
            if (h < 0) break;
            hs[made] = h; gx[made] = bx; gy[made] = by; made++;
        }
        for (int i = 0; i < 8; i++) {
            int32_t sx, sy;
            if (!soak_find_open(world, sd, bx, by, 1, &sx, &sy)) break;
            int h = Units_Spawn(sword, 1, 0, sx + (i % 3) * 18, sy + (i / 3) * 18);
            if (h < 0) break;
            hs[made] = h; gx[made] = ax; gy[made] = ay; made++;
        }
        soak_passive_all();
        for (int i = 0; i < made; i++) Units_CommandMoveUnit(hs[i], gx[i], gy[i]);
        soak_run(world, 3600);
        soak_mark_escapes();
        printf("\n    (%d units, strip at %d,%d) ", made, ax, ay);
        soak_report();
        soak_unload(&platform);
    }
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
    ASSERT(ran > 0);
    ASSERT(soak_sixty_ok());
}

/* A walled courtyard with one entrance. The units go in, the gap is
 * walled shut behind them, then they are ordered back out. */
TEST(soak_walled_courtyard) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    if (soak_boot(&platform, "Two Castles", "aramon", 0, &world) != 0) {
        printf("SKIP (map load) ");
        goto out;
    }
    {
    int wall = Units_FindDefByName("ARAWALL");
    int sword = Units_FindDefByName("ARASWORD");
    if (wall < 0 || sword < 0) { printf("SKIP (no wall def) "); goto out; }
    const UnitDef *sd = Units_GetDef(sword);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    if (n <= 0) { printf("SKIP (no spawned units) "); goto out; }
    int32_t cx, cy;
    if (!soak_find_open(world, sd, units[0].world_x + 600,
                        units[0].world_y + 600, 8, &cx, &cy)) {
        printf("SKIP (no courtyard site) ");
        goto out;
    }
    soak_reset("walled courtyard, one entrance");
    /* A ring of 2x2 wall segments with one tile left open on top. */
    const int R = 3;
    int32_t plugx = cx, plugy = cy - R * 32;
    for (int i = -R; i <= R; i++) {
        int32_t px[4], py[4];
        px[0] = cx + i * 32; py[0] = cy - R * 32;
        px[1] = cx + i * 32; py[1] = cy + R * 32;
        px[2] = cx - R * 32; py[2] = cy + i * 32;
        px[3] = cx + R * 32; py[3] = cy + i * 32;
        for (int k = 0; k < 4; k++) {
            if (k == 0 && i == 0) continue;      /* the entrance */
            Units_Spawn(wall, 1, 0, px[k], py[k]);
        }
    }
    int made = 0, hs[10];
    for (int i = 0; i < 8; i++) {
        int32_t sx, sy;
        if (!soak_find_open(world, sd, cx, cy - (R + 5) * 32, 1, &sx, &sy)) break;
        int h = Units_Spawn(sword, 1, 0, sx + (i % 3) * 18, sy + (i / 3) * 18);
        if (h < 0) break;
        hs[made++] = h;
    }
    soak_passive_all();
    for (int i = 0; i < made; i++) Units_CommandMoveUnit(hs[i], cx, cy);
    soak_run(world, 1800);
    {
        int inside = 0;
        units = Units_GetActive(&n);
        for (int i = 0; i < made; i++)
            if (soak_d2(units[hs[i]].world_x, units[hs[i]].world_y, cx, cy)
                <= (int64_t)(R * 32) * (R * 32)) inside++;
        printf("(%d of %d got into the courtyard) ", inside, made);
    }
    soak_report();
    /* Seal them in, then order them out through a wall with no door. */
    soak_reset("courtyard sealed behind them");
    Units_Spawn(wall, 1, 0, plugx, plugy);
    {
        int32_t outx = cx, outy = cy - (R + 7) * 32;
        for (int i = 0; i < made; i++) Units_CommandMoveUnit(hs[i], outx, outy);
    }
    soak_run(world, 2400);
    soak_mark_escapes();
    {
        int held = 0;
        units = Units_GetActive(&n);
        for (int i = 0; i < made; i++)
            if (units[hs[i]].cmd_kind != UNIT_CMD_NONE) held++;
        printf("(%d of %d still hold the way-out order) ", held, made);
    }
    soak_report();
    }
out:
    soak_unload(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* A large unit sent where only a small one fits. */
TEST(soak_large_unit_small_gap) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    if (soak_boot(&platform, "CASTLE", "aramon", 0, &world) != 0) {
        printf("SKIP (map load) ");
        goto out;
    }
    {
    int big = soak_big_mobile_def(world);
    int sword = Units_FindDefByName("ARASWORD");
    if (big < 0 || sword < 0) { printf("SKIP (no defs) "); goto out; }
    const UnitDef *bd = Units_GetDef(big);
    const UnitDef *sd = Units_GetDef(sword);
    const MoveClassDef *bmc = soak_mc(world, bd);
    int32_t ax, ay, bx, by;
    if (!soak_find_neck(world, sd, 1, 0, &ax, &ay, &bx, &by)) {
        int n0 = 0;
        const Unit *u0 = Units_GetActive(&n0);
        int wall = Units_FindDefByName("ARAWALL");
        if (n0 <= 0 || !soak_build_neck(world, sd, wall,
                                        u0[0].world_x + 900,
                                        u0[0].world_y - 900,
                                        &ax, &ay, &bx, &by)) {
            printf("NO SITE: no gap on the map and none could be built ");
            goto out;
        }
        printf("(built gap) ");
    }
    soak_reset("large unit into a one-cell gap");
    printf("(%s footprint %d tiles) ", bd->unitname, bmc ? bmc->footprint_x : 1);
    int made = 0, hs[6];
    for (int i = 0; i < 4; i++) {
        int32_t sx, sy;
        if (!soak_find_open(world, bd, ax, ay, 1, &sx, &sy)) break;
        int h = Units_Spawn(big, 1, 0, sx + i * 48, sy);
        if (h < 0) break;
        hs[made++] = h;
    }
    soak_passive_all();
    for (int i = 0; i < made; i++) Units_CommandMoveUnit(hs[i], bx, by);
    soak_run(world, 3600);
    soak_mark_escapes();
    printf("(%d large units) ", made);
    soak_report();
    }
out:
    soak_unload(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Ground the unit cannot occupy at all, and a destination with no route
 * to it anywhere on the map. The owner's bar: the order must end and
 * the unit must take a fresh one. */
TEST(soak_unreachable_destination) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    if (soak_boot(&platform, "Athri Cay", "aramon", 0, &world) != 0) {
        printf("SKIP (map load) ");
        goto out;
    }
    {
    int sword = Units_FindDefByName("ARASWORD");
    if (sword < 0) { printf("SKIP (no unit defs) "); goto out; }
    const UnitDef *sd = Units_GetDef(sword);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    if (n <= 0) { printf("SKIP (no spawned units) "); goto out; }
    int32_t bx = units[0].world_x, by = units[0].world_y;
    g_rng = 0xBEEF01u;
    soak_reset("unreachable destination");
    int made = 0, hs[12], kinds[12];
    int32_t gx[12], gy[12];
    for (int i = 0; i < 10; i++) {
        int32_t sx, sy, tx, ty;
        if (!soak_find_open(world, sd, bx + (i - 5) * 64, by, 1, &sx, &sy)) continue;
        int h = Units_Spawn(sword, 1, 0, sx, sy);
        if (h < 0) continue;
        int kind;
        if (i % 2 == 0) {
            if (!soak_find_blocked(world, sd, sx, sy, &tx, &ty)) {
                Units_DebugKillHandle(h); continue;
            }
            kind = 0;   /* ground it cannot stand on */
        } else {
            if (!soak_find_unreachable(world, sd, sx, sy, &tx, &ty)) {
                Units_DebugKillHandle(h); continue;
            }
            kind = 1;   /* walkable, but no route to it */
        }
        hs[made] = h; gx[made] = tx; gy[made] = ty; kinds[made] = kind;
        made++;
    }
    soak_passive_all();
    for (int i = 0; i < made; i++) Units_CommandMoveUnit(hs[i], gx[i], gy[i]);
    soak_run(world, 2400);
    soak_mark_escapes();
    {
        int held = 0, held_kind[2] = { 0, 0 };
        units = Units_GetActive(&n);
        for (int i = 0; i < made; i++)
            if (units[hs[i]].cmd_kind != UNIT_CMD_NONE) {
                held++;
                held_kind[kinds[i]]++;
            }
        printf("(%d units, %d still holding after 2400t: %d on blocked ground, "
               "%d with no route) ", made, held, held_kind[0], held_kind[1]);
    }
    soak_report();
    /* The second half of the owner's bar: a fresh order afterwards. */
    soak_reset("fresh order after an unreachable one");
    {
        int issued = 0;
        units = Units_GetActive(&n);
        for (int i = 0; i < made; i++) {
            int32_t nx, ny;
            if (!soak_find_open(world, sd, units[hs[i]].world_x + 250,
                                units[hs[i]].world_y, 1, &nx, &ny)) continue;
            Units_CommandMoveUnit(hs[i], nx, ny);
            gx[i] = nx; gy[i] = ny;
            issued++;
        }
        soak_run(world, 1800);
        int arrived = 0;
        units = Units_GetActive(&n);
        for (int i = 0; i < made; i++)
            if (soak_d2(units[hs[i]].world_x, units[hs[i]].world_y,
                        gx[i], gy[i]) <= (int64_t)96 * 96) arrived++;
        soak_mark_escapes();
        printf("(fresh order: %d issued, %d arrived) ", issued, arrived);
    }
    soak_report();
    }
out:
    soak_unload(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Packed in by its own side on every side. */
TEST(soak_surrounded_by_friends) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    if (soak_boot(&platform, "Two Castles", "aramon", 0, &world) != 0) {
        printf("SKIP (map load) ");
        goto out;
    }
    {
    int sword = Units_FindDefByName("ARASWORD");
    if (sword < 0) { printf("SKIP (no unit defs) "); goto out; }
    const UnitDef *sd = Units_GetDef(sword);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    if (n <= 0) { printf("SKIP (no spawned units) "); goto out; }
    int32_t cx, cy;
    if (!soak_find_open(world, sd, units[0].world_x + 500,
                        units[0].world_y + 500, 6, &cx, &cy)) {
        printf("SKIP (no open ring site) ");
        goto out;
    }
    soak_reset("ringed in by friends");
    int centre = Units_Spawn(sword, 1, 0, cx, cy);
    if (centre < 0) { printf("SKIP (spawn failed) "); goto out; }
    for (int r = 1; r <= 2; r++) {
        for (int k = 0; k < 8 * r; k++) {
            double a = 6.2831853 * k / (8.0 * r);
            Units_Spawn(sword, 1, 0,
                        cx + (int32_t)(cos(a) * 18.0 * r),
                        cy + (int32_t)(sin(a) * 18.0 * r));
        }
    }
    soak_passive_all();
    soak_run(world, 600);       /* let the ring settle and park */
    int32_t gx, gy;
    if (!soak_find_open(world, sd, cx + 700, cy, 1, &gx, &gy)) {
        printf("SKIP (no goal) ");
        goto out;
    }
    soak_reset("ringed in by friends");
    Units_CommandMoveUnit(centre, gx, gy);
    soak_run(world, 3600);
    units = Units_GetActive(&n);
    {
        int64_t left = soak_d2(units[centre].world_x, units[centre].world_y,
                               gx, gy);
        soak_mark_escapes();
        printf("(centre %s, %d px short) ",
               left <= (int64_t)96 * 96 ? "got out" : "did not arrive",
               (int)sqrt((double)left));
    }
    soak_report();
    }
out:
    soak_unload(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* Legitimate waits: a crowd packed onto one point. None of it may count
 * as a stall. */
TEST(soak_legitimate_waits) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    if (soak_boot(&platform, "Two Castles", "aramon", 0, &world) != 0) {
        printf("SKIP (map load) ");
        goto out;
    }
    {
    int sword = Units_FindDefByName("ARASWORD");
    if (sword < 0) { printf("SKIP (no unit defs) "); goto out; }
    const UnitDef *sd = Units_GetDef(sword);
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    if (n <= 0) { printf("SKIP (no spawned units) "); goto out; }
    int32_t cx, cy;
    if (!soak_find_open(world, sd, units[0].world_x + 400,
                        units[0].world_y + 400, 6, &cx, &cy)) {
        printf("SKIP (no open site) ");
        goto out;
    }
    soak_reset("crowd packed on one point");
    int made = 0, hs[30];
    for (int i = 0; i < 24; i++) {
        int32_t sx, sy;
        if (!soak_find_open(world, sd, cx + ((i % 6) - 3) * 48,
                            cy + ((i / 6) - 2) * 48, 1, &sx, &sy)) continue;
        int h = Units_Spawn(sword, 1, 0, sx, sy);
        if (h >= 0) hs[made++] = h;
    }
    soak_passive_all();
    for (int i = 0; i < made; i++) Units_CommandMoveUnit(hs[i], cx, cy);
    soak_run(world, 3600);
    soak_mark_escapes();
    printf("(%d units on one point) ", made);
    soak_report();
    }
out:
    soak_unload(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* The broad soak: many units, long random orders, several maps. */
static void soak_wander(TAK_Platform *platform, const char *map,
                        const char *kingdom, int with_ai, int count,
                        int ticks) {
    GameWorld *world = NULL;
    char tag[48];
    snprintf(tag, sizeof(tag), "%s wander%s", map, with_ai ? " with AI" : "");
    if (soak_boot(platform, map, kingdom, with_ai, &world) != 0) {
        printf("\n    [%s] SKIP (map load)", tag);
        return;
    }
    int sword = Units_FindDefByName("ARASWORD");
    int horse = Units_FindDefByName("ARAHORSE");
    const UnitDef *sd = sword >= 0 ? Units_GetDef(sword) : NULL;
    if (!sd) { soak_unload(platform); return; }
    int n = 0;
    const Unit *units = Units_GetActive(&n);
    if (n <= 0) { soak_unload(platform); return; }
    int32_t bx = units[0].world_x, by = units[0].world_y;
    g_rng = 0xC0FFEEu;
    soak_reset(tag);
    static int hs[256];
    int made = 0;
    for (int i = 0; i < count && made < 256; i++) {
        int32_t sx, sy;
        int32_t px = bx + (int32_t)(soak_rand() % 2400u) - 1200;
        int32_t py = by + (int32_t)(soak_rand() % 2400u) - 1200;
        if (!soak_find_open(world, sd, px, py, 1, &sx, &sy)) continue;
        int def = (horse >= 0 && (i % 3) == 0) ? horse : sword;
        int h = Units_Spawn(def, 1, 0, sx, sy);
        if (h >= 0) hs[made++] = h;
    }
    soak_passive_all();
    int tw = world->map_pixels_w, th = world->map_pixels_h;
    for (int i = 0; i < made; i++)
        Units_CommandMoveUnit(hs[i], (int32_t)(soak_rand() % (uint32_t)tw),
                              (int32_t)(soak_rand() % (uint32_t)th));
    /* Re-order anything that goes idle, so every unit holds an order
     * for the whole run. */
    for (int t = 0; t < ticks; t++) {
        InGame_DebugRunSimTicks(1);
        soak_sample(world);
        if ((t % 60) != 0) continue;
        units = Units_GetActive(&n);
        for (int i = 0; i < made; i++) {
            int h = hs[i];
            if (h < n && units[h].alive == UNIT_ALIVE_ACTIVE &&
                units[h].cmd_kind == UNIT_CMD_NONE) {
                Units_CommandMoveUnit(h, (int32_t)(soak_rand() % (uint32_t)tw),
                                      (int32_t)(soak_rand() % (uint32_t)th));
            }
        }
    }
    soak_mark_escapes();
    printf("\n    (%d units, %d ticks) ", made, ticks);
    soak_report();
    soak_unload(platform);
}

TEST(soak_wander_many_maps) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    soak_wander(&platform, "CASTLE", "aramon", 0, 60, 5400);
    soak_wander(&platform, "Two Castles", "aramon", 0, 60, 3600);
    soak_wander(&platform, "Athri Cay", "aramon", 0, 60, 3600);
    soak_wander(&platform, "Angvir's Maze", "aramon", 0, 60, 3600);
    soak_wander(&platform, "Sewers of Elam", "aramon", 0, 60, 3600);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* The owner's report is about the computer player's units, so this run
 * leaves the AI on and watches everything on the map. */
TEST(soak_ai_battle) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *world = NULL;
    if (soak_boot(&platform, "CASTLE", "aramon", 1, &world) != 0) {
        printf("SKIP (map load) ");
        goto out;
    }
    soak_reset("CASTLE, computer players left on");
    soak_run(world, 10800);   /* three minutes of battle */
    soak_mark_escapes();
    {
        int n = 0;
        Units_GetActive(&n);
        printf("(%d units at the end) ", n);
    }
    soak_report();
out:
    soak_unload(&platform);
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}


/* -- The report, on the ground it was reported on -----------------
 *
 * The owner was playing Castle. Lokken stood on a narrow strip of
 * grass between deep water and rock, with two of his own mounted
 * units beside him, and could not move at all.
 *
 * Everything else here sculpts its own terrain. This one takes Castle
 * exactly as it ships, finds a tile the monarch's whole footprint
 * fits on that his own cell will not let a route start from, stands
 * him on it and sends him across the map. Whatever the strip in the
 * screenshot was, this is the same kind of ground, chosen by the
 * engine's own predicates rather than by hand. */

static const MoveClassDef *lok_move_class(const GameWorld *w,
                                          const UnitDef *def) {
    if (!def || !def->movement_class[0]) return NULL;
    return TAK_MoveInfo_Find(&w->moveinfo, def->movement_class);
}

/* The mover's own ground test at one point: slope and the water
 * window, no footprint. */
static int lok_point_ok(const GameWorld *w, const UnitDef *def,
                        int32_t x, int32_t y) {
    const MoveClassDef *mc = lok_move_class(w, def);
    int slope = (mc && mc->max_slope > 0) ? (int)mc->max_slope
                                          : (def ? def->max_slope : 0);
    if (!Terrain_IsWalkable(w, x, y, slope)) return 0;
    if (w->water_height <= 0) return 1;
    int depth = w->water_height - Terrain_SampleHeight(w, x, y);
    if (depth < 0) depth = 0;
    if (depth > (mc ? (int)mc->max_water_depth : 0)) return 0;
    if (mc && mc->min_water_depth > 0 && depth < (int)mc->min_water_depth)
        return 0;
    return 1;
}

/* The whole footprint, the tiles the mover stamps and the tiles a
 * plan sweeps per cell. This is what "he can stand here" means. */
static int lok_footprint_ok(const GameWorld *w, const UnitDef *def,
                            int32_t x, int32_t y) {
    const MoveClassDef *mc = lok_move_class(w, def);
    int fx = (mc && mc->footprint_x > 0) ? (int)mc->footprint_x : 1;
    int fz = (mc && mc->footprint_z > 0) ? (int)mc->footprint_z : 1;
    int tx0 = Occ_TileOf(x - fx * 8), ty0 = Occ_TileOf(y - fz * 8);
    for (int row = 0; row < fz; row++)
        for (int col = 0; col < fx; col++)
            if (!lok_point_ok(w, def, (tx0 + col) * 16 + 8,
                              (ty0 + row) * 16 + 8)) return 0;
    return 1;
}

/* Ground he fits on that his own path cell refuses. Scans in from the
 * map edges so the tile found is a real pinch and not the border. */
static int lok_find_pinch(const GameWorld *w, const UnitDef *def,
                          int32_t *out_x, int32_t *out_y, int *out_count) {
    const MoveClassDef *mc = lok_move_class(w, def);
    int tw = w->map_pixels_w / 16, th = w->map_pixels_h / 16;
    int found = 0, count = 0;
    for (int ty = 4; ty < th - 4; ty++) {
        for (int tx = 4; tx < tw - 4; tx++) {
            int32_t x = tx * 16 + 8, y = ty * 16 + 8;
            if (!lok_footprint_ok(w, def, x, y)) continue;
            int bits = 0, clear = 0;
            TAK_PathDebugCellOpen(w, mc, def->max_slope, x / 32, y / 32,
                                  &bits, &clear);
            if (bits && clear) continue;
            count++;
            if (!found) { found = 1; *out_x = x; *out_y = y; }
        }
    }
    *out_count = count;
    return found;
}

/* He stands on Castle's own ground, on a tile no route can start
 * from, and is sent to the far side of the map. He does not have to
 * arrive: a goal can be genuinely unreachable. What he must never do
 * is stand there. Before this work he moved 19 px in 9000 ticks with
 * every escalation counter pinned under 5. */
TEST(the_monarch_leaves_a_castle_tile_no_route_starts_from) {
    if (setup_vfs() != 0) { printf("SKIP (no data dir) "); return; }
    TAK_Platform platform;
    if (setup_platform(&platform) != 0) { VFS_Shutdown(); return; }
    ASSERT_EQ_INT(0, UI_Init());
    GameWorld *w = NULL;
    ASSERT_EQ_INT(0, soak_boot(&platform, "castle", "taros", 0, &w));
    ASSERT_NOT_NULL(w);
    Units_ClearInstances();

    int di = Units_FindDefByName("TARNECRO");
    ASSERT(di >= 0);
    const UnitDef *def = Units_GetDef(di);
    int32_t px = 0, py = 0, pinches = 0;
    int have = lok_find_pinch(w, def, &px, &py, &pinches);
    printf("(%d such tiles, first at %d,%d", pinches, px, py);
    ASSERT(have);

    int h = Units_Spawn(di, 1, 0, px, py);
    ASSERT(h >= 0);
    Units_DebugSetAggro(h, UNIT_AGGRO_PASSIVE);
    /* Two of his own beside him, the way the report has them. */
    int fdi = Units_FindDefByName("TARBLACK");
    for (int i = 0; i < 2 && fdi >= 0; i++) {
        int fh = Units_Spawn(fdi, 1, 0, px + (i ? 48 : -48), py);
        if (fh >= 0) Units_DebugSetAggro(fh, UNIT_AGGRO_PASSIVE);
    }
    int32_t gx = w->map_pixels_w - px, gy = w->map_pixels_h - py;
    Units_CommandMoveUnit(h, gx, gy);

    int count = 0;
    const Unit *units = Units_GetActive(&count);
    int32_t x0 = units[h].world_x, y0 = units[h].world_y;
    int64_t furthest2 = 0;
    int order_ended = 0, ended_tick = -1;
    for (int i = 0; i < 3000; i++) {
        Units_TickEngines();
        units = Units_GetActive(&count);
        const Unit *u = &units[h];
        int64_t dx = (int64_t)u->world_x - x0, dy = (int64_t)u->world_y - y0;
        if (dx * dx + dy * dy > furthest2) furthest2 = dx * dx + dy * dy;
        if (!order_ended && u->cmd_kind != UNIT_CMD_MOVE) {
            order_ended = 1;
            ended_tick = i;
        }
    }
    units = Units_GetActive(&count);
    const Unit *u = &units[h];
    int travelled = 0;
    while ((int64_t)(travelled + 1) * (travelled + 1) <= furthest2) travelled++;
    printf(", moved %d px, order %s", travelled,
           order_ended ? "ended" : "still live");
    if (order_ended) printf(" at tick %d", ended_tick);
    printf(") ");

    /* The rule. Either he covered real ground, or the order he could
     * not serve was ended: never a live order and a unit that has not
     * moved. A footprint is 32 px, so 96 is three of them and well
     * past the slide along a refusal that used to pass for movement. */
    ASSERT(travelled >= 96 || order_ended);
    /* And he is standing somewhere his own footprint fits. */
    ASSERT(lok_footprint_ok(w, def, u->world_x, u->world_y));

    Units_ClearInstances();
    soak_unload(&platform);
    TAK_PathCacheReset();
    UI_Shutdown();
    teardown_platform(&platform);
    VFS_Shutdown();
}

/* The rule itself, over every scenario this process ran. It goes last
 * so it sees them all, and it is the case that covers the ones nobody
 * has written yet: any future soak scenario is inside it the moment it
 * calls soak_report. */
TEST(no_unit_with_a_live_order_stands_still_for_good) {
    printf("(%ld scenarios, worst %ld ticks", g_sixty_scenarios,
           g_sixty_worst);
    if (g_sixty_worst > 0) printf(" in %s", g_sixty_where);
    printf(", %ld still held) ", g_sixty_held);
    ASSERT(soak_sixty_ok());
}

int main(int argc, char **argv) {
    SDL_SetMainReady();
    if (argc > 1) g_test_filter = argv[1];
    printf("\n=== unit stall soak ===\n");
    RUN_SOAK(soak_open_ground_control);
    RUN_SOAK(soak_legitimate_waits);
    RUN_SOAK(soak_one_cell_gap_crowd);
    RUN_SOAK(soak_narrow_strip);
    RUN_SOAK(soak_walled_courtyard);
    RUN_SOAK(soak_large_unit_small_gap);
    RUN_SOAK(soak_surrounded_by_friends);
    RUN_SOAK(soak_unreachable_destination);
    RUN_SOAK(soak_wander_many_maps);
    RUN_SOAK(soak_ai_battle);
    RUN_SOAK(the_monarch_leaves_a_castle_tile_no_route_starts_from);
    RUN_SOAK(no_unit_with_a_live_order_stands_still_for_good);
    printf("\n%d/%d passed\n", _tf_pass_count, _tf_total_count);
    return _tf_fail_count ? 1 : 0;
}
