#ifndef TAK_AI_H
#define TAK_AI_H

typedef struct GameWorld GameWorld;

int TAK_AI_ClampDifficulty(int difficulty);
int TAK_AI_PursuitRadius(int sight_distance, int weapon_range, int difficulty);
void TAK_AI_TickSkirmish(GameWorld *world);
/* Force profile re-read on next tick (tests / map change). */
void TAK_AI_ResetProfile(void);

#endif /* TAK_AI_H */
