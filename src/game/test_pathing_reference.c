/* src/game/pathing.c at 0835251, before plan buffers were kept, for
 * test_path_buffers to hold the planner to. Below the renames it is that
 * revision, with the walk out of a trapped start and the first point of
 * a compressed route taken in as the planner has them, and the step cost
 * following the live planner's bad ground. */

#define TAK_PathDebugSetClock         Ref_PathDebugSetClock
#define TAK_PathDebugResetEvery       Ref_PathDebugResetEvery
#define TAK_PathDebugUseDistanceField Ref_PathDebugUseDistanceField
#define TAK_PathDebugGetCounters      Ref_PathDebugGetCounters
#define TAK_PathCacheReset            Ref_PathCacheReset
#define TAK_PathGroundConnected       Ref_PathGroundConnected
#define TAK_PathClearanceAt           Ref_PathClearanceAt
#define TAK_PathDebugCellOpen         Ref_PathDebugCellOpen
#define TAK_PathPlan                  Ref_PathPlan
#define TAK_PathPlanForMoveClass      Ref_PathPlanForMoveClass
#define TAK_PathPlanQuery             Ref_PathPlanQuery
#define TAK_PathPlanFlow              Ref_PathPlanFlow
#define TAK_PathDebugFlowBuilds       Ref_PathDebugFlowBuilds
#define TAK_PathDebugFlowClock        Ref_PathDebugFlowClock
#define TAK_PathDebugFlowSettled      Ref_PathDebugFlowSettled
#define TAK_PathDebugFlowWhole        Ref_PathDebugFlowWhole

#include "tak_pathing.h"
#include "tak_moveinfo.h"
#include "tak_terrain.h"
#include "tak_world.h"
#include "tak_occupancy.h"
#include "tak_memory.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define PATH_CELL_PX 32

static int iabs32(int v) { return v < 0 ? -v : v; }

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int cell_index(int x, int y, int w) {
    return y * w + x;
}

static int world_to_cell(int32_t p) {
    return (int)(p / PATH_CELL_PX);
}

static int32_t cell_to_world(int c) {
    return c * PATH_CELL_PX + PATH_CELL_PX / 2;
}

/* ── Occupancy in the plan ────────────────────────────────────────
 * Structures block, an own closed gate stays passable and is opened on
 * arrival (legacy:21986-22032), a foreign one blocks. Mobile units are
 * invisible while they move: the legacy cost grid only learns about a
 * unit footprint once it has held its cells for 10 frames
 * (legacy:188900-188960), and unit-vs-unit blocking on the move is
 * resolved at step time (legacy:219329-219340). The parked flag in the
 * occupancy layer is that "old stamp" state. */
#define OCC_PER_PATH_CELL (PATH_CELL_PX / TAK_OCC_TILE_PX)

static int movement_max_slope(const MoveClassDef *move_class,
                              int fallback_max_slope) {
    if (move_class && move_class->max_slope > 0) return move_class->max_slope;
    return fallback_max_slope;
}

/* Water-depth gate (legacy:219149-219156): planning must respect the
 * same [min, max] window the movement predicate enforces. waterheight
 * (sidedata) is in raw heightmap units; Terrain_SampleHeight returns
 * display-biased values (-32). */
static int water_ok(const struct GameWorld *world,
                    const MoveClassDef *move_class,
                    int32_t wx, int32_t wy) {
    if (!move_class || world->water_height <= 0) return 1;
    int raw_h = Terrain_SampleHeight(world, wx, wy);
    int depth = world->water_height - raw_h;
    if (depth < 0) depth = 0;
    if (depth > move_class->max_water_depth) return 0;
    if (move_class->min_water_depth > 0 &&
        depth < move_class->min_water_depth) return 0;
    return 1;
}

/* Centre of an occupancy tile, where the terrain is sampled. */
static int32_t tile_to_world(int t) {
    return (int32_t)t * TAK_OCC_TILE_PX + TAK_OCC_TILE_PX / 2;
}

/* ── Footprint placements inside a cell ───────────────────────────
 * A path cell is 32 px and an occupancy tile is 16, so a tile
 * aligned footprint has exactly two anchors per axis whose centre
 * falls inside one cell, whatever its size: four placements in all.
 * A placement anchored at tile t covers t to t + fp - 1 and its
 * centre is t * 16 + fp * 8, which is where a unit standing on it
 * has its own centre and the point the planner hands the mover. */
#define FP_PLACEMENTS 4

/* The upper of the two anchors, the one the cell test used to be the
 * whole of: fp tiles centred on the cell centre. */
static int cell_fp_anchor(int cell, int fp) {
    return Occ_TileOf(cell_to_world(cell) - (fp - 1) * 8);
}

/* The four placements, in one fixed order so every machine picks the
 * same one. The cell's own placement comes first, so ground that
 * was legal before is judged at the placement it always was. */
static void cell_fp_placements(int cell_x, int cell_y, int fx, int fz,
                               int tx[FP_PLACEMENTS],
                               int ty[FP_PLACEMENTS]) {
    static const int off[FP_PLACEMENTS][2] = {
        { 0, 0 }, { -1, 0 }, { 0, -1 }, { -1, -1 }
    };
    int ax = cell_fp_anchor(cell_x, fx), ay = cell_fp_anchor(cell_y, fz);
    for (int p = 0; p < FP_PLACEMENTS; p++) {
        tx[p] = ax + off[p][0];
        ty[p] = ay + off[p][1];
    }
}

/* Where a unit standing on this placement has its centre. */
static int32_t placement_centre(int t0, int fp) {
    return (int32_t)t0 * TAK_OCC_TILE_PX + fp * (TAK_OCC_TILE_PX / 2);
}

static void class_footprint(const MoveClassDef *mc, int *fx, int *fz) {
    *fx = (mc && mc->footprint_x > 0) ? mc->footprint_x : 1;
    *fz = (mc && mc->footprint_z > 0) ? mc->footprint_z : 1;
}

/* Which of the four placements stand on ground the class can cross,
 * one bit each. Every tile of the footprint is tested, which is the
 * sweep the original runs (legacy:219089-219131), and it is run at
 * each placement because the original anchors that sweep on the
 * unit's own position rounded to the tile grid (legacy:184166-184186)
 * and has no 32 px grid to be in step with.
 *
 * This used to test the cell's own placement alone, so a band of
 * walkable ground one cell wide was ground at one parity against the
 * cell grid and no ground at all at the other, however wide it was.
 *
 * Sampling: the path taken when a map has no tile grid to read. */
static int cell_fp_mask_slow(const struct GameWorld *world,
                             int x, int y, int cw, int ch,
                             const MoveClassDef *move_class,
                             int fallback_max_slope, int fp_x, int fp_z) {
    if (x < 0 || y < 0 || x >= cw || y >= ch) return 0;
    int slope = movement_max_slope(move_class, fallback_max_slope);
    int tx[FP_PLACEMENTS], ty[FP_PLACEMENTS];
    cell_fp_placements(x, y, fp_x, fp_z, tx, ty);
    int mask = 0;
    for (int p = 0; p < FP_PLACEMENTS; p++) {
        if (tx[p] < 0 || ty[p] < 0) continue;
        int open = 1;
        for (int row = 0; row < fp_z && open; row++) {
            for (int col = 0; col < fp_x; col++) {
                int32_t sx = tile_to_world(tx[p] + col);
                int32_t sy = tile_to_world(ty[p] + row);
                if (sx >= world->map_pixels_w || sy >= world->map_pixels_h ||
                    !Terrain_IsWalkable(world, sx, sy, slope) ||
                    !water_ok(world, move_class, sx, sy)) {
                    open = 0;
                    break;
                }
            }
        }
        if (open) mask |= 1 << p;
    }
    return mask;
}

/* ── Per-move-class caches ────────────────────────────────────────
 * Terrain is static; the slow path re-samples bilinear heights per
 * neighbor per A* call (~2.4ms/call, the 1000-unit killer). One
 * passability bitmap per (move class, slope) per map, holding a bit
 * per footprint placement in each cell, plus the clearance map on the
 * 16-px tile grid: for every tile, the side of the largest square of
 * tiles with that tile at its top left that the class can cross and no
 * structure stands on. Structures come and go, so the clearance map
 * carries the occupancy version it was built at and is rebuilt when
 * that moves. */
/* Probe counters (tak_pathing.h). Instrumentation only. */
static uint32_t g_dbg_plans;
static uint64_t g_dbg_work;
static uint32_t g_dbg_rebuilds;
static uint64_t g_dbg_rebuild_clock;
static uint64_t (*g_dbg_clock)(void);
static int g_dbg_reset_every;
static int g_dbg_since_reset;

void TAK_PathDebugSetClock(uint64_t (*now)(void)) { g_dbg_clock = now; }

void TAK_PathDebugResetEvery(int plans) {
    g_dbg_reset_every = plans > 0 ? plans : 0;
    g_dbg_since_reset = 0;
}

static int g_use_field = 1;

void TAK_PathDebugUseDistanceField(int on) { g_use_field = on ? 1 : 0; }

static uint64_t dbg_now(void) { return g_dbg_clock ? g_dbg_clock() : 0; }

/* How many marks the long route field measures from. */
#define PATH_FIELD_MARKS 3

#define PCACHE_MAX 16
static struct {
    const struct GameWorld *world;
    const MoveClassDef     *mc;
    int       fallback_slope;
    int       fx, fz;     /* the footprint the placement mask was swept with */
    int       cw, ch;
    uint8_t  *bits;
    uint8_t  *plain;      /* per 16 px tile: ground this class can cross */
    uint8_t  *clear;
    uint32_t  clear_version;
    int       tw, th;
    uint8_t  *cellbad;    /* per cell: half the surcharge of bad ground */
    int32_t  *field[PATH_FIELD_MARKS];  /* cost to a mark, -1 out of reach */
    int       field_n;
    int       field_tried;
    uint16_t *comp;       /* per path cell: connected ground label */
} g_pcache[PCACHE_MAX];
static int g_pcache_n = 0;

void TAK_PathDebugGetCounters(TAK_PathDebugCounters *out) {
    if (!out) return;
    out->plans = g_dbg_plans;
    out->work = g_dbg_work;
    out->rebuilds = g_dbg_rebuilds;
    out->rebuild_clock = g_dbg_rebuild_clock;
    uint32_t bytes = 0;
    for (int i = 0; i < g_pcache_n; i++) {
        bytes += (uint32_t)(g_pcache[i].cw * g_pcache[i].ch);
        if (g_pcache[i].plain) {
            bytes += (uint32_t)(g_pcache[i].tw * g_pcache[i].th);
        }
        if (g_pcache[i].clear) {
            bytes += (uint32_t)(g_pcache[i].tw * g_pcache[i].th);
        }
        if (g_pcache[i].cellbad) {
            bytes += (uint32_t)(g_pcache[i].cw * g_pcache[i].ch);
        }
        bytes += (uint32_t)(g_pcache[i].field_n * g_pcache[i].cw *
                            g_pcache[i].ch * 4);
        if (g_pcache[i].comp) {
            bytes += (uint32_t)(g_pcache[i].cw * g_pcache[i].ch) * 2u;
        }
    }
    out->cache_bytes = bytes;
}

static void flow_reset_all(void);

void TAK_PathCacheReset(void) {
    for (int i = 0; i < g_pcache_n; i++) {
        tak_free(g_pcache[i].bits);
        if (g_pcache[i].plain) tak_free(g_pcache[i].plain);
        if (g_pcache[i].clear) tak_free(g_pcache[i].clear);
        if (g_pcache[i].cellbad) tak_free(g_pcache[i].cellbad);
        for (int m = 0; m < PATH_FIELD_MARKS; m++) {
            if (g_pcache[i].field[m]) tak_free(g_pcache[i].field[m]);
        }
        if (g_pcache[i].comp) tak_free(g_pcache[i].comp);
    }
    memset(g_pcache, 0, sizeof(g_pcache));
    g_pcache_n = 0;
    flow_reset_all();
}

/* Bad ground costs 48 to the original's 24 on a step of 16
 * (legacy:22210-22222, legacy:22988-22990, legacy:23535-23543). On our
 * step of ten that is six, three at each end so it costs the same both ways. */
#define PATH_BAD_HALF 3

/* Whether tile (tx, ty) is ground the class crosses, and crosses at full
 * pace: inside its slope and depths and inside its bad slope and bad
 * depths (legacy:219720-219743). Off the map it is neither. */
