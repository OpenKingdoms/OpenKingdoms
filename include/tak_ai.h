#ifndef TAK_AI_H
#define TAK_AI_H

#include <stdint.h>

typedef struct GameWorld GameWorld;

int TAK_AI_ClampDifficulty(int difficulty);
int TAK_AI_PursuitRadius(int sight_distance, int weapon_range, int difficulty);
void TAK_AI_TickSkirmish(GameWorld *world);
/* Force profile re-read on next tick (tests / map change). */
void TAK_AI_ResetProfile(void);

/* units.c reports an enemy hit; the AI answers it on its next tick. */
void TAK_AI_NotifyDamage(int victim_handle, int shooter_handle);

/* Test observability. Orders issued by an AI player since the match
 * began: attack orders, plus marches unless attacks_only; the count
 * of base-defence orders; the enemy its waves currently go for. */
int  TAK_AI_DebugHostileOrders(int from_player, int to_player,
                               int attacks_only);
int  TAK_AI_DebugDefenceOrders(int player_id);
int  TAK_AI_DebugAttackPlayer(int player_id);
int  TAK_AI_DebugWaveTarget(int player_id);   /* unit handle, -1 none */

/* A unit slot about to take a new unit: drop any wave target or threat
 * that still names it. Units_Spawn calls this. */
void TAK_AI_ForgetUnit(int handle);

/* Start a match: clear what the last one left and derive the AI's own
 * stream from the session seed. Seed 0 gives the stream every match
 * used before there was a seed. World_BeginLoad calls this. */
void TAK_AI_BeginMatch(uint32_t seed);

/* The old name for the AI's share of the simulation hash, seeded the
 * way a caller with nothing to carry seeds it. The movement tests read
 * it and docs/MULTIPLAYER.md names it. The companion to
 * Units_DebugStateHash. */
unsigned int TAK_AI_DebugStateHash(void);

/* ── The AI in a save ────────────────────────────────────────────
 *
 * Every field TAK_SimHash_AI covers lives as a file static in
 * src/game/ai.c, so the save asks this module for its own bytes
 * rather than reaching across. The bytes go out at explicit widths in
 * a fixed order, so a save written by one build opens on another.
 *
 * The influence maps are not here. They are rebuilt on the AI's next
 * think and are not in the hash. */

/* How many bytes TAK_AI_SaveState writes. Constant for a build. */
unsigned int TAK_AI_StateBytes(void);

/* Serialise into `out`, which must hold TAK_AI_StateBytes(). */
void TAK_AI_SaveState(unsigned char *out);

/* Read back. Returns 0, or -1 when `len` is short. A payload longer
 * than this build writes is read to its prefix and the rest stepped
 * over, which is what lets a field be appended without a schema bump.
 * The AI generator comes back with it: the seed is the same every
 * match, so reseeding would put a loaded battle back at the start of
 * the sequence. */
int TAK_AI_LoadState(const unsigned char *in, unsigned int len);

#endif /* TAK_AI_H */
