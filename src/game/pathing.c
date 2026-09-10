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

/* ── Dynamic occupancy ────────────────────────────────────────────
 * Deliberately NOT folded into the passability cache below: that cache
 * assumes static terrain, while structures appear, die and open their
 * yards mid-game. One 32-px path cell covers 2x2 occupancy tiles. An
 * own closed gate stays passable and is opened on arrival
 * (legacy:21986-22032); a foreign one blocks.
 *
 * Mobile occupants are deliberately invisible here: the legacy
 * pathfinder's cost map only ever learns about structures (the yard
 * setter notifies it, legacy:219056), and unit-vs-unit blocking is
 * resolved at move time instead (legacy:219329-219340). Planning
 * around live units would make every route stale the tick after. */
#define OCC_PER_PATH_CELL (PATH_CELL_PX / TAK_OCC_TILE_PX)

static int cell_occ_ok(const struct GameWorld *world, int x, int y,
                       int player_id) {
    if (!world || !world->occ) return 1;
    int tx0 = x * OCC_PER_PATH_CELL;
    int ty0 = y * OCC_PER_PATH_CELL;
    for (int dy = 0; dy < OCC_PER_PATH_CELL; dy++) {
        for (int dx = 0; dx < OCC_PER_PATH_CELL; dx++) {
            if (Occ_QueryTileStatic(world, tx0 + dx, ty0 + dy,
                                    player_id) == 1) {
                return 0;
            }
        }
    }
    return 1;
}

static int cell_walkable(const struct GameWorld *world,
                         int x, int y, int cw, int ch, int max_slope,
                         int player_id) {
    if (x < 0 || y < 0 || x >= cw || y >= ch) return 0;
    if (!Terrain_IsWalkable(world, cell_to_world(x), cell_to_world(y),
                            max_slope)) {
        return 0;
    }
    return cell_occ_ok(world, x, y, player_id);
}

static int movement_max_slope(const MoveClassDef *move_class,
                              int fallback_max_slope) {
    if (move_class && move_class->max_slope > 0) return move_class->max_slope;
    return fallback_max_slope;
}

/* ── Per-move-class passability cache ─────────────────────────────
 * Terrain is static; the slow path re-samples bilinear heights per
 * neighbor per A* call (~2.4ms/call — the 1000-unit killer). Cache
 * one bitmap per (move class, fallback slope) per map. */
#define PCACHE_MAX 16
static struct {
    const struct GameWorld *world;
    const MoveClassDef     *mc;
    int       fallback_slope;
    int       cw, ch;
    uint8_t  *bits;
} g_pcache[PCACHE_MAX];
static int g_pcache_n = 0;

static int cell_walkable_slow(const struct GameWorld *world,
                              int x, int y, int cw, int ch,
                              const MoveClassDef *move_class,
                              int fallback_max_slope);

void TAK_PathCacheReset(void) {
    for (int i = 0; i < g_pcache_n; i++) tak_free(g_pcache[i].bits);
    g_pcache_n = 0;
}

static const uint8_t *pcache_get(const struct GameWorld *world,
                                 int cw, int ch,
                                 const MoveClassDef *mc,
                                 int fallback_slope) {
    for (int i = 0; i < g_pcache_n; i++) {
        if (g_pcache[i].world == world && g_pcache[i].mc == mc &&
            g_pcache[i].fallback_slope == fallback_slope &&
            g_pcache[i].cw == cw && g_pcache[i].ch == ch)
            return g_pcache[i].bits;
    }
    if (g_pcache_n >= PCACHE_MAX) {
        /* new map or class churn: drop everything */
        for (int i = 0; i < g_pcache_n; i++) tak_free(g_pcache[i].bits);
        g_pcache_n = 0;
    }
    uint8_t *bits = (uint8_t *)tak_malloc((size_t)cw * ch);
    if (!bits) return NULL;
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            bits[y * cw + x] = (uint8_t)cell_walkable_slow(
                world, x, y, cw, ch, mc, fallback_slope);
    g_pcache[g_pcache_n].world = world;
    g_pcache[g_pcache_n].mc = mc;
    g_pcache[g_pcache_n].fallback_slope = fallback_slope;
    g_pcache[g_pcache_n].cw = cw;
    g_pcache[g_pcache_n].ch = ch;
    g_pcache[g_pcache_n].bits = bits;
    g_pcache_n++;
    return bits;
}

