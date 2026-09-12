/*
 * perf_probe.c -- the --perf-probe scenarios and what they measure.
 *
 * Each scenario builds a fixed battle, runs it for a fixed number of
 * sim ticks and prints one line per 600 ticks, then a done line. The
 * fields are listed in docs/notes/2026-09-11-perf-probes.md. The
 * scenarios only spawn units and give move orders, so the simulation
 * itself behaves as it does in a normal skirmish.
 */

#include "tak_perf_probe.h"
#include "tak_world.h"
#include "tak_unit.h"
#include "tak_pathing.h"
#include "tak_occupancy.h"
#include "tak_moveinfo.h"
#include "tak_memory.h"
#include "tak_battle_config.h"
#include "tak_platform.h"
#include "tak_ai.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* Per-subsystem timers owned by the sim, read here and never reset:
 * every figure below is a difference between two reads. */
extern double g_sim_prof_ms[4];    /* ai, engines, economy, fog */
extern double g_eng_prof_ms[4];    /* combat, projectiles, cob, misc */
extern double g_path_plan_calls;
extern double g_path_prof_ms;

#define PP_WINDOW_TICKS  600
#define PP_MAX_UNITS     2048
/* Frame-time histogram: 0.1 ms steps to 100 ms, where the limits sit,
 * then 1 ms steps to a second, then one overflow bin. */
#define PP_FINE_BINS     1000
#define PP_FINE_MS       0.1
#define PP_COARSE_BINS   900
#define PP_COARSE_MS     1.0
#define PP_BINS          (PP_FINE_BINS + PP_COARSE_BINS)
#define PP_STALL_SAMPLE  60        /* ticks between stall census samples */
#define PP_STALL_HOLD    1800      /* 30 s within 32 px counts as stalled */
#define PP_STALL_NEAR    96        /* this close to the goal is arrival */
#define PP_STALL_SPOT    32        /* moving this far resets the anchor */
#define PP_FFA_TARGET    75        /* units per AI after a top-up */
#define PP_FFA_PERIOD    3600      /* one top-up per sim minute */
#define PP_CROWD_BLOB    150       /* walkers per blob */
#define PP_CROWD_PERIOD  3600      /* one order swap per sim minute */
#define PP_SPAWN_PITCH   48        /* spawn grid step, fits a 3x3 footprint */
#define PP_SPAWN_RINGS   48

typedef enum { PP_OFF = 0, PP_FFA, PP_CROWD, PP_MEASURE } PpKind;

typedef struct PpAnchor {
    uint32_t sid;
    int32_t  ax, ay;       /* where the unit was when the anchor was set */
    int32_t  gx, gy;       /* the goal it was heading for */
    int      at_tick;
    int16_t  cmd;
} PpAnchor;

static struct {
    PpKind   kind;
    char     name[16];
    int      ticks_total;
    int      tick;
    int      window;
    int      finished;
    const char *end;
    int      setup_done;
    int      spawned;
    /* ffa: up to four combat defs per seat. crowd: the mixed classes. */
    int      defs[TAK_MAX_PLAYERS + 1][4];
    int      def_count[TAK_MAX_PLAYERS + 1];
    /* crowd */
    int      blob_handle[2 * PP_CROWD_BLOB];
    uint8_t  blob_of[2 * PP_CROWD_BLOB];
    int      blob_count;
    int32_t  blob_x[2], blob_y[2];
    int      blob_flip;
    /* last sample, for tests */
    int      last_units;
    int      last_stall;
} pp;

static struct {
    double   sim0[4], eng0[4], path0, plans0;
    double   sim_ms, worst_tick, worst_path, worst_ai;
    double   prev_path, prev_ai;
    uint64_t work0, rebuild_clock0;
    uint32_t plans_pathing0, rebuilds0, parked0;
    int      ticks, frames, capped;
    int      stall_last, stall_max;
    int      bins[PP_BINS + 1];
} win;

static struct {
    int    bins[PP_BINS + 1];
    int    frames, capped;
    double worst_tick, worst_path, worst_ai;
    int    stall_max;
} run;

static PpAnchor pp_anchor[PP_MAX_UNITS];

