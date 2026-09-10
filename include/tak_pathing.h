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

#endif /* TAK_PATHING_H */
