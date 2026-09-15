#ifndef TAK_CREDITS_H
#define TAK_CREDITS_H

#include "tak_platform.h"

/* Full-screen clip screen (GAMESTATE_CREDITS). Plays one clip out of
 * the game folder, centred, and moves on when it ends or a key that
 * types a character dismisses it. A click does not. */

int  Credits_Init(TAK_Platform *platform);
int  Credits_Tick(TAK_Platform *platform, float frame_dt);
void Credits_Shutdown(void);

/* What the next entry plays, as a path under the game folder, and the
 * state it goes to after. Spent by Credits_Init: an entry nobody asked
 * for plays Movies/Credits.bik and comes back to the menu. */
void Credits_Request(const char *rel_path, int next_state);

/* Where the current or last entry goes after, so a clip that could not
 * open still lands the player where the request said. */
int  Credits_ReturnState(void);

#endif
