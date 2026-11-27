#ifndef TAK_KEYBINDS_SCREEN_H
#define TAK_KEYBINDS_SCREEN_H

#include "tak_platform.h"

/*
 * The key binds dialog (D-057): the table of tak_keybinds.h on the
 * shipped Diplomacy panel, data/guis/diplomacy.gui, which is a ruled
 * list of seven rows with Ok and Cancel. Options opens it from its
 * Interface page and hands it the frame while it is up.
 *
 * It draws over whatever is on the offscreen surface and never
 * presents. Its own Ok keeps the keys and writes them at once, and its
 * Cancel puts back what it found.
 */

int  KeyScreen_Open(void);
int  KeyScreen_IsOpen(void);
/* Take the frame's input and draw. Returns 0 once it has closed. */
int  KeyScreen_Tick(TAK_Platform *platform);
/* Closed any way but its Ok, it puts back the keys it found. */
void KeyScreen_Close(void);
/* 1 when the last one to close was left by Ok. */
int  KeyScreen_Kept(void);

#endif /* TAK_KEYBINDS_SCREEN_H */
