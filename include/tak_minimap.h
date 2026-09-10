#ifndef TAK_MINIMAP_H
#define TAK_MINIMAP_H

#include "tak_platform.h"

/* In-battle minimap widget. Reads from the current GameWorld (via
 * World_Get) during Minimap_Init, so the caller must ensure the world
 * is loaded (TNT parsed + palette ready) before calling.
 *
 * The widget draws the pre-rendered overview image
 * (tnt.ingame_minimap_bg) palette-expanded to RGBA and uploaded as a
 * GPU texture, then the fog composite, then a dot per visible unit in
 * its owner's colour, then the camera-viewport outline. */

/* Build the minimap texture from the current world. Idempotent: if a
 * texture already exists, returns 0 without re-doing the upload.
 * Returns -1 if there's no loaded world or the world has no
 * ingame_minimap_bg. Non-fatal — the in-game screen can still run
 * without a minimap. */
int  Minimap_Init(TAK_Platform *plat);

/* Draw the minimap into the top-right of the window. No-op if not
 * initialized. Call between TAK_Platform_FrameBegin and
 * TAK_Platform_Present on the same frame. */
void Minimap_Draw(TAK_Platform *plat);

/* Click-and-drag camera jump. ingame.c calls this every tick with
 * the current window-space mouse state. If the left mouse button is
 * held AND the cursor is over the minimap, out_cam_x/out_cam_y are
 * filled with the desired camera top-left (already clamped to map
 * bounds) and the function returns 1. Otherwise returns 0 and leaves
 * out_* untouched.
 *
 * Call this BEFORE edge-scroll in ingame.c — a minimap drag should
 * take priority over mouse-on-edge scroll so a user can drag near
 * the screen edge without the edge-scroll fighting the jump. */
int Minimap_HandleInput(TAK_Platform *plat,
                         int win_mouse_x, int win_mouse_y,
                         int left_button_down,
                         int32_t *out_cam_x, int32_t *out_cam_y);

/* Debug: the window-pixel rect a unit dot at this world position would
 * occupy, clipped to the drawn map. Returns 0 when the minimap is not
 * up or the position falls outside it. */
int Minimap_DebugDotRect(TAK_Platform *plat, int32_t world_x, int32_t world_y,
                         SDL_Rect *out);

/* Free the GPU texture. Called from World_End — the minimap's lifetime
 * matches the world's, not any single in-game session. Safe to call
 * multiple times or without a preceding Init (no-op). */
void Minimap_Shutdown(TAK_Platform *plat);

#endif /* TAK_MINIMAP_H */
