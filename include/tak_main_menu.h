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
 * to the cursor), read a door's state (2 rest, 5 enter clip, 6 hover
 * clip looping, 7 leave clip; -1 when that door has no clips) and the
 * frame its clip is on (-1 at rest). */
void MainMenu_DebugForceHover(int button);
/* The next frame takes a click on this button, numbered as above:
 * 0 Skirmish, 1 Story, 2 Multiplayer, 3 Credits, 4 Options, 5 Exit,
 * 6 Replays. */
void MainMenu_DebugPress(int button);
/* A strip of the menu, in 640x480 units, that holds no control. The
 * browser page puts its own plate there and nowhere else. */
SDL_Rect MainMenu_PlateRoom(void);
/* Every rect the menu answers a click in or writes text into; returns
 * how many were written, at most max. */
int  MainMenu_DebugControlRects(SDL_Rect *out, int max);
/* The version line the menu draws. */
const char *MainMenu_VersionText(void);
/* 1 while the replay list is up over the menu. */
int  MainMenu_ReplaysOpen(void);
int  MainMenu_DebugCharacterState(int character);
int  MainMenu_DebugCharacterFrame(int character);
/* Whether a door is drawn from a clip (1) or from its sheet (0);
 * -1 for a door with no clips. */
int  MainMenu_DebugCharacterDrawsClip(int character);

#endif /* TAK_MAIN_MENU_H */