/* ── small helpers ──────────────────────────────────────────────── */

static uint64_t pp_clock(void) { return SDL_GetPerformanceCounter(); }

static double pp_clock_ms(uint64_t ticks) {
    double f = (double)SDL_GetPerformanceFrequency();
    return f > 0.0 ? (double)ticks * 1000.0 / f : 0.0;
}

static int32_t pp_dist2(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    int64_t dx = ax - bx, dy = ay - by;
    int64_t d = dx * dx + dy * dy;
    return d > 0x7fffffff ? 0x7fffffff : (int32_t)d;
}

static const char *pp_side_prefix(int side) {
    static const char *const p[4] = { "ARA", "TAR", "VER", "ZON" };
    return (side >= 0 && side < 4) ? p[side] : "ARA";
}

static const MoveClassDef *pp_move_class(const GameWorld *w, const UnitDef *d) {
    if (!w || !d || !d->movement_class[0]) return NULL;
    return TAK_MoveInfo_Find(&w->moveinfo, d->movement_class);
}

/* A ground fighter of this side: moves, shoots, walks and is neither a
 * builder, a monarch nor a boat. */
static int pp_is_ground_fighter(const GameWorld *w, const UnitDef *d,
                                const char *prefix) {
    if (!d || d->max_velocity <= 0.0f || d->num_weapons <= 0) return 0;
    if (d->can_fly || (d->cap_flags & UNIT_CAP_BUILDER)) return 0;
    if (prefix && strncmp(d->category, prefix, 3) != 0) return 0;
    if (strstr(d->category, "Monarch")) return 0;
    const MoveClassDef *mc = pp_move_class(w, d);
    if (mc && mc->min_water_depth > 0) return 0;
    return 1;
}

/* Is the footprint at this point clear ground nobody stands on? */
static int pp_spot_free(const GameWorld *w, const UnitDef *d, int32_t wx, int32_t wy) {
    const MoveClassDef *mc = pp_move_class(w, d);
    int fx = (mc && mc->footprint_x > 0) ? mc->footprint_x : 1;
    int fz = (mc && mc->footprint_z > 0) ? mc->footprint_z : 1;
    int need = fx > fz ? fx : fz;
    int tx0 = Occ_TileOf(wx - fx * 8);
    int ty0 = Occ_TileOf(wy - fz * 8);
    if (TAK_PathClearanceAt(w, mc, d->max_slope, tx0, ty0) < need) return 0;
    if (!w->occ) return 1;
    for (int dy = 0; dy < fz; dy++) {
        for (int dx = 0; dx < fx; dx++) {
            int tx = tx0 + dx, ty = ty0 + dy;
            if (tx < 0 || ty < 0 || tx >= w->occ_w || ty >= w->occ_h) return 0;
            if (w->occ[(size_t)ty * w->occ_w + tx].unit_plus1) return 0;
        }
    }
    return 1;
}

/* Spawn on the first free grid point of a square ring walk out from
 * (cx, cy). Returns the handle, or -1 when nothing fits. */
static int pp_spawn_near(const GameWorld *w, int def_idx, int player, int color,
                         int32_t cx, int32_t cy, int first_ring) {
    const UnitDef *d = Units_GetDef(def_idx);
    if (!d) return -1;
    for (int r = first_ring; r <= PP_SPAWN_RINGS; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (dx != -r && dx != r && dy != -r && dy != r) continue;
                int32_t wx = cx + dx * PP_SPAWN_PITCH;
                int32_t wy = cy + dy * PP_SPAWN_PITCH;
                if (wx < 0 || wy < 0 ||
                    wx >= w->map_pixels_w || wy >= w->map_pixels_h) continue;
                if (!pp_spot_free(w, d, wx, wy)) continue;
                int h = Units_Spawn(def_idx, player, color, wx, wy);
                if (h >= 0) { pp.spawned++; return h; }
                return -1;   /* the unit array is full */
            }
        }
    }
    return -1;
}

/* ── the stall census ───────────────────────────────────────────────
 *
 * A unit counts as stalled when it holds a movement order, has stayed
 * within 32 px of one spot for 30 s, is more than 96 px from its goal,
 * is not in weapon range of its target and is not waiting on a plan. */

