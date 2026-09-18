/*
 * tak_png.h -- PNG to a heap RGBA32 buffer.
 */
#ifndef TAK_PNG_H
#define TAK_PNG_H

#include <stdint.h>
#include <stddef.h>

/* 0 on success, and the caller owns *out_px. */
int PNG_DecodeRGBA(const uint8_t *png, size_t size,
                   uint32_t **out_px, int *out_w, int *out_h);

#endif /* TAK_PNG_H */
