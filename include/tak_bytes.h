#ifndef TAK_BYTES_H
#define TAK_BYTES_H

#include <stdint.h>
#include <string.h>

/* Little endian byte moves, shared by everything that has to put a
 * value on a wire or in a file.
 *
 * The rule these exist to enforce: no struct is ever handed to a write
 * call. The shipping Windows build is 32 bit and the browser build is
 * wasm32 while macOS and Linux are 64 bit, so the same struct has two
 * different sizes and two different padding layouts. Every field goes
 * out at an explicit width through one of these.
 *
 * Floats travel as their IEEE-754 binary32 bit pattern, moved with
 * memcpy. A pointer cast breaks strict aliasing and a union is not
 * guaranteed to move the representation rather than a value the
 * compiler kept in a wider register. Every target the engine builds
 * for is binary32 little endian and no CMake file sets a fast math
 * flag, so this round trips bit for bit. */

static inline void tak_put_u8(uint8_t *p, uint8_t v) { p[0] = v; }

static inline void tak_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static inline void tak_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static inline void tak_put_u64(uint8_t *p, uint64_t v) {
    tak_put_u32(p, (uint32_t)(v & 0xffffffffu));
    tak_put_u32(p + 4, (uint32_t)((v >> 32) & 0xffffffffu));
}

static inline void tak_put_i16(uint8_t *p, int16_t v) { tak_put_u16(p, (uint16_t)v); }
static inline void tak_put_i32(uint8_t *p, int32_t v) { tak_put_u32(p, (uint32_t)v); }
static inline void tak_put_i64(uint8_t *p, int64_t v) { tak_put_u64(p, (uint64_t)v); }

static inline void tak_put_f32(uint8_t *p, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    tak_put_u32(p, bits);
}

static inline uint8_t tak_get_u8(const uint8_t *p) { return p[0]; }

static inline uint16_t tak_get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t tak_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t tak_get_u64(const uint8_t *p) {
    return (uint64_t)tak_get_u32(p) | ((uint64_t)tak_get_u32(p + 4) << 32);
}

static inline int16_t tak_get_i16(const uint8_t *p) { return (int16_t)tak_get_u16(p); }
static inline int32_t tak_get_i32(const uint8_t *p) { return (int32_t)tak_get_u32(p); }
static inline int64_t tak_get_i64(const uint8_t *p) { return (int64_t)tak_get_u64(p); }

static inline float tak_get_f32(const uint8_t *p) {
    uint32_t bits = tak_get_u32(p);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

#endif /* TAK_BYTES_H */