static int pp_weapon_range(const UnitDef *d) {
    int r = 0;
    for (int i = 0; i < d->num_weapons && i < 3; i++) {
        if (d->weapons[i].range > r) r = d->weapons[i].range;
    }
    return r;
}

static int pp_stall_census(void) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (!units) return 0;
    if (count > PP_MAX_UNITS) count = PP_MAX_UNITS;
    int stalled = 0;
    for (int i = 0; i < count; i++) {
        const Unit *u = &units[i];
        PpAnchor *a = &pp_anchor[i];
        const UnitDef *d = Units_GetDef(u->def_idx);
        if (!d || u->alive != UNIT_ALIVE_ACTIVE || d->max_velocity <= 0.0f ||
            u->cmd_kind == UNIT_CMD_NONE || u->under_construction) {
            a->sid = 0;
            continue;
        }
        int32_t gx = u->cmd_x, gy = u->cmd_y;
        int have_target = 0;
        if (u->target >= 0 && u->target < count &&
            (u->cmd_kind == UNIT_CMD_ATTACK || u->cmd_kind == UNIT_CMD_GUARD ||
             u->cmd_kind == UNIT_CMD_REPAIR || u->cmd_kind == UNIT_CMD_LOAD)) {
            gx = units[u->target].world_x;
            gy = units[u->target].world_y;
            have_target = 1;
        }
        /* A new identity, a new order or a goal that has moved far
         * starts the clock again. */
        if (a->sid != u->stable_id || a->cmd != u->cmd_kind ||
            pp_dist2(gx, gy, a->gx, a->gy) > PP_STALL_NEAR * PP_STALL_NEAR) {
            a->sid = u->stable_id;
            a->cmd = u->cmd_kind;
            a->gx = gx; a->gy = gy;
            a->ax = u->world_x; a->ay = u->world_y;
            a->at_tick = pp.tick;
            continue;
        }
        if (pp_dist2(u->world_x, u->world_y, a->ax, a->ay) >
            PP_STALL_SPOT * PP_STALL_SPOT) {
            a->ax = u->world_x; a->ay = u->world_y;
            a->at_tick = pp.tick;
            continue;
        }
        if (pp.tick - a->at_tick < PP_STALL_HOLD) continue;
        if (u->path_pending) continue;
        if (pp_dist2(u->world_x, u->world_y, gx, gy) <=
            PP_STALL_NEAR * PP_STALL_NEAR) continue;
        if (have_target) {
            int reach = pp_weapon_range(d) + 32;
            if (pp_dist2(u->world_x, u->world_y, gx, gy) <= reach * reach) continue;
        }
        stalled++;
    }
    return stalled;
}

/* ── scenarios ─────────────────────────────────────────────────────── */

static void pp_pick_defs(const GameWorld *w) {
    int n = Units_GetDefCount();
    if (pp.kind == PP_FFA) {
        for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
            if (w->cfg.players[p - 1].kind != TAK_SLOT_AI) continue;
            const char *prefix = pp_side_prefix(w->cfg.players[p - 1].side);
            for (int i = 0; i < n && pp.def_count[p] < 3; i++) {
                const UnitDef *d = Units_GetDef(i);
                if (!pp_is_ground_fighter(w, d, prefix)) continue;
                pp.defs[p][pp.def_count[p]++] = i;
            }
        }
        return;
    }
    /* crowd: the first fighters with distinct move classes, whatever
     * side they come from, so footprints and speeds vary. */
    char taken[4][TAK_MOVEINFO_NAME_MAX];
    int ntaken = 0;
    for (int i = 0; i < n && ntaken < 4; i++) {
        const UnitDef *d = Units_GetDef(i);
        if (!pp_is_ground_fighter(w, d, NULL)) continue;
        int dup = 0;
        for (int k = 0; k < ntaken; k++) {
            if (strcmp(taken[k], d->movement_class) == 0) dup = 1;
        }
        if (dup) continue;
        strncpy(taken[ntaken], d->movement_class, TAK_MOVEINFO_NAME_MAX - 1);
        taken[ntaken][TAK_MOVEINFO_NAME_MAX - 1] = '\0';
        ntaken++;
        pp.defs[1][pp.def_count[1]++] = i;
    }
}

