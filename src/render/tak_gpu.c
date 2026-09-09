#include "tak_gpu.h"
#include "tak_platform.h"
#include "tak_memory.h"
#include "SDL.h"


struct GPU_Texture {
    SDL_Texture *tex;
    int w;
    int h;
};


GPU_Texture *GPU_UploadRGBA(TAK_Platform *plat, const uint32_t *pixels, int w, int h) {
    if (!plat || !pixels) return NULL;

    GPU_Texture *gpu_tex_handle = (GPU_Texture*)tak_malloc(sizeof(GPU_Texture));
    if (!gpu_tex_handle) return NULL;

    gpu_tex_handle->w = w;
    gpu_tex_handle->h = h;
    gpu_tex_handle->tex = SDL_CreateTexture(plat->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);

    if (!gpu_tex_handle->tex) {
        tak_free(gpu_tex_handle);
        return NULL;
    }

    if (SDL_UpdateTexture(gpu_tex_handle->tex, NULL, pixels, w * 4) != 0) {
        fprintf(stderr, "TAK GPU: failed to update GPU texture: %s\n", SDL_GetError());
        GPU_FreeTexture(plat, gpu_tex_handle);
        return NULL;
    }

    return gpu_tex_handle;
}

void GPU_FreeTexture(TAK_Platform *plat, GPU_Texture *tex) {
    if (!tex || !tex->tex) return;
    if (!plat) return;

    SDL_DestroyTexture(tex->tex);
    tak_free(tex);
}

/* Introspection. Returns 0 on success, -1 on null tex. */
int GPU_TextureSize(const GPU_Texture *tex, int *out_w, int *out_h) {
    if (!tex || !tex->tex) return -1;

    *out_w = tex->w;
    *out_h = tex->h;

    return 0;
}


void GPU_DrawToWindow(TAK_Platform *plat, const GPU_Texture *tex, const SDL_Rect *src, const SDL_Rect *dst) {
    if (!plat || !plat->renderer || !tex || !tex->tex) return;
    SDL_RenderCopy(plat->renderer, tex->tex, src, dst);
}

void GPU_SetTextureFilter(GPU_Texture *tex, int linear) {
    if (!tex || !tex->tex) return;
    SDL_SetTextureScaleMode(tex->tex,
        linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
}

void GPU_SetTextureBlend(GPU_Texture *tex, int blend) {
    if (!tex || !tex->tex) return;
    SDL_SetTextureBlendMode(tex->tex,
        blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
}

int GPU_DrawGeometryRaw(TAK_Platform *plat, const GPU_Texture *tex,
                        const float *xy, const uint32_t *color,
                        const float *uv, int num_vertices,
                        const uint16_t *indices, int num_indices)
{
    if (!plat || !plat->renderer || !xy || !color || !uv || !indices) return -1;
    if (num_vertices <= 0 || num_indices <= 0) return -1;

    SDL_Texture *sdl_tex = (tex && tex->tex) ? tex->tex : NULL;
    int rc = SDL_RenderGeometryRaw(plat->renderer,
        sdl_tex,
        xy,                                  (int)(2 * sizeof(float)),
        (const SDL_Color *)color,            (int)sizeof(uint32_t),
        uv,                                  (int)(2 * sizeof(float)),
        num_vertices,
        indices, num_indices, (int)sizeof(uint16_t));
    if (rc != 0) {
        fprintf(stderr, "GPU_DrawGeometryRaw: %s\n", SDL_GetError());
    }
    return rc;
}
