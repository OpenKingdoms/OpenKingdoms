/*
 * blit.c -- Sprite blitting to SDL surfaces
 *
 * Copies RGBA pixel buffers onto an SDL_PIXELFORMAT_RGBA32 offscreen
 * surface with transparency. Pixels with alpha=0 are skipped.
 */

#include "tak_blit.h"
#include <string.h>

void Blit_RGBA(SDL_Surface *dst, int dst_x, int dst_y, const uint32_t *pixels, int src_w, int src_h) {
    if (!dst || !pixels || src_w <= 0 || src_h <= 0) return;

    /* Compute clipping bounds */
    int start_x = 0, start_y = 0;
    int end_x = src_w, end_y = src_h;

    if (dst_x < 0) { start_x = -dst_x; }
    if (dst_y < 0) { start_y = -dst_y; }
    if (dst_x + end_x > dst->w) { end_x = dst->w - dst_x; }
    if (dst_y + end_y > dst->h) { end_y = dst->h - dst_y; }

    if (start_x >= end_x || start_y >= end_y) return;

    SDL_LockSurface(dst);

    int bpp = dst->format->BytesPerPixel;
    uint32_t amask = dst->format->Amask;

    for (int y = start_y; y < end_y; y++) {
        const uint32_t *src_row = pixels + y * src_w + start_x;
        uint32_t *dst_row = (uint32_t *)((uint8_t *)dst->pixels +
                            (dst_y + y) * dst->pitch +
                            (dst_x + start_x) * bpp);

        int count = end_x - start_x;
        if (amask) {
            /* Surface has alpha channel -- check alpha via mask */
            for (int x = 0; x < count; x++) {
                uint32_t px = src_row[x];
                if ((px & amask) == 0) continue; /* fully transparent */
                dst_row[x] = px;
            }
        } else {
            /* No alpha channel -- just copy all non-zero pixels */
            for (int x = 0; x < count; x++) {
                uint32_t px = src_row[x];
                if (px == 0) continue;
                dst_row[x] = px;
            }
        }
    }

    SDL_UnlockSurface(dst);
}

void Blit_RGBA_Opaque(SDL_Surface *dst, int dst_x, int dst_y, const uint32_t *pixels, int src_w, int src_h) {
    if (!dst || !pixels || src_w <= 0 || src_h <= 0) return;

    /* Compute clipping bounds */
    int start_x = 0, start_y = 0;
    int end_x = src_w, end_y = src_h;

    if (dst_x < 0) { start_x = -dst_x; }
    if (dst_y < 0) { start_y = -dst_y; }
    if (dst_x + end_x > dst->w) { end_x = dst->w - dst_x; }
    if (dst_y + end_y > dst->h) { end_y = dst->h - dst_y; }

    if (start_x >= end_x || start_y >= end_y) return;

    SDL_LockSurface(dst);

    int bpp = dst->format->BytesPerPixel;

    for (int y = start_y; y < end_y; y++) {
        const uint32_t *src_row = pixels + y * src_w + start_x;
        uint32_t *dst_row = (uint32_t *)((uint8_t *)dst->pixels +
                            (dst_y + y) * dst->pitch +
                            (dst_x + start_x) * bpp);

        int count = end_x - start_x;
        /* Opaque = copy every pixel regardless of value. A terrain tile
         * with intentional black pixels (shadows, cave mouths) would be
         * holed out by an alpha-zero-skip path. memcpy also bulk-copies
         * without per-pixel branching — hotter path than a loop. */
        memcpy(dst_row, src_row, (size_t)count * sizeof(uint32_t));
    }

    SDL_UnlockSurface(dst);
}


