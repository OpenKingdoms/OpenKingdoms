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

#endif
