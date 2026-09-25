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

/* A game to join by its invite code once the server answers, from a
 * join link or --join. Letters and digits only, case folded. */
void        SelectGame_SetJoinCode(const char *code);
const char *SelectGame_JoinCode(void);

/* Returns the next GAMESTATE_, or its own while it stays up. */
int SelectGame_Tick(TAK_Platform *platform, float dt);

void SelectGame_Shutdown(void);

/* For the tests: how many rows the list is showing, which is selected,
 * and the text a row draws. NULL when the index is not a row. */
int         SelectGame_RowCount(void);
int         SelectGame_Selected(void);
/* Test seams: choose a row as a click would, and read a label of the
 * Game Information panel. LabelText returns 0 when the dialog has no
 * such label. */
void        SelectGame_SelectRow(int row);
int         SelectGame_LabelText(const char *name, char *out, size_t cap);
/* The list's top row, how many rows fit, and a press by widget name. */
int         SelectGame_Scroll(void);
int         SelectGame_RowsVisible(void);
void        SelectGame_Press(const char *name);
const char *SelectGame_RowName(int index);

/* Press a button by name, the way the runtime would on a click. A test
 * uses this to press one without working out where the mouse would
 * have to be. */
void SelectGame_HandleClick(const char *name);

/* The name this player is playing under. Never empty: a player who has
 * typed nothing is "Player", which is what the room shows. */
const char *SelectGame_PlayerName(void);
/* What is in the address box, for the screen tests. */
const char *SelectGame_Address(void);

/* The line the screen is showing a player, empty when there is none.
 * A connection that failed says so here rather than in a log. */
const char *SelectGame_Status(void);

#endif /* TAK_SELECT_GAME_H */