static void tile_ground(const struct GameWorld *world, const MoveClassDef *mc,
                        int slope, int tw, int th, int tx, int ty,
                        int *walk, int *good) {
    *walk = *good = 0;
    if (tx < 0 || ty < 0 || tx >= tw || ty >= th) return;
    if (slope <= 0) slope = 12;
    int span = 0;
    const TNTFile *t = &world->tnt;
    if (t->heightmap && t->height_w > 1 && t->height_h > 1) {
        int hx = tx >= t->height_w - 1 ? t->height_w - 2 : tx;
        int hz = ty >= t->height_h - 1 ? t->height_h - 2 : ty;
        const uint8_t *row = t->heightmap + (size_t)hz * t->height_w + hx;
        int a = row[0], b = row[1], c = row[t->height_w], d = row[t->height_w + 1];
        int lo = a < b ? a : b, hi = a < b ? b : a;
        if (c < lo) lo = c;
        if (c > hi) hi = c;
        if (d < lo) lo = d;
        if (d > hi) hi = d;
        span = hi - lo;
    }
    if (span > slope) return;
    int depth = 0, wet = 0;
    if (world->water_height > 0) {
        depth = world->water_height -
                Terrain_SampleHeight(world, tile_to_world(tx), tile_to_world(ty));
        wet = depth > 0;
        if (depth < 0) depth = 0;
        if (mc && (depth > mc->max_water_depth ||
                   (mc->min_water_depth > 0 && depth < mc->min_water_depth)))
            return;
    }
    *walk = 1;
    int bad = mc ? (wet ? mc->bad_water_slope : mc->bad_slope) : slope / 2;
    if (span > bad) return;
    if (mc && world->water_height > 0 &&
        (depth > mc->bad_max_water_depth ||
         (mc->bad_min_water_depth > 0 && depth < mc->bad_min_water_depth)))
        return;
    *good = 1;
}

/* Bad ground at one path cell: every placement it has stands on or
 * beside a tile not crossed at full pace (the ring at
 * legacy:219761-219791). Half the step surcharge, or 0. */
static int cell_bad_direct(const struct GameWorld *world, const MoveClassDef *mc,
                           int slope, int fx, int fz, int cx, int cy) {
    int tw = (world->map_pixels_w + TAK_OCC_TILE_PX - 1) / TAK_OCC_TILE_PX;
    int th = (world->map_pixels_h + TAK_OCC_TILE_PX - 1) / TAK_OCC_TILE_PX;
    int ptx[FP_PLACEMENTS], pty[FP_PLACEMENTS];
    cell_fp_placements(cx, cy, fx, fz, ptx, pty);
    int open = 0;
    for (int p = 0; p < FP_PLACEMENTS; p++) {
        int ok = 1, fine = 1, walk, good;
        for (int y = pty[p] - 1; y <= pty[p] + fz && ok; y++) {
            for (int x = ptx[p] - 1; x <= ptx[p] + fx; x++) {
                tile_ground(world, mc, slope, tw, th, x, y, &walk, &good);
                int inside = x >= ptx[p] && x < ptx[p] + fx &&
                             y >= pty[p] && y < pty[p] + fz;
                if (inside && !walk) { ok = 0; break; }
                if (!good) fine = 0;
            }
        }
        if (!ok) continue;
        if (fine) return 0;
        open = 1;
    }
    return open ? PATH_BAD_HALF : 0;
}

/* Tiles of [x0, x1] by [y0, y1] set in a prefix sum of tw by th, any
 * tile off the map counting as set. */
static int prefix_any(const int32_t *ps, int tw, int th,
                      int x0, int y0, int x1, int y1) {
    if (x0 < 0 || y0 < 0 || x1 >= tw || y1 >= th) return 1;
    int w = tw + 1;
    return ps[(y1 + 1) * w + x1 + 1] - ps[y0 * w + x1 + 1] -
           ps[(y1 + 1) * w + x0] + ps[y0 * w + x0] != 0;
}

/* cell_bad_direct for every cell, read off prefix sums of the tiles. */
static void cell_bad_fill(const struct GameWorld *world, const MoveClassDef *mc,
                          int fallback_slope, int fx, int fz, int cw, int ch,
                          uint8_t *out) {
    int slope = movement_max_slope(mc, fallback_slope);
    int tw = (world->map_pixels_w + TAK_OCC_TILE_PX - 1) / TAK_OCC_TILE_PX;
    int th = (world->map_pixels_h + TAK_OCC_TILE_PX - 1) / TAK_OCC_TILE_PX;
    size_t pn = (size_t)(tw > 0 ? tw + 1 : 1) * (size_t)(th > 0 ? th + 1 : 1);
    int32_t *nwalk = (int32_t *)tak_malloc(pn * sizeof(int32_t));
    int32_t *ngood = (int32_t *)tak_malloc(pn * sizeof(int32_t));
    if (!nwalk || !ngood || tw <= 0 || th <= 0) {
        if (nwalk) tak_free(nwalk);
        if (ngood) tak_free(ngood);
        for (int cy = 0; cy < ch; cy++)
            for (int cx = 0; cx < cw; cx++)
                out[cy * cw + cx] = (uint8_t)cell_bad_direct(world, mc, slope,
                                                             fx, fz, cx, cy);
        return;
    }
    memset(nwalk, 0, pn * sizeof(int32_t));
    memset(ngood, 0, pn * sizeof(int32_t));
    int w = tw + 1;
    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++) {
            int walk, good;
            tile_ground(world, mc, slope, tw, th, tx, ty, &walk, &good);
            int i = (ty + 1) * w + tx + 1;
            nwalk[i] = nwalk[i - 1] + nwalk[i - w] - nwalk[i - w - 1] + !walk;
            ngood[i] = ngood[i - 1] + ngood[i - w] - ngood[i - w - 1] + !good;
        }
    }
    for (int cy = 0; cy < ch; cy++) {
        for (int cx = 0; cx < cw; cx++) {
            int ptx[FP_PLACEMENTS], pty[FP_PLACEMENTS];
            cell_fp_placements(cx, cy, fx, fz, ptx, pty);
            int open = 0, fine = 0;
            for (int p = 0; p < FP_PLACEMENTS && !fine; p++) {
                int x0 = ptx[p], y0 = pty[p];
                if (prefix_any(nwalk, tw, th, x0, y0, x0 + fx - 1, y0 + fz - 1))
                    continue;
                open = 1;
                fine = !prefix_any(ngood, tw, th, x0 - 1, y0 - 1, x0 + fx, y0 + fz);
            }
            out[cy * cw + cx] = (uint8_t)(open && !fine ? PATH_BAD_HALF : 0);
        }
    }
    tak_free(nwalk);
    tak_free(ngood);
}

/* The mask is a set of footprint placements, so the footprint the
 * caller resolved is part of the key. A query may name one of its own
 * and a def whose move class carries none falls back to the def, so
 * the class alone does not say what the mask means. */
static int pcache_find(const struct GameWorld *world, int cw, int ch,
                       const MoveClassDef *mc, int fallback_slope,
                       int fx, int fz) {
    for (int i = 0; i < g_pcache_n; i++) {
        if (g_pcache[i].world == world && g_pcache[i].mc == mc &&
            g_pcache[i].fallback_slope == fallback_slope &&
            g_pcache[i].fx == fx && g_pcache[i].fz == fz &&
            g_pcache[i].cw == cw && g_pcache[i].ch == ch)
            return i;
    }
    if (g_pcache_n >= PCACHE_MAX) {
        /* new map or class churn: drop everything */
        TAK_PathCacheReset();
    }
    uint8_t *bits = (uint8_t *)tak_malloc((size_t)cw * ch);
    if (!bits) return -1;
    uint64_t build_t0 = dbg_now();
    /* Ground this class can cross, one answer per 16 px tile. It is
     * what both the bitmap and the clearance map are made of, which is
     * the only way the two can be relied on to agree: they used to be
     * built from different predicates and a plan applied both, so the
     * stricter won and a unit could be refused ground its own
     * clearance called wide enough. */
    int tw = (world->map_pixels_w + TAK_OCC_TILE_PX - 1) / TAK_OCC_TILE_PX;
    int th = (world->map_pixels_h + TAK_OCC_TILE_PX - 1) / TAK_OCC_TILE_PX;
    uint8_t *plain = NULL;
    if (tw > 0 && th > 0) plain = (uint8_t *)tak_malloc((size_t)tw * th);
    if (plain) {
        int slope = movement_max_slope(mc, fallback_slope);
        Terrain_WalkableTiles(world, slope, plain, tw, th);
        for (int ty = 0; ty < th; ty++) {
            for (int tx = 0; tx < tw; tx++) {
                if (plain[ty * tw + tx] &&
                    !water_ok(world, mc, tile_to_world(tx),
                              tile_to_world(ty))) {
                    plain[ty * tw + tx] = 0;
                }
            }
        }
        for (int y = 0; y < ch; y++) {
            for (int x = 0; x < cw; x++) {
                int ptx[FP_PLACEMENTS], pty[FP_PLACEMENTS];
                cell_fp_placements(x, y, fx, fz, ptx, pty);
                int mask = 0;
                for (int p = 0; p < FP_PLACEMENTS; p++) {
                    int tx0 = ptx[p], ty0 = pty[p];
                    if (tx0 < 0 || ty0 < 0 ||
                        tx0 + fx > tw || ty0 + fz > th) continue;
                    int open = 1;
                    for (int row = 0; row < fz && open; row++) {
                        for (int col = 0; col < fx; col++) {
                            if (!plain[(ty0 + row) * tw + tx0 + col]) {
                                open = 0;
                                break;
                            }
                        }
                    }
                    if (open) mask |= 1 << p;
                }
                bits[y * cw + x] = (uint8_t)mask;
            }
        }
    } else {
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                bits[y * cw + x] = (uint8_t)cell_fp_mask_slow(
                    world, x, y, cw, ch, mc, fallback_slope, fx, fz);
    }
    uint8_t *cellbad = (uint8_t *)tak_malloc((size_t)cw * ch);
    if (cellbad) cell_bad_fill(world, mc, fallback_slope, fx, fz, cw, ch, cellbad);
    g_dbg_rebuilds++;
    g_dbg_rebuild_clock += dbg_now() - build_t0;
    int i = g_pcache_n++;
    memset(&g_pcache[i], 0, sizeof(g_pcache[i]));
    g_pcache[i].world = world;
    g_pcache[i].mc = mc;
    g_pcache[i].fallback_slope = fallback_slope;
    g_pcache[i].fx = fx;
    g_pcache[i].fz = fz;
    g_pcache[i].cw = cw;
    g_pcache[i].ch = ch;
    g_pcache[i].bits = bits;
    g_pcache[i].plain = plain;
    g_pcache[i].clear = NULL;
    g_pcache[i].clear_version = 0;
    g_pcache[i].comp = NULL;
    g_pcache[i].tw = plain ? tw : 0;
    g_pcache[i].th = plain ? th : 0;
    g_pcache[i].cellbad = cellbad;
    return i;
}

/* A tile with nothing built on it. Gate cells count as open here
 * whoever owns them; the per-owner rule is applied live on the
 * footprint tiles. */
static int tile_unbuilt(const struct GameWorld *world, int tx, int ty) {
    if (!world->occ) return 1;
    if (Occ_IsGateTile(world, tx, ty)) return 1;
    return Occ_QueryTileStatic(world, tx, ty, 0) != 1;
}

static const uint8_t *clearance_get(int ci) {
    const struct GameWorld *world = g_pcache[ci].world;
    const uint8_t *plain = g_pcache[ci].plain;
    int tw = g_pcache[ci].tw, th = g_pcache[ci].th;
    if (!plain || tw <= 0 || th <= 0) return NULL;
    if (g_pcache[ci].clear &&
        g_pcache[ci].clear_version == world->occ_version) {
        return g_pcache[ci].clear;
    }
    if (!g_pcache[ci].clear) {
        g_pcache[ci].clear = (uint8_t *)tak_malloc((size_t)tw * th);
        if (!g_pcache[ci].clear) return NULL;
    }
    uint8_t *c = g_pcache[ci].clear;
    uint64_t build_t0 = dbg_now();
    /* Terrain comes from the same per tile map the bitmap is built
     * from, so the two structures cannot disagree about the ground a
     * class may plan on. It used to be one sample per 32 px cell,
     * inherited by both tiles of the pair, while the bitmap swept
     * footprint corners, and a plan applied both. */
    /* Largest free square anchored at each tile: one pass from the far
     * corner, each tile one more than the least of its right, lower and
     * diagonal neighbours. */
    for (int ty = th - 1; ty >= 0; ty--) {
        for (int tx = tw - 1; tx >= 0; tx--) {
            if (!plain[ty * tw + tx] || !tile_unbuilt(world, tx, ty)) {
                c[ty * tw + tx] = 0;
                continue;
            }
            int best = 0;
            if (tx + 1 < tw && ty + 1 < th) {
                best = c[ty * tw + tx + 1];
                if (c[(ty + 1) * tw + tx] < best) best = c[(ty + 1) * tw + tx];
                if (c[(ty + 1) * tw + tx + 1] < best)
                    best = c[(ty + 1) * tw + tx + 1];
            }
            c[ty * tw + tx] = (uint8_t)(best < 255 ? best + 1 : 255);
        }
    }
    g_pcache[ci].clear_version = world->occ_version;
    g_dbg_rebuilds++;
    g_dbg_rebuild_clock += dbg_now() - build_t0;
    return c;
}

