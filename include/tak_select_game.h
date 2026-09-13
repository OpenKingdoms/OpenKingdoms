#ifndef TAK_SELECT_GAME_H
#define TAK_SELECT_GAME_H

#include "tak_platform.h"

/*
 * Select Game (selectgame.gui), the screen between the main menu and a
 * battle room.
 *
 * It shows what rooms a server is offering and lets a player join one
 * or host their own, which is what the original's screen did. It reads
 * the room list off the session and never touches a socket.
 */

int SelectGame_Init(TAK_Platform *platform);

/* Returns the next GAMESTATE_, or its own while it stays up. */
int SelectGame_Tick(TAK_Platform *platform, float dt);

void SelectGame_Shutdown(void);

/* For the tests: how many rows the list is showing, which is selected,
 * and the text a row draws. NULL when the index is not a row. */
int         SelectGame_RowCount(void);
int         SelectGame_Selected(void);
const char *SelectGame_RowName(int index);

/* The line the screen is showing a player, empty when there is none.
 * A connection that failed says so here rather than in a log. */
const char *SelectGame_Status(void);

#endif /* TAK_SELECT_GAME_H */
