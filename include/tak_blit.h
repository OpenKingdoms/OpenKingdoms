#ifndef TAK_BLIT_H
#define TAK_BLIT_H

#include <SDL.h>
#include <stdint.h>

/*
 * Blit an RGBA pixel buffer onto an SDL surface at position (dst_x, dst_y).
 * Pixels with alpha=0 are skipped (transparent).
 * Handles clipping when the sprite extends beyond the surface edges.
 */
void Blit_RGBA(SDL_Surface *dst, int dst_x, int dst_y, const uint32_t *pixels, int src_w, int src_h);

void Blit_RGBA_Opaque(SDL_Surface *dst, int dst_x, int dst_y, const uint32_t *pixels, int src_w, int src_h);

/* Nearest-neighbour blit of the whole buffer into dst_rect, alpha 0
 * skipped. The original hands every widget's paint its own rect and
 * stretches the art to it (legacy:319725-319750). */
void Blit_RGBA_Scaled(SDL_Surface *dst, SDL_Rect dst_rect, const uint32_t *pixels, int src_w, int src_h);

#endif /* TAK_BLIT_H */
