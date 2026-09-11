#ifndef TAK_MULTIPLAYER_H
#define TAK_MULTIPLAYER_H

#include "tak_platform.h"
#include "tak_gui_render.h"

/* Multiplayer battle room (GAMESTATE_MULTIPLAYER), battlemenumulti.gui,
 * reached from the main menu's Multiplayer (multiknight) button. */

int  Multiplayer_Init(TAK_Platform *platform);
int  Multiplayer_Tick(TAK_Platform *platform, float frame_dt);
void Multiplayer_Shutdown(void);

/* Inspection and drive points for tests. */

/* The room's widget runtime, NULL before Init. */
GUIRuntime *Multiplayer_Runtime(void);

/* Choose the battle's map by its .ota base name. Returns 0 when the map
 * exists and is now the room's map. */
int Multiplayer_SelectMap(const char *key);

/* The host's Allow Creon choice. The room allows Creon only when the
 * expansion is present as well. */
void Multiplayer_SetAllowCreon(int allow);

/* 1 when this room allows Creon: the expansion is present and the host
 * allowed it. What a join handshake would carry. */
int  Multiplayer_CreonAllowed(void);

/* Press the host row's PlayerSide cell, and read the side it holds. */
void Multiplayer_CycleHostSide(void);
int  Multiplayer_HostSide(void);

#endif
