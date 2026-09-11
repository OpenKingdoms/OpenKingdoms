#ifndef TAK_SIM_RAND_H
#define TAK_SIM_RAND_H

#include <stdint.h>

/* The simulation generator (legacy:254475-254493). World_Rand(n) is
 * uniform over 0..n-1 and returns 0 when n, read as signed, is below
 * 2. Never call it from presentation code: the sequence has to match
 * on every peer. */
void     World_SeedRand(uint32_t seed);
uint32_t World_Rand(uint32_t n);

/* The RAND host every unit script draws through (legacy:306663-306673).
 * The signature is the COB engine's Cob_RandFn, and it lives here so
 * the engine and the VM tests register the same function. */
int32_t  World_ScriptRand(void *user, int32_t n);

/* One step in the original's own wrapping 32-bit arithmetic, which is
 * 16807 * g mod 2^31-1 (legacy:254481-254484). */
static inline uint32_t TAK_SimRandStep(uint32_t g) {
    g = g * 0x41a7u + (g / 0x1f31du) * 0x80000001u;
    if ((int32_t)g < 1) g += 0x7fffffffu;
    return g;
}

#endif /* TAK_SIM_RAND_H */
