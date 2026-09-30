#ifndef TAK_SAVE_BROWSER_H
#define TAK_SAVE_BROWSER_H

#include "tak_platform.h"
#include "tak_savegame.h"

#include <stddef.h>

/*
 * The original's own save and load dialogs, data/guis/savegame.gui and
 * data/guis/loadgame.gui, picked by which one the caller asked for
 * (legacy:158806-158813). Both are opened from the F1 menu, which
 * dispatches on the button name (legacy:154703-154712), and the load
 * one is also opened from the skirmish lobby's Load Game button
 * (legacy:137474-137477).
 *
 * They share a list box of the saved game directory, a Delete button
 * that deletes at once with no confirmation (legacy:159241-159244), a
 * right hand panel of Side, Map and Game Time filled on every selection
 * change (legacy:159131-159160), and Cancel. The save dialog adds a
 * Game Name edit box; clicking a row copies that name into it, which is
 * how the original offers to overwrite, and OK writes straight over any
 * existing file with no prompt (legacy:159300-159356, legacy:159247-159292).
 *
 * This module draws over whatever is behind it and never presents: the
 * screen hosting it owns the frame, the way the F1 menu owns it for
 * Options.
 */

typedef enum {
    SAVEBROWSER_SAVE = 0,
    SAVEBROWSER_LOAD = 1,
    /* The load dialog over the recorded battles: a row is the date it
     * was played, the panel its kingdom, map and length, and the help
     * strip its players. Load plays it. */
    SAVEBROWSER_REPLAYS = 2
} SaveBrowserMode;

typedef enum {
    SAVEBROWSER_OPEN = 0,    /* still up                                */
    SAVEBROWSER_CANCELLED,   /* the player backed out, or had no saves  */
    SAVEBROWSER_SAVED,       /* a file was written                      */
    SAVEBROWSER_LOAD_READY,  /* a save is open, waiting to be taken      */
    SAVEBROWSER_REPLAY_READY /* a replay is open and checked, see Replay_Open */
} SaveBrowserResult;

/* Open one of the two dialogs. Returns 0, or -1 when the shipped art
 * will not load. Opening the load dialog with an empty directory still
 * succeeds: it comes up showing "There are no saved games." and closes
 * on the first press, the way the original does (legacy:158733-158758). */
int  SaveBrowser_Open(SaveBrowserMode mode);
void SaveBrowser_Close(void);
int  SaveBrowser_IsOpen(void);

/* One frame. Draws the dialog and returns what the player did. */
SaveBrowserResult SaveBrowser_Tick(TAK_Platform *platform);

/* The save the player chose, after SAVEBROWSER_LOAD_READY. Ownership
 * passes to the caller, which closes it with Save_ReadClose. NULL when
 * nothing is waiting. */
TAK_SaveGame *SaveBrowser_TakeLoad(void);

/* ── Introspection, for the screen tests ───────────────────────────── */

const char *SaveBrowser_DialogPath(void);
int         SaveBrowser_HasWidget(const char *name);
const char *SaveBrowser_Accelerators(void);

int         SaveBrowser_RowCount(void);
const char *SaveBrowser_RowName(int row);
int         SaveBrowser_SelectedRow(void);
void        SaveBrowser_SelectRow(int row);

/* Where the load dialog paints the saved battle. 0 when the dialog
 * does not author the panel, which the save dialog does not. */
int         SaveBrowser_RadarViewRect(SDL_Rect *out);

/* Where the load dialog paints the saved battle. 0 when the dialog
 * does not author the panel, which the save dialog does not. */
int         SaveBrowser_RadarViewRect(SDL_Rect *out);

const char *SaveBrowser_DetailSide(void);
const char *SaveBrowser_DetailMap(void);
const char *SaveBrowser_DetailTime(void);
/* The selected row's help strip line, "" for a save. */
const char *SaveBrowser_DetailPlayers(void);

/* The message box text, "" when none is up. */
const char *SaveBrowser_Message(void);
/* Press the message box's OK, the way Enter and Escape do. */
SaveBrowserResult SaveBrowser_DismissMessage(void);

const char *SaveBrowser_Name(void);
void        SaveBrowser_SetName(const char *name);

/* Press a named widget, the way a click on it would. */
SaveBrowserResult SaveBrowser_Press(const char *name);
/* Press whatever the root's accelerator string binds to "Enter"/"Esc". */
SaveBrowserResult SaveBrowser_PressKey(const char *key);

#endif /* TAK_SAVE_BROWSER_H */
