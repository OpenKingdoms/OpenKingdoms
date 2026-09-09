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
 * movement and build placement. World coordinates are pixels. */
int TAK_PathPlan(const struct GameWorld *world,
                 int32_t start_x, int32_t start_y,
                 int32_t goal_x, int32_t goal_y,
                 int max_slope,
                 TAK_Path *out_path);

/* Drop the per-map passability cache (call on world load/unload). */
void TAK_PathCacheReset(void);

int TAK_PathPlanForMoveClass(const struct GameWorld *world,
                             int32_t start_x, int32_t start_y,
                             int32_t goal_x, int32_t goal_y,
                             const struct MoveClassDef *move_class,
                             int fallback_max_slope,
                             TAK_Path *out_path);

#endif /* TAK_PATHING_H */
