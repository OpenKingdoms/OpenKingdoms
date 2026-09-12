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

/* The first tile of the footprint a unit covers with its centre in
 * this cell: fp tiles centred on the cell centre. An even footprint
 * sits on the cell's own two tiles, an odd one straddles them
 * symmetrically, and a one tile class is the tile its centre is in,
 * which is the tile the cell test has always used.
 *
 * Anchoring at Occ_TileOf(centre - fp * 8) instead, the way the
 * occupancy stamp does, is half a tile off for an odd footprint: it
 * would move a one tile class onto the other tile of its cell and
 * change which ground it can plan on for no reason. */
static int cell_fp_anchor(int cell, int fp) {
    return Occ_TileOf(cell_to_world(cell) - (fp - 1) * 8);
}

static void class_footprint(const MoveClassDef *mc, int *fx, int *fz) {
    *fx = (mc && mc->footprint_x > 0) ? mc->footprint_x : 1;
    *fz = (mc && mc->footprint_z > 0) ? mc->footprint_z : 1;
}

/* Per-cell terrain test: the ground under the footprint the mover
 * would stamp with its centre in this cell. The footprint is anchored
 * at Occ_TileOf(centre - footprint * 8), the same tiles occ_step_blocked
 * walks, and every one of them is tested, which is the sweep the
 * original runs per cell (legacy:219089-219131).
 *
 * This used to sample four corners at plus and minus half the
 * footprint. Those reach one tile past the footprint on the positive
 * side, so a 2 by 2 class was asked for three tiles of ground and
 * whether it got them turned on the strip's parity against the cell
 * grid rather than on its width. */
static int cell_walkable_slow(const struct GameWorld *world,
                              int x, int y, int cw, int ch,
                              const MoveClassDef *move_class,
                              int fallback_max_slope) {
    if (x < 0 || y < 0 || x >= cw || y >= ch) return 0;
    int slope = movement_max_slope(move_class, fallback_max_slope);
    int fp_x, fp_z;
    class_footprint(move_class, &fp_x, &fp_z);
    int tx0 = cell_fp_anchor(x, fp_x);
    int ty0 = cell_fp_anchor(y, fp_z);
    for (int row = 0; row < fp_z; row++) {
        for (int col = 0; col < fp_x; col++) {
            int32_t sx = tile_to_world(tx0 + col);
            int32_t sy = tile_to_world(ty0 + row);
            if (!Terrain_IsWalkable(world, sx, sy, slope)) return 0;
            if (!water_ok(world, move_class, sx, sy)) return 0;
        }
    }
    return 1;
}

/* ── Per-move-class caches ────────────────────────────────────────
 * Terrain is static; the slow path re-samples bilinear heights per
 * neighbor per A* call (~2.4ms/call, the 1000-unit killer). One
 * passability bitmap per (move class, slope) per map, plus the
 * clearance map on the 16-px tile grid: for every tile, the side of
 * the largest square of tiles with that tile at its top left that the
 * class can cross and no structure stands on. Structures come and go,
 * so the clearance map carries the occupancy version it was built at
 * and is rebuilt when that moves. */
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

static uint64_t dbg_now(void) { return g_dbg_clock ? g_dbg_clock() : 0; }

#define PCACHE_MAX 16
static struct {
    const struct GameWorld *world;
    const MoveClassDef     *mc;
    int       fallback_slope;
    int       cw, ch;
    uint8_t  *bits;
    uint8_t  *plain;      /* per 16 px tile: ground this class can cross */
    uint8_t  *clear;
    uint32_t  clear_version;
    int       tw, th;
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
    }
    out->cache_bytes = bytes;
}

void TAK_PathCacheReset(void) {
    for (int i = 0; i < g_pcache_n; i++) {
        tak_free(g_pcache[i].bits);
        if (g_pcache[i].plain) tak_free(g_pcache[i].plain);
        if (g_pcache[i].clear) tak_free(g_pcache[i].clear);
    }
    g_pcache_n = 0;
}

