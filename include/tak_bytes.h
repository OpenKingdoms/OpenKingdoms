#ifndef TAK_BYTES_H
#define TAK_BYTES_H

#include <stddef.h>
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

/*
 * Bounded cursors over the byte moves above. A reader that runs off the
 * end sets a sticky overrun flag and every later read returns zero
 * without touching memory, so a parser can decode a whole message and
 * check once at the end instead of testing after every field. A writer
 * that runs out of room behaves the same way and writes nothing past
 * its buffer. No allocation, which is what an internet facing parser
 * needs and what the fuzz target leans on.
 */

/* ── Bounded writer ─────────────────────────────────────────────────── */

typedef struct TAK_ByteWriter {
    uint8_t *data;
    size_t   cap;
    size_t   len;
    int      overflow;   /* sticky */
} TAK_ByteWriter;

static inline void TAK_BW_Init(TAK_ByteWriter *w, void *buf, size_t cap) {
    w->data = (uint8_t *)buf;
    w->cap = buf ? cap : 0;
    w->len = 0;
    w->overflow = 0;
}

/* Reserve n bytes and return where they start, or NULL when they do not
 * fit. Sets overflow in that case. */
static inline uint8_t *TAK_BW_Claim(TAK_ByteWriter *w, size_t n) {
    if (w->overflow || n > w->cap - w->len) { w->overflow = 1; return NULL; }
    uint8_t *at = w->data + w->len;
    w->len += n;
    return at;
}

static inline void TAK_BW_U8(TAK_ByteWriter *w, uint8_t v) {
    uint8_t *p = TAK_BW_Claim(w, 1); if (p) tak_put_u8(p, v);
}
static inline void TAK_BW_U16(TAK_ByteWriter *w, uint16_t v) {
    uint8_t *p = TAK_BW_Claim(w, 2); if (p) tak_put_u16(p, v);
}
static inline void TAK_BW_U32(TAK_ByteWriter *w, uint32_t v) {
    uint8_t *p = TAK_BW_Claim(w, 4); if (p) tak_put_u32(p, v);
}
static inline void TAK_BW_U64(TAK_ByteWriter *w, uint64_t v) {
    uint8_t *p = TAK_BW_Claim(w, 8); if (p) tak_put_u64(p, v);
}
static inline void TAK_BW_I32(TAK_ByteWriter *w, int32_t v) {
    TAK_BW_U32(w, (uint32_t)v);
}

static inline void TAK_BW_Bytes(TAK_ByteWriter *w, const void *src, size_t n) {
    uint8_t *p = TAK_BW_Claim(w, n);
    if (p && n) memcpy(p, src, n);
}

/* A fixed width text field, NUL padded. Never reads past the field and
 * never writes an unterminated one. */
static inline void TAK_BW_Str(TAK_ByteWriter *w, const char *s, size_t field) {
    uint8_t *p = TAK_BW_Claim(w, field);
    if (!p) return;
    memset(p, 0, field);
    if (!s || field == 0) return;
    size_t n = strlen(s);
    if (n > field - 1) n = field - 1;
    memcpy(p, s, n);
}

static inline int TAK_BW_Ok(const TAK_ByteWriter *w) { return !w->overflow; }
static inline size_t TAK_BW_Len(const TAK_ByteWriter *w) { return w->len; }

/* ── Bounded reader ─────────────────────────────────────────────────── */

typedef struct TAK_ByteReader {
    const uint8_t *data;
    size_t         cap;
    size_t         pos;
    int            overrun;   /* sticky */
} TAK_ByteReader;

static inline void TAK_BR_Init(TAK_ByteReader *r, const void *buf, size_t cap) {
    r->data = (const uint8_t *)buf;
    r->cap = buf ? cap : 0;
    r->pos = 0;
    r->overrun = 0;
}

/* n bytes at the cursor, or NULL past the end. */
static inline const uint8_t *TAK_BR_Take(TAK_ByteReader *r, size_t n) {
    if (r->overrun || n > r->cap - r->pos) { r->overrun = 1; return NULL; }
    const uint8_t *at = r->data + r->pos;
    r->pos += n;
    return at;
}

static inline uint8_t TAK_BR_U8(TAK_ByteReader *r) {
    const uint8_t *p = TAK_BR_Take(r, 1); return p ? tak_get_u8(p) : 0;
}
static inline uint16_t TAK_BR_U16(TAK_ByteReader *r) {
    const uint8_t *p = TAK_BR_Take(r, 2); return p ? tak_get_u16(p) : 0;
}
static inline uint32_t TAK_BR_U32(TAK_ByteReader *r) {
    const uint8_t *p = TAK_BR_Take(r, 4); return p ? tak_get_u32(p) : 0;
}
static inline uint64_t TAK_BR_U64(TAK_ByteReader *r) {
    const uint8_t *p = TAK_BR_Take(r, 8); return p ? tak_get_u64(p) : 0;
}
static inline int32_t TAK_BR_I32(TAK_ByteReader *r) {
    return (int32_t)TAK_BR_U32(r);
}

static inline void TAK_BR_Bytes(TAK_ByteReader *r, void *dst, size_t n) {
    const uint8_t *p = TAK_BR_Take(r, n);
    if (p) { if (n) memcpy(dst, p, n); }
    else if (n) memset(dst, 0, n);
}

/* A fixed width text field. `out` holds `field` bytes and always comes back
 * NUL terminated, whatever the sender wrote. */
static inline void TAK_BR_Str(TAK_ByteReader *r, char *out, size_t field) {
    if (field == 0) return;
    TAK_BR_Bytes(r, out, field);
    out[field - 1] = '\0';
}

static inline size_t TAK_BR_Remaining(const TAK_ByteReader *r) {
    return r->overrun ? 0 : r->cap - r->pos;
}

static inline int TAK_BR_Ok(const TAK_ByteReader *r) { return !r->overrun; }

/* True when the message was read exactly, with nothing left over. A
 * trailing byte means the sender and this parser disagree, so refuse it. */
static inline int TAK_BR_Done(const TAK_ByteReader *r) {
    return !r->overrun && r->pos == r->cap;
}

#endif /* TAK_BYTES_H */
