#ifndef TAK_MAIN_MENU_H
#define TAK_MAIN_MENU_H

#include "tak_platform.h"

/*
 * Main menu screen (GAMESTATE_MENU).
 *
 * Loads and displays the main menu from mainmenu.gui assets:
 * - Background: mainscreen.gaf "MainBG"
 * - Character sprites: singlemachine.gaf, bodgirl.gaf, multiknight.gaf
 * - Buttons: Exit, Options (from mainscreen.gaf)
 *
 * Each GAF loads its palette from the matching .pcx file.
 * Character animations cycle through entries on hover.
 */

/* Initialize the main menu: load all GAF assets, decode to RGBA.
 * Call once when entering GAMESTATE_MENU. Returns 0 on success. */
int MainMenu_Init(TAK_Platform *platform);

/* Per-frame update and render. Call every frame while in GAMESTATE_MENU.
 * Returns the game state to transition to, or GAMESTATE_MENU to stay. */
int MainMenu_Tick(TAK_Platform *platform, float frame_dt);

/* Free all main menu resources. Call when leaving GAMESTATE_MENU. */
void MainMenu_Shutdown(void);

/* Test hooks: force which button counts as hovered (-1 none, -2 back
 * to the cursor) and read a door's state (2 rest, 5 enter clip, 6
 * hover clip held, 7 leave clip; -1 when that door has no clips). */
void MainMenu_DebugForceHover(int button);
int  MainMenu_DebugCharacterState(int character);

#endif /* TAK_MAIN_MENU_H */
