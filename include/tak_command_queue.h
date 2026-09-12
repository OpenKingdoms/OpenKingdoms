#ifndef TAK_COMMAND_QUEUE_H
#define TAK_COMMAND_QUEUE_H

#include "tak_commands.h"

/*
 * The command queue: every player action waits here for its tick.
 *
 * A single player game runs the same queue at zero delay, so the only
 * path an order can take to the simulation is the multiplayer one and
 * every session exercises it. In a match the delay is the turn the
 * server scheduled the command for.
 *
 * Order inside a tick is by seat and then by arrival, never by which
 * client's packet landed first, so eight machines apply the same tick
 * in the same order.
 */

#define TAK_CMD_QUEUE_MAX 512

void     TAK_CmdQueue_Reset(uint32_t delay_ticks);
/* Ticks between submitting a command and running it. Zero for a
 * single player game. */
uint32_t TAK_CmdQueue_Delay(void);
void     TAK_CmdQueue_SetDelay(uint32_t delay_ticks);

/* The tick the queue is about to run. Starts at zero and advances one
 * per TAK_CmdQueue_Run. */
uint32_t TAK_CmdQueue_Tick(void);

/* A command from the local seat. Stamps the seat and schedules it for
 * the current tick plus the delay. Returns 0, or -1 when the queue is
 * full or the command is one this build cannot run. */
int      TAK_CmdQueue_Submit(uint8_t seat, const TAK_GameCommand *cmd);

/* A command that already carries its tick, which is how a turn bundle
 * from the server arrives. Returns 0 or -1. */
int      TAK_CmdQueue_SubmitAt(const TAK_GameCommand *cmd);

/* Run everything due at the current tick, then advance. Returns the
 * number of commands applied. */
int      TAK_CmdQueue_Run(void);

/* Commands waiting for a tick yet to come. */
int      TAK_CmdQueue_Pending(void);

/* Called with every command the moment before it is applied, in the
 * order they are applied. What a replay writer records. A reset leaves
 * it in place. */
typedef void (*TAK_CmdQueueObserver)(const TAK_GameCommand *cmd, void *user);
void     TAK_CmdQueue_SetObserver(TAK_CmdQueueObserver fn, void *user);

/* ── The queue in a save ─────────────────────────────────────────
 *
 * A save can be taken with orders still waiting for their tick, and
 * in fact usually is: the queue runs at the top of a tick and the AI
 * submits for the next one at the bottom, so between ticks it is
 * rarely empty. A save that dropped them would quietly cancel every
 * order in flight.
 *
 * The queue is not in the simulation hash. Two peers are entitled to
 * hold different commands in flight, because a command a player has
 * just given has not reached the others yet, and hashing that would
 * report a desync on every order. */

/* The arrival counter, which orders commands inside one seat's tick. */
uint32_t TAK_CmdQueue_Arrival(void);

/* Walk the waiting commands. `index` runs 0..TAK_CMD_QUEUE_MAX-1 over
 * the slots; a slot holding nothing returns 0 and fills nothing in.
 * Returns 1 when `out` and `arrival` were written. */
int      TAK_CmdQueue_At(int index, TAK_GameCommand *out, uint32_t *arrival);

/* Empty the queue and put its counters back. Follow with
 * TAK_CmdQueue_Put for each command that was waiting. */
void     TAK_CmdQueue_Restore(uint32_t tick, uint32_t delay, uint32_t arrival);

/* Put one command back exactly as it was, tick, seat and arrival
 * included. Returns 0, or -1 when the queue is full. */
int      TAK_CmdQueue_Put(const TAK_GameCommand *cmd, uint32_t arrival);

/* Commands applied since the last reset, and the last one applied. */
int      TAK_CmdQueue_AppliedCount(void);
const TAK_GameCommand *TAK_CmdQueue_LastApplied(void);

#endif /* TAK_COMMAND_QUEUE_H */