/* ── The long route distance field ────────────────────────────────
 * A straight line to the goal tells a search nothing about the ground
 * in between, so on a long order it opens every cell in a growing
 * disc and only learns about the detour when it walks into it. The
 * field replaces that guess with measured ground distance to a few
 * fixed marks. For any mark m, |d(m, goal) - d(m, cell)| can never
 * exceed the real cell to goal distance, so taking the largest of
 * them, and the straight line, still never overstates what is left.
 * A search with a heuristic that never overstates still returns a
 * cheapest route, so the field buys nodes and not route quality.
 *
 * The distances are measured over the terrain bitmap alone. That is
 * every cell a live search may enter and more, because clearance,
 * structures and parked units only ever take cells away, so the
 * measurement stays under the live distance whatever is built. The
 * field belongs to the bitmap it was measured on and is held and
 * dropped with it, so a destroyed blocking feature takes it too.
 *
 * Steps cost what the search charges: ten straight, fourteen
 * diagonal, twice the height step, and no cutting a blocked corner. */

/* The dearest step the sweep charges: fourteen plus twice the largest
 * height step a byte heightmap can hold. Capping a dearer step only
 * lowers a measured distance, which keeps it under the live one. */
#define PATH_FIELD_MAX_EDGE 525
/* Below this many cells of straight line, a plan is short enough that
 * the flat search opens few cells and the field is not worth it. */
#define PATH_FIELD_MIN_CELLS 32

typedef struct FieldHeap {
    int *key;
    int *node;
    int  n;
    int  cap;
} FieldHeap;

static void fheap_push(FieldHeap *h, int key, int node) {
    if (h->n >= h->cap) return;
    int i = h->n++;
    h->key[i] = key;
    h->node[i] = node;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (h->key[p] <= h->key[i]) break;
        int tk = h->key[i]; h->key[i] = h->key[p]; h->key[p] = tk;
        int tn = h->node[i]; h->node[i] = h->node[p]; h->node[p] = tn;
        i = p;
    }
}

static int fheap_pop(FieldHeap *h, int *out_key) {
    int node = h->node[0];
    *out_key = h->key[0];
    h->n--;
    h->key[0] = h->key[h->n];
    h->node[0] = h->node[h->n];
    int i = 0;
    for (;;) {
        int l = i * 2 + 1, r = l + 1, b = i;
        if (l < h->n && h->key[l] < h->key[b]) b = l;
        if (r < h->n && h->key[r] < h->key[b]) b = r;
        if (b == i) break;
        int tk = h->key[i]; h->key[i] = h->key[b]; h->key[b] = tk;
        int tn = h->node[i]; h->node[i] = h->node[b]; h->node[b] = tn;
        i = b;
    }
    return node;
}

/* Ground distance from one cell to every other over the terrain
 * bitmap. Fills dist with -1 where the cell cannot be reached, and
 * returns the reached cell that is furthest away, lowest index first,
 * which is the next mark to measure from. */
static int field_sweep(const uint8_t *bits, const uint8_t *cellbad,
                       int cw, int ch, int src, int32_t *dist,
                       FieldHeap *heap) {
    int cells = cw * ch;
    for (int i = 0; i < cells; i++) dist[i] = -1;
    heap->n = 0;
    dist[src] = 0;
    fheap_push(heap, 0, src);
    static const int dirs[8][3] = {
        { 1, 0,10 }, {-1, 0,10 }, { 0, 1,10 }, { 0,-1,10 },
        { 1, 1,14 }, {-1, 1,14 }, { 1,-1,14 }, {-1,-1,14 }
    };
    int far = src, far_d = 0;
    while (heap->n > 0) {
        int d;
        int cur = fheap_pop(heap, &d);
        if (d != dist[cur]) continue;         /* an older, dearer entry */
        if (d > far_d || (d == far_d && cur < far)) { far_d = d; far = cur; }
        int cx = cur % cw, cy = cur / cw;
        int bad0 = cellbad[cur];
        for (int di = 0; di < 8; di++) {
            int nx = cx + dirs[di][0], ny = cy + dirs[di][1];
            if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
            int ni = ny * cw + nx;
            if (!bits[ni]) continue;
            if (dirs[di][0] && dirs[di][1]) {
                if (!bits[cy * cw + nx] || !bits[ny * cw + cx]) continue;
            }
            int w = dirs[di][2] + bad0 + cellbad[ni];
            if (w > PATH_FIELD_MAX_EDGE) w = PATH_FIELD_MAX_EDGE;
            int nd = d + w;
            if (dist[ni] < 0 || nd < dist[ni]) {
                dist[ni] = nd;
                fheap_push(heap, nd, ni);
            }
        }
    }
    return far;
}

/* Build the marks and their distances, once per map and move class.
 * The first mark is the cell furthest from the middle of the map, the
 * second the cell furthest from the first, and the third the cell
 * furthest from both, which is the usual way to spread them so that
 * one of them lies beyond whatever the route has to go round. */
static int field_build(int ci) {
    const uint8_t *bits = g_pcache[ci].bits;
    const uint8_t *cellbad = g_pcache[ci].cellbad;
    int cw = g_pcache[ci].cw, ch = g_pcache[ci].ch;
    int cells = cw * ch;
    g_pcache[ci].field_tried = 1;
    if (!bits || !cellbad || cells <= 0) return 0;

    /* Seed at the open cell nearest the middle. */
    int seed = -1, seed_d = INT_MAX;
    for (int i = 0; i < cells; i++) {
        if (!bits[i]) continue;
        int d = iabs32(i % cw - cw / 2) + iabs32(i / cw - ch / 2);
        if (d < seed_d) { seed_d = d; seed = i; }
    }
    if (seed < 0) return 0;

    uint64_t build_t0 = dbg_now();
    FieldHeap heap;
    /* One entry per edge that ever improves a cell, plus the source:
     * the sweep can never hold more than that at once. */
    heap.cap = cells * 8 + 8;
    heap.key = (int *)tak_malloc((size_t)heap.cap * sizeof(int));
    heap.node = (int *)tak_malloc((size_t)heap.cap * sizeof(int));
    int32_t *scratch = (int32_t *)tak_malloc((size_t)cells * sizeof(int32_t));
    if (!heap.key || !heap.node || !scratch) {
        if (heap.key) tak_free(heap.key);
        if (heap.node) tak_free(heap.node);
        if (scratch) tak_free(scratch);
        return 0;
    }
    int mark = field_sweep(bits, cellbad, cw, ch, seed, scratch, &heap);
    int kept = 0;
    for (int m = 0; m < PATH_FIELD_MARKS; m++) {
        int32_t *f = (int32_t *)tak_malloc((size_t)cells * sizeof(int32_t));
        if (!f) break;
        int next = field_sweep(bits, cellbad, cw, ch, mark, f, &heap);
        g_pcache[ci].field[kept++] = f;
        if (m + 1 >= PATH_FIELD_MARKS) break;
        /* The next mark is the cell furthest from every mark so far. */
        int best = -1, best_d = -1;
        for (int i = 0; i < cells; i++) {
            if (!bits[i]) continue;
            int32_t least = -1;
            for (int k = 0; k < kept; k++) {
                int32_t v = g_pcache[ci].field[k][i];
                if (v < 0) { least = -1; break; }
                if (least < 0 || v < least) least = v;
            }
            if (least > best_d) { best_d = least; best = i; }
        }
        mark = best >= 0 ? best : next;
    }
    g_pcache[ci].field_n = kept;
    tak_free(heap.key);
    tak_free(heap.node);
    tak_free(scratch);
    g_dbg_rebuilds++;
    g_dbg_rebuild_clock += dbg_now() - build_t0;
    return kept;
}

static int field_get(int ci) {
    if (!g_pcache[ci].field_tried) field_build(ci);
    return g_pcache[ci].field_n;
}

/* ── Connected ground ─────────────────────────────────────────────
 * One label per path cell: two cells with the same label have a
 * route between them, and 0 is ground this class cannot stand on.
 * The search takes a diagonal step only when both orthogonal
 * neighbours are open, so a four way fill labels exactly what it
 * can walk. It is built from the passability bitmap, which is
 * terrain alone, so nothing built or parked ever divides the map.
 * Built on first ask and held with the rest of the layer. */
static const uint16_t *components_get(int ci) {
    if (g_pcache[ci].comp) return g_pcache[ci].comp;
    int cw = g_pcache[ci].cw, ch = g_pcache[ci].ch;
    const uint8_t *bits = g_pcache[ci].bits;
    if (!bits || cw <= 0 || ch <= 0) return NULL;
    int cells = cw * ch;
    uint16_t *comp = (uint16_t *)tak_malloc((size_t)cells * sizeof(uint16_t));
    int *stack = (int *)tak_malloc((size_t)cells * sizeof(int));
    if (!comp || !stack) {
        if (comp) tak_free(comp);
        if (stack) tak_free(stack);
        return NULL;
    }
    uint64_t build_t0 = dbg_now();
    memset(comp, 0, (size_t)cells * sizeof(uint16_t));
    static const int dirs4[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    unsigned label = 0;
    for (int seed = 0; seed < cells; seed++) {
        if (!bits[seed] || comp[seed]) continue;
        /* Out of labels: what is left keeps 0, which reads as
         * unknown and lets a caller plan rather than refuse. */
        if (label >= 0xFFFFu) break;
        label++;
        int n = 0;
        stack[n++] = seed;
        comp[seed] = (uint16_t)label;
        while (n > 0) {
            int cur = stack[--n];
            int cx = cur % cw, cy = cur / cw;
            for (int k = 0; k < 4; k++) {
                int nx = cx + dirs4[k][0], ny = cy + dirs4[k][1];
                if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
                int ni = ny * cw + nx;
                if (!bits[ni] || comp[ni]) continue;
                comp[ni] = (uint16_t)label;
                stack[n++] = ni;
            }
        }
    }
    tak_free(stack);
    g_pcache[ci].comp = comp;
    g_dbg_rebuilds++;
    g_dbg_rebuild_clock += dbg_now() - build_t0;
    return comp;
}

/* The label at a cell, or at the nearest labelled cell within two
 * of it, so a unit parked on ground its own class calls closed
 * still answers for the ground beside it. 0 when there is none. */
static uint16_t comp_near(const uint16_t *comp, int cw, int ch,
                          int cx, int cy) {
    for (int r = 0; r <= 2; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (r > 0 && dx > -r && dx < r && dy > -r && dy < r)
                    continue;
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= cw || y >= ch) continue;
                uint16_t v = comp[y * cw + x];
                if (v) return v;
            }
        }
    }
    return 0;
}