/* The start the map gives this seat number, or the map centre. */
static void pp_start_of(const GameWorld *w, int player, int32_t *x, int32_t *y) {
    for (int i = 0; i < w->num_start_positions; i++) {
        if (w->start_positions[i].player != player) continue;
        *x = w->start_positions[i].x * 16;
        *y = w->start_positions[i].z * 16;
        return;
    }
    *x = w->map_pixels_w / 2;
    *y = w->map_pixels_h / 2;
}

static void pp_ffa_topup(const GameWorld *w) {
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (w->cfg.players[p - 1].kind != TAK_SLOT_AI) continue;
        if (pp.def_count[p] <= 0) continue;
        int live = 0;
        for (int i = 0; i < count; i++) {
            const UnitDef *d = Units_GetDef(units[i].def_idx);
            if (units[i].alive != UNIT_ALIVE_ACTIVE) continue;
            if (units[i].player_id != p || !d || d->max_velocity <= 0.0f) continue;
            live++;
        }
        int32_t sx, sy;
        pp_start_of(w, p, &sx, &sy);
        for (int k = live; k < PP_FFA_TARGET; k++) {
            int def = pp.defs[p][k % pp.def_count[p]];
            if (pp_spawn_near(w, def, p, w->cfg.players[p - 1].color,
                              sx, sy, 2) < 0) break;
        }
        units = Units_GetActive(&count);
    }
}

static void pp_crowd_order(void) {
    for (int i = 0; i < pp.blob_count; i++) {
        int blob = pp.blob_of[i];
        int to = pp.blob_flip ? blob : 1 - blob;
        Units_CommandMoveUnit(pp.blob_handle[i], pp.blob_x[to], pp.blob_y[to]);
    }
    pp.blob_flip = !pp.blob_flip;
}

static void pp_crowd_setup(GameWorld *w) {
    pp_start_of(w, 3, &pp.blob_x[0], &pp.blob_y[0]);
    pp_start_of(w, 5, &pp.blob_x[1], &pp.blob_y[1]);
    if (pp.def_count[1] <= 0) return;
    int color = w->cfg.players[0].color;
    for (int b = 0; b < 2; b++) {
        for (int k = 0; k < PP_CROWD_BLOB; k++) {
            int def = pp.defs[1][k % pp.def_count[1]];
            int h = pp_spawn_near(w, def, 1, color, pp.blob_x[b], pp.blob_y[b], 1);
            if (h < 0) break;
            pp.blob_of[pp.blob_count] = (uint8_t)b;
            pp.blob_handle[pp.blob_count++] = h;
        }
    }
    /* Watch the ground the two blobs cross, not a seat's start. */
    w->cam_x = (pp.blob_x[0] + pp.blob_x[1]) / 2 - w->viewport_w / 2;
    w->cam_y = (pp.blob_y[0] + pp.blob_y[1]) / 2 - w->viewport_h / 2;
    if (w->cam_x < 0) w->cam_x = 0;
    if (w->cam_y < 0) w->cam_y = 0;
    pp_crowd_order();
}

/* ── reporting ─────────────────────────────────────────────────────── */

static void pp_bin_frame(double ms) {
    int b;
    if (ms < PP_FINE_BINS * PP_FINE_MS) {
        b = (int)(ms / PP_FINE_MS);
    } else {
        b = PP_FINE_BINS + (int)((ms - PP_FINE_BINS * PP_FINE_MS) / PP_COARSE_MS);
    }
    if (b < 0) b = 0;
    if (b > PP_BINS) b = PP_BINS;
    win.bins[b]++;
    run.bins[b]++;
}

/* Upper edge of a bin, in ms. */
static double pp_bin_edge(int b) {
    if (b < PP_FINE_BINS) return (b + 1) * PP_FINE_MS;
    return PP_FINE_BINS * PP_FINE_MS + (b - PP_FINE_BINS + 1) * PP_COARSE_MS;
}

