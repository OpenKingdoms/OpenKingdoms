#ifndef TAK_CREDITS_H
#define TAK_CREDITS_H

#include "tak_platform.h"

/* Credits screen (GAMESTATE_CREDITS). Plays Movies/Credits.bik
 * full-screen, centered. Returns to GAMESTATE_MENU when the video
 * ends, Escape is pressed, or the user clicks. */

int  Credits_Init(TAK_Platform *platform);
int  Credits_Tick(TAK_Platform *platform, float frame_dt);
void Credits_Shutdown(void);

#endif
