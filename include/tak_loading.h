#ifndef TAK_LOADING_H
#define TAK_LOADING_H

#include "tak_platform.h"

/* Loading screen (GAMESTATE_GAME_LOADING).
 *
 * Shows loadscreen.gui (the parchment + banner) with a progress bar and
 * status text. The engine lane pumps progress via Loading_SetProgress
 * as it loads maps/units/etc.; when progress hits 1.0 and a full frame
 * has rendered, Loading_Tick returns GAMESTATE_IN_GAME.
 */

int  Loading_Init(TAK_Platform *platform);
int  Loading_Tick(TAK_Platform *platform, float frame_dt);
void Loading_Shutdown(void);

/* 0.0 .. 1.0 — clamped on call. */
void Loading_SetProgress(float fraction);
void Loading_SetStatus(const char *status_line);  /* e.g. "Loading units..." */

#endif
