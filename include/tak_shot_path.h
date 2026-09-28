#ifndef TAK_SHOT_PATH_H
#define TAK_SHOT_PATH_H

/* What a shot in flight meets on the map.
 *
 * The original runs one test for each 16 px map cell a shot passes
 * through, after every substep of its flight (legacy:245377-245476).
 * The order matters and is kept: off the map, a unit on the ground, a
 * unit in the air, then for a weapon that is not unitsonly a feature,
 * the sea and the ground. Integer only, like every lockstep module. */

#include <stdint.h>

struct GameWorld;

#define TAK_SHOT_FLY      0   /* nothing here, the shot flies on */
#define TAK_SHOT_OFFMAP   1   /* left the map, gone without a burst */
#define TAK_SHOT_UNIT     2   /* struck the unit named in *out_unit */
#define TAK_SHOT_FEATURE  3   /* a tree, rock or wreck taller than the shot */
#define TAK_SHOT_WATER    4   /* came down on the sea */
#define TAK_SHOT_GROUND   5   /* at or below the ground of its cell */
#define TAK_SHOT_BOUNCE   6   /* groundbounce: the caller turns it upward */

/* The weapon flags the test reads (legacy:250034-250038). */
#define TAK_SHOT_UNITS_ONLY    0x01u  /* unitsonly: only units stop it */
#define TAK_SHOT_GROUND_BOUNCE 0x02u  /* groundbounce */
#define TAK_SHOT_WATER_WEAPON  0x04u  /* waterweapon: flies under the sea */

/* The unit layer answers for bodies, which this module never sees. */
typedef struct TAK_ShotBodies {
    /* Unit `handle` holds the occupancy cell the shot is in. 1 when it
     * is not `owner`'s and height h is inside its body. */
    int  (*ground)(void *user, int handle, int owner, int h);
    /* A unit in the air over (x, y), not `owner`'s, whose body holds
     * height h. Its handle, or -1. */
    int  (*air)(void *user, int32_t x, int32_t y, int h, int owner);
    void *user;
} TAK_ShotBodies;

/* The ground of the 16 px cell under (x, y): its lowest corner, which
 * is what the original compares a shot against (legacy:224590-224622).
 * 0 when the point is off the map or the map has no heights. */
int ShotPath_CellFloor(const struct GameWorld *w, int32_t x, int32_t y,
                       int *out_floor);

/* One cell test for a shot owned by player `owner` at (x, y) and
 * height h in whole pixels. Returns a TAK_SHOT_* kind. *out_unit is
 * the unit struck, or -1. A world with no heights lets everything fly. */
int ShotPath_Test(struct GameWorld *w, int32_t x, int32_t y, int h,
                  int owner, unsigned flags, const TAK_ShotBodies *bodies,
                  int *out_unit);

/* How many steps of at most 16 px a move of (dx, dy) takes, at least
 * one (legacy:250425-250433). */
int ShotPath_Substeps(int32_t dx, int32_t dy);

#endif /* TAK_SHOT_PATH_H */
