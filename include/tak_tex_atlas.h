#ifndef TAK_TEX_ATLAS_H
#define TAK_TEX_ATLAS_H

#include "tak_platform.h"
#include "tak_gpu.h"
#include <SDL.h>

/* ── Texture atlas manager (Phase C M3) ─────────────────────────────
 *
 * Loads every GAF under `textures/` once at world-load time. Decodes each
 * GAF entry's frame 0 into palette indices, packs the entries from one
 * GAF into a single GPU_Texture (one atlas per GAF, a copy of the art
 * per shade band, see TexShadeMap), and registers the
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

/* Parse every GAF under textures/, build atlases, register names. The
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

/* Team colour: 10 or more pictures in a GAF named *logo*, where all 126 the
 * data ships live. Any other entry of more than one picture plays by time. */
int TexAtlas_IsTeamColour(const char *gaf_path, int pictures);

/* A played texture's pictures lie side by side, GetByName's rect picture 0.
 * Animate picks one at `frame` in 30 Hz frames, AnimU its u shift. */
void  TexAtlas_Animate(uint32_t frame);
int   TexAtlas_AnimOf(const char *texture_name);
float TexAtlas_AnimU(int anim);

/* Free every atlas + the lookup table. NULL-safe.
 * Must run before TAK_Platform_Shutdown — atlases are
 * SDL_Texture-backed. */
void TexAtlas_Shutdown(TAK_Platform *plat);

/* Faces are lit through the side's .shd ramp: 32 rows of 256 indices, row 15
 * close to the plain palette, darker below, up to about 1.74x above. */
#define TEX_SHADE_LEVELS   32
#define TEX_SHADE_IDENTITY 15

/* The model light, fitted to captures of the original's buildings, not
 * read from legacy:197710-197791. */
#define MODEL_LIGHT_X        -0.472f /* toward the light: x east, y up, z south */
#define MODEL_LIGHT_Y         0.788f
#define MODEL_LIGHT_Z         0.396f
#define MODEL_LIGHT_SCALE     16     /* shade levels per unit of cosine */
#define MODEL_LIGHT_AMBIENT   5      /* level of a face edge on to the light */
/* The part of the light a mobile unit takes: its shading is painted in,
 * so 0 draws it flat at level 15 and 1 lights it as a building. */
#define UNIT_LIGHT_FACTOR     0.15f
/* A stone or ice statue's own scale and ambient, fitted to shots of the
 * original's statues, which reach brighter rows. */
#define STATUE_LIGHT_SCALE    18
#define STATUE_LIGHT_AMBIENT  5

/* An atlas can stack a copy of its art per .shd row from 15 to the brightest
 * level a statue reaches, so a lit face draws its exact row. It keeps only the
 * rows its meshes reach, and one copy when the GPU shades it. */
#define TEX_SHADE_TOP_BUILDING (MODEL_LIGHT_AMBIENT + MODEL_LIGHT_SCALE)
#define TEX_SHADE_TOP_STATUE   (STATUE_LIGHT_AMBIENT + STATUE_LIGHT_SCALE)
#define TEX_SHADE_TOP      (TEX_SHADE_TOP_STATUE > TEX_SHADE_TOP_BUILDING ? \
                            TEX_SHADE_TOP_STATUE : TEX_SHADE_TOP_BUILDING)
#define TEX_SHADE_BANDS    (TEX_SHADE_TOP - TEX_SHADE_IDENTITY + 1)

/* Per shade level, the stacked copy a face samples and the vertex grey that
 * takes its row to the level. Copies sit `band_dv` apart, `bands` of them
 * uploaded; a level past them takes the last. */
typedef struct TexShadeMap {
    float   band_dv;
    uint8_t bands;
    uint8_t band[TEX_SHADE_LEVELS];
    uint8_t grey[TEX_SHADE_LEVELS];
} TexShadeMap;

/* Fill `out` from a palette and its .shd table (NULL: every level plain)
 * for an atlas of `bands` copies, rows 15 up, `band_dv` apart. */
void TexAtlas_BuildShadeMap(const uint32_t *rgba, const uint8_t *shd,
                            int bands, float band_dv, TexShadeMap *out);

/* The shade map of an atlas TexAtlas_GetByName returned. Returns -1
 * and an identity map for any other texture. */
int TexAtlas_ShadeMap(const GPU_Texture *atlas, TexShadeMap *out);

/* GPU shading (the GL classic path): one index copy of an atlas and its
 * palette's ramp. 1 with both, 0 when the atlas is drawn from its copies. */
struct GL3D_Texture;
int TexAtlas_GpuShade(const GPU_Texture *atlas, const struct GL3D_Texture **index,
                      const struct GL3D_Texture **ramp);

/* The band path keeps only the copies an atlas's meshes reach: note the
 * brightest level each texture can take, then plan once, before any atlas
 * goes to the GPU. 1 while a plan is still wanted. */
int  TexAtlas_ShadePlanWanted(void);
/* The band path's v offset for a face at `level`: its copy, or the last uploaded. */
float TexAtlas_ShadeBandV(const TexShadeMap *map, int level);
void TexAtlas_NoteShadeTop(const char *texture_name, int level);
void TexAtlas_PlanShades(void);

/* A data/palettes/ palette through every .shd row, `level * 256 + index`,
 * loaded on first use. NULL when it cannot be read. */
const uint32_t *TexAtlas_ShadeRamp(const char *pcx_name);

/* Diagnostics. */
int  TexAtlas_GetEntryCount(void);   /* total registered names */
int  TexAtlas_GetAtlasCount(void);   /* number of GPU textures created */

#endif /* TAK_TEX_ATLAS_H */
