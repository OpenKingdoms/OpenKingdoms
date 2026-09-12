#ifndef TAK_INGAME_MENU_H
#define TAK_INGAME_MENU_H

#include "tak_platform.h"
#include "tak_battle_config.h"
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
 * Resume closes it (legacy:154721-154726). Game Information, Load Game
 * and Save Game are drawn by the shipped art and do nothing here: we
 * have no save system and no game information screen yet. */

/* Restart replays the battle the exit submenu was opened over
 * (legacy:156329-156336). Returns 1 when one is waiting, fills in what
 * World_BeginLoad needs, and clears the request. */
int  InGameMenu_TakeRestart(BattleConfig *out_cfg,
                            char *out_map, size_t map_cap,
                            char *out_kingdom, size_t kingdom_cap);

/* Load the dialog for the battle's mode. 0 on success. */
int  InGameMenu_Open(void);

/* Handle input and paint over the battle. Returns GAMESTATE_IN_GAME
 * while the menu stays up, else the state a button chose. */
int  InGameMenu_Tick(TAK_Platform *platform);

void InGameMenu_Close(void);
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
