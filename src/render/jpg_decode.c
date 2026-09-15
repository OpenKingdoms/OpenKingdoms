#include "tak_jpg.h"
#include "tak_memory.h"

int JPG_UsesFFmpeg(void) { return 0; }

/* stb_image, on every platform. FFmpeg used to decode these when it
 * was present, but a release carries an FFmpeg with the Bink codec
 * alone, and a decoder that depends on which FFmpeg it was linked
 * against is one that works on the build machine and nowhere else. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"
#include <string.h>

int JPG_DecodeRGBA(const uint8_t *jpg, size_t size,
                    uint32_t **out_px, int *out_w, int *out_h) {
    if (out_px) *out_px = NULL;
    if (out_w)  *out_w  = 0;
    if (out_h)  *out_h  = 0;
    if (!jpg || size == 0 || !out_px || !out_w || !out_h) return -1;

    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = stbi_load_from_memory(jpg, (int)size,
                                                &w, &h, &comp, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        return -1;
    }
    uint32_t *px = (uint32_t *)tak_malloc((size_t)w * h * 4);
    if (!px) {
        stbi_image_free(rgba);
        return -1;
    }
    memcpy(px, rgba, (size_t)w * h * 4);
    stbi_image_free(rgba);
    *out_px = px;
    *out_w = w;
    *out_h = h;
    return 0;
}