int TAK_PathGroundConnected(const struct GameWorld *world,
                            const struct MoveClassDef *move_class,
                            int fallback_max_slope,
                            int32_t ax, int32_t ay,
                            int32_t bx, int32_t by) {
    if (!world || world->map_pixels_w <= 0 || world->map_pixels_h <= 0)
        return 1;
    int cw = (world->map_pixels_w + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int ch = (world->map_pixels_h + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int slope = movement_max_slope(move_class, fallback_max_slope);
    int cfx, cfz;
    class_footprint(move_class, &cfx, &cfz);
    int ci = pcache_find(world, cw, ch, move_class, slope, cfx, cfz);
    if (ci < 0) return 1;
    const uint16_t *comp = components_get(ci);
    if (!comp) return 1;
    if (ax < 0 || ay < 0 || bx < 0 || by < 0) return 1;
    int axc = world_to_cell(ax), ayc = world_to_cell(ay);
    int bxc = world_to_cell(bx), byc = world_to_cell(by);
    if (axc >= cw || ayc >= ch || bxc >= cw || byc >= ch) return 1;
    uint16_t a = comp_near(comp, cw, ch, axc, ayc);
    uint16_t b = comp_near(comp, cw, ch, bxc, byc);
    if (a == 0 || b == 0) return 1;
    return a == b;
}

int TAK_PathClearanceAt(const struct GameWorld *world,
                        const struct MoveClassDef *move_class,
                        int fallback_max_slope,
                        int tile_x, int tile_y) {
    if (!world || world->map_pixels_w <= 0 || world->map_pixels_h <= 0)
        return 0;
    int cw = (world->map_pixels_w + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int ch = (world->map_pixels_h + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int slope = movement_max_slope(move_class, fallback_max_slope);
    int cfx, cfz;
    class_footprint(move_class, &cfx, &cfz);
    int ci = pcache_find(world, cw, ch, move_class, slope, cfx, cfz);
    if (ci < 0) return 0;
    const uint8_t *c = clearance_get(ci);
    if (!c) return 0;
    if (tile_x < 0 || tile_y < 0 || tile_x >= g_pcache[ci].tw ||
        tile_y >= g_pcache[ci].th) return 0;
    return c[tile_y * g_pcache[ci].tw + tile_x];
}

/* Debug view of the two cached structures a plan judges ground with:
 * the per cell placement mask and the clearance map, each asked
 * whether any placement in this path cell is open to this class.
 * They are built from one predicate and must answer alike, and a test
 * asserts it. Terrain and structures only, with no live occupancy. */
int TAK_PathDebugCellOpen(const struct GameWorld *world,
                          const struct MoveClassDef *move_class,
                          int fallback_max_slope,
                          int cell_x, int cell_y,
                          int *bitmap_open, int *clearance_open) {
    if (bitmap_open) *bitmap_open = 0;
    if (clearance_open) *clearance_open = 0;
    if (!world || world->map_pixels_w <= 0 || world->map_pixels_h <= 0)
        return 0;
    int cw = (world->map_pixels_w + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int ch = (world->map_pixels_h + PATH_CELL_PX - 1) / PATH_CELL_PX;
    if (cell_x < 0 || cell_y < 0 || cell_x >= cw || cell_y >= ch) return 0;
    int slope = movement_max_slope(move_class, fallback_max_slope);
    int fx, fz;
    class_footprint(move_class, &fx, &fz);
    int ci = pcache_find(world, cw, ch, move_class, slope, fx, fz);
    if (ci < 0) return 0;
    const uint8_t *clear = clearance_get(ci);
    int need = fx > fz ? fx : fz;
    int bits_open = g_pcache[ci].bits
                  ? (g_pcache[ci].bits[cell_y * cw + cell_x] != 0) : 0;
    int ptx[FP_PLACEMENTS], pty[FP_PLACEMENTS];
    cell_fp_placements(cell_x, cell_y, fx, fz, ptx, pty);
    int clear_open = 0;
    if (clear) {
        for (int p = 0; p < FP_PLACEMENTS; p++) {
            int tx0 = ptx[p], ty0 = pty[p];
            if (tx0 < 0 || ty0 < 0 ||
                tx0 >= g_pcache[ci].tw || ty0 >= g_pcache[ci].th) continue;
            if (clear[ty0 * g_pcache[ci].tw + tx0] >= need) {
                clear_open = 1;
                break;
            }
        }
    }
    /* The bitmap is terrain, the clearance map is terrain plus what
     * is built: on ground with nothing built they have to agree. */
    if (bitmap_open) *bitmap_open = bits_open;
    if (clearance_open) *clearance_open = clear_open;
    return 1;
}

/* One resolved plan request. */
typedef struct PlanCtx {
    const struct GameWorld *world;
    const MoveClassDef *mc;
    int slope;
    int player_id;
    int self_plus1;
    int fx, fz;          /* footprint tiles, at least 1 */
    int need;            /* clearance needed: the larger side */
    int cw, ch;
    int mtw, mth;        /* map extent in tiles, for placement bounds */
    const uint8_t *bits;
    const uint8_t *plain;    /* per tile ground, for the crossing test */
    const uint8_t *clear;
    int tw, th;
    /* Set when the unit's own cell is not a cell a route may start
     * from. The search may then also cross ground the unit can walk
     * but not plan on, at a heavy cost, so the route it hands back
     * begins under the unit's feet instead of across a bay. */
    int allow_pinch;
    /* Set for a flow field: ground and what is built, and no one parked
     * on it, since a group's own members stand all over its start. */
    int ignore_live;
    /* One byte per cell: PMASK_UNASKED, or which placements are legal
     * there with the live layer on, after mask_connected(). */
    uint8_t *pmask;
    /* Bad ground per cell, and the long route field: the marks
     * this plan may use and each mark's distance to the goal cell. */
    const uint8_t *cellbad;
    const int32_t *field[PATH_FIELD_MARKS];
    int32_t field_goal[PATH_FIELD_MARKS];
    int field_n;
} PlanCtx;

/* What a cell of that kind costs. Ten is one cell of open ground, so
 * this is a hundred cells of detour: a route uses a pinch only when
 * there is really nothing else. */
#define PATH_PINCH_COST 1000

/* The first legal placement of the footprint inside this cell, or -1.
 * Ground comes from the cached placement mask, width from the
 * clearance map, and occupancy is asked of exactly the tiles a unit
 * standing on that placement would stamp. The mask refuses most
 * cells in one byte, so the clearance lookup and the occupancy sweep
 * run once per surviving placement and usually once per cell.
 *
 * The occupancy block used to be widened to the cell's own two tiles
 * per axis. That covered the tile a one tile unit really stamped
 * standing on a cell centre, which is not the tile the terrain test
 * looked at. The waypoint is the placement centre now, so the block
 * is the footprint and nothing more. */
static int placement_legal(const PlanCtx *c, int tx0, int ty0, int live) {
    if (tx0 < 0 || ty0 < 0 ||
        tx0 + c->fx > c->mtw || ty0 + c->fz > c->mth) return 0;
    if (c->clear) {
        if (tx0 >= c->tw || ty0 >= c->th) return 0;
        if (c->clear[ty0 * c->tw + tx0] < c->need) return 0;
    }
    if (live && c->world->occ) {
        /* The owner rule for gates and parked units, over the tiles
         * this placement covers. */
        for (int dy = 0; dy < c->fz; dy++) {
            for (int dx = 0; dx < c->fx; dx++) {
                if (Occ_QueryTilePlan(c->world, tx0 + dx, ty0 + dy,
                                      c->player_id, c->self_plus1) == 1)
                    return 0;
            }
        }
    }
    return 1;
}

/* Which of the four placements are legal, one bit each in the order
 * cell_fp_placements() gives them. */
static int cell_legal_mask(const PlanCtx *c, int x, int y, int live) {
    if (x < 0 || y < 0 || x >= c->cw || y >= c->ch) return 0;
    int mask = c->bits
             ? c->bits[y * c->cw + x]
             : cell_fp_mask_slow(c->world, x, y, c->cw, c->ch, c->mc,
                                 c->slope, c->fx, c->fz);
    if (!mask) return 0;
    int ptx[FP_PLACEMENTS], pty[FP_PLACEMENTS];
    cell_fp_placements(x, y, c->fx, c->fz, ptx, pty);
    int legal = 0;
    for (int p = 0; p < FP_PLACEMENTS; p++) {
        if (!(mask & (1 << p))) continue;
        if (placement_legal(c, ptx[p], pty[p], live)) legal |= 1 << p;
    }
    return legal;
}

static int cell_placement(const PlanCtx *c, int x, int y, int live,
                          int *out_tx, int *out_ty) {
    int legal = cell_legal_mask(c, x, y, live);
    if (!legal) return -1;
    int ptx[FP_PLACEMENTS], pty[FP_PLACEMENTS];
    cell_fp_placements(x, y, c->fx, c->fz, ptx, pty);
    for (int p = 0; p < FP_PLACEMENTS; p++) {
        if (!(legal & (1 << p))) continue;
        if (out_tx) *out_tx = ptx[p];
        if (out_ty) *out_ty = pty[p];
        return p;
    }
    return -1;
}

/* ── Links between cells ───────────────────────────────────────────
 * A cell with any legal placement used to be a cell a route could
 * enter from any neighbour. That is not ground a unit can cross: two
 * neighbours can each hold a placement with no legal placement in
 * between, and the unit that was routed through them stops at the gap
 * with a route still in hand. A step between cells is now a slide of
 * the footprint by one tile from a legal placement in the one to a
 * legal placement in the other, and the four placements of a cell
 * count only while they hang together, so a route of cells is always
 * a chain of placements each one tile from the last.
 *
 * Bits follow cell_fp_placements(): 0 is (0,0), 1 is (-1,0), 2 is
 * (0,-1), 3 is (-1,-1), the offsets from the cell's own anchor. */
#define PMASK_UNASKED 0xFF

static int pbit(int ox, int oy) { return (ox < 0 ? 1 : 0) + (oy < 0 ? 2 : 0); }

/* Two placements on a diagonal with neither of the others between
 * them do not hang together. Keep the first, so every machine keeps
 * the same one. */
static int mask_connected(int mask) {
    if (mask == 0x9) return 0x1;
    if (mask == 0x6) return 0x2;
    return mask;
}

static int plan_mask(const PlanCtx *c, int x, int y) {
    if (x < 0 || y < 0 || x >= c->cw || y >= c->ch) return 0;
    int live = !c->ignore_live;
    if (!c->pmask) return mask_connected(cell_legal_mask(c, x, y, live));
    uint8_t *m = &c->pmask[y * c->cw + x];
    if (*m == PMASK_UNASKED)
        *m = (uint8_t)mask_connected(cell_legal_mask(c, x, y, live));
    return *m;
}

/* The placement in the cell being left and the one in the cell being
 * entered that a step of (dx,dy) slides between, tried in a fixed
 * order, want_a first when it is one of them. 0 when there is none. A
 * diagonal slide needs the two placements it passes beside as well,
 * which belong to the cells either side of the corner. */
static int cells_link(const PlanCtx *c, int ax, int ay, int dx, int dy,
                      int want_a, int *out_a, int *out_b) {
    int A = plan_mask(c, ax, ay), B = plan_mask(c, ax + dx, ay + dy);
    if (!A || !B) return 0;
    if (dx != 0 && dy != 0) {
        int a = pbit(dx > 0 ? 0 : -1, dy > 0 ? 0 : -1);
        int b = pbit(dx > 0 ? -1 : 0, dy > 0 ? -1 : 0);
        if (!(A & (1 << a)) || !(B & (1 << b))) return 0;
        int C = plan_mask(c, ax + dx, ay), D = plan_mask(c, ax, ay + dy);
        if (C && !(C & (1 << pbit(dx > 0 ? -1 : 0, dy > 0 ? 0 : -1)))) return 0;
        if (D && !(D & (1 << pbit(dx > 0 ? 0 : -1, dy > 0 ? -1 : 0)))) return 0;
        if (out_a) *out_a = a;
        if (out_b) *out_b = b;
        return 1;
    }
    /* Along one axis the slide keeps its lane on the other, and there
     * are two lanes. */
    int found = 0, fa = 0, fb = 0;
    for (int lane = 0; lane >= -1; lane--) {
        int a, b;
        if (dx != 0) {
            a = pbit(dx > 0 ? 0 : -1, lane);
            b = pbit(dx > 0 ? -1 : 0, lane);
        } else {
            a = pbit(lane, dy > 0 ? 0 : -1);
            b = pbit(lane, dy > 0 ? -1 : 0);
        }
        if (!(A & (1 << a)) || !(B & (1 << b))) continue;
        if (!found || a == want_a) { fa = a; fb = b; found = 1; }
        if (a == want_a) break;
    }
    if (!found) return 0;
    if (out_a) *out_a = fa;
    if (out_b) *out_b = fb;
    return 1;
}

static int cell_ok_live(const PlanCtx *c, int x, int y, int live) {
    return cell_placement(c, x, y, live, NULL, NULL) >= 0;
}

static int cell_ok(const PlanCtx *c, int x, int y) {
    /* A plan asks about the same cell from every neighbour. */
    if (c->pmask) return plan_mask(c, x, y) != 0;
    return cell_ok_live(c, x, y, !c->ignore_live);
}

/* Where a waypoint in this cell goes: the centre of the placement the
 * plan accepted, so the point the mover walks to is a point the
 * footprint fits. A goal that is a unit is judged without the live
 * layer, the way the goal cell was chosen, and a cell the route only
 * crosses has no placement at all, so its centre is the best there
 * is. */
static void cell_waypoint(const PlanCtx *c, int cell,
                          int32_t *wx, int32_t *wy) {
    int x = cell % c->cw, y = cell / c->cw;
    int tx = 0, ty = 0;
    int p = cell_placement(c, x, y, 1, &tx, &ty);
    if (p < 0) p = cell_placement(c, x, y, 0, &tx, &ty);
    if (p < 0) {
        *wx = cell_to_world(x);
        *wy = cell_to_world(y);
        return;
    }
    *wx = placement_centre(tx, c->fx);
    *wy = placement_centre(ty, c->fz);
}

/* ── The way out of a trapped start ─────────────────────────────────
 * A unit whose body fits no placement of its cell walks out by the
 * step's own rule, 16 px tiles under its centre, to tiles that centre
 * a legal placement. Each is a start for the cell search. */
#define ESC_SEEDS 256
#define ESC_R 128                       /* tiles each way from the unit */
#define ESC_W (2 * ESC_R + 1)
#define ESC_TILES (ESC_W * ESC_W)
#define ESC_SETTLE_MAX 4096
#define ESC_HEAP (ESC_SETTLE_MAX * 8 + 8)
/* A tile in its own cell or in a cell a route may stand in costs what
 * open ground does. One in a cell with no placement costs, a cell of
 * it, PATH_PINCH_COST more, the price the cell crossings had. */
#define ESC_NEAR 5
#define ESC_NEAR_DIAG 7
#define ESC_FAR ((PATH_PINCH_COST + 10) / 2)
#define ESC_FAR_DIAG ((PATH_PINCH_COST + 14) / 2)
/* A search that runs out within this many cells began in a pocket the
 * step let the unit's body into and no slide of the cell grid leads out
 * of. It is walked out the way a trapped start is, by the step's own
 * rule, to placements outside the pocket: where the body could not get
 * out, nor can the walk. */
#define PATH_POCKET_CELLS 64

typedef struct EscSeed { int cell, bit, tile, cost; } EscSeed;

static const int g_esc_d8[8][2] = {
    { 1, 0 }, {-1, 0 }, { 0, 1 }, { 0,-1 },
    { 1, 1 }, {-1, 1 }, { 1,-1 }, {-1,-1 }
};
#define ESC_NONE 0xFF

static struct {
    int ox, oy;                         /* the window's top left tile */
    int settled;
    int32_t dist[ESC_TILES];
    uint8_t from[ESC_TILES];            /* the step that reached it */
    uint8_t seen[ESC_TILES];            /* 1 queued, 2 settled */
    uint8_t bad[ESC_TILES];             /* body counts, 0xFF not yet */
    int32_t hkey[ESC_HEAP], hnode[ESC_HEAP];
    int hn;
    EscSeed seed[ESC_SEEDS];
    int nseed;
    int chain[ESC_SETTLE_MAX + 1];
} g_esc;

/* Terrain and water at the tile, as the placement mask reads them. */
static int esc_ground(const PlanCtx *c, int tx, int ty) {
    if (tx < 0 || ty < 0 || tx >= c->mtw || ty >= c->mth) return 0;
    if (c->plain) return tx < c->tw && ty < c->th && c->plain[ty * c->tw + tx];
    int32_t px = tile_to_world(tx), py = tile_to_world(ty);
    return Terrain_IsWalkable(c->world, px, py, c->slope) &&
           water_ok(c->world, c->mc, px, py);
}

/* The walk's node for a tile is the point a body stands on there: the
 * tile's centre for an odd footprint, its top left corner for an even
 * one, where the body is the placement this tile centres. */
static int32_t esc_pos(int t, int fp) {
    return (int32_t)t * TAK_OCC_TILE_PX + (fp & 1) * (TAK_OCC_TILE_PX / 2);
}

/* Water at the point the body stands on there, where the step tests it.
 * The mover never relaxes it (unit_step_refused). */
static int esc_dry(const PlanCtx *c, int tx, int ty) {
    return water_ok(c->world, c->mc, esc_pos(tx, c->fx), esc_pos(ty, c->fz));
}

/* Tiles of that body that stand on ground the class may not, as the
 * step counts them. */
static int esc_body_bad(const PlanCtx *c, int tx, int ty) {
    int bx = tx - c->fx / 2, by = ty - c->fz / 2, bad = 0;
    for (int dy = 0; dy < c->fz; dy++)
        for (int dx = 0; dx < c->fx; dx++)
            bad += !esc_ground(c, bx + dx, by + dy);
    return bad;
}

/* A node's body count, counted once a walk. */
static int esc_bad(const PlanCtx *c, int tx, int ty) {
    uint8_t *m = &g_esc.bad[(ty - g_esc.oy) * ESC_W + (tx - g_esc.ox)];
    if (*m == 0xFF) *m = (uint8_t)esc_body_bad(c, tx, ty);
    return *m;
}

/* Nothing built or parked under the body of a unit centred on this
 * tile, the tiles the step stamps (Occ_TileOf(centre - fx * 8)). */
static int esc_body_clear(const PlanCtx *c, int tx, int ty) {
    if (!c->world->occ) return 1;
    int bx = tx - c->fx / 2, by = ty - c->fz / 2;
    for (int dy = 0; dy < c->fz; dy++)
        for (int dx = 0; dx < c->fx; dx++)
            if (Occ_QueryTilePlan(c->world, bx + dx, by + dy,
                                  c->player_id, c->self_plus1) == 1)
                return 0;
    return 1;
}

/* Is this tile the centre tile of a legal placement, and which? That
 * placement's centre lies in the tile, so it is one of its cell's. */
static int esc_exit(const PlanCtx *c, int tx, int ty, int *cell, int *bit) {
    int ax = tx - c->fx / 2, ay = ty - c->fz / 2;
    int cx = world_to_cell(placement_centre(ax, c->fx));
    int cy = world_to_cell(placement_centre(ay, c->fz));
    if (ax < 0 || ay < 0 || cx < 0 || cy < 0 || cx >= c->cw || cy >= c->ch)
        return 0;
    int ptx[FP_PLACEMENTS], pty[FP_PLACEMENTS];
    cell_fp_placements(cx, cy, c->fx, c->fz, ptx, pty);
    for (int p = 0; p < FP_PLACEMENTS; p++) {
        if (ptx[p] != ax || pty[p] != ay) continue;
        if (!((plan_mask(c, cx, cy) >> p) & 1)) return 0;
        *cell = cell_index(cx, cy, c->cw);
        *bit = p;
        return 1;
    }
    return 0;
}

/* Ties go to the lower tile index, so every machine settles alike. */
static int esc_less(int ka, int na, int kb, int nb) {
    return ka < kb || (ka == kb && na < nb);
}

static void esc_push(int key, int node) {
    int i = g_esc.hn++;
    while (i > 0) {
        int up = (i - 1) / 2;
        if (!esc_less(key, node, g_esc.hkey[up], g_esc.hnode[up])) break;
        g_esc.hkey[i] = g_esc.hkey[up];
        g_esc.hnode[i] = g_esc.hnode[up];
        i = up;
    }
    g_esc.hkey[i] = key;
    g_esc.hnode[i] = node;
}

static int esc_pop(int *node) {
    if (g_esc.hn <= 0) return -1;
    int key = g_esc.hkey[0];
    *node = g_esc.hnode[0];
    int lk = g_esc.hkey[--g_esc.hn], ln = g_esc.hnode[g_esc.hn];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        int mk = lk, mn = ln;
        if (l < g_esc.hn && esc_less(g_esc.hkey[l], g_esc.hnode[l], mk, mn)) {
            m = l;
            mk = g_esc.hkey[l];
            mn = g_esc.hnode[l];
        }
        if (r < g_esc.hn && esc_less(g_esc.hkey[r], g_esc.hnode[r], mk, mn))
            m = r;
        if (m == i) break;
        g_esc.hkey[i] = g_esc.hkey[m];
        g_esc.hnode[i] = g_esc.hnode[m];
        i = m;
    }
    g_esc.hkey[i] = lk;
    g_esc.hnode[i] = ln;
    return key;
}

/* Walk out from the unit's tile, to no start in a skip cell, by the
 * step's rule: water at the centre never, and never onto more bad
 * ground under the body than it stands on, beside a diagonal too.
 * What is built or parked is crossed only while the body already
 * stands on some, as the mover's occ_escape allows. Gives how many
 * starts were found, the cheapest for each cell. */
static int esc_flood(const PlanCtx *c, int32_t x, int32_t y,
                     const uint8_t *skip) {
    /* The node whose body is the one the unit stands in now. */
    int t0x = Occ_TileOf(x - c->fx * 8 + 8) + c->fx / 2;
    int t0y = Occ_TileOf(y - c->fz * 8 + 8) + c->fz / 2;
    g_esc.ox = t0x - ESC_R;
    g_esc.oy = t0y - ESC_R;
    g_esc.hn = 0;
    g_esc.nseed = 0;
    g_esc.settled = 0;
    memset(g_esc.seen, 0, sizeof(g_esc.seen));
    memset(g_esc.bad, 0xFF, sizeof(g_esc.bad));
    int scx = t0x / OCC_PER_PATH_CELL, scy = t0y / OCC_PER_PATH_CELL;
    int n0 = ESC_R * ESC_W + ESC_R;
    g_esc.dist[n0] = 0;
    g_esc.from[n0] = ESC_NONE;
    g_esc.seen[n0] = 1;
    esc_push(0, n0);
    int node, key;
    while (g_esc.settled < ESC_SETTLE_MAX && (key = esc_pop(&node)) >= 0) {
        if (g_esc.seen[node] == 2 || key != g_esc.dist[node]) continue;
        g_esc.seen[node] = 2;
        g_esc.settled++;
        int wx = node % ESC_W, wy = node / ESC_W;
        int tx = g_esc.ox + wx, ty = g_esc.oy + wy;
        int cell = 0, bit = 0;
        if (node != n0 && esc_exit(c, tx, ty, &cell, &bit) &&
            !(skip && skip[cell])) {
            int dup = 0;
            for (int i = 0; i < g_esc.nseed && !dup; i++)
                dup = g_esc.seed[i].cell == cell;
            if (!dup && g_esc.nseed < ESC_SEEDS) {
                EscSeed *s = &g_esc.seed[g_esc.nseed++];
                s->cell = cell;
                s->bit = bit;
                s->tile = node;
                s->cost = key;
            }
            continue;
        }
        int body_bad = esc_bad(c, tx, ty);
        int body_clear = esc_body_clear(c, tx, ty);
        for (int di = 0; di < 8; di++) {
            int ddx = g_esc_d8[di][0], ddy = g_esc_d8[di][1];
            int nwx = wx + ddx, nwy = wy + ddy;
            if (nwx < 0 || nwy < 0 || nwx >= ESC_W || nwy >= ESC_W) continue;
            int ni = nwy * ESC_W + nwx;
            if (g_esc.seen[ni] == 2) continue;
            int ntx = tx + ddx, nty = ty + ddy;
            if (ntx < 0 || nty < 0 || ntx >= c->mtw || nty >= c->mth) continue;
            if (!esc_dry(c, ntx, nty)) continue;
            int diag = ddx && ddy;
            if (esc_bad(c, ntx, nty) > body_bad) continue;
            if (diag && (esc_bad(c, ntx, ty) > body_bad ||
                         esc_bad(c, tx, nty) > body_bad)) continue;
            if (body_clear) {
                if (!esc_body_clear(c, ntx, nty)) continue;
                if (diag && (!esc_body_clear(c, ntx, ty) ||
                             !esc_body_clear(c, tx, nty))) continue;
            }
            int ncx = ntx / OCC_PER_PATH_CELL, ncy = nty / OCC_PER_PATH_CELL;
            int near = (ncx == scx && ncy == scy) || plan_mask(c, ncx, ncy);
            int nk = key + (near ? (diag ? ESC_NEAR_DIAG : ESC_NEAR)
                                 : (diag ? ESC_FAR_DIAG : ESC_FAR));
            if (g_esc.seen[ni] && g_esc.dist[ni] <= nk) continue;
            if (g_esc.hn >= ESC_HEAP) continue;
            g_esc.seen[ni] = 1;
            g_esc.dist[ni] = nk;
            g_esc.from[ni] = (uint8_t)di;
            esc_push(nk, ni);
        }
    }
    return g_esc.nseed;
}

static const EscSeed *esc_seed_of(int cell) {
    for (int i = 0; i < g_esc.nseed; i++)
        if (g_esc.seed[i].cell == cell) return &g_esc.seed[i];
    return NULL;
}

/* The walk out to a start as raw route points: its first tile, the
 * tiles where it turns, then the start placement's centre. Gives how
 * many, or -1 when they do not fit in cap. */
static int esc_lay(const PlanCtx *c, const EscSeed *s, int *px, int *py,
                   int cap) {
    int *chain = g_esc.chain;
    int len = 0;
    for (int t = s->tile;;) {
        if (len > ESC_SETTLE_MAX) return -1;
        chain[len++] = t;
        int d = g_esc.from[t];
        if (d == ESC_NONE) break;
        t -= g_esc_d8[d][1] * ESC_W + g_esc_d8[d][0];
    }
    /* chain runs exit first, the unit's own tile last. */
    int n = 0;
    for (int i = len - 2; i >= 1; i--) {
        int prev = chain[i + 1], t = chain[i], next = chain[i - 1];
        if (i < len - 2 && t - prev == next - t) continue;
        if (n + 1 >= cap) return -1;
        px[n] = esc_pos(g_esc.ox + t % ESC_W, c->fx);
        py[n] = esc_pos(g_esc.oy + t / ESC_W, c->fz);
        n++;
    }
    int ptx[FP_PLACEMENTS], pty[FP_PLACEMENTS];
    cell_fp_placements(s->cell % c->cw, s->cell / c->cw, c->fx, c->fz, ptx, pty);
    if (n + 1 >= cap) return -1;
    px[n] = placement_centre(ptx[s->bit], c->fx);
    py[n] = placement_centre(pty[s->bit], c->fz);
    return n + 1;
}

/* Would the step walk the unit from a to b? Its centre never into
 * water it may not enter, and its body never onto more bad ground. */
static int step_line_ok(const PlanCtx *c, int32_t ax, int32_t ay,
                        int32_t bx, int32_t by) {
    int64_t dx = (int64_t)bx - ax, dy = (int64_t)by - ay;
    int64_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int64_t steps = adx > ady ? adx : ady;
    int lx = Occ_TileOf(ax - c->fx * 8 + 8) + c->fx / 2;
    int ly = Occ_TileOf(ay - c->fz * 8 + 8) + c->fz / 2;
    int bad = esc_body_bad(c, lx, ly);
    for (int64_t s = 1; s <= steps; s++) {
        int32_t x = ax + (int32_t)(dx * s / steps);
        int32_t y = ay + (int32_t)(dy * s / steps);
        if (!water_ok(c->world, c->mc, x, y)) return 0;
        int tx = Occ_TileOf(x - c->fx * 8 + 8) + c->fx / 2;
        int ty = Occ_TileOf(y - c->fz * 8 + 8) + c->fz / 2;
        if (tx == lx && ty == ly) continue;
        if (tx != lx && ty != ly &&
            (esc_body_bad(c, tx, ly) > bad || esc_body_bad(c, lx, ty) > bad))
            return 0;
        int b = esc_body_bad(c, tx, ty);
        if (b > bad) return 0;
        bad = b;
        lx = tx;
        ly = ty;
    }
    return 1;
}

/* Does the body with its top left at tile (tx, ty) stand wholly on
 * ground the class crosses, with nothing built under it? */
static int body_at_ok(const PlanCtx *c, int tx, int ty) {
    if (tx < 0 || ty < 0 || tx + c->fx > c->mtw || ty + c->fz > c->mth) return 0;
    if (c->clear)
        return tx < c->tw && ty < c->th && c->clear[ty * c->tw + tx] >= c->need;
    for (int r = 0; r < c->fz; r++)
        for (int k = 0; k < c->fx; k++)
            if (!esc_ground(c, tx + k, ty + r)) return 0;
    return 1;
}

/* Does a body walking a to b, anchored as the step anchors it, stand
 * wholly on that ground at every pixel and beside every corner? */
static int body_line_ok(const PlanCtx *c, int32_t ax, int32_t ay,
                        int32_t bx, int32_t by) {
    int64_t dx = (int64_t)bx - ax, dy = (int64_t)by - ay;
    int64_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int64_t steps = adx > ady ? adx : ady;
    int lx = INT_MIN, ly = INT_MIN;
    for (int64_t s = 0; s <= steps; s++) {
        int32_t x = steps ? ax + (int32_t)(dx * s / steps) : ax;
        int32_t y = steps ? ay + (int32_t)(dy * s / steps) : ay;
        int32_t bxp = x - c->fx * 8 + 8, byp = y - c->fz * 8 + 8;
        if (bxp < 0 || byp < 0) return 0;
        int tx = (int)(bxp / TAK_OCC_TILE_PX), ty = (int)(byp / TAK_OCC_TILE_PX);
        if (tx == lx && ty == ly) continue;
        if (lx != INT_MIN && tx != lx && ty != ly &&
            (!body_at_ok(c, tx, ly) || !body_at_ok(c, lx, ty)))
            return 0;
        lx = tx;
        ly = ty;
        if (!body_at_ok(c, tx, ty)) return 0;
    }
    return 1;
}

static int start_reaches_cell(const PlanCtx *c, int cx, int cy,
                              int32_t x, int32_t y) {
    int legal = mask_connected(cell_legal_mask(c, cx, cy, 0));
    int ptx[FP_PLACEMENTS], pty[FP_PLACEMENTS];
    cell_fp_placements(cx, cy, c->fx, c->fz, ptx, pty);
    for (int p = 0; p < FP_PLACEMENTS; p++) {
        if (!((legal >> p) & 1)) continue;
        if (body_line_ok(c, x, y, placement_centre(ptx[p], c->fx),
                         placement_centre(pty[p], c->fz)))
            return 1;
    }
    return 0;
}

/* Shift a goal off ground no route can end on. ignore_live skips the
 * live occupancy check, which is what a goal that IS a unit wants:
 * the target's own parked footprint must not push the route a cell
 * short of contact. */
static int nearest_open(const PlanCtx *c, int *x, int *y, int ignore_live) {
    *x = clampi(*x, 0, c->cw - 1);
    *y = clampi(*y, 0, c->ch - 1);
    if (cell_ok_live(c, *x, *y, !ignore_live)) return 1;
    int best_x = -1, best_y = -1;
    int best_d = INT_MAX;
    for (int r = 1; r <= 16; r++) {
        for (int yy = *y - r; yy <= *y + r; yy++) {
            for (int xx = *x - r; xx <= *x + r; xx++) {
                if (xx != *x - r && xx != *x + r &&
                    yy != *y - r && yy != *y + r) {
                    continue;
                }
                if (!cell_ok(c, xx, yy)) continue;
                int d = iabs32(xx - *x) + iabs32(yy - *y);
                if (d < best_d) {
                    best_d = d;
                    best_x = xx;
                    best_y = yy;
                }
            }
        }
        if (best_x >= 0) {
            *x = best_x;
            *y = best_y;
            return 1;
        }
    }
    return 0;
}

static int heuristic(int ax, int ay, int bx, int by) {
    int dx = iabs32(ax - bx);
    int dy = iabs32(ay - by);
    int mn = dx < dy ? dx : dy;
    int mx = dx > dy ? dx : dy;
    return 14 * mn + 10 * (mx - mn);
}

/* What is left to pay from this cell: the straight line, and for
 * every mark the field carries, the difference between its distance
 * to the goal and its distance to here. None of them can overstate
 * the real remainder, so the largest is the best estimate that still
 * leaves the route a cheapest one. */
static int plan_h(const PlanCtx *c, int cell, int gx, int gy) {
    int best = heuristic(cell % c->cw, cell / c->cw, gx, gy);
    for (int m = 0; m < c->field_n; m++) {
        int32_t v = c->field[m][cell];
        if (v < 0) continue;
        int32_t d = c->field_goal[m] - v;
        if (d < 0) d = -d;
        if ((int)d > best) best = (int)d;
    }
    return best;
}

static int plan_bad(const PlanCtx *c, int cell) {
    if (c->cellbad) return c->cellbad[cell];
    return cell_bad_direct(c->world, c->mc, c->slope, c->fx, c->fz,
                           cell % c->cw, cell / c->cw);
}

static int heap_less(const int *f, int a, int b) {
    if (f[a] != f[b]) return f[a] < f[b];
    return a < b;
}

static void heap_push(int *heap, int *n, const int *f, int v) {
    int i = (*n)++;
    heap[i] = v;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!heap_less(f, heap[i], heap[p])) break;
        int tmp = heap[i]; heap[i] = heap[p]; heap[p] = tmp;
        i = p;
    }
}

static int heap_pop(int *heap, int *n, const int *f) {
    int out = heap[0];
    heap[0] = heap[--(*n)];
    int i = 0;
    for (;;) {
        int l = i * 2 + 1;
        int r = l + 1;
        int b = i;
        if (l < *n && heap_less(f, heap[l], heap[b])) b = l;
        if (r < *n && heap_less(f, heap[r], heap[b])) b = r;
        if (b == i) break;
        int tmp = heap[i]; heap[i] = heap[b]; heap[b] = tmp;
        i = b;
    }
    return out;
}

int TAK_PathPlan(const struct GameWorld *world,
                 int32_t start_x, int32_t start_y,
                 int32_t goal_x, int32_t goal_y,
                 int max_slope,
                 int player_id,
                 TAK_Path *out_path) {
    return TAK_PathPlanForMoveClass(world, start_x, start_y, goal_x, goal_y,
                                    NULL, max_slope, player_id, out_path);
}

int TAK_PathPlanForMoveClass(const struct GameWorld *world,
                             int32_t start_x, int32_t start_y,
                             int32_t goal_x, int32_t goal_y,
                             const MoveClassDef *move_class,
                             int fallback_max_slope,
                             int player_id,
                             TAK_Path *out_path) {
    TAK_PathQuery q;
    memset(&q, 0, sizeof(q));
    q.move_class = move_class;
    q.fallback_max_slope = fallback_max_slope;
    q.player_id = player_id;
    return TAK_PathPlanQuery(world, start_x, start_y, goal_x, goal_y, &q,
                             out_path);
}

/* Append one waypoint; returns 0 when the path is full. */
static int path_put(TAK_Path *out, const PlanCtx *c, int cell) {
    if (out->count >= TAK_PATH_MAX_WAYPOINTS) return 0;
    cell_waypoint(c, cell, &out->x[out->count], &out->y[out->count]);
    out->count++;
    return 1;
}

/* ── Laying the route ──────────────────────────────────────────────
 * The search gives cells. The mover walks points, in straight lines,
 * so the points are the placements the links slid between: every one
 * is a tile from the one before it, and a line between two that are
 * kept is a line along placements that were each found legal. A
 * compressed route keeps a point where the direction of that chain
 * changes and the last, as the cell route did (legacy:22488-22495,
 * legacy:22528-22535). An uncompressed one keeps the last placement
 * in each cell, a point a cell as before. A cell the route only
 * crosses has no placement, so its centre is the best there is, and
 * the chain starts again on the far side. */
static void lay_anchor(const PlanCtx *c, int cell, int bit, int *tx, int *ty) {
    int ptx[FP_PLACEMENTS], pty[FP_PLACEMENTS];
    cell_fp_placements(cell % c->cw, cell / c->cw, c->fx, c->fz, ptx, pty);
    *tx = ptx[bit];
    *ty = pty[bit];
}

static void route_lay(TAK_Path *out, PlanCtx *c, int start, int start_bit,
                      const int *chain, int chain_len,
                      int32_t unit_x, int32_t unit_y, int compress,
                      int *px, int *py, int n0, int cap) {
    int n = n0;                 /* raw points, in world pixels */
    int have = 0, cur = 0;      /* the placement the chain stands on */
    int prev = start;
    /* Where each cell's last raw point is, for the uncompressed form. */
    int last_of_cell_from = n0;
    int32_t ox = unit_x, oy = unit_y;

    int smask = plan_mask(c, start % c->cw, start / c->cw);
    if (start_bit >= 0) {
        cur = start_bit;
        have = 1;
    } else if (smask) {
        /* The placement nearest where the unit really stands. */
        int64_t best = -1;
        for (int p = 0; p < FP_PLACEMENTS; p++) {
            if (!(smask & (1 << p))) continue;
            int tx, ty;
            lay_anchor(c, start, p, &tx, &ty);
            int64_t dx = (int64_t)placement_centre(tx, c->fx) - unit_x;
            int64_t dy = (int64_t)placement_centre(ty, c->fz) - unit_y;
            int64_t d2 = dx * dx + dy * dy;
            if (best < 0 || d2 < best) { best = d2; cur = p; }
        }
        have = 1;
        int tx, ty;
        lay_anchor(c, start, cur, &tx, &ty);
        ox = placement_centre(tx, c->fx);
        oy = placement_centre(ty, c->fz);
    }

    out->count = 0;
    if (!compress) {
        for (int i = 0; i < n0 && out->count < TAK_PATH_MAX_WAYPOINTS; i++) {
            out->x[out->count] = px[i];
            out->y[out->count] = py[i];
            out->count++;
        }
    }
    for (int i = 0; i < chain_len && n + 3 < cap; i++) {
        int cell = chain[i];
        int dx = cell % c->cw - prev % c->cw;
        int dy = cell / c->cw - prev / c->cw;
        int a = 0, b = 0;
        int first_of_cell = n;
        if (have && cells_link(c, prev % c->cw, prev / c->cw, dx, dy, cur,
                               &a, &b)) {
            if (a != cur) {
                /* Move inside the cell being left to the placement the
                 * slide goes from: one tile, or two round the corner
                 * through whichever of the others is legal. */
                int pm = plan_mask(c, prev % c->cw, prev / c->cw);
                int beside = ((pm >> (cur ^ 1)) & 1) + ((pm >> (cur ^ 2)) & 1);
                if ((a ^ cur) == 3 && beside < 2) {
                    int via = (pm & (1 << (cur ^ 1))) ? (cur ^ 1) : (cur ^ 2);
                    int tx, ty;
                    lay_anchor(c, prev, via, &tx, &ty);
                    px[n] = placement_centre(tx, c->fx);
                    py[n] = placement_centre(ty, c->fz);
                    n++;
                }
                int tx, ty;
                lay_anchor(c, prev, a, &tx, &ty);
                px[n] = placement_centre(tx, c->fx);
                py[n] = placement_centre(ty, c->fz);
                n++;
                first_of_cell = n;
            }
            int tx, ty;
            lay_anchor(c, cell, b, &tx, &ty);
            px[n] = placement_centre(tx, c->fx);
            py[n] = placement_centre(ty, c->fz);
            n++;
            cur = b;
        } else {
            /* Into, out of or along a crossing. */
            int tx = 0, ty = 0;
            int p = cell_placement(c, cell % c->cw, cell / c->cw, 1, &tx, &ty);
            if (p < 0) p = cell_placement(c, cell % c->cw, cell / c->cw, 0,
                                          &tx, &ty);
            if (p >= 0) {
                px[n] = placement_centre(tx, c->fx);
                py[n] = placement_centre(ty, c->fz);
                cur = p;
                have = (plan_mask(c, cell % c->cw, cell / c->cw) >> p) & 1;
            } else {
                px[n] = cell_to_world(cell % c->cw);
                py[n] = cell_to_world(cell / c->cw);
                have = 0;
            }
            n++;
        }
        if (!compress) {
            /* A point a cell: the last placement stood on in it. The
             * points laid in the cell being left were that cell's. */
            if (first_of_cell > last_of_cell_from && out->count > 0) {
                out->x[out->count - 1] = px[first_of_cell - 1];
                out->y[out->count - 1] = py[first_of_cell - 1];
            }
            if (out->count >= TAK_PATH_MAX_WAYPOINTS) break;
            out->x[out->count] = px[n - 1];
            out->y[out->count] = py[n - 1];
            out->count++;
            last_of_cell_from = n;
        }
        prev = cell;
    }
    if (!compress || n == 0) return;

    /* Keep a point where the chain turns, and the last. The first is
     * kept when skipping it would take the body off its ground, or the
     * unit, from where it stands, where the step will not. */
    int first = 0;
    if (n > 1) {
        int k = 1;
        while (k < n - 1 && px[k] - px[k - 1] == px[k + 1] - px[k] &&
               py[k] - py[k - 1] == py[k + 1] - py[k])
            k++;
        int turn = px[0] - ox != px[1] - px[0] || py[0] - oy != py[1] - py[0];
        first = (turn && !body_line_ok(c, ox, oy, px[k], py[k])) ||
                !step_line_ok(c, unit_x, unit_y, px[k], py[k]);
    }
    for (int i = 0; i < n; i++) {
        int keep = i == n - 1 || (i == 0 && first);
        if (!keep && i > 0) {
            int fx = px[i] - px[i - 1], fy = py[i] - py[i - 1];
            int tx = px[i + 1] - px[i], ty = py[i + 1] - py[i];
            keep = fx != tx || fy != ty;
        }
        if (!keep) continue;
        if (out->count >= TAK_PATH_MAX_WAYPOINTS) break;
        out->x[out->count] = px[i];
        out->y[out->count] = py[i];
        out->count++;
    }
}

/* One plan's view of the world: who plans, with what footprint, over
 * which cached layers. Gives the layer's cache slot, or -1. */
static int plan_ctx_setup(PlanCtx *c, const struct GameWorld *world,
                          const TAK_PathQuery *query) {
    memset(c, 0, sizeof(*c));
    c->world = world;
    c->mc = query->move_class;
    c->slope = movement_max_slope(c->mc, query->fallback_max_slope);
    c->player_id = query->player_id;
    c->self_plus1 = query->self_plus1;
    c->fx = query->footprint_x > 0 ? query->footprint_x
          : (c->mc && c->mc->footprint_x > 0) ? c->mc->footprint_x : 1;
    c->fz = query->footprint_z > 0 ? query->footprint_z
          : (c->mc && c->mc->footprint_z > 0) ? c->mc->footprint_z : 1;
    c->need = c->fx > c->fz ? c->fx : c->fz;
    c->cw = (world->map_pixels_w + PATH_CELL_PX - 1) / PATH_CELL_PX;
    c->ch = (world->map_pixels_h + PATH_CELL_PX - 1) / PATH_CELL_PX;
    c->mtw = world->map_pixels_w / TAK_OCC_TILE_PX;
    c->mth = world->map_pixels_h / TAK_OCC_TILE_PX;
    if (c->cw <= 0 || c->ch <= 0) return -1;
    /* Key on the RESOLVED slope: a class with its own maxslope ignores
     * the per-def fallback entirely, so keying on the fallback gave one
     * cache entry per unit type and thrashed the 16-slot table. */
    int ci = pcache_find(world, c->cw, c->ch, c->mc, c->slope, c->fx, c->fz);
    if (ci >= 0) {
        c->bits = g_pcache[ci].bits;
        c->plain = g_pcache[ci].plain;
        c->clear = clearance_get(ci);
        c->tw = g_pcache[ci].tw;
        c->th = g_pcache[ci].th;
        c->cellbad = g_pcache[ci].cellbad;
    }
    return ci;
}

/* Whether ground shuts in the closed cells, parked units not counted. */
static int pocket_of_ground(const PlanCtx *c, const uint8_t *closed) {
    PlanCtx g = *c;
    g.pmask = NULL;
    g.ignore_live = 1;
    for (int cur = 0; cur < c->cw * c->ch; cur++) {
        if (!closed[cur]) continue;
        int cx = cur % c->cw, cy = cur / c->cw;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = cx + dx, ny = cy + dy;
                if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= c->cw || ny >= c->ch)
                    continue;
                if (closed[ny * c->cw + nx]) continue;
                if (dx && dy && (!plan_mask(&g, nx, cy) || !plan_mask(&g, cx, ny)))
                    continue;
                if (cells_link(&g, cx, cy, dx, dy, -1, NULL, NULL)) return 0;
            }
    }
    return 1;
}

