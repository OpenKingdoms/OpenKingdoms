#ifndef TAK_JPG_H
#define TAK_JPG_H

#include "tak_types.h"

/* Decode a baseline JPEG to a heap-allocated RGBA32 buffer.
 *   jpg_data / jpg_size: the raw JPEG bytes (e.g. from VFS_ReadFile).
 *   out_pixels:          set to tak_malloc'd uint32_t * (caller frees).
 *   out_w, out_h:        set to decoded image dimensions.
 * Returns 0 on success, -1 on any failure (bad JPEG, alloc fail,
 * libav error). On failure *out_pixels is NULL. */
int JPG_DecodeRGBA(const uint8_t *jpg_data, size_t jpg_size, uint32_t **out_pixels, int *out_w, int *out_h);

#endif /* TAK_JPG_H */