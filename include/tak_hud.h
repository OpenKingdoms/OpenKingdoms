#ifndef TAK_HUD_H
#define TAK_HUD_H

#include "tak_platform.h"
#include "tak_world.h"

/* ── In-game HUD ──────────────────────────────────────────────────
 *
 * The HUD is the per-faction `<side>ingame.gui` dialog rendered into
 * the 640x480 UI canvas. Every layout figure comes off that dialog
 * (see HUD_GetViewportRect below), never from a hardcoded constant. */

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
    HUD_CMD_ACTIVATE    = 122,  /* Active button: open the gate  (legacy:151449) */
    HUD_CMD_DEACTIVATE  = 123,  /* Inactive button: close it     (legacy:151470) */

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

/* Fire an immediate (non-targeting) command as if its button had been
 * clicked. Returns 1 when the mode is one the HUD dispatches. */
int  HUD_TriggerCommand(int mode);

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

/* Arm a pending command as its sidebar button would. Test hook. */
void HUD_SetCommandMode(int mode);

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

/* ── Layout ──────────────────────────────────────────────────────────
 *
 * Both rects come back in window pixels through the platform's canvas
 * transform, so the world (drawn straight to the renderer) lines up
 * with the HUD art. The play area is what the dialog leaves free, left
 * of the sidebar and above the bottom strip, the bounds legacy derives
 * at legacy:150187-150214. Return 0 when unavailable. */
int  HUD_GetViewportRect(const TAK_Platform *plat, SDL_Rect *out);
int  HUD_GetMinimapRect(const TAK_Platform *plat, SDL_Rect *out);
/* The same play area in 640x480 canvas units, for text drawn into the
 * UI canvas. Return 0 when the HUD has no dialog. */
int  HUD_GetViewportCanvasRect(SDL_Rect *out);

/* ── Introspection (tests) ─────────────────────────────────────────── */

/* 1 when the named widget exists in the HUD dialog and every copy of it
 * is hidden. araingame.gui authors some names twice. */
int  HUD_WidgetHidden(const char *name);
/* The frame the named widget draws with right now, -1 if absent. */
int  HUD_WidgetFrame(const char *name);
/* Copy a widget's current display text. 1 when the widget exists. */
int  HUD_WidgetText(const char *name, char *out, size_t cap);
/* Selected-unit panel rects in dialog space: the name label and the
 * portrait. Legacy authors them side by side and relies on widget art
 * staying inside its own rect, so these must not overlap. */
int  HUD_GetUnitInfoRects(SDL_Rect *out_text, SDL_Rect *out_image);
/* The fractions the sidebar gauges were last drawn at: the selected
 * unit's health and own mana, and the player's pool (crystal ball). */
void HUD_GetGaugeFractions(float *out_health, float *out_mana, float *out_pool);
/* Window rect of a visible action button (MOVE, PATROL, ...) by its
 * command mode, as laid out by the last HUD_Draw. 0 when the current
 * selection shows no such button. */
int  HUD_GetActionButtonRect(int mode, SDL_Rect *out);
/* Build buttons as laid out by the last HUD_Draw, in dialog space. */
int  HUD_BuildSlotCount(void);
/* 1 when the in-game dialog has a widget of that name and it shows. */
int  HUD_WidgetVisible(const char *name);
int  HUD_GetBuildSlotDialogRect(int slot, SDL_Rect *out, int *out_def_idx);
/* The queue-count text box inside a build button, dialog space.
 * Returns 0 when that button has no queue. */
int  HUD_GetQueueBadgeDialogRect(int slot, SDL_Rect *out);

#endif /* TAK_HUD_H */
