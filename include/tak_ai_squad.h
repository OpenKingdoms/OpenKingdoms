#ifndef TAK_AI_SQUAD_H
#define TAK_AI_SQUAD_H

#include <stdint.h>

/* ── Potential fields for a squad on the march ─────────────────────
 *
 * Two charges steer a group's members once it is out (A-010). The
 * group pulls a member that has run ahead of it back to its pace, and
 * the enemy pushes a ranged member's firing position round to the side
 * of the target where it stands thinnest. Integer only, and pure: the
 * AI hands in positions and reads a decision back. */

/* A member this far ahead of the group stands until the rest close:
 * it is nearer the target than the members' mean by more than the lead,
 * or by more than half of it once it is already standing, so a member
 * does not stop and start on the line. */
#define AI_SQUAD_LEAD 256

int AI_Squad_ShouldWait(int32_t member_d, int64_t sum_d, int members,
                        int moving);

/* One enemy's push: where it stands and its combat value. */
typedef struct AiSquadCharge {
    int32_t x, y;
    int32_t value;
} AiSquadCharge;

/* How far an enemy's push reaches, and the walk in pixels one point of
 * its combat value is worth at its centre. */
#define AI_SQUAD_CHARGE_REACH 256
#define AI_SQUAD_CHARGE_WEIGHT 48
#define AI_SQUAD_RING_POINTS 16

/* The firing position for a member at (mx, my) against a target at
 * (tx, ty): of sixteen points on a ring of radius r round the target,
 * the one where the walk to it plus the enemy's push there is least,
 * the first of equals. Writes the point and returns its index. */
int AI_Squad_FiringPoint(int32_t mx, int32_t my, int32_t tx, int32_t ty,
                         int32_t r, const AiSquadCharge *enemies, int count,
                         int32_t *out_x, int32_t *out_y);

/* The push at a point, in pixels of walk. */
int64_t AI_Squad_Push(int32_t x, int32_t y, const AiSquadCharge *enemies,
                      int count);

#endif /* TAK_AI_SQUAD_H */
