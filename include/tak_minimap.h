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

/* The same picture, drawn on the CPU into `out_rgb` at `tw` by `th`,
 * three bytes per pixel. No window and no renderer, because a save is
 * written from wherever the player pressed the button and the GPU
 * surface behind the dialog is not it.
 *
 * Terrain, then fog as the viewer has it, then one dot per unit the
 * viewer can see: the three the sidebar radar composites. Returns 0,
 * or -1 when there is no world or no overview image to draw from, in
 * which case `out_rgb` is untouched. */
int Minimap_RenderThumbnail(uint8_t *out_rgb, int tw, int th);

/* The map point under a window point on the minimap, in world pixels.
 * With clamp the pointer is first held to the map's rect, so a look
 * dragged past the edge keeps tracking. Returns 0 when the minimap is
 * not up, or when the point is off it and clamp is 0. */
int Minimap_PointToWorld(TAK_Platform *plat, int win_x, int win_y, int clamp,
                         int32_t *out_x, int32_t *out_y);

/* Debug: the window-pixel rect a unit dot at this world position would
 * occupy, clipped to the drawn map. Returns 0 when the minimap is not
 * up or the position falls outside it. */
int Minimap_DebugDotRect(TAK_Platform *plat, int32_t world_x, int32_t world_y,
                         SDL_Rect *out);

/* Debug: the window-pixel rect the map image is drawn into, and how
 * many draws the fog over it has taken since the process began. */
int      Minimap_DebugMapRect(TAK_Platform *plat, SDL_Rect *out);
uint32_t Minimap_DebugFogDraws(void);

/* Free the GPU texture. Called from World_End — the minimap's lifetime
 * matches the world's, not any single in-game session. Safe to call
 * multiple times or without a preceding Init (no-op). */
void Minimap_Shutdown(TAK_Platform *plat);

#endif /* TAK_MINIMAP_H */
