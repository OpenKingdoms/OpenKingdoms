#ifndef TAK_NET_PLAYER_H
#define TAK_NET_PLAYER_H

#include <stdint.h>

#include "tak_net_protocol.h"

/*
 * Who a player is on the leaderboard: the device token their client
 * says hello with, the same one a rejoin is recognised by. The token
 * is a secret the relay never shows, so the board names a player by a
 * one way hash of it. Anyone can read the id, nobody can work back to
 * a token that makes it, and a typed name no longer decides whose
 * record a game goes on.
 */

/* The id a token stands for. 0 for a token that is all zeros, which is
 * a client too old to send one. Never 0 otherwise. */
uint64_t TAK_Player_FromToken(const uint8_t token[TAK_NET_TOKEN_BYTES]);

#endif /* TAK_NET_PLAYER_H */
