#ifndef TAK_MISSION_SCRIPT_H
#define TAK_MISSION_SCRIPT_H

#include <stdint.h>

/* ── A campaign mission's moving parts ─────────────────────────────
 *
 * Two things the mission's .ota does not say by itself. The order list
 * a placed unit carries (InitialMission, legacy:228246) is a queue the
 * unit works through one order at a time, waits included. And most
 * missions ship a map script beside the .ota, missions\<stem>.cob,
 * which the original runs on the unit script machine with a host of
 * its own (legacy:177876): it creates units, sets triggers on the map,
 * hands out order lists and calls the mission won or lost.
 *
 * Script numbers: a unit is its handle plus one so that nought is "no
 * unit", a player is the seat less one (legacy:178382 indexes the
 * player table with it), and a place is in 16 pixel squares like the
 * .ota (legacy:178391). */

#define TAK_MS_TRIGGERS 16   /* legacy:178237 */

/* Start a mission's script. stem is the mission file's name without
 * its extension. A mission with no script is not an error: the order
 * lists still run. start_index is the local player's start position,
 * which is what Start is handed (legacy:177949). */
int  MissionScript_Begin(const char *stem, int local_player, int start_index);
/* The same with a script the caller built and keeps, for tests. */
struct CobScript;
int  MissionScript_BeginWith(struct CobScript *script, int local_player,
                             int start_index);
/* The units a seat may have, which is what the script's port 7 reads. */
void MissionScript_SetUnitLimit(int limit);
void MissionScript_End(void);
/* 1 between Begin and End. */
int  MissionScript_Active(void);
/* 1 when the mission has a map script running. */
int  MissionScript_HasScript(void);
/* One simulation tick: events, the script's threads, the order lists. */
void MissionScript_Tick(void);
/* What the script has called: 1 won, -1 lost, 0 nothing yet (the SET of
 * port 2, legacy:178706). */
int  MissionScript_Verdict(void);
/* A ScreenShake the script asked for since the last call. */
int  MissionScript_TakeShake(int *magnitude, int *frames);
/* Where PLAY-SOUND goes. flags are the script's (bits 0-2 category). */
typedef void (*MissionScript_SoundFn)(const char *name, int flags);
void MissionScript_SetSoundHook(MissionScript_SoundFn fn);
/* Called for each unit a script creates, so the battle can do what it
 * does for a placed unit. */
typedef void (*MissionScript_SpawnFn)(int handle);
void MissionScript_SetSpawnHook(MissionScript_SpawnFn fn);

/* Give a unit an order list, replacing the one it had. Returns the
 * number of orders queued. */
int  MissionOrders_Give(int handle, const char *text);
/* Bind an .ota Ident to the unit that carries it. */
void MissionOrders_NameUnit(const char *ident, int handle);
/* 1 while the unit still has orders to work through. */
int  MissionOrders_Running(int handle);
/* Orders the lists have finished since Begin, for tests and probes. */
int  MissionOrders_DebugDone(void);

/* State for the save and for the simulation hash. */
unsigned MissionScript_SaveSize(void);
void     MissionScript_SaveState(unsigned char *out);
int      MissionScript_LoadState(const unsigned char *in, unsigned len);
uint32_t TAK_SimHash_Mission(uint32_t h);

/* Test seams. */
int  MissionScript_DebugTrigger(int id, int *x, int *z, int *x2, int *z2,
                                int *radius);
int  MissionScript_DebugStatic(int index, int32_t *out);

#endif /* TAK_MISSION_SCRIPT_H */
