#ifndef TAK_UI_H
#define TAK_UI_H

#include "tak_types.h"
#include "tak_gaf.h"
#include "tak_platform.h"
#include <SDL.h>

/*
 * UI helpers shared across screens (main menu, battle setup, in-game HUD, etc).
 *
 *   UI_Init / UI_Shutdown — create / destroy a single 640×480 RGBA32 offscreen
 *   surface that every screen composites into. main.c calls these once.
 *
 *   UI_Offscreen / UI_RGBAFormat / UI_Present — cheap accessors + the per-frame
 *   "blit offscreen to the window" call.
 *
 *   UI_LoadGAFWithPalette / UI_DecodeFrame / UI_DecodeEntryIdle — the GAF + PCX
 *   combo every screen needs. Lifted from main_menu.c so new screens don't
 *   copy-paste.
 */

/* ── Offscreen surface lifecycle ─────────────────────────────────────── */

/* Create the 640x480 RGBA32 compositing surface. Call once at program
 * start, before any screen. Returns 0 on success. */
int UI_Init(void);

/* Free the compositing surface. Call once at program exit. */
void UI_Shutdown(void);

/* The offscreen compositing surface. Screens blit sprites onto this
 * and then call UI_Present to push it to the window. */
SDL_Surface *UI_Offscreen(void);

/* Pixel format of the offscreen surface (same as UI_Offscreen()->format).
 * Pass to Palette_BuildRGBATable so palette entries match the surface. */
SDL_PixelFormat *UI_RGBAFormat(void);

/* Copy the offscreen surface onto the window's back buffer. Call once
 * per frame, after all drawing for that frame is done. */
void UI_Present(TAK_Platform *platform);

/* ── GAF + palette helpers ───────────────────────────────────────────── */

/* Load a GAF and its matching PCX palette, build an RGBA lookup table
 * using the offscreen surface's pixel format (so transparency works).
 *   gaf_path — e.g. "data/anims/mainscreen.gaf"
 *   pcx_path — e.g. "data/anims/mainscreen.pcx"
 *   out_gaf  — receives the GAFFile handle (caller owns, GAF_Close later)
 *   rgba_table — caller-provided uint32_t[256]
 * Returns 0 on success; on palette failure a greyscale ramp is used so
 * rendering degrades instead of crashing. */
int UI_LoadGAFWithPalette(const char *gaf_path, const char *pcx_path,
                          GAFFile **out_gaf, uint32_t *rgba_table);

/* Decode a specific frame by (entry_offset, frame_index).
 *   entry_offset — the absolute byte offset returned by GAF_FindSequence
 *   frame_index  — which frame inside that entry (0-based)
 *   out_w/out_h  — frame dimensions (written on success)
 * Returns a newly allocated RGBA buffer (caller frees with tak_free),
 * or NULL on failure. */
uint32_t *UI_DecodeFrame(GAFFile *gaf, int entry_offset, int frame_index,
                         const uint32_t *rgba_table,
                         int *out_w, int *out_h);

/* Decode frame 0 of a specific entry chosen by index in the entry
 * pointer table (i.e. the Nth animation in the GAF, zero-based).
 * Also reports the hotspot (offset_x, offset_y) since many callers
 * need it for placement. */
uint32_t *UI_DecodeEntryIdle(GAFFile *gaf, int entry_idx,
                             const uint32_t *rgba_table,
                             int *out_w, int *out_h,
                             int *out_ox, int *out_oy);

#endif /* TAK_UI_H */