static int plan_query(const struct GameWorld *world,
                      int32_t start_x, int32_t start_y,
                      int32_t goal_x, int32_t goal_y,
                      const TAK_PathQuery *query,
                      TAK_Path *out_path, int walk_out);

int TAK_PathPlanQuery(const struct GameWorld *world,
                      int32_t start_x, int32_t start_y,
                      int32_t goal_x, int32_t goal_y,
                      const TAK_PathQuery *query,
                      TAK_Path *out_path) {
    if (!world || !out_path || !query || world->map_pixels_w <= 0 ||
        world->map_pixels_h <= 0) {
        return 0;
    }
    g_dbg_plans++;
    if (g_dbg_reset_every > 0 &&
        ++g_dbg_since_reset >= g_dbg_reset_every) {
        g_dbg_since_reset = 0;
        TAK_PathCacheReset();
    }
    return plan_query(world, start_x, start_y, goal_x, goal_y, query,
                      out_path, 0);
}

/* walk_out starts the unit as a trapped one, whatever its cell. */
static int plan_query(const struct GameWorld *world,
                      int32_t start_x, int32_t start_y,
                      int32_t goal_x, int32_t goal_y,
                      const TAK_PathQuery *query,
                      TAK_Path *out_path, int walk_out) {
    memset(out_path, 0, sizeof(*out_path));
    PlanCtx c;
    int ci = plan_ctx_setup(&c, world, query);
    int cells = c.cw * c.ch;
    if (c.cw <= 0 || c.ch <= 0 || cells <= 0) return 0;

    int sx = clampi(world_to_cell(start_x), 0, c.cw - 1);
    int sy = clampi(world_to_cell(start_y), 0, c.ch - 1);
    int gx = world_to_cell(goal_x),  gy = world_to_cell(goal_y);
    /* ── Where a route begins ──────────────────────────────────────
     * Under the unit, always. Whoever is parked around it does not
     * make its own cell a cell to plan away from. Only the ground and
     * what is built there do, and when even that refuses the cell the
     * answer is a route out along ground it can walk, not a start it
     * cannot reach. The ring scan this replaced took the nearest cell
     * it liked within 16, with no connectivity test of any kind, and
     * on the reported map that put the start 208 px away across a
     * bay: the unit pressed into the shore for two and a half
     * minutes because the route it was following began over there. */
    c.allow_pinch = walk_out || !cell_ok_live(&c, sx, sy, 0) ||
                    (body_line_ok(&c, start_x, start_y, start_x, start_y) &&
                     !start_reaches_cell(&c, sx, sy, start_x, start_y));
    if (c.allow_pinch) {
        /* The way out is ground the unit has to walk, so the route
         * starts where it really is rather than at a cell centre. */
        out_path->start_x = start_x;
        out_path->start_y = start_y;
    } else {
        /* The first segment runs from the start cell, not from
         * wherever the unit stands in it (legacy:22528-22535). */
        out_path->start_x = cell_to_world(sx);
        out_path->start_y = cell_to_world(sy);
    }
    if (!nearest_open(&c, &gx, &gy, query->goal_is_unit ? 1 : 0)) return 0;

    int *g = (int *)tak_malloc((size_t)cells * sizeof(int));
    int *f = (int *)tak_malloc((size_t)cells * sizeof(int));
    int *parent = (int *)tak_malloc((size_t)cells * sizeof(int));
    int *heap = (int *)tak_malloc(((size_t)cells * 8u + ESC_SEEDS) * sizeof(int));
    uint8_t *closed = (uint8_t *)tak_malloc((size_t)cells);
    c.pmask = (uint8_t *)tak_malloc((size_t)cells);
    if (c.pmask) memset(c.pmask, PMASK_UNASKED, (size_t)cells);
    if (!g || !f || !parent || !heap || !closed || !c.pmask) {
        if (g) tak_free(g);
        if (f) tak_free(f);
        if (parent) tak_free(parent);
        if (heap) tak_free(heap);
        if (closed) tak_free(closed);
        if (c.pmask) tak_free(c.pmask);
        return 0;
    }
    for (int i = 0; i < cells; i++) {
        g[i] = INT_MAX / 4;
        f[i] = INT_MAX / 4;
        parent[i] = -1;
    }
    memset(closed, 0, (size_t)cells);

    int start = cell_index(sx, sy, c.cw);
    int goal = cell_index(gx, gy, c.cw);
    /* A long enough order gets the field. Every cell the search opens
     * is ground a route may stand on, so its estimate holds. */
    if (g_use_field && ci >= 0 && c.bits &&
        heuristic(sx, sy, gx, gy) >= PATH_FIELD_MIN_CELLS * 10) {
        int marks = field_get(ci);
        for (int m = 0; m < marks; m++) {
            const int32_t *fld = g_pcache[ci].field[m];
            if (!fld || fld[goal] < 0) continue;
            c.field[c.field_n] = fld;
            c.field_goal[c.field_n] = fld[goal];
            c.field_n++;
        }
    }
    int heap_n = 0;
    int best = -1;
    int best_h = INT_MAX;
    int seeds = 0;
    if (c.allow_pinch) {
        seeds = esc_flood(&c, start_x, start_y, NULL);
        for (int i = 0; i < seeds; i++) {
            const EscSeed *s = &g_esc.seed[i];
            int cost = s->cost + plan_bad(&c, s->cell);
            g[s->cell] = cost;
            f[s->cell] = cost + plan_h(&c, s->cell, gx, gy);
            heap_push(heap, &heap_n, f, s->cell);
        }
    } else {
        g[start] = 0;
        f[start] = plan_h(&c, start, gx, gy);
        heap_push(heap, &heap_n, f, start);
        best = start;
        best_h = plan_h(&c, start, gx, gy);
    }

    static const int dirs[8][3] = {
        { 1, 0,10 }, {-1, 0,10 }, { 0, 1,10 }, { 0,-1,10 },
        { 1, 1,14 }, {-1, 1,14 }, { 1,-1,14 }, {-1,-1,14 }
    };
    int found = 0;
    int capped = 0;
    int expanded = 0;
    /* 1024 nodes only covers ~a 32x32-cell bubble; long routes hit the
     * cap and fell back to "closest explored", which walks into dead
     * ends. Node expansion is a byte lookup now (passability bitmap),
     * so a deeper search is affordable; the per-tick plan budget in
     * units.c bounds total cost. */
    int max_expanded = cells;
    if (max_expanded > 8192) max_expanded = 8192;
    int walked = c.allow_pinch;
    for (int pass = 0;; pass++) {
    while (heap_n > 0) {
        int cur = heap_pop(heap, &heap_n, f);
        if (closed[cur]) continue;
        closed[cur] = 1;
        if (++expanded > max_expanded) {
            capped = 1;
            break;
        }
        if (cur == goal) { found = 1; break; }
        int cx = cur % c.cw;
        int cy = cur / c.cw;
        int cur_h = plan_h(&c, cur, gx, gy);
        if (cur_h < best_h) {
            best_h = cur_h;
            best = cur;
        }
        int bad0 = plan_bad(&c, cur);
        int free_start = !plan_mask(&c, cx, cy) && cur == start && !c.allow_pinch;
        if (!plan_mask(&c, cx, cy) && !free_start) continue;
        for (int di = 0; di < 8; di++) {
            int nx = cx + dirs[di][0];
            int ny = cy + dirs[di][1];
            if (!cell_ok(&c, nx, ny)) continue;
            /* No corner cutting past a blocked cell. */
            if (dirs[di][0] != 0 && dirs[di][1] != 0) {
                if (!cell_ok(&c, cx + dirs[di][0], cy)) continue;
                if (!cell_ok(&c, cx, cy + dirs[di][1])) continue;
            }
            int ni = cell_index(nx, ny, c.cw);
            if (closed[ni]) continue;
            if (!free_start &&
                !cells_link(&c, cx, cy, dirs[di][0], dirs[di][1], -1,
                            NULL, NULL)) continue;
            int step = dirs[di][2] + bad0 + plan_bad(&c, ni);
            int ng = g[cur] + step;
            if (ng < g[ni]) {
                parent[ni] = cur;
                g[ni] = ng;
                f[ni] = ng + plan_h(&c, ni, gx, gy);
                heap_push(heap, &heap_n, f, ni);
            }
        }
    }
    if (pass == 0 && !found && !capped && !c.allow_pinch &&
        expanded <= PATH_POCKET_CELLS && pocket_of_ground(&c, closed)) {
        walked = 1;
        seeds = esc_flood(&c, start_x, start_y, closed);
        if (seeds > 0) {
            c.allow_pinch = 1;
            /* Nearest the goal by this search, as a trapped start's. */
            best = -1;
            best_h = INT_MAX;
            out_path->start_x = start_x;
            out_path->start_y = start_y;
            for (int i = 0; i < seeds; i++) {
                const EscSeed *s = &g_esc.seed[i];
                int cost = s->cost + plan_bad(&c, s->cell);
                g[s->cell] = cost;
                f[s->cell] = cost + plan_h(&c, s->cell, gx, gy);
                parent[s->cell] = -1;
                heap_push(heap, &heap_n, f, s->cell);
            }
            continue;
        }
    }
    break;
    }

    if (walked) expanded += g_esc.settled;
    g_dbg_work += (uint64_t)expanded;
    if (found || (capped && best >= 0 && best != start)) {
        int end = found ? goal : best;
        int root = end, chain_len = 0;
        while (parent[root] >= 0 && chain_len <= cells) {
            root = parent[root];
            chain_len++;
        }
        for (int p = end, i = chain_len - 1; i >= 0; p = parent[p], i--) {
            heap[i] = p;
        }
        int n0 = 0, sbit = -1;
        const EscSeed *s = c.allow_pinch ? esc_seed_of(root) : NULL;
        if (c.allow_pinch) {
            n0 = s ? esc_lay(&c, s, g, f, cells / 2) : -1;
            sbit = s ? s->bit : -1;
        }
        if (n0 < 0 || chain_len > cells) {
            /* no route */
        } else if (chain_len == 0 && n0 == 0) {
            path_put(out_path, &c, end);
        } else {
            route_lay(out_path, &c, root, sbit, heap, chain_len, start_x,
                      start_y, query->compress, g, f, n0, cells);
        }
    }

    /* A route the step cannot start from where the unit stands begins
     * with a walk out instead, as a trapped start's does. */
    int retry = !walk_out && !c.allow_pinch && out_path->count > 0 &&
        !step_line_ok(&c, start_x, start_y, out_path->x[0], out_path->y[0]);
    tak_free(g);
    tak_free(f);
    tak_free(parent);
    tak_free(heap);
    tak_free(closed);
    if (c.pmask) tak_free(c.pmask);
    if (retry)
        return plan_query(world, start_x, start_y, goal_x, goal_y, query,
                          out_path, 1);
    return out_path->count;
}

