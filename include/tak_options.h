#ifndef TAK_OPTIONS_H
#define TAK_OPTIONS_H

#include "tak_platform.h"

/* Options screen (GAMESTATE_OPTIONS). Renders data/guis/options.gui
 * with the generic GUI runtime. "Previous"/"Cancel" → back to the
 * previous state the caller set via Options_SetReturnState. */

int  Options_Init(TAK_Platform *platform);
int  Options_Tick(TAK_Platform *platform, float frame_dt);
void Options_Shutdown(void);

/* The screen that launched Options (Main Menu or Battle Setup). When the
 * user cancels, we return to this state. Default is GAMESTATE_MENU. */
void Options_SetReturnState(int state);

#endif
