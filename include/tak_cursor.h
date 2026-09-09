#ifndef TAK_CURSOR_H
#define TAK_CURSOR_H

#include <SDL.h>

/* ── Cursor type enum ───────────────────────────────────────────────
 *
 * Matches the 20 cursor sequences in cursors.gaf, in the same order
 * as the original engine's loading code (the legacy reference lines
 * 162227-162265). The enum values are used as indices into the
 * internal cursor table. */

typedef enum CursorType {
    CURSOR_NORMAL = 0,
    CURSOR_HOURGLASS,
    CURSOR_GREEN,
    CURSOR_RED,
    CURSOR_FINDSITE,
    CURSOR_SELECT,
    CURSOR_MOVE,
    CURSOR_UNLOAD,
    CURSOR_LOAD,
    CURSOR_RECLAMATE,
    CURSOR_REVIVE,
    CURSOR_TELEPORT,
    CURSOR_PICKUP,
    CURSOR_PATROL,
    CURSOR_REPAIR,
    CURSOR_DEFEND,
    CURSOR_CAPTURE,
    CURSOR_TOOFAR,
    CURSOR_AIRSTRIKE,
    CURSOR_ATTACK,
    CURSOR_COUNT
} CursorType;

/* Initialize the cursor system. Loads cursors.gaf + cursors.pcx from
 * the VFS, decodes every frame of every cursor type to RGBA, and builds
 * SDL_Cursor objects with the correct hotspots. Sets the initial cursor
 * to CURSOR_NORMAL.
 *
 * Call after VFS_Init and UI_Init. Returns 0 on success, -1 on failure
 * (the game can still run with the default OS cursor). */
int Cursor_Init(void);

/* Release all SDL_Cursor objects and GAF resources. Call before
 * SDL_Quit / TAK_Platform_Shutdown. */
void Cursor_Shutdown(void);

/* Switch to a different cursor type. Takes effect immediately. */
void Cursor_SetType(CursorType type);

/* Get the current cursor type. */
CursorType Cursor_GetType(void);

/* Advance animated cursors by one tick. Call once per frame. Cursors
 * with a single frame are unaffected. */
void Cursor_Tick(void);

/* Show or hide the cursor. visible=1 shows, visible=0 hides. */
void Cursor_SetVisible(int visible);

#endif /* TAK_CURSOR_H */