/* ── Flow fields ───────────────────────────────────────────────────
 * A group sent to one place shares one field instead of a search
 * each: the ground distance from the goal to every cell a route may
 * stand on, and from each cell the next one on the way. A member's
 * route is that chain from where it stands, laid into waypoints the
 * way a search's route is. The ground is terrain and what is built,
 * with nobody parked on it, since the group's own members stand all
 * over its start. The mover's give way and a replan around whoever
 * is parked take care of the rest.
 *
 * The field is a function of the ground alone, so a cold cache
 * builds the same field a warm one holds. It is kept for the few
 * goals in use and dropped with the rest of the layer. */
#define PATH_FLOW_SLOTS 4

typedef struct FlowField {
    int used;
    const MoveClassDef *mc;
    int slope, fx, fz, cw, ch;
    int goal;
    uint32_t occ_version;
    uint32_t last_use;
    int32_t *dist;
    int32_t *next;
    /* The sweep so far: cells whose distance is final, and the frontier
     * it stopped at, kept so the next member picks it up. */
    uint8_t *closed;
    FieldHeap heap;
} FlowField;

static FlowField g_flow[PATH_FLOW_SLOTS];
static uint32_t g_flow_clock;
static uint32_t g_dbg_flow_builds;
static uint64_t g_dbg_flow_clock;
static uint64_t g_dbg_flow_settled;
static int      g_dbg_flow_whole;

