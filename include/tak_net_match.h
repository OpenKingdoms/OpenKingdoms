#ifndef TAK_NET_MATCH_H
#define TAK_NET_MATCH_H

#include <stdint.h>

#include "tak_commands.h"
#include "tak_net_client.h"

/*
 * The simulation running on the server's turns instead of its own
 * clock.
 *
 * Three things change in a match and nothing else does.
 *
 * A local order goes to the server rather than into the local queue.
 * Every order already funnels through one function in command_emit, so
 * that is one branch and not a sweep.
 *
 * A turn that arrives is unpacked into the queue with the tick the
 * turn owns. The queue was built for this: TAK_CmdQueue_SubmitAt takes
 * a command that already carries its tick, and TAK_CommandBuffer_Stamp
 * writes the seat and tick onto a whole bundle.
 *
 * And the simulation never runs past the turns it holds, which is the
 * whole of lockstep in one sentence. A single player game holds every
 * turn the instant it wants one, so the same code runs both.
 */

/* A match has started. `turn_ticks` is how many simulation ticks one
 * turn covers, which the server names in START_GAME. */
void TAK_Match_Begin(TAK_NetClient *client, uint8_t seat, uint8_t turn_ticks);

void TAK_Match_End(void);

/* 1 while a match is running, which is what tells command_emit to send
 * rather than queue. */
int  TAK_Match_IsLive(void);

/* Our seat, or TAK_NET_SEAT_NONE outside a match. */
uint8_t TAK_Match_Seat(void);

/* Send one local command to the server. It is not queued locally: it
 * comes back in a turn like everyone else's, which is what keeps eight
 * machines applying the same tick in the same order. Returns 0, or -1
 * when it would not fit. */
int  TAK_Match_SubmitLocal(const TAK_GameCommand *cmd);

/* Take every turn the client is holding and put its commands in the
 * queue. Call it before the tick loop. Returns how many turns were
 * taken. */
int  TAK_Match_Pump(void);

/* May the simulation run the tick it is about to run? Outside a match
 * this is always 1. */
int  TAK_Match_CanAdvance(void);

/* The simulation has finished a tick. Acknowledges the turn it
 * completes, and carries the state hash on the ticks the protocol asks
 * for one. */
void TAK_Match_TickDone(uint32_t tick, uint32_t state_hash);

/* How far the held turns let the simulation run, for a test and for
 * the overlay that says who is being waited for. */
uint32_t TAK_Match_TickLimit(void);

#endif /* TAK_NET_MATCH_H */
