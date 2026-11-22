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
/* Drop the handle of a texture whose renderer is already gone, and
 * took the texture with it. */
void GPU_AbandonTexture(GPU_Texture *tex);

/* Introspection. Returns 0 on success, -1 on null tex. */
int GPU_TextureSize(const GPU_Texture *tex, int *out_w, int *out_h);

/* The SDL texture underneath, for a renderer that binds it through
 * SDL_GL_BindTexture. NULL for a null handle. */
SDL_Texture *GPU_TextureSDL(const GPU_Texture *tex);

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

/* NONE overwrites, ALPHA is src * a + dst * (1 - a), ADD is src * a + dst.
 * Every SDL renderer has all three, software and WebGL included. */
typedef enum GPU_Blend {
    GPU_BLEND_NONE = 0,
    GPU_BLEND_ALPHA,
    GPU_BLEND_ADD
} GPU_Blend;

/* The blend a texture draws with. Default after upload is NONE, so art
 * with transparent pixels (unit atlases, sprite GAFs) wants ALPHA. */
void GPU_SetTextureBlend(GPU_Texture *tex, GPU_Blend blend);

/* A constant colour and alpha multiplied into every texel drawn from
 * the texture. 255 everywhere draws it as it is. */
void GPU_SetTextureMod(GPU_Texture *tex, uint8_t r, uint8_t g, uint8_t b,
                       uint8_t a);

/* The blend untextured draws (lines, points, flat geometry) use from
 * now on. Returns the one it replaces, for the caller to put back. */
GPU_Blend GPU_SetDrawBlend(TAK_Platform *plat, GPU_Blend blend);

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