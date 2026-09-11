#ifndef TAK_AI_H
#define TAK_AI_H

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

#endif /* TAK_AI_H */
