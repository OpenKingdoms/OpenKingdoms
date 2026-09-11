#ifndef TAK_SHA256_H
#define TAK_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* SHA-256 (FIPS 180-4). Integer only and self contained, so the same
 * bytes hash the same on every target the engine builds for, the
 * browser included. */

#define TAK_SHA256_BYTES 32

typedef struct TAK_Sha256 {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t  buffer[64];
    size_t   buffered;
} TAK_Sha256;

void TAK_Sha256_Init(TAK_Sha256 *ctx);
void TAK_Sha256_Update(TAK_Sha256 *ctx, const void *data, size_t len);
void TAK_Sha256_Final(TAK_Sha256 *ctx, uint8_t out[TAK_SHA256_BYTES]);

/* One-shot helper. */
void TAK_Sha256_Hash(const void *data, size_t len, uint8_t out[TAK_SHA256_BYTES]);

/* 64 lower-case hex digits plus a terminator. */
void TAK_Sha256_ToHex(const uint8_t digest[TAK_SHA256_BYTES], char out[65]);

#endif /* TAK_SHA256_H */
