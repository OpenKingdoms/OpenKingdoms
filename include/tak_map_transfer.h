#ifndef TAK_MAP_TRANSFER_H
#define TAK_MAP_TRANSFER_H

#include <stddef.h>
#include <stdint.h>

#include "tak_net_client.h"

/* The room's map on its way, both ends of it (protocol 11): the host says
 * whether it may be sent and answers each piece, and a player without it
 * asks, checks, keeps and mounts it, and tells the room. */

/* Every frame the room is up. */
void MapTransfer_Tick(TAK_NetClient *c, uint64_t now_ms);

/* Forget everything, for a session that ended or a test. */
void MapTransfer_Reset(void);

/* What this player is told about the room's map now, "" for nothing,
 * and a count that moves whenever it changes. */
const char *MapTransfer_Text(void);
unsigned    MapTransfer_TextSerial(void);

/* A line for the room's chat about someone's copy of the map, taken
 * once: 1 with out filled, 0 when there is none. */
int MapTransfer_TakeLine(char *out, size_t cap);

/* Why the host's Play was refused for a missing map, from the room as
 * it stands. */
void MapTransfer_WhyNotStart(const TAK_MsgRoomState *rs, char *out, size_t cap);

#endif /* TAK_MAP_TRANSFER_H */