static double pp_percentile(const int *bins, int total, double p) {
    if (total <= 0) return 0.0;
    int want = (int)(total * p);
    if (want < 1) want = 1;
    int seen = 0;
    for (int b = 0; b <= PP_BINS; b++) {
        seen += bins[b];
        if (seen >= want) return pp_bin_edge(b);
    }
    return pp_bin_edge(PP_BINS);
}

static void pp_window_reset(void) {
    memset(win.bins, 0, sizeof(win.bins));
    for (int i = 0; i < 4; i++) {
        win.sim0[i] = g_sim_prof_ms[i];
        win.eng0[i] = g_eng_prof_ms[i];
    }
    win.path0 = g_path_prof_ms;
    win.plans0 = g_path_plan_calls;
    /* Per-tick deltas measure from here, not from zero: the timers are
     * process wide and another test may have run before this one. */
    win.prev_path = g_path_prof_ms;
    win.prev_ai = g_sim_prof_ms[0];
    TAK_PathDebugCounters c;
    TAK_PathDebugGetCounters(&c);
    win.work0 = c.work;
    win.rebuild_clock0 = c.rebuild_clock;
    win.rebuilds0 = c.rebuilds;
    win.parked0 = Occ_DebugParkedChanges();
    win.sim_ms = 0.0;
    win.worst_tick = win.worst_path = win.worst_ai = 0.0;
    win.ticks = win.frames = win.capped = 0;
    win.stall_last = win.stall_max = 0;
}

static void pp_print_window(const GameWorld *w) {
    TAK_PathDebugCounters c;
    TAK_PathDebugGetCounters(&c);
    TakMemStats mem;
    tak_mem_get_stats(&mem);
    int units = 0;
    const Unit *list = Units_GetActive(&units);
    int live = 0;
    for (int i = 0; i < units; i++) {
        if (list[i].alive == UNIT_ALIVE_ACTIVE) live++;
    }
    pp.last_units = live;
    char orders[96];
    orders[0] = '\0';
    if (pp.kind == PP_FFA) {
        int n = 0;
        n += snprintf(orders + n, sizeof(orders) - n, " orders=");
        for (int p = 1; p <= 4 && n < (int)sizeof(orders) - 8; p++) {
            int sum = 0;
            for (int q = 1; q <= TAK_MAX_PLAYERS; q++) {
                if (q != p) sum += TAK_AI_DebugHostileOrders(p, q, 0);
            }
            n += snprintf(orders + n, sizeof(orders) - n, "%s%d",
                          p > 1 ? "/" : "", sum);
        }
    }
    printf("perf-probe %s w%02d tick=%d ticks=%d sim=%.2f worst=%.2f "
           "ai=%.2f ai_worst=%.2f eng=%.2f eco=%.2f fog=%.2f "
           "path=%.2f path_worst=%.2f cmb=%.2f prj=%.2f cob=%.2f misc=%.2f "
           "plans=%d work=%llu rebuilds=%u rebuild=%.2f parked=%u "
           "frames=%d p50=%.1f p95=%.1f p99=%.1f capped=%d "
           "units=%d heap=%u pmem=%u stall=%d stall_max=%d%s\n",
           pp.name, pp.window, pp.tick, win.ticks, win.sim_ms, win.worst_tick,
           g_sim_prof_ms[0] - win.sim0[0], win.worst_ai,
           g_sim_prof_ms[1] - win.sim0[1], g_sim_prof_ms[2] - win.sim0[2],
           g_sim_prof_ms[3] - win.sim0[3],
           g_path_prof_ms - win.path0, win.worst_path,
           g_eng_prof_ms[0] - win.eng0[0], g_eng_prof_ms[1] - win.eng0[1],
           g_eng_prof_ms[2] - win.eng0[2], g_eng_prof_ms[3] - win.eng0[3],
           (int)(g_path_plan_calls - win.plans0),
           (unsigned long long)(c.work - win.work0),
           (unsigned)(c.rebuilds - win.rebuilds0),
           pp_clock_ms(c.rebuild_clock - win.rebuild_clock0),
           (unsigned)(Occ_DebugParkedChanges() - win.parked0),
           win.frames,
           pp_percentile(win.bins, win.frames, 0.50),
           pp_percentile(win.bins, win.frames, 0.95),
           pp_percentile(win.bins, win.frames, 0.99),
           win.capped, live, mem.live_bytes / 1024u, c.cache_bytes / 1024u,
           win.stall_last, win.stall_max, orders);
    fflush(stdout);
    (void)w;
}

