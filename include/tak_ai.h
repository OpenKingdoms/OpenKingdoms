#ifndef TAK_AI_H
#define TAK_AI_H

#include <stdint.h>

typedef struct GameWorld GameWorld;

int TAK_AI_ClampDifficulty(int difficulty);
int TAK_AI_PursuitRadius(int sight_distance, int weapon_range, int difficulty);
void TAK_AI_TickSkirmish(GameWorld *world);
/* Force profile re-read on next tick (tests / map change). */
void TAK_AI_ResetProfile(void);

/* units.c reports an enemy hit; the AI answers it on its next tick. */
void TAK_AI_NotifyDamage(int victim_handle, int shooter_handle);

/* Test observability. Orders issued by an AI player since the match
 * began: attack orders, plus marches unless attacks_only; the count
 * of base-defence orders; the enemy its waves currently go for. */
int  TAK_AI_DebugHostileOrders(int from_player, int to_player,
                               int attacks_only);
int  TAK_AI_DebugDefenceOrders(int player_id);
int  TAK_AI_DebugAttackPlayer(int player_id);
int  TAK_AI_DebugWaveTarget(int player_id);   /* unit handle, -1 none */

/* A unit slot about to take a new unit: drop any wave target or threat
 * that still names it. Units_Spawn calls this. */
void TAK_AI_ForgetUnit(int handle);

/* Start a match: clear what the last one left and derive the AI's own
 * stream from the session seed. Seed 0 gives the stream every match
 * used before there was a seed. World_BeginLoad calls this. */
void TAK_AI_BeginMatch(uint32_t seed);

/* FNV-1a over the AI's integer state and its random cursor, in a
 * fixed order. The companion to Units_DebugStateHash. */
unsigned int TAK_AI_DebugStateHash(void);

#endif /* TAK_AI_H */
