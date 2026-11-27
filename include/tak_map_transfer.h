#ifndef TAK_MAP_TRANSFER_H
#define TAK_MAP_TRANSFER_H

#include <stddef.h>
#include <stdint.h>

#include "tak_net_client.h"

/*
 * The room's map on its way from the host to a player without it, both
 * ends of it, over the session's client (protocol 11).
 *
 * As host it says whether the room's map may be sent (tak_map_share.h)
 * and answers each piece a player asks for. As a player without the map
 * it asks for the pieces a few at a time, says how far it has come,
 * checks what came, keeps it and mounts it, and tells the room it has
 * the map. A map that may not be sent is never asked for, and the room
 * says what the player needs instead.
 */

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