static void pp_print_done(void) {
    printf("perf-probe %s done tick=%d windows=%d frames=%d "
           "p50=%.1f p95=%.1f p99=%.1f capped=%d worst=%.2f "
           "path_worst=%.2f ai_worst=%.2f stall_max=%d end=%s\n",
           pp.name, pp.tick, pp.window, run.frames,
           pp_percentile(run.bins, run.frames, 0.50),
           pp_percentile(run.bins, run.frames, 0.95),
           pp_percentile(run.bins, run.frames, 0.99),
           run.capped, run.worst_tick, run.worst_path, run.worst_ai,
           run.stall_max, pp.end ? pp.end : "complete");
    fflush(stdout);
}

/* ── entry points ──────────────────────────────────────────────────── */

static void pp_reset(void) {
    memset(&pp, 0, sizeof(pp));
    memset(&win, 0, sizeof(win));
    memset(&run, 0, sizeof(run));
    memset(pp_anchor, 0, sizeof(pp_anchor));
}

int PerfProbe_Select(const char *scenario) {
    if (!scenario) return -1;
    PpKind k = PP_OFF;
    if (strcmp(scenario, "ffa") == 0) k = PP_FFA;
    else if (strcmp(scenario, "crowd") == 0) k = PP_CROWD;
    if (k == PP_OFF) return -1;
    pp_reset();
    pp.kind = k;
    strncpy(pp.name, scenario, sizeof(pp.name) - 1);
    pp.ticks_total = (k == PP_FFA) ? 43200 : 18000;
    return 0;
}

void PerfProbe_SetTicks(int ticks) {
    if (ticks > 0) pp.ticks_total = ticks;
}

int PerfProbe_Active(void) { return pp.kind != PP_OFF && !pp.finished; }
int PerfProbe_Finished(void) { return pp.finished; }
int PerfProbe_Windows(void) { return pp.window; }
int PerfProbe_Ticks(void) { return pp.tick; }
int PerfProbe_LastUnits(void) { return pp.last_units; }
int PerfProbe_LastStall(void) { return pp.last_stall; }
int PerfProbe_Spawned(void) { return pp.spawned; }

void PerfProbe_Stop(void) { pp_reset(); }

void PerfProbe_Finish(const char *reason) {
    if (pp.kind == PP_OFF || pp.finished) return;
    if (pp.tick > 0) {
        /* Report the part window rather than losing it: a run cut
         * short still has to say what it measured. */
        pp.window++;
        pp_print_window(NULL);
        run.frames += win.frames;
        run.capped += win.capped;
        pp_window_reset();
        pp.end = reason ? reason : "stopped";
        pp_print_done();
    }
    pp.finished = 1;
}

void PerfProbe_BeginMeasureOnly(const char *label, int ticks) {
    pp_reset();
    pp.kind = PP_MEASURE;
    strncpy(pp.name, label ? label : "probe", sizeof(pp.name) - 1);
    pp.ticks_total = ticks > 0 ? ticks : 12000;
    pp.setup_done = 1;
    TAK_PathDebugSetClock(pp_clock);
    pp_window_reset();
}

