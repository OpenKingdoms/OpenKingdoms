/*
 * sim_rand.c: the one simulation generator. It lives apart from the
 * world so data-free tests can pin its sequence.
 */

#include "tak_sim_rand.h"

/* Seeded by xor with 0x66e29572 and forced odd (legacy:254493).
 * Scripts draw from it too. */
static uint32_t g_sim_rand_state = 1;

void World_SeedRand(uint32_t seed) {
    g_sim_rand_state = (seed ^ 0x66e29572u) | 1u;
}

uint32_t World_Rand(uint32_t n) {
    if ((int32_t)n < 2) return 0;   /* signed, as legacy:254478 */
    g_sim_rand_state = TAK_SimRandStep(g_sim_rand_state);
    return g_sim_rand_state % n;
}

int32_t World_ScriptRand(void *user, int32_t n) {
    (void)user;
    return n > 1 ? (int32_t)World_Rand((uint32_t)n) : 0;
}

uint32_t World_RandState(void) {
    return g_sim_rand_state;
}
