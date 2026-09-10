#ifndef TAK_END_SCREEN_H
#define TAK_END_SCREEN_H

#include "tak_platform.h"
#include "tak_world.h"
#include <stddef.h>

/* The victory and defeat statistics screen the original shows three
 * seconds after the banner (docs/notes/2026-09-10-end-of-battle.md).
 * It loads defeat.gui or victory<side>.gui, fills one row per player
 * from the world's tallies, and leaves on the two authored buttons:
 * Main Menu (cancel.wav) and Proceed (ok.wav, the skirmish battle
 * room). Enter presses Proceed and Escape presses Main Menu, as the
 * dialog's own accelerator string says. */

/* Load the dialog for the world's verdict. Returns 0 on success. */
int  EndScreen_Open(TAK_Platform *platform, const GameWorld *world);

/* Handle input and paint into the UI canvas. Returns GAMESTATE_IN_GAME
 * while the screen stays up, else the state a button chose. */
int  EndScreen_Tick(TAK_Platform *platform, const GameWorld *world);

void EndScreen_Close(void);
int  EndScreen_IsOpen(void);

/* ── Introspection (tests) ─────────────────────────────────────────── */

/* Path of the dialog in use, "" when closed. */
const char *EndScreen_DialogPath(void);
/* 1 when the row for player slot `slot` (0-based) is shown. */
int  EndScreen_RowShown(int slot);
/* The text a row shows in one column: "PlayerName", "UnitsBuilt",
 * "Kills", "Losses", "Time" or "Score". Returns 0 on success. */
int  EndScreen_RowText(int slot, const char *column, char *out, size_t cap);
/* The help strip text for a named button ("Proceed" or "MainMenu"). */
const char *EndScreen_ButtonHelp(const char *button);
/* Press a named button the way a click would (sound included) and
 * return the state it leads to. */
int  EndScreen_Press(const char *button);
/* Name of the last sound the screen asked for, "" if none. */
const char *EndScreen_LastSound(void);

#endif /* TAK_END_SCREEN_H */