int PerfProbe_BeginWorld(TAK_Platform *plat) {
    if (pp.kind != PP_FFA && pp.kind != PP_CROWD) return -1;
    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    cfg.monarch_expendable = 1;
    const char *map, *kingdom;
    if (pp.kind == PP_FFA) {
        /* The largest map with four starts. Four AI seats, no human. */
        map = "Lake Lokken";
        kingdom = "taros";
        static const int sides[4] = {
            TAK_SIDE_ARAMON, TAK_SIDE_TAROS, TAK_SIDE_VERUNA, TAK_SIDE_ZHON
        };
        for (int p = 0; p < 4; p++) {
            cfg.players[p].kind = TAK_SLOT_AI;
            cfg.players[p].side = sides[p];
            cfg.players[p].team = p + 1;
            cfg.players[p].color = p;
            cfg.players[p].ai_difficulty = 2;
        }
        for (int p = 4; p < TAK_MAX_PLAYERS; p++) cfg.players[p].kind = TAK_SLOT_CLOSED;
    } else {
        /* Open ground, two seats nobody drives: the walkers are the
         * whole scenario. */
        map = "Tarosian Plain";
        kingdom = "taros";
        cfg.players[0].kind = TAK_SLOT_HUMAN;
        cfg.players[0].side = TAK_SIDE_ARAMON;
        cfg.players[0].color = 0;
        cfg.players[1].kind = TAK_SLOT_HUMAN;
        cfg.players[1].side = TAK_SIDE_TAROS;
        cfg.players[1].color = 1;
        for (int p = 2; p < TAK_MAX_PLAYERS; p++) cfg.players[p].kind = TAK_SLOT_CLOSED;
    }
    strncpy(cfg.map_name, map, sizeof(cfg.map_name) - 1);
    if (World_BeginLoad(plat, &cfg, map, kingdom) != 0) {
        fprintf(stderr, "perf-probe: could not load map '%s'\n", map);
        return -1;
    }
    TAK_PathDebugSetClock(pp_clock);
    pp_window_reset();
    return 0;
}

void PerfProbe_BeforeTick(GameWorld *world) {
    if (pp.kind == PP_OFF || pp.finished || !world) return;
    if (!pp.setup_done) {
        pp_pick_defs(world);
        if (pp.kind == PP_CROWD) pp_crowd_setup(world);
        pp.setup_done = 1;
    }
    if (pp.kind == PP_FFA && pp.tick % PP_FFA_PERIOD == 0) {
        pp_ffa_topup(world);
    } else if (pp.kind == PP_CROWD && pp.tick > 0 &&
               pp.tick % PP_CROWD_PERIOD == 0) {
        pp_crowd_order();
    }
}

void PerfProbe_AfterTick(GameWorld *world, double tick_ms) {
    if (pp.kind == PP_OFF || pp.finished) return;
    pp.tick++;
    win.ticks++;
    win.sim_ms += tick_ms;
    if (tick_ms > win.worst_tick) win.worst_tick = tick_ms;
    if (tick_ms > run.worst_tick) run.worst_tick = tick_ms;
    double path_tick = g_path_prof_ms - win.prev_path;
    double ai_tick = g_sim_prof_ms[0] - win.prev_ai;
    win.prev_path = g_path_prof_ms;
    win.prev_ai = g_sim_prof_ms[0];
    if (path_tick > win.worst_path) win.worst_path = path_tick;
    if (path_tick > run.worst_path) run.worst_path = path_tick;
    if (ai_tick > win.worst_ai) win.worst_ai = ai_tick;
    if (ai_tick > run.worst_ai) run.worst_ai = ai_tick;
    if (pp.tick % PP_STALL_SAMPLE == 0) {
        int s = pp_stall_census();
        win.stall_last = s;
        pp.last_stall = s;
        if (s > win.stall_max) win.stall_max = s;
        if (s > run.stall_max) run.stall_max = s;
    }
    int over = pp.tick >= pp.ticks_total;
    int ended = world && world->skirmish_game_over;
    if (pp.tick % PP_WINDOW_TICKS == 0 || over || ended) {
        pp.window++;
        pp_print_window(world);
        run.frames += win.frames;
        run.capped += win.capped;
        pp_window_reset();
    }
    if (over || ended) {
        pp.end = ended && !over ? "gameover" : "complete";
        pp_print_done();
        pp.finished = 1;
    }
}

void PerfProbe_FrameTicks(int ticks, int cap) {
    if (pp.kind == PP_OFF || pp.finished) return;
    if (cap > 0 && ticks >= cap) win.capped++;
}

void PerfProbe_EndFrame(double frame_ms) {
    if (pp.kind == PP_OFF || pp.finished || pp.tick <= 0) return;
    win.frames++;
    pp_bin_frame(frame_ms);
}