static void flow_free_sweep(FlowField *f) {
    if (f->closed) tak_free(f->closed);
    if (f->heap.key) tak_free(f->heap.key);
    if (f->heap.node) tak_free(f->heap.node);
    f->closed = NULL;
    memset(&f->heap, 0, sizeof(f->heap));
}

static void flow_free(FlowField *f) {
    flow_free_sweep(f);
    if (f->dist) tak_free(f->dist);
    if (f->next) tak_free(f->next);
    memset(f, 0, sizeof(*f));
}

static void flow_reset_all(void) {
    for (int i = 0; i < PATH_FLOW_SLOTS; i++) flow_free(&g_flow[i]);
}

/* Distance to the goal from every cell a route may stand on, over the
 * steps a search takes: a slide between placements, no cutting past a
 * blocked corner, ten straight, fourteen diagonal and twice the height
 * step. The walk is from the goal out, so each step is taken from the
 * far cell into the near one, which is the way a member walks it. */
/* A frontier push that grows rather than drops, so a sweep never loses
 * an entry however it is paused. */
static int flow_heap_push(FieldHeap *h, int key, int node) {
    if (h->n >= h->cap) {
        int cap = h->cap ? h->cap * 2 : 1024;
        int *k = (int *)tak_realloc(h->key, (size_t)cap * sizeof(int));
        if (!k) return 0;
        h->key = k;
        int *nd = (int *)tak_realloc(h->node, (size_t)cap * sizeof(int));
        if (!nd) return 0;
        h->node = nd;
        h->cap = cap;
    }
    fheap_push(h, key, node);
    return 1;
}

