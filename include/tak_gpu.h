#ifndef TAK_GPU_H
#define TAK_GPU_H

#include "tak_platform.h"
#include "tak_types.h"


// GPU Texture opaque type.. so calling code doesn't have to worry about 
// impl details. Under the hood this is usually an SDL texture
typedef struct GPU_Texture GPU_Texture;

// Given an RGBA pixel buffer - send it to the GPU
// Caller must free this texture via GPU_FreeTexture
GPU_Texture *GPU_UploadRGBA(TAK_Platform *plat, const uint32_t *pixels, int w, int h);

void GPU_FreeTexture(TAK_Platform *plat, GPU_Texture *tex);

/* Introspection. Returns 0 on success, -1 on null tex. */
int GPU_TextureSize(const GPU_Texture *tex, int *out_w, int *out_h);

/* Stamp a texture into the window render target. src == NULL means
 * "entire texture"; dst == NULL means "stretch to fill the window".
 * Must be called after TAK_Platform_FrameBegin and before
 * TAK_Platform_Present on the same frame. Coordinates in dst are
 * window pixels, not canvas pixels. */
void GPU_DrawToWindow(TAK_Platform *plat, const GPU_Texture *tex, const SDL_Rect *src, const SDL_Rect *dst);

/* Pick the sampling filter used when the texture is drawn at a size
 * different from its native resolution. Default after upload is
 * nearest-neighbour (blocky at non-integer scales); pass linear=1 for
 * bilinear, which is what you want for content that's typically
 * scaled down (minimap, icons). */
void GPU_SetTextureFilter(GPU_Texture *tex, int linear);

/* Enable or disable alpha blending when the texture is drawn. Default
 * after upload is SDL_BLENDMODE_NONE — transparent pixels get written
 * as opaque black. Pass blend=1 for SDL_BLENDMODE_BLEND so alpha=0
 * pixels become see-through. Required for any atlas that has
 * colorkey-transparent pixels (unit textures, sprite GAFs). */
void GPU_SetTextureBlend(GPU_Texture *tex, int blend);

/* Triangle-list geometry submission (Phase C M4+). Wraps
 * SDL_RenderGeometryRaw so callers don't need to peek at GPU_Texture's
 * SDL guts. Pass NULL for `tex` to draw flat-shaded triangles using
 * only the per-vertex color (used by both the M4 white-ghost path
 * and the M5 flat-color batch).
 *
 * Strides match the SoA layout in units.c:
 *   xy:    pairs of float, screen-space pixels
 *   color: u32 packed RGBA (memory-compatible with SDL_Color)
 *   uv:    pairs of float, atlas-space [0..1]
 *   indices: uint16 triplet per triangle
 *
 * Returns 0 on success, -1 on invalid args, or the SDL error otherwise. */
int  GPU_DrawGeometryRaw(TAK_Platform *plat, const GPU_Texture *tex,
                         const float *xy, const uint32_t *color,
                         const float *uv, int num_vertices,
                         const uint16_t *indices, int num_indices);

#endif /* TAK_GPU_H */