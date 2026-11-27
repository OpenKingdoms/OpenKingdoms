#ifndef TAK_INGAME_MENU_H
#define TAK_INGAME_MENU_H

#include "tak_platform.h"
#include "tak_battle_config.h"
#include "tak_save_browser.h"
#include <stddef.h>

/* ── The F1 menu ──────────────────────────────────────────────────────
 *
 * F1 opens a dialog over the running battle (keys.tdf:169 binds it to
 * F2Menu, loader legacy:122746-122785). The file is chosen by mode
 * (legacy:154643-154655): f2menusingleplayer.gui for a campaign
 * mission, f2menuskirmish.gui for a skirmish, f2menumultiplayer.gui
 * for a netplay battle. The dialog owns the keyboard and the mouse
 * while it is up, and its root accelerator string "#Enter#Resume#Esc#
 * Resume" says which button Enter and Escape press.
 *
 * Resume closes it (legacy:154721-154726). Load Game and Save Game
 * open the original's own dialogs over the menu, which stays drawn
 * behind them (legacy:154703-154712). Game Information is drawn by
 * the shipped art and does nothing here: we have no game information
 * screen yet. */

/* Restart replays the battle the exit submenu was opened over
 * (legacy:156329-156336). Returns 1 when one is waiting, fills in what
 * World_BeginLoad needs, and clears the request. */
int  InGameMenu_TakeRestart(BattleConfig *out_cfg,
                            char *out_map, size_t map_cap,
                            char *out_kingdom, size_t kingdom_cap);

/* Load the dialog for the battle's mode. 0 on success. The platform is
 * what its Options page sizes. */
int  InGameMenu_Open(TAK_Platform *platform);

/* Handle input and paint over the battle. Returns GAMESTATE_IN_GAME
 * while the menu stays up, else the state a button chose. */
int  InGameMenu_Tick(TAK_Platform *platform);

void InGameMenu_Close(void);

/* Ctrl+S and Ctrl+L: the menu with its save or load dialog up. -1 and
 * nothing opened in a match, or for a save in a replay. */
int  InGameMenu_OpenBrowser(int load);

/* What the save or load dialog just did. GAMESTATE_GAME_LOADING when
 * a save was chosen and the battle it names is being brought up,
 * GAMESTATE_IN_GAME otherwise. The menu calls this itself each
 * frame; it is public so a test can drive the dialog directly. */
int  InGameMenu_TakeBrowserResult(SaveBrowserResult result);

/* 1 while the save or load dialog is up over the menu. */
int  InGameMenu_BrowserOpen(void);
int  InGameMenu_IsOpen(void);

/* ── Introspection (tests) ─────────────────────────────────────────── */

/* Path of the dialog on top, "" when closed. */
const char *InGameMenu_DialogPath(void);
/* 1 when the dialog carries a widget of that name. */
int  InGameMenu_HasButton(const char *name);
/* The help strip text a button carries, "" when it has none. */
const char *InGameMenu_ButtonHelp(const char *name);
/* The root's accelerator string, e.g. "#Enter#Resume#Esc#Resume". */
const char *InGameMenu_Accelerators(void);
/* Press a named button the way a click would, and return the state it
 * leads to. */
int  InGameMenu_Press(const char *name);
/* Press whichever button an accelerator key names, "Enter" or "Esc". */
int  InGameMenu_PressKey(const char *key);
/* Name of the last sound the menu asked for, "" if none. */
const char *InGameMenu_LastSound(void);

#endif /* TAK_INGAME_MENU_H */
