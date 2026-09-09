#ifndef TAK_MULTIPLAYER_H
#define TAK_MULTIPLAYER_H

#include "tak_platform.h"

/* Multiplayer battle lobby (GAMESTATE_MULTIPLAYER) — battlemenumulti.gui,
 * reached by clicking the multiknight (Multiplayer) button on the main
 * menu. */

int  Multiplayer_Init(TAK_Platform *platform);
int  Multiplayer_Tick(TAK_Platform *platform, float frame_dt);
void Multiplayer_Shutdown(void);

#endif
