#ifndef TAK_STORY_H
#define TAK_STORY_H

#include "tak_platform.h"

/* Story / campaign screen (GAMESTATE_CAMPAIGN) — the "Book of Deeds"
 * dialog (bod.gui) that the user reaches by clicking the bodgirl
 * (Story) button on the main menu. */

int  Story_Init(TAK_Platform *platform);
int  Story_Tick(TAK_Platform *platform, float frame_dt);
void Story_Shutdown(void);

/* Start one Book of Darien mission by zero-based campaign index.
 * Primarily used by tests and the Story Play button. Returns
 * GAMESTATE_GAME_LOADING on success, GAMESTATE_CAMPAIGN on failure. */
int  Story_StartMission(TAK_Platform *platform, int mission_index);

#endif
