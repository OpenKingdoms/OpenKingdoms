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

/* Act on a named widget as if clicked: a tab button, a checkbox on the
 * current page, or the dialog's own Ok and Cancel. Returns 1 when the
 * name was handled. */
int  Options_ClickWidget(const char *name);

/* The level the current page shows, in percent, or -1 where the page
 * carries no slider. */
int  Options_DebugVolume(void);

/* Move that slider as a drag would. Returns 0 where there is nothing to
 * move, which is also what a slider greyed out by Music On reports. */
int  Options_DebugSetVolume(int percent);

#endif
