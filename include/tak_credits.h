#ifndef TAK_CREDITS_H
#define TAK_CREDITS_H

#include "tak_platform.h"

/* Full-screen clip screen (GAMESTATE_CREDITS). Plays one clip out of
 * the game folder, or two in a row, centred, and moves on when it ends
 * or a key that types a character dismisses it. A click does not. */

int  Credits_Init(TAK_Platform *platform);
int  Credits_Tick(TAK_Platform *platform, float frame_dt);
void Credits_Shutdown(void);

/* What the next entry plays, as a path under the game folder, and the
 * state it goes to after. Spent by Credits_Init: an entry nobody asked
 * for plays Movies/Credits.bik and comes back to the menu. */
void Credits_Request(const char *rel_path, int next_state);
/* The same with a second clip after the first, NULL for none. A key on
 * the first skips both, and a first the install lacks plays the second. */
void Credits_RequestPair(const char *first, const char *then, int next_state);
/* What the next entry will play and where it goes after, as asked. */
const char *Credits_RequestedClip(void);
const char *Credits_RequestedThen(void);   /* "" when one clip */
int  Credits_RequestedState(void);
/* Frames of the reel's soundtrack the mixer has taken, for tests. */
int  Credits_SoundPlayed(void);

/* Where the current or last entry goes after, so a clip that could not
 * open still lands the player where the request said. */
int  Credits_ReturnState(void);

#endif
