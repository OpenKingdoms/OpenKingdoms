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
 * invisible while they move: the legacy cost grid only ever learns
 * about a unit footprint once its move stamp is old
 * (legacy:188962-188972), and unit-vs-unit blocking on the move is
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

/* Cell-centre terrain test with the footprint corners sampled too, so
 * a wide class does not plan its centre along a cliff edge. */
static int cell_walkable_slow(const struct GameWorld *world,
                              int x, int y, int cw, int ch,
                              const MoveClassDef *move_class,
                              int fallback_max_slope) {
    if (x < 0 || y < 0 || x >= cw || y >= ch) return 0;
    int slope = movement_max_slope(move_class, fallback_max_slope);
    int32_t wx = cell_to_world(x);
    int32_t wy = cell_to_world(y);
    if (!Terrain_IsWalkable(world, wx, wy, slope)) return 0;
    if (!water_ok(world, move_class, wx, wy)) return 0;

    int fp_x = (move_class && move_class->footprint_x > 0)
             ? move_class->footprint_x : 1;
    int fp_z = (move_class && move_class->footprint_z > 0)
             ? move_class->footprint_z : 1;
    int half_x = (fp_x * 16) / 2;
    int half_z = (fp_z * 16) / 2;
    if (half_x <= 8 && half_z <= 8) return 1;

    static const int corners[4][2] = {
        {-1,-1}, { 1,-1}, {-1, 1}, { 1, 1}
    };
    for (int i = 0; i < 4; i++) {
        int32_t sx = wx + corners[i][0] * half_x;
        int32_t sy = wy + corners[i][1] * half_z;
        if (!Terrain_IsWalkable(world, sx, sy, slope)) return 0;
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
#define PCACHE_MAX 16
static struct {
    const struct GameWorld *world;
    const MoveClassDef     *mc;
    int       fallback_slope;
    int       cw, ch;
    uint8_t  *bits;
    uint8_t  *clear;
    uint32_t  clear_version;
    int       tw, th;
} g_pcache[PCACHE_MAX];
static int g_pcache_n = 0;

void TAK_PathCacheReset(void) {
    for (int i = 0; i < g_pcache_n; i++) {
        tak_free(g_pcache[i].bits);
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
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            bits[y * cw + x] = (uint8_t)cell_walkable_slow(
                world, x, y, cw, ch, mc, fallback_slope);
    int i = g_pcache_n++;
    g_pcache[i].world = world;
    g_pcache[i].mc = mc;
    g_pcache[i].fallback_slope = fallback_slope;
    g_pcache[i].cw = cw;
    g_pcache[i].ch = ch;
    g_pcache[i].bits = bits;
    g_pcache[i].clear = NULL;
    g_pcache[i].clear_version = 0;
    g_pcache[i].tw = 0;
    g_pcache[i].th = 0;
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
    int tw = (world->map_pixels_w + TAK_OCC_TILE_PX - 1) / TAK_OCC_TILE_PX;
    int th = (world->map_pixels_h + TAK_OCC_TILE_PX - 1) / TAK_OCC_TILE_PX;
    if (tw <= 0 || th <= 0) return NULL;
    if (g_pcache[ci].clear && g_pcache[ci].tw == tw &&
        g_pcache[ci].th == th &&
        g_pcache[ci].clear_version == world->occ_version) {
        return g_pcache[ci].clear;
    }
    if (!g_pcache[ci].clear || g_pcache[ci].tw != tw ||
        g_pcache[ci].th != th) {
        if (g_pcache[ci].clear) tak_free(g_pcache[ci].clear);
        g_pcache[ci].clear = (uint8_t *)tak_malloc((size_t)tw * th);
        if (!g_pcache[ci].clear) return NULL;
        g_pcache[ci].tw = tw;
        g_pcache[ci].th = th;
    }
    uint8_t *c = g_pcache[ci].clear;
    int slope = movement_max_slope(g_pcache[ci].mc,
                                   g_pcache[ci].fallback_slope);
    /* Terrain is judged per path cell with a one tile footprint, the
     * same test the bitmap uses before its corner samples: the
     * clearance itself is what accounts for the footprint. */
    int cw = g_pcache[ci].cw, ch = g_pcache[ci].ch;
    uint8_t *plain = (uint8_t *)tak_malloc((size_t)cw * ch);
    if (!plain) return NULL;
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            int32_t wx = cell_to_world(x), wy = cell_to_world(y);
            plain[y * cw + x] = (uint8_t)(
                Terrain_IsWalkable(world, wx, wy, slope) &&
                water_ok(world, g_pcache[ci].mc, wx, wy));
        }
    }
    /* Largest free square anchored at each tile: one pass from the far
     * corner, each tile one more than the least of its right, lower and
     * diagonal neighbours. */
    for (int ty = th - 1; ty >= 0; ty--) {
        for (int tx = tw - 1; tx >= 0; tx--) {
            int cx = tx / OCC_PER_PATH_CELL, cy = ty / OCC_PER_PATH_CELL;
            if (cx >= cw || cy >= ch || !plain[cy * cw + cx] ||
                !tile_unbuilt(world, tx, ty)) {
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
    tak_free(plain);
    g_pcache[ci].clear_version = world->occ_version;
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
    const uint8_t *clear;
    int tw, th;
} PlanCtx;

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
    /* The footprint sits on the tiles the mover stamps: top left at
     * centre minus half the footprint (occ_step_blocked in units.c). */
    int tx0 = Occ_TileOf(wx - c->fx * 8);
    int ty0 = Occ_TileOf(wy - c->fz * 8);
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

/* The start is where the unit already stands: whoever is parked on
 * the surrounding tiles does not make it a cell to plan away from,
 * only the ground and what is built there do. */
static int nearest_open(const PlanCtx *c, int *x, int *y, int is_start) {
    *x = clampi(*x, 0, c->cw - 1);
    *y = clampi(*y, 0, c->ch - 1);
    if (cell_ok_live(c, *x, *y, !is_start)) return 1;
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
        c.clear = clearance_get(ci);
        c.tw = g_pcache[ci].tw;
        c.th = g_pcache[ci].th;
    }

    int sx = world_to_cell(start_x), sy = world_to_cell(start_y);
    int gx = world_to_cell(goal_x),  gy = world_to_cell(goal_y);
    if (!nearest_open(&c, &sx, &sy, 1)) return 0;
    if (!nearest_open(&c, &gx, &gy, 0)) return 0;
    out_path->start_x = cell_to_world(sx);
    out_path->start_y = cell_to_world(sy);

    int *g = (int *)tak_malloc((size_t)cells * sizeof(int));
    int *f = (int *)tak_malloc((size_t)cells * sizeof(int));
    int *parent = (int *)tak_malloc((size_t)cells * sizeof(int));
    int *heap = (int *)tak_malloc((size_t)cells * 8u * sizeof(int));
    uint8_t *closed = (uint8_t *)tak_malloc((size_t)cells);
    if (!g || !f || !parent || !heap || !closed) {
        if (g) tak_free(g);
        if (f) tak_free(f);
        if (parent) tak_free(parent);
        if (heap) tak_free(heap);
        if (closed) tak_free(closed);
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
            int nh = Terrain_SampleHeight(world, cell_to_world(nx), cell_to_world(ny));
            int step = dirs[di][2] + iabs32(nh - ch0) * 2;
            int ng = g[cur] + step;
            if (ng < g[ni]) {
                parent[ni] = cur;
                g[ni] = ng;
                f[ni] = ng + heuristic(nx, ny, gx, gy);
                heap_push(heap, &heap_n, f, ni);
            }
        }
    }

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
    return out_path->count;
}
