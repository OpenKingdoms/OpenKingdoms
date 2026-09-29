#ifndef TAK_AI_SQUAD_H
#define TAK_AI_SQUAD_H

#include <stdint.h>

/* ── A squad on the march ─────────────────────────────────────────────
 *
 * The group pulls a member that has run ahead of it back to its pace
 * (A-010). Integer only, and pure: the AI hands in distances and reads a
 * decision back. */

/* A member this far ahead of the group stands until the rest close:
 * it is nearer the target than the members' mean by more than the lead,
 * or by more than half of it once it is already standing, so a member
 * does not stop and start on the line. */
#define AI_SQUAD_LEAD 256

int AI_Squad_ShouldWait(int32_t member_d, int64_t sum_d, int members,
                        int moving);

#endif /* TAK_AI_SQUAD_H */