static int cell_walkable_for_move_class(const struct GameWorld *world,
                                        int x, int y, int cw, int ch,
                                        const MoveClassDef *move_class,
                                        int fallback_max_slope,
                                        int player_id) {
    if (x < 0 || y < 0 || x >= cw || y >= ch) return 0;
    /* Key on the RESOLVED slope: a class with its own maxslope ignores
     * the per-def fallback entirely, so keying on the fallback gave one
     * cache entry per unit type and thrashed the 16-slot table. */
    int slope = movement_max_slope(move_class, fallback_max_slope);
    const uint8_t *bits = pcache_get(world, cw, ch, move_class, slope);
    if (bits) {
        if (!bits[y * cw + x]) return 0;
    } else if (!cell_walkable_slow(world, x, y, cw, ch, move_class,
                                   slope)) {
        return 0;
    }
    return cell_occ_ok(world, x, y, player_id);
}

static int cell_walkable_slow(const struct GameWorld *world,
                              int x, int y, int cw, int ch,
                              const MoveClassDef *move_class,
                              int fallback_max_slope) {
    if (x < 0 || y < 0 || x >= cw || y >= ch) return 0;
    int slope = movement_max_slope(move_class, fallback_max_slope);
    int32_t wx = cell_to_world(x);
    int32_t wy = cell_to_world(y);
    if (!Terrain_IsWalkable(world, wx, wy, slope)) return 0;

    /* Water-depth gate (legacy:219149-219156): planning must
     * respect the same [min, max] water depth window the movement
     * predicate enforces, or A* hands units illegal water routes. */
    if (move_class && world->water_height > 0) {
        /* waterheight (sidedata) is in raw heightmap units;
         * Terrain_SampleHeight returns display-biased values (−32). */
        int raw_h = Terrain_SampleHeight(world, wx, wy);  /* raw map units */
        int depth = world->water_height - raw_h;
        if (depth < 0) depth = 0;
        if (depth > move_class->max_water_depth) return 0;
        if (move_class->min_water_depth > 0 &&
            depth < move_class->min_water_depth) return 0;
    }

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

static int nearest_walkable(const struct GameWorld *world,
                            int *x, int *y, int cw, int ch, int max_slope,
                            int player_id) {
    *x = clampi(*x, 0, cw - 1);
    *y = clampi(*y, 0, ch - 1);
    if (cell_walkable(world, *x, *y, cw, ch, max_slope, player_id)) return 1;
    int best_x = -1, best_y = -1;
    int best_d = INT_MAX;
    for (int r = 1; r <= 16; r++) {
        for (int yy = *y - r; yy <= *y + r; yy++) {
            for (int xx = *x - r; xx <= *x + r; xx++) {
                if (xx != *x - r && xx != *x + r &&
                    yy != *y - r && yy != *y + r) {
                    continue;
                }
                if (!cell_walkable(world, xx, yy, cw, ch, max_slope,
                                   player_id)) {
                    continue;
                }
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

static int nearest_walkable_for_move_class(const struct GameWorld *world,
                                           int *x, int *y, int cw, int ch,
                                           const MoveClassDef *move_class,
                                           int fallback_max_slope,
                                           int player_id) {
    *x = clampi(*x, 0, cw - 1);
    *y = clampi(*y, 0, ch - 1);
    if (cell_walkable_for_move_class(world, *x, *y, cw, ch, move_class,
                                     fallback_max_slope, player_id)) {
        return 1;
    }
    int best_x = -1, best_y = -1;
    int best_d = INT_MAX;
    for (int r = 1; r <= 16; r++) {
        for (int yy = *y - r; yy <= *y + r; yy++) {
            for (int xx = *x - r; xx <= *x + r; xx++) {
                if (xx != *x - r && xx != *x + r &&
                    yy != *y - r && yy != *y + r) {
                    continue;
                }
                if (!cell_walkable_for_move_class(world, xx, yy, cw, ch,
                                                  move_class,
                                                  fallback_max_slope,
                                                  player_id)) {
                    continue;
                }
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
    if (!world || !out_path || world->map_pixels_w <= 0 ||
        world->map_pixels_h <= 0) {
        return 0;
    }
    memset(out_path, 0, sizeof(*out_path));
    int cw = (world->map_pixels_w + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int ch = (world->map_pixels_h + PATH_CELL_PX - 1) / PATH_CELL_PX;
    int cells = cw * ch;
    if (cw <= 0 || ch <= 0 || cells <= 0) return 0;

    int sx = world_to_cell(start_x), sy = world_to_cell(start_y);
    int gx = world_to_cell(goal_x),  gy = world_to_cell(goal_y);
    if (move_class) {
        if (!nearest_walkable_for_move_class(world, &sx, &sy, cw, ch,
                                             move_class,
                                             fallback_max_slope, player_id)) {
            return 0;
        }
        if (!nearest_walkable_for_move_class(world, &gx, &gy, cw, ch,
                                             move_class,
                                             fallback_max_slope, player_id)) {
            return 0;
        }
    } else {
        if (!nearest_walkable(world, &sx, &sy, cw, ch, fallback_max_slope,
                              player_id)) return 0;
        if (!nearest_walkable(world, &gx, &gy, cw, ch, fallback_max_slope,
                              player_id)) return 0;
    }

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

    int start = cell_index(sx, sy, cw);
    int goal = cell_index(gx, gy, cw);
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
    /* 1024 nodes only covers ~a 32x32-cell bubble — long routes hit the
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
        int cx = cur % cw;
        int cy = cur / cw;
        int cur_h = heuristic(cx, cy, gx, gy);
        if (cur_h < best_h) {
            best_h = cur_h;
            best = cur;
        }
        int ch0 = Terrain_SampleHeight(world, cell_to_world(cx), cell_to_world(cy));
        for (int di = 0; di < 8; di++) {
            int nx = cx + dirs[di][0];
            int ny = cy + dirs[di][1];
            if (move_class) {
                if (!cell_walkable_for_move_class(world, nx, ny, cw, ch,
                                                  move_class,
                                                  fallback_max_slope,
                                                  player_id)) {
                    continue;
                }
            } else if (!cell_walkable(world, nx, ny, cw, ch,
                                      fallback_max_slope, player_id)) {
                continue;
            }
            if (dirs[di][0] != 0 && dirs[di][1] != 0) {
                if (move_class) {
                    if (!cell_walkable_for_move_class(world, cx + dirs[di][0],
                                                      cy, cw, ch, move_class,
                                                      fallback_max_slope,
                                                      player_id)) {
                        continue;
                    }
                    if (!cell_walkable_for_move_class(world, cx,
                                                      cy + dirs[di][1],
                                                      cw, ch, move_class,
                                                      fallback_max_slope,
                                                      player_id)) {
                        continue;
                    }
                } else {
                    if (!cell_walkable(world, cx + dirs[di][0], cy, cw, ch,
                                       fallback_max_slope, player_id)) continue;
                    if (!cell_walkable(world, cx, cy + dirs[di][1], cw, ch,
                                       fallback_max_slope, player_id)) continue;
                }
            }
            int ni = cell_index(nx, ny, cw);
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
        int written = 0;
        for (int step = chain_len - 1; step >= 0 &&
             written < TAK_PATH_MAX_WAYPOINTS; step--) {
            int p = end;
            for (int k = 0; k < step; k++) p = parent[p];
            out_path->x[written] = cell_to_world(p % cw);
            out_path->y[written] = cell_to_world(p / cw);
            written++;
        }
        out_path->count = written;
    }

    tak_free(g);
    tak_free(f);
    tak_free(parent);
    tak_free(heap);
    tak_free(closed);
    return out_path->count;
}
