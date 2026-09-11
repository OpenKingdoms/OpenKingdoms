#ifndef TAK_COMMAND_EXEC_H
#define TAK_COMMAND_EXEC_H

#include "tak_commands.h"

/*
 * The command executor: the one place a player command turns into a
 * change in the simulation.
 *
 * It holds the single ownership check. A command carries the seat the
 * server stamped on it, every unit it names has to belong to that
 * seat, and nothing further down looks at a seat again. That is what
 * stops one client ordering another player's army, and it is why the
 * order primitives in tak_unit.h take no owner.
 *
 * Seats here run 1 to 8, as in the world. The relay's TURN message
 * numbers them 0 to 7, so the client glue adds one before it calls in.
 */

/* Apply one command. Returns how many units it moved, or for the
 * seat-wide commands 1 when it took and 0 when the room refused it.
 * A command whose units are dead, whose seat does not own them, or
 * whose type this build cannot run applies nothing and returns 0. */
int TAK_CommandExec_Apply(const TAK_GameCommand *cmd);

#endif /* TAK_COMMAND_EXEC_H */
