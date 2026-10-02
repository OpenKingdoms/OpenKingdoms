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
    int       clear_full;  /* stale in a way the stamp log cannot say */
    int       tw, th;
    int16_t  *cellh;      /* terrain height at each cell centre */
    int32_t  *field[PATH_FIELD_MARKS];  /* cost to a mark, -1 out of reach */
    int       mark[PATH_FIELD_MARKS];
    int32_t  *seed_dist;  /* cost to the cell nearest the middle */
    int       seed;
    int       field_n;
    int       field_tried;
    uint16_t *comp;       /* per path cell: connected ground label */
} g_pcache[PCACHE_MAX];
static int g_pcache_n = 0;

/* ── Plan buffers ─────────────────────────────────────────────────
 * Per cell search state kept between plans, every entry at its start
 * value. A plan lists the cells it writes and the next one puts them
 * back, so a plan costs the cells it reaches, not the map. */
#define PATH_MAX_EXPANDED 8192
#define PATH_G_UNSET (INT_MAX / 4)
#define PMASK_UNASKED 0xFF

typedef struct PlanBuffers {
    int      cells;        /* cells each per cell array holds */
    int      heap_cap;
    int     *g, *f, *parent, *heap;
    uint8_t *closed, *pinched, *pmask, *cross;
    int     *px, *py;      /* the laid route's points, scratch */
    /* Cells the last plan wrote: search state, and pmask or cross. */
    int     *dirty, *dirty_memo;
    int      dirty_n, dirty_memo_n;
} PlanBuffers;

static PlanBuffers g_pbuf;

/* At most eight pushes per expanded cell and fewer than four per cell
 * of map, and after the search the heap holds the route, a cell each. */
static int plan_heap_cap(int cells) {
    int expand = cells < PATH_MAX_EXPANDED + 1 ? cells : PATH_MAX_EXPANDED + 1;
    int cap = expand * 8;
    return cap > cells ? cap : cells;
}

static void plan_buffers_free(void) {
    void *held[] = { g_pbuf.g, g_pbuf.f, g_pbuf.parent, g_pbuf.heap,
                     g_pbuf.closed, g_pbuf.pinched, g_pbuf.pmask,
                     g_pbuf.cross, g_pbuf.px, g_pbuf.py, g_pbuf.dirty,
                     g_pbuf.dirty_memo };
    for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
        if (held[i]) tak_free(held[i]);
    }
    memset(&g_pbuf, 0, sizeof(g_pbuf));
}

static int plan_buffers_grow(int cells) {
    plan_buffers_free();
    size_t n = (size_t)cells;
    int heap_cap = plan_heap_cap(cells);
    g_pbuf.g = (int *)tak_malloc(n * sizeof(int));
    g_pbuf.f = (int *)tak_malloc(n * sizeof(int));
    g_pbuf.parent = (int *)tak_malloc(n * sizeof(int));
    g_pbuf.heap = (int *)tak_malloc((size_t)heap_cap * sizeof(int));
    g_pbuf.closed = (uint8_t *)tak_malloc(n);
    g_pbuf.pinched = (uint8_t *)tak_malloc(n);
    g_pbuf.pmask = (uint8_t *)tak_malloc(n);
    g_pbuf.cross = (uint8_t *)tak_malloc(n);
    g_pbuf.px = (int *)tak_malloc(n * sizeof(int));
    g_pbuf.py = (int *)tak_malloc(n * sizeof(int));
    g_pbuf.dirty = (int *)tak_malloc(n * sizeof(int));
    g_pbuf.dirty_memo = (int *)tak_malloc(n * sizeof(int));
    if (!g_pbuf.g || !g_pbuf.f || !g_pbuf.parent || !g_pbuf.heap ||
        !g_pbuf.closed || !g_pbuf.pinched || !g_pbuf.pmask ||
        !g_pbuf.cross || !g_pbuf.px || !g_pbuf.py || !g_pbuf.dirty ||
        !g_pbuf.dirty_memo) {
        plan_buffers_free();
        return 0;
    }
    for (int i = 0; i < cells; i++) {
        g_pbuf.g[i] = PATH_G_UNSET;
        g_pbuf.f[i] = PATH_G_UNSET;
        g_pbuf.parent[i] = -1;
    }
    memset(g_pbuf.closed, 0, n);
    memset(g_pbuf.pinched, 0, n);
    memset(g_pbuf.pmask, PMASK_UNASKED, n);
    memset(g_pbuf.cross, 0, n);
    g_pbuf.cells = cells;
    g_pbuf.heap_cap = heap_cap;
    return 1;
}

/* The buffers for one plan over this many cells, with every cell the
 * last plan wrote put back first. */
static PlanBuffers *plan_buffers_begin(int cells) {
    if (cells > g_pbuf.cells) return plan_buffers_grow(cells) ? &g_pbuf : NULL;
    PlanBuffers *b = &g_pbuf;
    for (int k = 0; k < b->dirty_n; k++) {
        int i = b->dirty[k];
        b->g[i] = PATH_G_UNSET;
        b->f[i] = PATH_G_UNSET;
        b->parent[i] = -1;
        b->closed[i] = 0;
        b->pinched[i] = 0;
    }
    for (int k = 0; k < b->dirty_memo_n; k++) {
        int i = b->dirty_memo[k];
        b->pmask[i] = PMASK_UNASKED;
        b->cross[i] = 0;
    }
    b->dirty_n = 0;
    b->dirty_memo_n = 0;
    return b;
}

