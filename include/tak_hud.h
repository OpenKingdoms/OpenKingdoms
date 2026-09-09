#ifndef TAK_HUD_H
#define TAK_HUD_H

#include "tak_platform.h"
#include "tak_world.h"

/* ── In-game HUD (Phase K) ────────────────────────────────────────
 *
 * Stage 1 (current): placeholder rectangles in the right sidebar
 * and bottom strip. The game viewport is shrunk to leave room.
 * Stage 2 wires selection + portrait. Stage 3 swaps placeholders
 * for the real per-faction GAF panel art that TAK ships. */

/* Layout constants — sized off a 1024x768 reference window from the
 * manual screenshots. The right sidebar is roughly 25% of width and
 * the bottom strip ~12% of height. Math is in pixels. */
#define HUD_SIDEBAR_W       256
#define HUD_BOTTOM_H         80
#define HUD_MINIMAP_PADDING   8

/* Initialise the HUD layout for the current window. Updates
 * world->viewport_w/h to exclude HUD-occluded pixels so camera
 * bounds + spawn-at-center math reflect the visible game area. */
void HUD_Init(TAK_Platform *plat, GameWorld *world);

/* Render the HUD on top of the game viewport. Call after
 * Terrain_Render / Units_Render and before Minimap_Draw (so the
 * minimap sits at the top of the right sidebar). */
void HUD_Draw(TAK_Platform *plat, const GameWorld *world);

/* Convert a window-pixel position into "is this in the HUD?" so
 * mouse handlers can dispatch correctly between game-world clicks
 * and HUD interactions. */
int  HUD_HitTest(int win_x, int win_y, TAK_Platform *plat);

/* ── Command-mode interaction (legacy click-then-click flow) ──
 *
 * Player clicks an action button → HUD enters a "pending command"
 * mode (cursor changes to that command's icon). Next world-click
 * triggers the actual command, mode clears. Right-click cancels.
 *
 * Mirrors the legacy reference ~151328 where `Console_RegisterHotkeys(N)`
 * sets a pending-cmd state that the world-click handler reads. */
typedef enum {
    HUD_CMD_NONE        = 0,
    /* Targeting commands (cursor swaps to that mode's GAF cursor;
     * next world-click executes the order). */
    HUD_CMD_MOVE        = 1,
    HUD_CMD_ATTACK      = 2,
    HUD_CMD_GUARD       = 3,
    HUD_CMD_PATROL      = 4,
    HUD_CMD_LOAD        = 5,    /* pickup transport target          */
    HUD_CMD_UNLOAD      = 6,    /* drop transport at point          */
    HUD_CMD_HEAL        = 7,    /* repair/heal target               */
    HUD_CMD_CLEAR       = 8,    /* clear/reclaim feature            */
    HUD_CMD_W_SPECIAL   = 9,    /* fire Special-slot weapon at point */
    /* Immediate (non-targeting) commands — fire on click, no cursor swap. */
    HUD_CMD_STOP        = 100,
    HUD_CMD_AGGRO_OFF   = 101,  /* set selection to UNIT_AGGRO_OFFENSIVE */
    HUD_CMD_AGGRO_DEF   = 102,
    HUD_CMD_AGGRO_PAS   = 103,
    HUD_CMD_W_PRIMARY   = 110,  /* set weapon slot 0                */
    HUD_CMD_W_SECONDARY = 111,  /* set weapon slot 1                */
    HUD_CMD_W_SET_SPEC  = 112,  /* set weapon slot 2 (no cursor swap) */
    HUD_CMD_CLOAK_ON    = 120,
    HUD_CMD_CLOAK_OFF   = 121,

    /* Context cursors (not commands) — the default-cursor hover logic
     * uses these ids in the same cursor-sprite table: select hand over
     * friendlies, normal pointer over terrain, red for illegal. */
    HUD_CUR_SELECT      = 124,
    HUD_CUR_NORMAL      = 125,
    HUD_CUR_RED         = 126,
    /* Building placement: click a build-menu icon → enter this mode
     * with HUD_GetBuildPlacementDefIdx() returning the buildable's
     * def_idx. World-click commits the building site. Right-click
     * cancels. */
    HUD_CMD_PLACE_BUILD = 200,
} HUDCommandMode;

/* When in HUD_CMD_PLACE_BUILD, this is the def_idx of the buildable
 * the player picked from the menu. -1 if not in placement mode. */
int  HUD_GetBuildPlacementDefIdx(void);
void HUD_BeginBuildPlacement(int def_idx);

/* Returns 1 if the given mode is a "targeting" mode (cursor swap +
 * world-click expected). Otherwise it's an immediate-action button. */
int  HUD_IsTargetingMode(int mode);

/* Returns 1 if (win_x, win_y) hit an action button and the mode
 * was changed/issued; the caller (ingame.c click handler) should
 * treat the click as consumed. Stop is fired immediately and the
 * mode reverts to NONE. */
int  HUD_HandleSidebarClick(int win_x, int win_y, TAK_Platform *plat);

/* Active pending command, or HUD_CMD_NONE. ingame.c reads this on
 * world clicks to dispatch Move/Attack/Patrol etc. */
int  HUD_GetCommandMode(void);

/* Reset to HUD_CMD_NONE — used on world-click execution and on
 * right-click cancel. */
void HUD_ClearCommandMode(void);

/* Render the active-command's cursor at (win_x, win_y) in place of
 * the OS cursor. Called every frame from ingame.c when the mouse
 * is in the world viewport AND a command mode is active. */
void HUD_DrawCommandCursor(TAK_Platform *plat, int win_x, int win_y);

/* Draw a specific cursor sprite (HUD_CMD_* targeting id or HUD_CUR_*
 * context id) at the window position, honoring the GAF hotspot.
 * Returns 1 if a sprite was drawn, 0 if none is loaded for that id. */
int  HUD_DrawCursorById(TAK_Platform *plat, int cursor_id,
                        int win_x, int win_y);
/* Right-click on the sidebar (build-card dequeue). Returns 1 if a
 * slot consumed the click. */
int  HUD_HandleSidebarRightClick(int win_x, int win_y, TAK_Platform *plat);
/* Draw queue-count badges over the build cards. Call AFTER UI_Present. */
void HUD_DrawQueueBadges(TAK_Platform *plat);

#endif /* TAK_HUD_H */
