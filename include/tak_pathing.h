#ifndef TAK_PATHING_H
#define TAK_PATHING_H

#include "tak_types.h"

struct GameWorld;
struct MoveClassDef;

#define TAK_PATH_MAX_WAYPOINTS 96

typedef struct TAK_Path {
    int count;
    int32_t x[TAK_PATH_MAX_WAYPOINTS];
    int32_t y[TAK_PATH_MAX_WAYPOINTS];
    /* Centre of the cell the search started from, which may differ
     * from the requested start when that stood on blocked ground. The
     * first route segment runs from here (legacy:22528-22535). */
    int32_t start_x, start_y;
} TAK_Path;

/* Deterministic terrain-grid A* over the same passability predicate used by
 * movement and build placement. World coordinates are pixels.
 *
 * player_id is the owner planning the route: structures on the unit
 * occupancy layer block, except the planner's own closed gates, which
 * it routes through and opens on arrival (legacy:21986-22032). Pass 0
 * for "no owner" (everything on the layer blocks). */
int TAK_PathPlan(const struct GameWorld *world,
                 int32_t start_x, int32_t start_y,
                 int32_t goal_x, int32_t goal_y,
                 int max_slope,
                 int player_id,
                 TAK_Path *out_path);

/* Drop the per-map passability cache (call on world load/unload). The
 * cache is terrain only; occupancy is sampled live on top of it. */
void TAK_PathCacheReset(void);

int TAK_PathPlanForMoveClass(const struct GameWorld *world,
                             int32_t start_x, int32_t start_y,
                             int32_t goal_x, int32_t goal_y,
                             const struct MoveClassDef *move_class,
                             int fallback_max_slope,
                             int player_id,
                             TAK_Path *out_path);

/* Who is planning. footprint_x/z are in 16-px tiles; 0 takes the move
 * class footprint (1 without a class). self_plus1 is the planner's own
 * occupancy id so its parked footprint never blocks its own start.
 * compress keeps only the points where the route changes direction,
 * plus the last one, the way the original stores a route
 * (legacy:22488-22495). */
typedef struct TAK_PathQuery {
    const struct MoveClassDef *move_class;
    int fallback_max_slope;
    int player_id;
    int self_plus1;
    int footprint_x;
    int footprint_z;
    int compress;
    /* 1 when the goal is a unit's position: whoever is parked on the
     * goal cell (the target itself) does not shift the goal, so an
     * attacker walks up to contact. */
    int goal_is_unit;
} TAK_PathQuery;

/* Footprint-aware plan. A cell is open when the largest square
 * footprint that fits at it (the clearance map) covers this unit's
 * footprint, and no structure or parked unit other than self stands on
 * the footprint tiles. */
int TAK_PathPlanQuery(const struct GameWorld *world,
                      int32_t start_x, int32_t start_y,
                      int32_t goal_x, int32_t goal_y,
                      const TAK_PathQuery *query,
                      TAK_Path *out_path);

/* Clearance at a 16-px tile for a move class: the side of the largest
 * square of tiles with this tile at its top left that the class can
 * cross and no structure stands on. 0 when the tile itself is blocked. */
int TAK_PathClearanceAt(const struct GameWorld *world,
                        const struct MoveClassDef *move_class,
                        int fallback_max_slope,
                        int tile_x, int tile_y);

/* Probe counters. Cumulative since the process started and never
 * reset: a caller takes two readings and subtracts. The clock hook
 * lets a probe time cache rebuilds without this module reading a
 * clock of its own. Nothing here feeds back into a plan. */
typedef struct TAK_PathDebugCounters {
    uint32_t plans;           /* searches run */
    uint64_t work;            /* nodes expanded by those searches */
    uint32_t rebuilds;        /* passability and clearance builds */
    uint64_t rebuild_clock;   /* clock ticks spent in them */
    uint32_t cache_bytes;     /* bytes held by the per-layer caches */
} TAK_PathDebugCounters;

void TAK_PathDebugGetCounters(TAK_PathDebugCounters *out);
void TAK_PathDebugSetClock(uint64_t (*now)(void));

/* Drop the cached layers every n searches, so a test can run the
 * same battle against cold caches. 0 turns it off. Cache warmth
 * must never reach the simulation, and this is how that is
 * checked. */
void TAK_PathDebugResetEvery(int plans);

#endif /* TAK_PATHING_H */