static int flow_sweep_begin(FlowField *f, int goal, int cells) {
    for (int i = 0; i < cells; i++) { f->dist[i] = -1; f->next[i] = -1; }
    f->closed = (uint8_t *)tak_malloc((size_t)cells);
    if (!f->closed) return 0;
    memset(f->closed, 0, (size_t)cells);
    memset(&f->heap, 0, sizeof(f->heap));
    f->dist[goal] = 0;
    return flow_heap_push(&f->heap, 0, goal);
}

/* Dijkstra out from the goal until `until` is settled, or everything
 * is when it is -1. Cells settle in the same order however often the
 * sweep stops, and every cell on a settled cell's way to the goal
 * settled before it, so a route read off a part swept field is the
 * route the whole field gives. */
static int flow_sweep(PlanCtx *c, FlowField *f, int until) {
    int32_t *dist = f->dist, *next = f->next;
    FieldHeap *heap = &f->heap;
    static const int dirs[8][3] = {
        { 1, 0,10 }, {-1, 0,10 }, { 0, 1,10 }, { 0,-1,10 },
        { 1, 1,14 }, {-1, 1,14 }, { 1,-1,14 }, {-1,-1,14 }
    };
    while (heap->n > 0 && (until < 0 || !f->closed[until])) {
        int d;
        int cur = fheap_pop(heap, &d);
        if (d != dist[cur]) continue;
        f->closed[cur] = 1;
        g_dbg_flow_settled++;
        int cx = cur % c->cw, cy = cur / c->cw;
        int bad0 = plan_bad(c, cur);
        for (int di = 0; di < 8; di++) {
            int nx = cx + dirs[di][0], ny = cy + dirs[di][1];
            if (nx < 0 || ny < 0 || nx >= c->cw || ny >= c->ch) continue;
            if (!cell_ok(c, nx, ny)) continue;
            /* From the neighbour into this cell. */
            int sx = -dirs[di][0], sy = -dirs[di][1];
            if (sx != 0 && sy != 0) {
                if (!cell_ok(c, nx + sx, ny) || !cell_ok(c, nx, ny + sy))
                    continue;
            }
            if (!cells_link(c, nx, ny, sx, sy, -1, NULL, NULL)) continue;
            int ni = ny * c->cw + nx;
            int step = dirs[di][2] + bad0 + plan_bad(c, ni);
            int nd = d + step;
            if (dist[ni] < 0 || nd < dist[ni]) {
                dist[ni] = nd;
                next[ni] = cur;
                if (!flow_heap_push(heap, nd, ni)) return 0;
            }
        }
    }
    if (heap->n == 0) flow_free_sweep(f);
    return 1;
}

static FlowField *flow_get(PlanCtx *c, int goal, int start) {
    uint64_t t0 = dbg_now();
    uint32_t ver = c->world->occ_version;
    FlowField *slot = NULL;
    for (int i = 0; i < PATH_FLOW_SLOTS; i++) {
        FlowField *f = &g_flow[i];
        if (!f->used) { if (!slot) slot = f; continue; }
        if (f->mc == c->mc && f->slope == c->slope && f->fx == c->fx &&
            f->fz == c->fz && f->cw == c->cw && f->ch == c->ch &&
            f->goal == goal && f->occ_version == ver) {
            f->last_use = ++g_flow_clock;
            int ok = !f->closed || flow_sweep(c, f, g_dbg_flow_whole ? -1 : start);
            g_dbg_flow_clock += dbg_now() - t0;
            if (!ok) { flow_free(f); return NULL; }
            return f;
        }
    }
    if (!slot) {
        slot = &g_flow[0];
        for (int i = 1; i < PATH_FLOW_SLOTS; i++)
            if (g_flow[i].last_use < slot->last_use) slot = &g_flow[i];
    }
    flow_free(slot);
    int cells = c->cw * c->ch;
    slot->dist = (int32_t *)tak_malloc((size_t)cells * sizeof(int32_t));
    slot->next = (int32_t *)tak_malloc((size_t)cells * sizeof(int32_t));
    int swept = slot->dist && slot->next && flow_sweep_begin(slot, goal, cells) &&
                flow_sweep(c, slot, g_dbg_flow_whole ? -1 : start);
    g_dbg_flow_clock += dbg_now() - t0;
    if (!swept) {
        flow_free(slot);
        return NULL;
    }
    slot->used = 1;
    slot->mc = c->mc;
    slot->slope = c->slope;
    slot->fx = c->fx;
    slot->fz = c->fz;
    slot->cw = c->cw;
    slot->ch = c->ch;
    slot->goal = goal;
    slot->occ_version = ver;
    slot->last_use = ++g_flow_clock;
    g_dbg_flow_builds++;
    return slot;
}

int TAK_PathPlanFlow(const struct GameWorld *world,
                     int32_t start_x, int32_t start_y,
                     int32_t goal_x, int32_t goal_y,
                     const TAK_PathQuery *query,
                     TAK_Path *out_path) {
    if (!world || !out_path || !query || world->map_pixels_w <= 0 ||
        world->map_pixels_h <= 0) {
        return -1;
    }
    memset(out_path, 0, sizeof(*out_path));
    /* A route off a field is a plan like any other to the counters and
     * to the cold cache test. */
    g_dbg_plans++;
    if (g_dbg_reset_every > 0 &&
        ++g_dbg_since_reset >= g_dbg_reset_every) {
        g_dbg_since_reset = 0;
        TAK_PathCacheReset();
    }
    PlanCtx c;
    int ci = plan_ctx_setup(&c, world, query);
    int cells = c.cw * c.ch;
    if (ci < 0 || cells <= 0 || !c.bits) return -1;
    c.ignore_live = 1;
    c.pmask = (uint8_t *)tak_malloc((size_t)cells);
    int *px = (int *)tak_malloc((size_t)cells * sizeof(int));
    int *py = (int *)tak_malloc((size_t)cells * sizeof(int));
    int *chain = (int *)tak_malloc((size_t)cells * sizeof(int));
    int n = -1;
    if (!c.pmask || !px || !py || !chain) goto done;
    memset(c.pmask, PMASK_UNASKED, (size_t)cells);

    int sx = clampi(world_to_cell(start_x), 0, c.cw - 1);
    int sy = clampi(world_to_cell(start_y), 0, c.ch - 1);
    int gx = world_to_cell(goal_x), gy = world_to_cell(goal_y);
    /* A member standing where no route may start takes a search, which
     * knows the way out. */
    if (!cell_ok(&c, sx, sy)) goto done;
    if (!nearest_open(&c, &gx, &gy, 1)) goto done;
    int start = cell_index(sx, sy, c.cw);
    int goal = cell_index(gx, gy, c.cw);
    FlowField *f = flow_get(&c, goal, start);
    if (!f || f->dist[start] < 0) goto done;

    out_path->start_x = cell_to_world(sx);
    out_path->start_y = cell_to_world(sy);
    if (start == goal) {
        path_put(out_path, &c, goal);
        n = out_path->count;
        goto done;
    }
    int len = 0;
    for (int p = f->next[start]; p >= 0 && len < cells; p = f->next[p]) {
        chain[len++] = p;
        if (p == goal) break;
    }
    if (len == 0 || chain[len - 1] != goal) goto done;
    route_lay(out_path, &c, start, -1, chain, len, start_x, start_y,
              query->compress, px, py, 0, cells);
    n = out_path->count > 0 ? out_path->count : -1;
    /* One the step cannot start from where the unit stands is a search's. */
    if (n > 0 && !step_line_ok(&c, start_x, start_y, out_path->x[0], out_path->y[0]))
        n = -1;
done:
    if (c.pmask) tak_free(c.pmask);
    if (px) tak_free(px);
    if (py) tak_free(py);
    if (chain) tak_free(chain);
    return n;
}

uint32_t TAK_PathDebugFlowBuilds(void) { return g_dbg_flow_builds; }
uint64_t TAK_PathDebugFlowClock(void) { return g_dbg_flow_clock; }
uint64_t TAK_PathDebugFlowSettled(void) { return g_dbg_flow_settled; }
void TAK_PathDebugFlowWhole(int on) { g_dbg_flow_whole = on ? 1 : 0; }