static int pcache_find(const struct GameWorld *world, int cw, int ch,
                       const MoveClassDef *mc, int fallback_slope) {
    for (int i = 0; i < g_pcache_n; i++) {
        if (g_pcache[i].world == world && g_pcache[i].mc == mc &&
            g_pcache[i].fallback_slope == fallback_slope &&
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
        for (int ty = 0; ty < th; ty++) {
            for (int tx = 0; tx < tw; tx++) {
                int32_t sx = tile_to_world(tx), sy = tile_to_world(ty);
                plain[ty * tw + tx] = (uint8_t)(
                    Terrain_IsWalkable(world, sx, sy, slope) &&
                    water_ok(world, mc, sx, sy));
            }
        }
        int fx, fz;
        class_footprint(mc, &fx, &fz);
        for (int y = 0; y < ch; y++) {
            for (int x = 0; x < cw; x++) {
                int tx0 = cell_fp_anchor(x, fx), ty0 = cell_fp_anchor(y, fz);
                int open = 1;
                for (int row = 0; row < fz && open; row++) {
                    for (int col = 0; col < fx; col++) {
                        int tx = tx0 + col, ty = ty0 + row;
                        if (tx < 0 || ty < 0 || tx >= tw || ty >= th ||
                            !plain[ty * tw + tx]) { open = 0; break; }
                    }
                }
                bits[y * cw + x] = (uint8_t)open;
            }
        }
    } else {
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                bits[y * cw + x] = (uint8_t)cell_walkable_slow(
                    world, x, y, cw, ch, mc, fallback_slope);
    }
    g_dbg_rebuilds++;
    g_dbg_rebuild_clock += dbg_now() - build_t0;
    int i = g_pcache_n++;
    g_pcache[i].world = world;
    g_pcache[i].mc = mc;
    g_pcache[i].fallback_slope = fallback_slope;
    g_pcache[i].cw = cw;
    g_pcache[i].ch = ch;
    g_pcache[i].bits = bits;
    g_pcache[i].plain = plain;
    g_pcache[i].clear = NULL;
    g_pcache[i].clear_version = 0;
    g_pcache[i].tw = plain ? tw : 0;
    g_pcache[i].th = plain ? th : 0;
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

int TAK_PathClearanceAt(const struct GameWorld *world,
                        const struct MoveClassDef *move_class,
                        int fallback_max_slope,
                        int tile_x, int tile_y) {
    if (!world || world->map_pixels_w <= 0 || world->map_pixels_h <= 0)
        return 0;
    int cw = (world->map_pixels_w + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int ch = (world->map_pixels_h + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int slope = movement_max_slope(move_class, fallback_max_slope);
    int ci = pcache_find(world, cw, ch, move_class, slope);
    if (ci < 0) return 0;
    const uint8_t *c = clearance_get(ci);
    if (!c) return 0;
    if (tile_x < 0 || tile_y < 0 || tile_x >= g_pcache[ci].tw ||
        tile_y >= g_pcache[ci].th) return 0;
    return c[tile_y * g_pcache[ci].tw + tile_x];
}

/* Debug view of the two cached structures a plan judges ground with:
 * the per cell passability bitmap and the clearance map, each asked
 * whether this path cell is open to this class. They are built from
 * one predicate and must answer alike; a test asserts it. Terrain and
 * structures only, with no live occupancy. */
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
    int ci = pcache_find(world, cw, ch, move_class, slope);
    if (ci < 0) return 0;
    const uint8_t *clear = clearance_get(ci);
    int fx, fz;
    class_footprint(move_class, &fx, &fz);
    int need = fx > fz ? fx : fz;
    int bits_open = g_pcache[ci].bits
                  ? (g_pcache[ci].bits[cell_y * cw + cell_x] != 0) : 0;
    int tx0 = cell_fp_anchor(cell_x, fx), ty0 = cell_fp_anchor(cell_y, fz);
    int clear_open = 0;
    if (clear && tx0 >= 0 && ty0 >= 0 &&
        tx0 < g_pcache[ci].tw && ty0 < g_pcache[ci].th) {
        clear_open = clear[ty0 * g_pcache[ci].tw + tx0] >= need;
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
    const uint8_t *bits;
    const uint8_t *plain;    /* per tile ground, for the crossing test */
    const uint8_t *clear;
    int tw, th;
    /* Set when the unit's own cell is not a cell a route may start
     * from. The search may then also cross ground the unit can walk
     * but not plan on, at a heavy cost, so the route it hands back
     * begins under the unit's feet instead of across a bay. */
    int allow_pinch;
    /* One byte per cell, 0 not asked yet, 1 no, 2 yes. Crossability
     * costs a terrain sample and four occupancy reads, and a pinched
     * search asks about the same cell from several neighbours. */
    uint8_t *cross_memo;
} PlanCtx;

/* What a cell of that kind costs. Ten is one cell of open ground, so
 * this is a hundred cells of detour: a route uses a pinch only when
 * there is really nothing else. */
#define PATH_PINCH_COST 1000

static int cell_ok_live(const PlanCtx *c, int x, int y, int live) {
    if (x < 0 || y < 0 || x >= c->cw || y >= c->ch) return 0;
    if (c->bits) {
        if (!c->bits[y * c->cw + x]) return 0;
    } else if (!cell_walkable_slow(c->world, x, y, c->cw, c->ch, c->mc,
                                   c->slope)) {
        return 0;
    }
    int32_t wx = cell_to_world(x);
    int32_t wy = cell_to_world(y);
    /* The same tiles the bitmap swept, so the two answers are about
     * one piece of ground. */
    int tx0 = cell_fp_anchor(x, c->fx);
    int ty0 = cell_fp_anchor(y, c->fz);
    if (c->clear) {
        if (tx0 < 0 || ty0 < 0 || tx0 >= c->tw || ty0 >= c->th) return 0;
        if (c->clear[ty0 * c->tw + tx0] < c->need) return 0;
    }
    if (live && c->world->occ) {
        /* Live part: the owner rule for gates and parked units. The
         * block is never smaller than the cell's own tiles. */
        int fx = c->fx < OCC_PER_PATH_CELL ? OCC_PER_PATH_CELL : c->fx;
        int fz = c->fz < OCC_PER_PATH_CELL ? OCC_PER_PATH_CELL : c->fz;
        int bx0 = Occ_TileOf(wx - fx * 8);
        int by0 = Occ_TileOf(wy - fz * 8);
        for (int dy = 0; dy < fz; dy++) {
            for (int dx = 0; dx < fx; dx++) {
                if (Occ_QueryTilePlan(c->world, bx0 + dx, by0 + dy,
                                      c->player_id, c->self_plus1) == 1) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int cell_ok(const PlanCtx *c, int x, int y) {
    return cell_ok_live(c, x, y, 1);
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
static int path_put(TAK_Path *out, int cell, int cw) {
    if (out->count >= TAK_PATH_MAX_WAYPOINTS) return 0;
    out->x[out->count] = cell_to_world(cell % cw);
    out->y[out->count] = cell_to_world(cell / cw);
    out->count++;
    return 1;
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
    memset(&c, 0, sizeof(c));
    c.world = world;
    c.mc = query->move_class;
    c.slope = movement_max_slope(c.mc, query->fallback_max_slope);
    c.player_id = query->player_id;
    c.self_plus1 = query->self_plus1;
    c.fx = query->footprint_x > 0 ? query->footprint_x
         : (c.mc && c.mc->footprint_x > 0) ? c.mc->footprint_x : 1;
    c.fz = query->footprint_z > 0 ? query->footprint_z
         : (c.mc && c.mc->footprint_z > 0) ? c.mc->footprint_z : 1;
    c.need = c.fx > c.fz ? c.fx : c.fz;
    c.cw = (world->map_pixels_w + PATH_CELL_PX - 1) / PATH_CELL_PX;
    c.ch = (world->map_pixels_h + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int cells = c.cw * c.ch;
    if (c.cw <= 0 || c.ch <= 0 || cells <= 0) return 0;
    /* Key on the RESOLVED slope: a class with its own maxslope ignores
     * the per-def fallback entirely, so keying on the fallback gave one
     * cache entry per unit type and thrashed the 16-slot table. */
    int ci = pcache_find(world, c.cw, c.ch, c.mc, c.slope);
    if (ci >= 0) {
        c.bits = g_pcache[ci].bits;
        c.plain = g_pcache[ci].plain;
        c.clear = clearance_get(ci);
        c.tw = g_pcache[ci].tw;
        c.th = g_pcache[ci].th;
    }

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

    int *g = (int *)tak_malloc((size_t)cells * sizeof(int));
    int *f = (int *)tak_malloc((size_t)cells * sizeof(int));
    int *parent = (int *)tak_malloc((size_t)cells * sizeof(int));
    int *heap = (int *)tak_malloc((size_t)cells * 8u * sizeof(int));
    uint8_t *closed = (uint8_t *)tak_malloc((size_t)cells);
    uint8_t *pinched = NULL;
    if (c.allow_pinch) {
        c.cross_memo = (uint8_t *)tak_malloc((size_t)cells);
        if (c.cross_memo) memset(c.cross_memo, 0, (size_t)cells);
        pinched = (uint8_t *)tak_malloc((size_t)cells);
        if (pinched) memset(pinched, 0, (size_t)cells);
    }
    if (!g || !f || !parent || !heap || !closed ||
        (c.allow_pinch && !pinched)) {
        if (g) tak_free(g);
        if (f) tak_free(f);
        if (parent) tak_free(parent);
        if (heap) tak_free(heap);
        if (closed) tak_free(closed);
        if (c.cross_memo) tak_free(c.cross_memo);
        if (pinched) tak_free(pinched);
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
    int heap_n = 0;
    g[start] = 0;
    f[start] = heuristic(sx, sy, gx, gy);
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
    if (max_expanded > 8192) max_expanded = 8192;
    int best = start;
    int best_h = heuristic(sx, sy, gx, gy);
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
        int cur_h = heuristic(cx, cy, gx, gy);
        if (cur_h < best_h) {
            best_h = cur_h;
            best = cur;
        }
        int ch0 = Terrain_SampleHeight(world, cell_to_world(cx), cell_to_world(cy));
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
            int nh = Terrain_SampleHeight(world, cell_to_world(nx), cell_to_world(ny));
            int step = dirs[di][2] + iabs32(nh - ch0) * 2 + extra;
            int ng = g[cur] + step;
            if (ng < g[ni]) {
                parent[ni] = cur;
                g[ni] = ng;
                f[ni] = ng + heuristic(nx, ny, gx, gy);
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
            path_put(out_path, end, c.cw);
        } else if (!query->compress) {
            for (int i = 0; i < chain_len; i++) {
                if (!path_put(out_path, heap[i], c.cw)) break;
            }
        } else {
            /* Keep a point only where the direction changes, and always
             * the last (legacy:22488-22495, 22528-22535). */
            int prev = start;
            int pdx = 0, pdy = 0;
            for (int i = 0; i < chain_len; i++) {
                int p = heap[i];
                int dx = p % c.cw - prev % c.cw;
                int dy = p / c.cw - prev / c.cw;
                if (i > 0 && (dx != pdx || dy != pdy)) {
                    if (!path_put(out_path, prev, c.cw)) break;
                }
                if (i == chain_len - 1) {
                    if (!path_put(out_path, p, c.cw)) break;
                }
                pdx = dx;
                pdy = dy;
                prev = p;
            }
        }
    }

    tak_free(g);
    tak_free(f);
    tak_free(parent);
    tak_free(heap);
    tak_free(closed);
    if (c.cross_memo) tak_free(c.cross_memo);
    if (pinched) tak_free(pinched);
    return out_path->count;
}
