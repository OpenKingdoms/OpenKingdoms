#ifndef TAK_SIM_HASH_H
#define TAK_SIM_HASH_H

#include <stdint.h>
#include <string.h>

/* One number for the whole simulation.
 *
 * Two uses, one function. A save reloads and compares this against the
 * value taken before the write, which catches a field the serialiser
 * dropped. Lockstep multiplayer compares it between peers, which
 * catches a divergence at the tick it happened rather than a minute
 * later when a unit visibly walks somewhere else.
 *
 * What it covers, and why each is not optional:
 *   - every unit slot in [0, count), dead ones included, because slots
 *     are never reused and every handle in the simulation is a slot
 *     index
 *   - the live prefix of each unit's path, not the dead tail
 *   - each unit's COB engine: every piece, the per instance static
 *     variables, and all sixteen thread slots including the dead ones,
 *     because a dead slot still answers a weapon's aim query
 *   - all three weapon states per unit
 *   - projectiles in flight
 *   - features and corpses, with their decompose counters
 *   - the per player fog layers, which are history and cannot be
 *     recomputed from the present
 *   - the economy, the AI and the simulation generator
 *
 * What it deliberately leaves out. Derived state that a load rebuilds:
 * the occupancy grid, the unit spatial grid, the influence maps, the
 * path plan cache. Local view state that two lockstep peers are
 * entitled to disagree about: the camera, the selection, the control
 * groups, the HUD. And world->occ_version, which a load bumps on
 * purpose to invalidate the clearance cache.
 *
 * A dead slot contributes only its lifecycle byte and its stable id.
 * The rest of a dead unit's record is whatever it held when it died,
 * the save writes dead slots as a tombstone carrying exactly those two
 * fields, and hashing the stale remainder would fail every correct
 * load after the first casualty.
 *
 * Mixing is FNV-1a over explicitly widened little endian bytes, so the
 * 32 bit Windows build, the wasm32 browser build and the 64 bit macOS
 * and Linux builds all produce the same number from the same state. */

#define TAK_SIM_HASH_SEED 2166136261u

static inline uint32_t TAK_HashBytes(uint32_t h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint32_t)b[i];
        h *= 16777619u;
    }
    return h;
}

static inline uint32_t TAK_HashU32(uint32_t h, uint32_t v) {
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xffu);
    b[1] = (unsigned char)((v >> 8) & 0xffu);
    b[2] = (unsigned char)((v >> 16) & 0xffu);
    b[3] = (unsigned char)((v >> 24) & 0xffu);
    return TAK_HashBytes(h, b, sizeof(b));
}

static inline uint32_t TAK_HashI32(uint32_t h, int32_t v) {
    return TAK_HashU32(h, (uint32_t)v);
}

/* Floats go in by bit pattern, moved with memcpy. The mover keeps its
 * float heading, speed and subpixel until the fixed point conversion
 * lands, and a bit pattern is the only form that compares equal for
 * the same value. */
static inline uint32_t TAK_HashF32(uint32_t h, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return TAK_HashU32(h, bits);
}

/* A string, length first so two different splits cannot collide. */
static inline uint32_t TAK_HashStr(uint32_t h, const char *s) {
    size_t n = s ? strlen(s) : 0;
    h = TAK_HashU32(h, (uint32_t)n);
    return n ? TAK_HashBytes(h, s, n) : h;
}

/* The whole simulation. Zero when no world is live. */
uint32_t TAK_SimHash(void);

/* The AI's own state, folded into the above. It lives in src/game/ai.c
 * because g_ai_players, the order matrices, g_ai_last_tick and the AI
 * generator are file statics there. Takes the running value and
 * returns it, so the caller controls the order. */
uint32_t TAK_SimHash_AI(uint32_t h);

#endif /* TAK_SIM_HASH_H */
