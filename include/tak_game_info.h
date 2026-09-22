#ifndef TAK_GAME_INFO_H
#define TAK_GAME_INFO_H

#include "tak_platform.h"

/* ── Game Information ──────────────────────────────────────────────
 *
 * The F1 menu's second button. The original opens gameinformation.gui
 * over the menu with two tabs in its placement guide (legacy:154864-
 * 154930): Briefing, the mission's text a line to a row
 * (gameinfobriefing.gui, legacy:155034-155107), and Game Settings, the
 * battle's options a row each (gamesettings.gui, legacy:155139-155250).
 * A campaign opens on the briefing and anything else on the settings,
 * since only a mission has a briefing. Ok and Escape close it. */

struct GameWorld;

int  GameInfo_Open(const struct GameWorld *world);
void GameInfo_Close(void);
int  GameInfo_IsOpen(void);
/* One frame over the menu: draws it and takes its input. 1 on the
 * frame it closes. */
int  GameInfo_Tick(TAK_Platform *platform);

/* The tab on show, "Briefing" or "GameSettings", a named press, and
 * the rows of the tab on show, for tests. */
const char *GameInfo_Tab(void);
int         GameInfo_Press(const char *name);
int         GameInfo_RowCount(void);
const char *GameInfo_Row(int i);
int         GameInfo_Scroll(void);

#endif /* TAK_GAME_INFO_H */
