#ifndef TAK_MAP_SCENARIO_H
#define TAK_MAP_SCENARIO_H

#include <stdint.h>

/* ── A scripted multiplayer map ────────────────────────────────────
 *
 * A map whose .ota says hasscenario=1 carries its script in the .crt
 * beside it: units each start position begins with, and per player
 * rules of conditions and actions that the original checks once a
 * game second (legacy:172546-173338). It runs only in a match with the
 * room's Use Map Script on (legacy:177664-177693), and then it, not the
 * usual rules, decides who wins and who loses (legacy:206645).
 *
 * Ours runs every player's rules on every machine in seat order, so it
 * is part of the lockstep simulation: in the hash and in the save. The
 * original ran each machine's own players and let the network carry
 * the results. */

/* 1 when the map's header says it has a script (legacy:169012). */
int  MapScenario_MapHasScript(const char *map_key);

/* Load the map's script for this battle. 1 when the map has one to run,
 * 0 when it has none, -1 when its .crt does not read. */
int  MapScenario_Begin(const char *map_key);
void MapScenario_End(void);
/* 1 between a Begin that found a script and End. */
int  MapScenario_Active(void);
/* A new battle: each seat's start units, then the first look at the
 * rules with "game start" true (legacy:173086-173338). Not for a
 * battle out of a save, whose units and state the save brings. */
void MapScenario_Start(void);
/* One simulation tick. The rules are read once a game second. */
void MapScenario_Tick(void);

/* The units hooks: a kill credited to `killer`, and a unit's record
 * ending, death or removal (legacy:173375-173486). */
void MapScenario_NoteKill(int killer, int def_idx);
void MapScenario_NoteLoss(int owner, int def_idx);

/* 1 once the script has turned the battle clock on (legacy:171681). */
int  MapScenario_ClockShown(void);

/* Where the script's own presentation goes. */
typedef void (*MapScenario_SpawnFn)(int handle);
void MapScenario_SetSpawnHook(MapScenario_SpawnFn fn);
/* A message line for seat `to`, said by seat `from` (legacy:171683-171727). */
typedef void (*MapScenario_MessageFn)(int to, int from, const char *text);
void MapScenario_SetMessageHook(MapScenario_MessageFn fn);

/* State for the save and for the simulation hash. */
unsigned MapScenario_SaveSize(void);
void     MapScenario_SaveState(unsigned char *out);
int      MapScenario_LoadState(const unsigned char *in, unsigned len);
uint32_t TAK_SimHash_Scenario(uint32_t h);

/* Test seams: a seat's timer and counter, -1 when it has none. */
int  MapScenario_DebugTimer(int player, int id, int32_t *out);
int  MapScenario_DebugCounter(int player, int id, int32_t *out);

#endif /* TAK_MAP_SCENARIO_H */
