#ifndef TAK_TEX_ATLAS_H
#define TAK_TEX_ATLAS_H

#include "tak_platform.h"
#include "tak_gpu.h"
#include <SDL.h>

/* ── Texture atlas manager (Phase C M3) ─────────────────────────────
 *
 * Loads every `textures/*.gaf` once at world-load time. Decodes each
 * GAF entry's frame 0 into RGBA, packs the entries from one GAF into
 * a single GPU_Texture (one atlas per GAF), and registers the
 * decoded entries in a global hashtable keyed by lowercased
 * texture-name.
 *
 * Why per-GAF atlas instead of one giant atlas: see PHASE_C_3DO.md
 * §3.6. Short version — texture names don't partition cleanly by
 * faction (R4 finding), and per-GAF gives us 88 small atlases that
 * each pack trivially without a heavyweight rect-packer.
 *
 * Lookup is by texture name. Same name in multiple GAFs (333 known
 * collisions across shipped data) resolves to first-load-wins, which
 * is alphabetical filename order and matches the original engine.
 *
 * Missing texture (~350 names referenced by 3DOs but absent from any
 * GAF): GetByName returns NULL. Caller falls back to flat-colour
 * mode using the primitive's color_idx. */

/* Parse every textures/*.gaf, build atlases, register names. The
 * faction palette to decode 8-bit GAF pixels comes from the
 * loaded GameWorld's terrain_rgba (so call this AFTER LS_LOAD_PALETTE).
 *
 * Returns 0 on success, -1 if no texture GAFs found or all failed.
 * Partial failure (some GAFs malformed) is non-fatal — we log and
 * continue, so a single broken GAF doesn't break the whole load. */
int  TexAtlas_LoadAll(TAK_Platform *plat);

/* Look up a texture by name (case-insensitive) for a given player.
 *
 * `color_idx` is the requesting player's team-color slot (0..11).
 * Atlas variants are lazy-built per color: the first lookup with a
 * given color_idx triggers a CPU-side atlas-mutation pass that swaps
 * pixels at side-palette indices 0x10..0x1F to that player's 16-shade
 * ramp, then uploads as a new GPU texture. Subsequent lookups with
 * the same color_idx are pointer-fetches.
 *
 * Returns the player-specific atlas GPU_Texture and writes the entry's
 * UV rect (atlas-relative 0..1 floats) to *out_uv. NULL on miss.
 *
 * Atlases that contain NO team-color pixels share a single GPU texture
 * across all colors (the variant build short-circuits). */
GPU_Texture *TexAtlas_GetByName(const char *texture_name, int color_idx,
                                 SDL_FRect *out_uv);

/* Free every atlas + the lookup table. NULL-safe.
 * Must run before TAK_Platform_Shutdown — atlases are
 * SDL_Texture-backed. */
void TexAtlas_Shutdown(TAK_Platform *plat);

/* Light-level control for the .shd shade-table application. The shade
 * table maps each (level, pixel-index) → effective palette index,
 * letting one base palette produce many light tones.
 * Valid range: 0..31. Default: 15 (per decomp). */
int  TexAtlas_GetLightLevel(void);
void TexAtlas_SetLightLevel(int level);

/* Drop all atlases and re-build with the current light level. Use to
 * apply a light-level change without restarting the skirmish. */
int  TexAtlas_Reload(TAK_Platform *plat);

/* Diagnostics. */
int  TexAtlas_GetEntryCount(void);   /* total registered names */
int  TexAtlas_GetAtlasCount(void);   /* number of GPU textures created */

#endif /* TAK_TEX_ATLAS_H */