void TAK_PathDebugGetCounters(TAK_PathDebugCounters *out) {
    if (!out) return;
    out->plans = g_dbg_plans;
    out->work = g_dbg_work;
    out->rebuilds = g_dbg_rebuilds;
    out->rebuild_clock = g_dbg_rebuild_clock;
    out->plan_bytes = (uint32_t)((size_t)g_pbuf.cells *
                                 (7 * sizeof(int) + 4) +
                                 (size_t)g_pbuf.heap_cap * sizeof(int));
    uint32_t bytes = 0;
    for (int i = 0; i < g_pcache_n; i++) {
        bytes += (uint32_t)(g_pcache[i].cw * g_pcache[i].ch);
        if (g_pcache[i].plain) {
            bytes += (uint32_t)(g_pcache[i].tw * g_pcache[i].th);
        }
        if (g_pcache[i].clear) {
            bytes += (uint32_t)(g_pcache[i].tw * g_pcache[i].th);
        }
        if (g_pcache[i].cellh) {
            bytes += (uint32_t)(g_pcache[i].cw * g_pcache[i].ch * 2);
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
static void field_scratch_free(void);

void TAK_PathCacheReset(void) {
    for (int i = 0; i < g_pcache_n; i++) {
        tak_free(g_pcache[i].bits);
        if (g_pcache[i].plain) tak_free(g_pcache[i].plain);
        if (g_pcache[i].clear) tak_free(g_pcache[i].clear);
        if (g_pcache[i].cellh) tak_free(g_pcache[i].cellh);
        for (int m = 0; m < PATH_FIELD_MARKS; m++) {
            if (g_pcache[i].field[m]) tak_free(g_pcache[i].field[m]);
        }
        if (g_pcache[i].seed_dist) tak_free(g_pcache[i].seed_dist);
        if (g_pcache[i].comp) tak_free(g_pcache[i].comp);
    }
    memset(g_pcache, 0, sizeof(g_pcache));
    g_pcache_n = 0;
    flow_reset_all();
    plan_buffers_free();
    field_scratch_free();
}

/* Ground the class's water depth refuses, taken out of a span of
 * the per tile map. */
static void plain_water(const struct GameWorld *world, const MoveClassDef *mc,
                        uint8_t *plain, int tw, int x0, int y0, int x1, int y1) {
    for (int ty = y0; ty <= y1; ty++) {
        for (int tx = x0; tx <= x1; tx++) {
            if (plain[ty * tw + tx] &&
                !water_ok(world, mc, tile_to_world(tx), tile_to_world(ty))) {
                plain[ty * tw + tx] = 0;
            }
        }
    }
}

/* Which placements of a cell stand wholly on ground the class can
 * cross, one bit each, read off the per tile map. */
static uint8_t cell_bits(const uint8_t *plain, int tw, int th,
                         int x, int y, int fx, int fz) {
    int ptx[FP_PLACEMENTS], pty[FP_PLACEMENTS];
    cell_fp_placements(x, y, fx, fz, ptx, pty);
    int mask = 0;
    for (int p = 0; p < FP_PLACEMENTS; p++) {
        int tx0 = ptx[p], ty0 = pty[p];
        if (tx0 < 0 || ty0 < 0 || tx0 + fx > tw || ty0 + fz > th) continue;
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
    return (uint8_t)mask;
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
        plain_water(world, mc, plain, tw, 0, 0, tw - 1, th - 1);
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                bits[y * cw + x] = cell_bits(plain, tw, th, x, y, fx, fz);
    } else {
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                bits[y * cw + x] = (uint8_t)cell_fp_mask_slow(
                    world, x, y, cw, ch, mc, fallback_slope, fx, fz);
    }
    /* Height at every cell centre, sampled once. The search read it
     * nine times per expanded cell and the distance field would read
     * it again. */
    int16_t *cellh = (int16_t *)tak_malloc((size_t)cw * ch * sizeof(int16_t));
    if (cellh) {
        for (int y = 0; y < ch; y++) {
            for (int x = 0; x < cw; x++) {
                cellh[y * cw + x] = (int16_t)Terrain_SampleHeight(
                    world, cell_to_world(x), cell_to_world(y));
            }
        }
    }
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
    g_pcache[i].cellh = cellh;
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

/* Largest free square anchored at each tile of a span, from its far
 * corner. A tile reads only tiles up to 255 right of and below it. */
static void clear_fill(int ci, int x0, int y0, int x1, int y1) {
    const struct GameWorld *world = g_pcache[ci].world;
    const uint8_t *plain = g_pcache[ci].plain;
    uint8_t *c = g_pcache[ci].clear;
    int tw = g_pcache[ci].tw, th = g_pcache[ci].th;
    for (int ty = y1; ty >= y0; ty--) {
        for (int tx = x1; tx >= x0; tx--) {
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
    int x0 = 0, y0 = 0, x1 = tw - 1, y1 = th - 1;
    if (!g_pcache[ci].clear) {
        g_pcache[ci].clear = (uint8_t *)tak_malloc((size_t)tw * th);
        if (!g_pcache[ci].clear) return NULL;
    } else if (!g_pcache[ci].clear_full &&
               Occ_ChangedSince(world, g_pcache[ci].clear_version,
                                &x0, &y0, &x1, &y1)) {
        /* Only structures moved since: redo what can see them. */
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= tw) x1 = tw - 1;
        if (y1 >= th) y1 = th - 1;
        x0 = x0 > 255 ? x0 - 255 : 0;
        y0 = y0 > 255 ? y0 - 255 : 0;
    } else {
        x0 = y0 = 0;
        x1 = tw - 1;
        y1 = th - 1;
    }
    uint64_t build_t0 = dbg_now();
    /* Terrain comes from the same per tile map the bitmap is built
     * from, so the two structures cannot disagree about the ground a
     * class may plan on. It used to be one sample per 32 px cell,
     * inherited by both tiles of the pair, while the bitmap swept
     * footprint corners, and a plan applied both. */
    if (x0 <= x1 && y0 <= y1) clear_fill(ci, x0, y0, x1, y1);
    g_pcache[ci].clear_full = 0;
    g_pcache[ci].clear_version = world->occ_version;
    g_dbg_rebuilds++;
    g_dbg_rebuild_clock += dbg_now() - build_t0;
    return g_pcache[ci].clear;
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

/* ── Field sweeps ──────────────────────────────────────────────────
 * A step costs 10 to PATH_FIELD_MAX_EDGE, so a ring of buckets stands
 * in for a heap. Distances and the far cell do not depend on order. */
#define FQ_RING 1024   /* a power of two above PATH_FIELD_MAX_EDGE */

typedef struct FieldScratch {
    int      cells;
    int     *qnode, *qnext;   /* bucket entries */
    int      qcap;
    FieldHeap heap;           /* for patches, whose sources are spread */
    int     *list;            /* cells a patch measures again */
    int     *work;
    int      work_cap;
    uint8_t *flag;
} FieldScratch;

static FieldScratch g_fs;

static void field_scratch_free(void) {
    void *held[] = { g_fs.qnode, g_fs.qnext, g_fs.heap.key, g_fs.heap.node,
                     g_fs.list, g_fs.work, g_fs.flag };
    for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++)
        if (held[i]) tak_free(held[i]);
    memset(&g_fs, 0, sizeof(g_fs));
}

static int field_scratch_get(int cells) {
    if (g_fs.cells >= cells && g_fs.qnode) return 1;
    field_scratch_free();
    /* One entry per step that ever improves a cell, plus the sources. */
    int cap = cells * 8 + 16;
    g_fs.qnode = (int *)tak_malloc((size_t)cap * sizeof(int));
    g_fs.qnext = (int *)tak_malloc((size_t)cap * sizeof(int));
    g_fs.heap.key = (int *)tak_malloc((size_t)cap * sizeof(int));
    g_fs.heap.node = (int *)tak_malloc((size_t)cap * sizeof(int));
    g_fs.list = (int *)tak_malloc((size_t)cells * sizeof(int));
    g_fs.work = (int *)tak_malloc((size_t)cap * sizeof(int));
    g_fs.flag = (uint8_t *)tak_malloc((size_t)cells);
    if (!g_fs.qnode || !g_fs.qnext || !g_fs.heap.key || !g_fs.heap.node ||
        !g_fs.list || !g_fs.work || !g_fs.flag) {
        field_scratch_free();
        return 0;
    }
    memset(g_fs.flag, 0, (size_t)cells);
    g_fs.cells = cells;
    g_fs.qcap = cap;
    g_fs.heap.cap = cap;
    g_fs.work_cap = cap;
    return 1;
}

static const int g_fdirs[8][3] = {
    { 1, 0,10 }, {-1, 0,10 }, { 0, 1,10 }, { 0,-1,10 },
    { 1, 1,14 }, {-1, 1,14 }, { 1,-1,14 }, {-1,-1,14 }
};

/* The cost of the step from (x, y) in direction di, or -1 off the map,
 * onto closed ground or past a blocked corner. Same both ways. */
static int field_step(const uint8_t *bits, const int16_t *cellh,
                      int cw, int ch, int x, int y, int di, int *out_n) {
    int nx = x + g_fdirs[di][0], ny = y + g_fdirs[di][1];
    if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) return -1;
    int ni = ny * cw + nx;
    if (!bits[ni]) return -1;
    if (g_fdirs[di][0] && g_fdirs[di][1]) {
        if (!bits[y * cw + nx] || !bits[ny * cw + x]) return -1;
    }
    int dh = cellh[ni] - cellh[y * cw + x];
    if (dh < 0) dh = -dh;
    int w = g_fdirs[di][2] + dh * 2;
    if (w > PATH_FIELD_MAX_EDGE) w = PATH_FIELD_MAX_EDGE;
    *out_n = ni;
    return w;
}

/* The reached cell furthest from the source, lowest index first. */
static int field_far(const int32_t *dist, int cells, int src) {
    int far = src;
    int32_t far_d = 0;
    for (int i = 0; i < cells; i++)
        if (dist[i] > far_d) { far_d = dist[i]; far = i; }
    return far;
}

/* Ground distance from one cell to every other over the terrain
 * bitmap. Fills dist with -1 where the cell cannot be reached, and
 * returns the reached cell that is furthest away, lowest index first,
 * which is the next mark to measure from, or -1 out of scratch. */
static int field_sweep(const uint8_t *bits, const int16_t *cellh,
                       int cw, int ch, int src, int32_t *dist) {
    int cells = cw * ch;
    for (int i = 0; i < cells; i++) dist[i] = -1;
    int head[FQ_RING];
    for (int b = 0; b < FQ_RING; b++) head[b] = -1;
    int used = 0, live = 1;
    dist[src] = 0;
    g_fs.qnode[used] = src;
    g_fs.qnext[used] = -1;
    head[0] = used++;
    for (int32_t d = 0; live > 0; d++) {
        int b = d & (FQ_RING - 1);
        while (head[b] >= 0) {
            int e = head[b];
            head[b] = g_fs.qnext[e];
            live--;
            int cur = g_fs.qnode[e];
            if (dist[cur] != d) continue;     /* an older, dearer entry */
            int cx = cur % cw, cy = cur / cw;
            for (int di = 0; di < 8; di++) {
                int ni;
                int w = field_step(bits, cellh, cw, ch, cx, cy, di, &ni);
                if (w < 0) continue;
                int32_t nd = d + w;
                if (dist[ni] >= 0 && nd >= dist[ni]) continue;
                if (used >= g_fs.qcap) return -1;
                dist[ni] = nd;
                int nb = nd & (FQ_RING - 1);
                g_fs.qnode[used] = ni;
                g_fs.qnext[used] = head[nb];
                head[nb] = used++;
                live++;
            }
        }
    }
    return field_far(dist, cells, src);
}

/* Settle what a patch pushed, over the whole bitmap. */
static void field_settle(const uint8_t *bits, const int16_t *cellh,
                         int cw, int ch, int32_t *dist) {
    FieldHeap *h = &g_fs.heap;
    while (h->n > 0) {
        int d;
        int cur = fheap_pop(h, &d);
        if (d != dist[cur]) continue;
        int cx = cur % cw, cy = cur / cw;
        for (int di = 0; di < 8; di++) {
            int ni;
            int w = field_step(bits, cellh, cw, ch, cx, cy, di, &ni);
            if (w < 0) continue;
            int32_t nd = d + w;
            if (dist[ni] >= 0 && nd >= dist[ni]) continue;
            dist[ni] = nd;
            fheap_push(h, nd, ni);
        }
    }
}

/* The cheapest way into open cell v from a reached neighbour that is
 * not skipped, or -1. */
static int32_t field_best_in(const uint8_t *bits, const int16_t *cellh,
                             int cw, int ch, const int32_t *dist, int v,
                             const uint8_t *skip) {
    int vx = v % cw, vy = v / cw;
    int32_t best = -1;
    for (int di = 0; di < 8; di++) {
        int u;
        int w = field_step(bits, cellh, cw, ch, vx, vy, di, &u);
        if (w < 0 || dist[u] < 0 || (skip && skip[u])) continue;
        int32_t d = dist[u] + w;
        if (best < 0 || d < best) best = d;
    }
    return best;
}

/* A sweep brought up to date after cells closed: a cell no neighbour
 * still gives its distance is measured again. 0 when out of scratch. */
static int field_patch_closed(const uint8_t *bits, const int16_t *cellh,
                              int cw, int ch, int src, int32_t *dist,
                              const int *closed, int nclosed) {
    uint8_t *orphan = g_fs.flag;
    int *list = g_fs.list, nlist = 0;
    int *work = g_fs.work, nwork = 0;
    for (int k = 0; k < nclosed; k++) {
        int c = closed[k];
        dist[c] = -1;
        int x = c % cw, y = c / cw;
        for (int di = 0; di < 8; di++) {
            int nx = x + g_fdirs[di][0], ny = y + g_fdirs[di][1];
            if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
            if (nwork >= g_fs.work_cap) return 0;
            work[nwork++] = ny * cw + nx;
        }
    }
    int ok = 1;
    while (nwork > 0 && ok) {
        int v = work[--nwork];
        if (orphan[v] || dist[v] <= 0 || v == src) continue;
        int vx = v % cw, vy = v / cw, held = 0;
        for (int di = 0; di < 8 && !held; di++) {
            int u;
            int w = field_step(bits, cellh, cw, ch, vx, vy, di, &u);
            if (w < 0 || dist[u] < 0 || orphan[u]) continue;
            if (dist[u] + w == dist[v]) held = 1;
        }
        if (held) continue;
        orphan[v] = 1;
        list[nlist++] = v;
        /* Whoever this cell gave its distance to has to be asked again. */
        for (int di = 0; di < 8; di++) {
            int nx = vx + g_fdirs[di][0], ny = vy + g_fdirs[di][1];
            if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
            int n = ny * cw + nx;
            if (dist[n] <= dist[v]) continue;
            if (nwork >= g_fs.work_cap) { ok = 0; break; }
            work[nwork++] = n;
        }
    }
    if (!ok) {
        for (int k = 0; k < nlist; k++) orphan[list[k]] = 0;
        return 0;
    }
    for (int k = 0; k < nlist; k++) dist[list[k]] = -1;
    g_fs.heap.n = 0;
    for (int k = 0; k < nlist; k++) {
        int v = list[k];
        int32_t best = field_best_in(bits, cellh, cw, ch, dist, v, orphan);
        if (best < 0) continue;
        dist[v] = best;
        fheap_push(&g_fs.heap, best, v);
    }
    for (int k = 0; k < nlist; k++) orphan[list[k]] = 0;
    field_settle(bits, cellh, cw, ch, dist);
    return 1;
}

/* The same after cells opened: what a new step improves spreads. */
static void field_patch_opened(const uint8_t *bits, const int16_t *cellh,
                               int cw, int ch, int32_t *dist,
                               const int *opened, int nopened) {
    g_fs.heap.n = 0;
    for (int k = 0; k < nopened; k++) {
        int c = opened[k];
        int x = c % cw, y = c / cw;
        for (int di = -1; di < 8; di++) {
            int nx = di < 0 ? x : x + g_fdirs[di][0];
            int ny = di < 0 ? y : y + g_fdirs[di][1];
            if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
            int v = ny * cw + nx;
            if (!bits[v]) continue;
            int32_t best = field_best_in(bits, cellh, cw, ch, dist, v, NULL);
            if (best < 0 || (dist[v] >= 0 && best >= dist[v])) continue;
            dist[v] = best;
            fheap_push(&g_fs.heap, best, v);
        }
    }
    field_settle(bits, cellh, cw, ch, dist);
}

/* Seed at the open cell nearest the middle. */
static int field_seed(const uint8_t *bits, int cw, int ch) {
    int cells = cw * ch;
    int seed = -1, seed_d = INT_MAX;
    for (int i = 0; i < cells; i++) {
        if (!bits[i]) continue;
        int d = iabs32(i % cw - cw / 2) + iabs32(i / cw - ch / 2);
        if (d < seed_d) { seed_d = d; seed = i; }
    }
    return seed;
}

/* The open cell furthest from every mark so far, or -1. */
static int field_next_mark(const uint8_t *bits, int cells,
                           int32_t *const *field, int kept) {
    int best = -1, best_d = -1;
    for (int i = 0; i < cells; i++) {
        if (!bits[i]) continue;
        int32_t least = -1;
        for (int k = 0; k < kept; k++) {
            int32_t v = field[k][i];
            if (v < 0) { least = -1; break; }
            if (least < 0 || v < least) least = v;
        }
        if (least > best_d) { best_d = least; best = i; }
    }
    return best;
}

/* Forget the field, so the next long plan builds it afresh. */
static void field_drop(int ci) {
    for (int m = 0; m < PATH_FIELD_MARKS; m++) {
        if (g_pcache[ci].field[m]) tak_free(g_pcache[ci].field[m]);
        g_pcache[ci].field[m] = NULL;
    }
    if (g_pcache[ci].seed_dist) tak_free(g_pcache[ci].seed_dist);
    g_pcache[ci].seed_dist = NULL;
    g_pcache[ci].field_n = 0;
    g_pcache[ci].field_tried = 0;
}

/* Build the marks and their distances, once per map and move class.
 * The first mark is the cell furthest from the middle of the map, the
 * second the cell furthest from the first, and the third the cell
 * furthest from both, which is the usual way to spread them so that
 * one of them lies beyond whatever the route has to go round. The
 * sweep from the middle is kept for patching. */
static int field_build(int ci) {
    const uint8_t *bits = g_pcache[ci].bits;
    const int16_t *cellh = g_pcache[ci].cellh;
    int cw = g_pcache[ci].cw, ch = g_pcache[ci].ch;
    int cells = cw * ch;
    g_pcache[ci].field_tried = 1;
    if (!bits || !cellh || cells <= 0) return 0;

    int seed = field_seed(bits, cw, ch);
    if (seed < 0) return 0;

    uint64_t build_t0 = dbg_now();
    int32_t *seed_dist = (int32_t *)tak_malloc((size_t)cells * sizeof(int32_t));
    if (!seed_dist || !field_scratch_get(cells)) {
        if (seed_dist) tak_free(seed_dist);
        return 0;
    }
    g_pcache[ci].seed = seed;
    g_pcache[ci].seed_dist = seed_dist;
    int mark = field_sweep(bits, cellh, cw, ch, seed, seed_dist);
    int kept = 0;
    for (int m = 0; m < PATH_FIELD_MARKS && mark >= 0; m++) {
        int32_t *f = (int32_t *)tak_malloc((size_t)cells * sizeof(int32_t));
        if (!f) break;
        int next = field_sweep(bits, cellh, cw, ch, mark, f);
        g_pcache[ci].mark[kept] = mark;
        g_pcache[ci].field[kept++] = f;
        if (next < 0) { mark = -1; break; }
        if (m + 1 >= PATH_FIELD_MARKS) break;
        int best = field_next_mark(bits, cells, g_pcache[ci].field, kept);
        mark = best >= 0 ? best : next;
    }
    g_pcache[ci].field_n = kept;
    if (mark < 0) {
        /* Out of scratch: no field, as when the memory was short. */
        field_drop(ci);
        g_pcache[ci].field_tried = 1;
    }
    g_dbg_rebuilds++;
    g_dbg_rebuild_clock += dbg_now() - build_t0;
    return g_pcache[ci].field_n;
}

/* One sweep patched, or swept again when its source moved. Returns
 * the far cell, or -1. */
static int field_refresh(int ci, int32_t *dist, int old_src, int src,
                         const int *closed, int nclosed,
                         const int *opened, int nopened) {
    const uint8_t *bits = g_pcache[ci].bits;
    const int16_t *cellh = g_pcache[ci].cellh;
    int cw = g_pcache[ci].cw, ch = g_pcache[ci].ch;
    if (src != old_src ||
        (nclosed > 0 && !field_patch_closed(bits, cellh, cw, ch, src, dist,
                                            closed, nclosed)))
        return field_sweep(bits, cellh, cw, ch, src, dist);
    if (nopened > 0)
        field_patch_opened(bits, cellh, cw, ch, dist, opened, nopened);
    return field_far(dist, cw * ch, src);
}

/* The bitmap changed: closed and opened list the cells that stopped or
 * started being ground. Leaves the field as a fresh build would. */
static void field_bits_changed(int ci, const int *closed, int nclosed,
                               const int *opened, int nopened) {
    if (!g_pcache[ci].field_tried) return;
    int cw = g_pcache[ci].cw, ch = g_pcache[ci].ch, cells = cw * ch;
    const uint8_t *bits = g_pcache[ci].bits;
    if (g_pcache[ci].field_n != PATH_FIELD_MARKS || !g_pcache[ci].seed_dist ||
        (nclosed > 0 && nopened > 0) || !field_scratch_get(cells)) {
        field_drop(ci);
        return;
    }
    uint64_t t0 = dbg_now();
    int seed = field_seed(bits, cw, ch);
    if (seed < 0) { field_drop(ci); return; }
    int mark = field_refresh(ci, g_pcache[ci].seed_dist, g_pcache[ci].seed,
                             seed, closed, nclosed, opened, nopened);
    g_pcache[ci].seed = seed;
    for (int m = 0; m < PATH_FIELD_MARKS && mark >= 0; m++) {
        int next = field_refresh(ci, g_pcache[ci].field[m], g_pcache[ci].mark[m],
                                 mark, closed, nclosed, opened, nopened);
        g_pcache[ci].mark[m] = mark;
        if (next < 0) { mark = -1; break; }
        if (m + 1 >= PATH_FIELD_MARKS) break;
        int best = field_next_mark(bits, cells, g_pcache[ci].field, m + 1);
        mark = best >= 0 ? best : next;
    }
    if (mark < 0) field_drop(ci);
    g_dbg_rebuild_clock += dbg_now() - t0;
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

/* ── A blocking feature came or went ──────────────────────────────
 * Corpses come and go all battle. Redo the layers over the span the
 * feature reaches and patch the field, as a fresh build would hold. */
void TAK_PathCacheFeatureChanged(const struct GameWorld *world,
                                 int tx0, int ty0, int tx1, int ty1) {
    flow_reset_all();
    if (!world) return;
    for (int ci = 0; ci < g_pcache_n; ci++) {
        if (g_pcache[ci].world != world) continue;
        uint8_t *plain = g_pcache[ci].plain;
        int tw = g_pcache[ci].tw, th = g_pcache[ci].th;
        if (!plain || !g_pcache[ci].bits || tw <= 0 || th <= 0) {
            /* Built the slow way, so nothing to patch: start over. */
            TAK_PathCacheReset();
            return;
        }
        int x0 = tx0 < 0 ? 0 : tx0, y0 = ty0 < 0 ? 0 : ty0;
        int x1 = tx1 >= tw ? tw - 1 : tx1, y1 = ty1 >= th ? th - 1 : ty1;
        if (x0 > x1 || y0 > y1) continue;
        const MoveClassDef *mc = g_pcache[ci].mc;
        int fx = g_pcache[ci].fx, fz = g_pcache[ci].fz;
        int cw = g_pcache[ci].cw, ch = g_pcache[ci].ch;
        Terrain_WalkableTilesRect(world, movement_max_slope(mc, g_pcache[ci].fallback_slope),
                                  plain, tw, th, x0, y0, x1, y1);
        plain_water(world, mc, plain, tw, x0, y0, x1, y1);
        /* Every cell with a placement over the span, and one to spare. */
        int cx0 = (x0 - fx - 2) / 2 - 1, cy0 = (y0 - fz - 2) / 2 - 1;
        int cx1 = (x1 + fx + 2) / 2 + 1, cy1 = (y1 + fz + 2) / 2 + 1;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 >= cw) cx1 = cw - 1;
        if (cy1 >= ch) cy1 = ch - 1;
        int span = (cx1 - cx0 + 1) * (cy1 - cy0 + 1);
        int *flips = span > 0 ? (int *)tak_malloc((size_t)span * 2 * sizeof(int)) : NULL;
        if (span > 0 && !flips) { TAK_PathCacheReset(); return; }
        int *closed = flips, *opened = flips ? flips + span : NULL;
        int nclosed = 0, nopened = 0;
        uint8_t *bits = g_pcache[ci].bits;
        for (int y = cy0; y <= cy1; y++) {
            for (int x = cx0; x <= cx1; x++) {
                int i = y * cw + x;
                uint8_t was = bits[i];
                bits[i] = cell_bits(plain, tw, th, x, y, fx, fz);
                if (was && !bits[i]) closed[nclosed++] = i;
                else if (!was && bits[i]) opened[nopened++] = i;
            }
        }
        if (g_pcache[ci].clear && g_pcache[ci].clear_version == world->occ_version)
            clear_fill(ci, x0 > 255 ? x0 - 255 : 0, y0 > 255 ? y0 - 255 : 0, x1, y1);
        else if (g_pcache[ci].clear)
            g_pcache[ci].clear_full = 1;
        if (nclosed > 0 || nopened > 0) {
            if (g_pcache[ci].comp) tak_free(g_pcache[ci].comp);
            g_pcache[ci].comp = NULL;
            field_bits_changed(ci, closed, nclosed, opened, nopened);
        }
        if (flips) tak_free(flips);
    }
}

/* How many cached layers of this world differ from a fresh build. */
int TAK_PathDebugCheckCache(const struct GameWorld *world) {
    int bad = 0;
    for (int ci = 0; ci < g_pcache_n; ci++) {
        if (g_pcache[ci].world != world || !g_pcache[ci].plain) continue;
        int tw = g_pcache[ci].tw, th = g_pcache[ci].th;
        int cw = g_pcache[ci].cw, ch = g_pcache[ci].ch, cells = cw * ch;
        const MoveClassDef *mc = g_pcache[ci].mc;
        uint8_t *plain = (uint8_t *)tak_malloc((size_t)tw * th);
        if (!plain) return -1;
        Terrain_WalkableTiles(world, movement_max_slope(mc, g_pcache[ci].fallback_slope),
                              plain, tw, th);
        plain_water(world, mc, plain, tw, 0, 0, tw - 1, th - 1);
        if (memcmp(plain, g_pcache[ci].plain, (size_t)tw * th) != 0) bad++;
        for (int i = 0; i < cells; i++) {
            if (cell_bits(plain, tw, th, i % cw, i / cw, g_pcache[ci].fx,
                          g_pcache[ci].fz) != g_pcache[ci].bits[i]) {
                bad++;
                break;
            }
        }
        tak_free(plain);
        uint8_t *held = g_pcache[ci].clear;
        if (held && g_pcache[ci].clear_version == world->occ_version) {
            uint8_t *fresh = (uint8_t *)tak_malloc((size_t)tw * th);
            if (!fresh) return -1;
            g_pcache[ci].clear = fresh;
            clear_fill(ci, 0, 0, tw - 1, th - 1);
            g_pcache[ci].clear = held;
            if (memcmp(fresh, held, (size_t)tw * th) != 0) bad++;
            tak_free(fresh);
        }
        if (g_pcache[ci].field_tried && g_pcache[ci].field_n > 0) {
            /* Build afresh into a spare slot and compare. */
            if (g_pcache_n >= PCACHE_MAX) continue;
            int spare = g_pcache_n;
            g_pcache[spare] = g_pcache[ci];
            for (int m = 0; m < PATH_FIELD_MARKS; m++) g_pcache[spare].field[m] = NULL;
            g_pcache[spare].seed_dist = NULL;
            g_pcache[spare].field_n = 0;
            g_pcache[spare].field_tried = 0;
            field_build(spare);
            if (g_pcache[spare].field_n != g_pcache[ci].field_n ||
                g_pcache[spare].seed != g_pcache[ci].seed) {
                bad++;
            } else {
                if (memcmp(g_pcache[spare].seed_dist, g_pcache[ci].seed_dist,
                           (size_t)cells * sizeof(int32_t)) != 0) bad++;
                for (int m = 0; m < g_pcache[ci].field_n; m++) {
                    if (g_pcache[spare].mark[m] != g_pcache[ci].mark[m] ||
                        memcmp(g_pcache[spare].field[m], g_pcache[ci].field[m],
                               (size_t)cells * sizeof(int32_t)) != 0) bad++;
                }
            }
            field_drop(spare);
            memset(&g_pcache[spare], 0, sizeof(g_pcache[spare]));
        }
    }
    return bad;
}

/* Build a class's layers at load rather than in its first plan. A
 * footprint of 0 is the class's own. Never pushes a layer out. */
void TAK_PathCacheWarm(const struct GameWorld *world,
                       const struct MoveClassDef *move_class,
                       int fallback_max_slope, int fx, int fz) {
    if (!world || world->map_pixels_w <= 0 || world->map_pixels_h <= 0) return;
    int cw = (world->map_pixels_w + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int ch = (world->map_pixels_h + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int slope = movement_max_slope(move_class, fallback_max_slope);
    if (fx <= 0 || fz <= 0) class_footprint(move_class, &fx, &fz);
    int held = 0;
    for (int i = 0; i < g_pcache_n && !held; i++)
        held = g_pcache[i].world == world && g_pcache[i].mc == move_class &&
               g_pcache[i].fallback_slope == slope && g_pcache[i].fx == fx &&
               g_pcache[i].fz == fz && g_pcache[i].cw == cw && g_pcache[i].ch == ch;
    if (!held && g_pcache_n >= PCACHE_MAX) return;
    int ci = pcache_find(world, cw, ch, move_class, slope, fx, fz);
    if (ci < 0) return;
    if (g_use_field) field_get(ci);
    clearance_get(ci);
    components_get(ci);
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
    /* One byte per cell, 0 not asked yet, 1 no, 2 yes. Crossability
     * costs a terrain sample and four occupancy reads, and a pinched
     * search asks about the same cell from several neighbours. */
    uint8_t *cross_memo;
    /* One byte per cell: PMASK_UNASKED, or which placements are legal
     * there with the live layer on, after mask_connected(). */
    uint8_t *pmask;
    /* Cells whose pmask this plan filled, for the next plan to reset. */
    int *memo_dirty;
    int *memo_dirty_n;
    /* Height at each cell centre, and the long route field: the marks
     * this plan may use and each mark's distance to the goal cell. */
    const int16_t *cellh;
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
        /* Occ_QueryTilePlan() == 1 over the placement, read in place. */
        const struct GameWorld *w = c->world;
        for (int dy = 0; dy < c->fz; dy++) {
            int ty = ty0 + dy;
            if (ty < 0 || ty >= w->occ_h) continue;
            const TAK_OccCell *row = &w->occ[(size_t)ty * w->occ_w];
            for (int dx = 0; dx < c->fx; dx++) {
                int tx = tx0 + dx;
                if (tx < 0 || tx >= w->occ_w) continue;
                const TAK_OccCell *o = &row[tx];
                if (!o->unit_plus1) continue;
                if (o->flags & TAK_OCC_MOBILE) {
                    if (!(o->flags & TAK_OCC_PARKED)) continue;
                    if (c->self_plus1 && o->unit_plus1 == (uint16_t)c->self_plus1)
                        continue;
                    return 0;
                }
                if ((o->flags & TAK_OCC_GATE) && o->owner == (uint8_t)c->player_id)
                    continue;
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
    if (*m == PMASK_UNASKED) {
        *m = (uint8_t)mask_connected(cell_legal_mask(c, x, y, live));
        c->memo_dirty[(*c->memo_dirty_n)++] = y * c->cw + x;
    }
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

/* Ground the unit can physically cross, footprint or no footprint:
 * the single point terrain and water test the mover applies per step,
 * plus nothing else standing on the cell. A unit wedged on a spit its
 * footprint does not fit still walks along it, and this is the ground
 * a pinched search is allowed to use. */
static int cell_crossable(const PlanCtx *c, int x, int y) {
    if (x < 0 || y < 0 || x >= c->cw || y >= c->ch) return 0;
    /* TERRAIN is what a crossing relaxes, and only terrain. One
     * walkable tile in the cell is enough: the unit walks a line
     * through it, not a footprint on its centre, and a cell holds two
     * tiles per axis so a 16 px band lies on one of them. */
    int tx0 = x * OCC_PER_PATH_CELL, ty0 = y * OCC_PER_PATH_CELL;
    int any = 0;
    for (int dy = 0; dy < OCC_PER_PATH_CELL && !any; dy++) {
        for (int dx = 0; dx < OCC_PER_PATH_CELL; dx++) {
            int tx = tx0 + dx, ty = ty0 + dy;
            if (c->plain) {
                if (tx < c->tw && ty < c->th && c->plain[ty * c->tw + tx]) {
                    any = 1;
                    break;
                }
            } else {
                int32_t px = tile_to_world(tx), py = tile_to_world(ty);
                if (Terrain_IsWalkable(c->world, px, py, c->slope) &&
                    water_ok(c->world, c->mc, px, py)) {
                    any = 1;
                    break;
                }
            }
        }
    }
    if (!any) return 0;
    if (!c->world->occ) return 1;
    /* OCCUPANCY is never relaxed, and it is asked of the whole
     * footprint, not of the cell. The mover's escape hatch relaxes
     * slope and features so a unit can leave illegal ground and never
     * relaxes what is built (occ_step_blocked in units.c), so a
     * crossing that puts a four tile unit's body through a one tile
     * gap between two buildings is a route it could never walk. The
     * terrain test above cannot catch that: a gap can be walkable,
     * dry and unoccupied at its own tiles and still be a gap the unit
     * does not fit in. */
    int fx = c->fx < OCC_PER_PATH_CELL ? OCC_PER_PATH_CELL : c->fx;
    int fz = c->fz < OCC_PER_PATH_CELL ? OCC_PER_PATH_CELL : c->fz;
    int bx0 = cell_fp_anchor(x, fx), by0 = cell_fp_anchor(y, fz);
    for (int dy = 0; dy < fz; dy++) {
        for (int dx = 0; dx < fx; dx++) {
            if (Occ_QueryTilePlan(c->world, bx0 + dx, by0 + dy,
                                  c->player_id, c->self_plus1) == 1) {
                return 0;
            }
        }
    }
    return 1;
}

/* Can the search step onto this cell, and what does it cost over the
 * plain move? A cell a route may start and stand on is free.
 *
 * A cell the unit can only walk across, not plan on, is offered ONLY
 * while the search is still on the trapped ground it began in:
 * from_pinch says the cell being expanded is the start or is itself
 * one of those. That is a rule, not a price. Charging a price alone
 * and allowing the crossing anywhere meant that once a legal detour
 * ran past a hundred cells the search would rather squeeze a four
 * tile unit through a one tile gap, which is the clearance guarantee
 * inverted. The price is still there, so even inside the pinch the
 * route leaves it at the first opportunity. */
static int cell_step_cost(PlanCtx *c, int x, int y, int from_pinch,
                          int *extra) {
    *extra = 0;
    if (cell_ok(c, x, y)) return 1;
    if (!c->allow_pinch || !from_pinch) return 0;
    if (x < 0 || y < 0 || x >= c->cw || y >= c->ch) return 0;
    int cross;
    if (c->cross_memo) {
        /* cell_ok above filled this cell's pmask, which listed the
         * cell, so the cross entry is put back with it. */
        uint8_t *m = &c->cross_memo[y * c->cw + x];
        if (!*m) *m = (uint8_t)(cell_crossable(c, x, y) ? 2 : 1);
        cross = (*m == 2);
    } else {
        cross = cell_crossable(c, x, y);
    }
    if (!cross) return 0;
    *extra = PATH_PINCH_COST;
    return 1;
}

static int cell_steppable(PlanCtx *c, int x, int y, int from_pinch) {
    int extra;
    return cell_step_cost(c, x, y, from_pinch, &extra);
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

/* Terrain height at a cell centre, from the cached per cell array
 * when the map was big enough to build one. */
static int plan_height(const PlanCtx *c, int cell) {
    if (c->cellh) return c->cellh[cell];
    return Terrain_SampleHeight(c->world, cell_to_world(cell % c->cw),
                                cell_to_world(cell / c->cw));
}

/* Keys are read from f as it stands, so a cell whose f falls while it
 * waits is not moved. The moving entry's key is read once. */
static void heap_push(int *heap, int *n, const int *f, int v) {
    int i = (*n)++;
    int fv = f[v];
    while (i > 0) {
        int p = (i - 1) / 2;
        int pv = heap[p], fp = f[pv];
        if (!(fv < fp || (fv == fp && v < pv))) break;
        heap[i] = pv;
        i = p;
    }
    heap[i] = v;
}

static int heap_pop(int *heap, int *n, const int *f) {
    int out = heap[0];
    int cnt = --(*n);
    int x = heap[cnt];
    int fx = f[x];
    int i = 0;
    for (;;) {
        int l = i * 2 + 1;
        if (l >= cnt) break;
        int b = i, bv = x, fb = fx;
        int lv = heap[l], fl = f[lv];
        if (fl < fb || (fl == fb && lv < bv)) { b = l; bv = lv; fb = fl; }
        int r = l + 1;
        if (r < cnt) {
            int rv = heap[r], fr = f[rv];
            if (fr < fb || (fr == fb && rv < bv)) { b = r; bv = rv; fb = fr; }
        }
        if (b == i) break;
        heap[i] = bv;
        i = b;
    }
    heap[i] = x;
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

static void route_lay(TAK_Path *out, PlanCtx *c, int start,
                      const int *chain, int chain_len,
                      int32_t unit_x, int32_t unit_y, int compress,
                      int *px, int *py, int cap) {
    int n = 0;                  /* raw points, in world pixels */
    int have = 0, cur = 0;      /* the placement the chain stands on */
    int prev = start;
    /* Where each cell's last raw point is, for the uncompressed form. */
    int last_of_cell_from = 0;

    int smask = plan_mask(c, start % c->cw, start / c->cw);
    if (smask) {
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
    }

    out->count = 0;
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

    /* Keep a point where the chain turns, and the last. */
    for (int i = 0; i < n; i++) {
        int keep = (i == n - 1);
        if (!keep) {
            int fx = i > 0 ? px[i] - px[i - 1] : 0;
            int fy = i > 0 ? py[i] - py[i - 1] : 0;
            int tx = px[i + 1] - px[i], ty = py[i + 1] - py[i];
            keep = i > 0 && (fx != tx || fy != ty);
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
        c->cellh = g_pcache[ci].cellh;
    }
    return ci;
}

/* plan_mask with the memo read in place. */
static int plan_mask_fast(const PlanCtx *c, int x, int y) {
    if ((unsigned)x >= (unsigned)c->cw || (unsigned)y >= (unsigned)c->ch) return 0;
    uint8_t m = c->pmask[y * c->cw + x];
    return m != PMASK_UNASKED ? m : plan_mask(c, x, y);
}

/* What cells_link asks of a step in each direction: placement bits of
 * the cell left (a), entered (b) and beside a diagonal (c, d). */
typedef struct PlanLink {
    int diag, offset_y, side_c, side_d;
    int a0, b0, a1, b1, c, d;
} PlanLink;

static PlanLink g_plan_link[8];

static void plan_link_init(void) {
    static const int dirs[8][2] = {
        { 1, 0 }, {-1, 0 }, { 0, 1 }, { 0,-1 },
        { 1, 1 }, {-1, 1 }, { 1,-1 }, {-1,-1 }
    };
    for (int di = 0; di < 8; di++) {
        int dx = dirs[di][0], dy = dirs[di][1];
        PlanLink *l = &g_plan_link[di];
        memset(l, 0, sizeof(*l));
        l->offset_y = dy;
        if (dx && dy) {
            l->diag = 1;
            l->side_c = dx > 0 ? 0 : 1;
            l->side_d = dy > 0 ? 2 : 3;
            l->a0 = 1 << pbit(dx > 0 ? 0 : -1, dy > 0 ? 0 : -1);
            l->b0 = 1 << pbit(dx > 0 ? -1 : 0, dy > 0 ? -1 : 0);
            l->c = 1 << pbit(dx > 0 ? -1 : 0, dy > 0 ? 0 : -1);
            l->d = 1 << pbit(dx > 0 ? 0 : -1, dy > 0 ? -1 : 0);
            continue;
        }
        for (int k = 0; k < 2; k++) {
            int lane = k == 0 ? 0 : -1;
            int a = dx ? pbit(dx > 0 ? 0 : -1, lane) : pbit(lane, dy > 0 ? 0 : -1);
            int bb = dx ? pbit(dx > 0 ? -1 : 0, lane) : pbit(lane, dy > 0 ? -1 : 0);
            if (k == 0) { l->a0 = 1 << a; l->b0 = 1 << bb; }
            else        { l->a1 = 1 << a; l->b1 = 1 << bb; }
        }
    }
}

/* One step of the search onto ni from cur, as the loop takes it. */
static void plan_relax(PlanCtx *c, PlanBuffers *b, int cur, int gcur, int ni,
                       int base, int ch0, int gx, int gy, int *heap_n) {
    int nh = plan_height(c, ni);
    int step = base + iabs32(nh - ch0) * 2;
    int ng = gcur + step;
    if (ng >= b->g[ni]) return;
    if (b->g[ni] == PATH_G_UNSET) b->dirty[b->dirty_n++] = ni;
    b->parent[ni] = cur;
    b->g[ni] = ng;
    b->f[ni] = ng + plan_h(c, ni, gx, gy);
    heap_push(b->heap, heap_n, b->f, ni);
}

int TAK_PathPlanQuery(const struct GameWorld *world,
                      int32_t start_x, int32_t start_y,
                      int32_t goal_x, int32_t goal_y,
                      const TAK_PathQuery *query,
                      TAK_Path *out_path) {
    if (!world || !out_path || !query || world->map_pixels_w <= 0 ||
        world->map_pixels_h <= 0) {
        return 0;
    }
    memset(out_path, 0, sizeof(*out_path));
    g_dbg_plans++;
    if (g_dbg_reset_every > 0 &&
        ++g_dbg_since_reset >= g_dbg_reset_every) {
        g_dbg_since_reset = 0;
        TAK_PathCacheReset();
    }
    PlanCtx c;
    int ci = plan_ctx_setup(&c, world, query);
    int cells = c.cw * c.ch;
    if (c.cw <= 0 || c.ch <= 0 || cells <= 0) return 0;
    if (!g_plan_link[0].a0) plan_link_init();

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
    c.allow_pinch = !cell_ok_live(&c, sx, sy, 0);
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

    PlanBuffers *b = plan_buffers_begin(cells);
    if (!b) return 0;
    int *g = b->g, *f = b->f, *parent = b->parent, *heap = b->heap;
    uint8_t *closed = b->closed;
    int *dirty = b->dirty;
    c.pmask = b->pmask;
    c.memo_dirty = b->dirty_memo;
    c.memo_dirty_n = &b->dirty_memo_n;
    uint8_t *pinched = NULL;
    if (c.allow_pinch) {
        c.cross_memo = b->cross;
        pinched = b->pinched;
    }

    int start = cell_index(sx, sy, c.cw);
    int goal = cell_index(gx, gy, c.cw);
    /* A long enough order gets the field. A pinched one does not: it
     * may cross cells the field never measured, and a heuristic that
     * jumps where it runs out would cost the route its guarantee. */
    if (g_use_field && ci >= 0 && c.bits && !c.allow_pinch &&
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
    dirty[b->dirty_n++] = start;
    g[start] = 0;
    f[start] = plan_h(&c, start, gx, gy);
    heap_push(heap, &heap_n, f, start);

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
    if (max_expanded > PATH_MAX_EXPANDED) max_expanded = PATH_MAX_EXPANDED;
    int best = start;
    int best_h = plan_h(&c, start, gx, gy);
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
        /* f was set to g plus this cell's estimate, together with g. */
        int cur_h = f[cur] - g[cur];
        if (cur_h < best_h) {
            best_h = cur_h;
            best = cur;
        }
        int ch0 = plan_height(&c, cur);
        if (!pinched) {
            /* The loop below without crossings, each neighbour's mask
             * asked once. */
            int A = plan_mask_fast(&c, cx, cy);
            int gcur = g[cur];
            int M[8];
            for (int di = 0; di < 8; di++)
                M[di] = plan_mask_fast(&c, cx + dirs[di][0], cy + dirs[di][1]);
            for (int di = 0; di < 8; di++) {
                int B = M[di];
                if (!B) continue;
                const PlanLink *lk = &g_plan_link[di];
                if (lk->diag) {
                    int C = M[lk->side_c], D = M[lk->side_d];
                    if (!C || !D) continue;
                    int ni = cur + lk->offset_y * c.cw + dirs[di][0];
                    if (closed[ni]) continue;
                    if (A && !((A & lk->a0) && (B & lk->b0) &&
                               (C & lk->c) && (D & lk->d))) continue;
                    plan_relax(&c, b, cur, gcur, ni, dirs[di][2], ch0, gx, gy, &heap_n);
                } else {
                    int ni = cur + lk->offset_y * c.cw + dirs[di][0];
                    if (closed[ni]) continue;
                    if (A && !(((A & lk->a0) && (B & lk->b0)) ||
                               ((A & lk->a1) && (B & lk->b1)))) continue;
                    plan_relax(&c, b, cur, gcur, ni, dirs[di][2], ch0, gx, gy, &heap_n);
                }
            }
            continue;
        }
        /* Still on the ground the unit is trapped on? Only then may
         * the next step be a crossing rather than a route cell. */
        int from_pinch = pinched && (cur == start || pinched[cur]);
        for (int di = 0; di < 8; di++) {
            int nx = cx + dirs[di][0];
            int ny = cy + dirs[di][1];
            int extra = 0;
            if (!cell_step_cost(&c, nx, ny, from_pinch, &extra)) continue;
            /* No corner cutting past a blocked cell. */
            if (dirs[di][0] != 0 && dirs[di][1] != 0) {
                if (!cell_steppable(&c, cx + dirs[di][0], cy, from_pinch))
                    continue;
                if (!cell_steppable(&c, cx, cy + dirs[di][1], from_pinch))
                    continue;
            }
            int ni = cell_index(nx, ny, c.cw);
            if (closed[ni]) continue;
            /* Between two cells a route may stand in, the step has to
             * be a slide between placements. A crossing, in or out, is
             * the pinch's business and keeps its own rule. */
            if (!extra && plan_mask(&c, cx, cy) &&
                !cells_link(&c, cx, cy, dirs[di][0], dirs[di][1], -1,
                            NULL, NULL)) continue;
            int nh = plan_height(&c, ni);
            int step = dirs[di][2] + iabs32(nh - ch0) * 2 + extra;
            int ng = g[cur] + step;
            if (ng < g[ni]) {
                /* A cell's first route makes it this plan's to put back. */
                if (g[ni] == PATH_G_UNSET) dirty[b->dirty_n++] = ni;
                parent[ni] = cur;
                g[ni] = ng;
                f[ni] = ng + plan_h(&c, ni, gx, gy);
                if (pinched) pinched[ni] = (uint8_t)(extra != 0);
                heap_push(heap, &heap_n, f, ni);
            }
        }
    }

    g_dbg_work += (uint64_t)expanded;
    if (found || (capped && best != start)) {
        int end = found ? goal : best;
        int chain_len = 0;
        for (int p = end; p >= 0 && p != start; p = parent[p]) {
            chain_len++;
            if (chain_len > cells) { chain_len = 0; break; }
        }
        /* The heap is free now: hold the chain in it, start first. */
        for (int p = end, i = chain_len - 1; i >= 0 && p >= 0;
             p = parent[p], i--) {
            heap[i] = p;
        }
        if (chain_len == 0) {
            /* Already in the goal cell: one point, so a caller can
             * tell "here" from "no route". */
            path_put(out_path, &c, end);
        } else {
            route_lay(out_path, &c, start, heap, chain_len, start_x,
                      start_y, query->compress, b->px, b->py, cells);
        }
    }
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
        int hc = plan_height(c, cur);
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
            int step = dirs[di][2] + iabs32(plan_height(c, ni) - hc) * 2;
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
    PlanBuffers *b = plan_buffers_begin(cells);
    if (!b) return -1;
    c.pmask = b->pmask;
    c.memo_dirty = b->dirty_memo;
    c.memo_dirty_n = &b->dirty_memo_n;
    /* A field route runs no search, so the heap is free for its chain. */
    int *px = b->px, *py = b->py, *chain = b->heap;
    int n = -1;

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
    route_lay(out_path, &c, start, chain, len, start_x, start_y,
              query->compress, px, py, cells);
    n = out_path->count > 0 ? out_path->count : -1;
done:
    return n;
}

uint32_t TAK_PathDebugFlowBuilds(void) { return g_dbg_flow_builds; }
uint64_t TAK_PathDebugFlowClock(void) { return g_dbg_flow_clock; }
uint64_t TAK_PathDebugFlowSettled(void) { return g_dbg_flow_settled; }
void TAK_PathDebugFlowWhole(int on) { g_dbg_flow_whole = on ? 1 : 0; }
