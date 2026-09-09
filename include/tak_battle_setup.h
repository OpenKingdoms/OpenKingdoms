#ifndef TAK_BATTLE_SETUP_H
#define TAK_BATTLE_SETUP_H

#include "tak_platform.h"


/* Initialize the battle setup screen: load all GAF assets, decode to RGBA.
 * Call once when entering GAMESTATE_BATTLE_SETUP. Returns 0 on success. */
int BattleSetup_Init(TAK_Platform *platform);

/* Per-frame update and render. Call every frame while in GAMESTATE_BATTLE_SETUP.
 * Returns the game state to transition to, or GAMESTATE_BATTLE_SETUP to stay. */
int BattleSetup_Tick(TAK_Platform *platform, float frame_dt);

/* Free all battle setup screen resources. Call when leaving GAMESTATE_BATTLE_SETUP. */
void BattleSetup_Shutdown(void);

#endif /* TAK_BATTLE_SETUP_H */
