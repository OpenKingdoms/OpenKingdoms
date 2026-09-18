/*
 * png_decode.c -- PNG to RGBA, for the textures inside a glTF model.
 *
 * Its own translation unit with its own private copy of stb_image
 * built for PNG alone. The JPEG decoder beside it is built for JPEG
 * alone, so the two share no symbols and neither can change what the
 * other already decodes.
 */
#include "tak_png.h"
#include "tak_memory.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"
#include <string.h>

int PNG_DecodeRGBA(const uint8_t *png, size_t size,
                   uint32_t **out_px, int *out_w, int *out_h) {
    if (out_px) *out_px = NULL;
    if (out_w)  *out_w  = 0;
    if (out_h)  *out_h  = 0;
    if (!png || size == 0 || size > (size_t)0x7fffffff ||
        !out_px || !out_w || !out_h) return -1;

    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = stbi_load_from_memory(png, (int)size, &w, &h, &comp, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        return -1;
    }
    uint32_t *px = (uint32_t *)tak_malloc((size_t)w * (size_t)h * 4);
    if (!px) {
        stbi_image_free(rgba);
        return -1;
    }
    memcpy(px, rgba, (size_t)w * (size_t)h * 4);
    stbi_image_free(rgba);
    *out_px = px;
    *out_w = w;
    *out_h = h;
    return 0;
}
