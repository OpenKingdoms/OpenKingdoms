#ifndef TAK_BATTLE_SETUP_H
#define TAK_BATTLE_SETUP_H

#include "tak_platform.h"
#include "tak_battle_config.h"


/* Initialize the battle setup screen: load all GAF assets, decode to RGBA.
 * Call once when entering GAMESTATE_BATTLE_SETUP. Returns 0 on success. */
int BattleSetup_Init(TAK_Platform *platform);

/* Per-frame update and render. Call every frame while in GAMESTATE_BATTLE_SETUP.
 * Returns the game state to transition to, or GAMESTATE_BATTLE_SETUP to stay. */
int BattleSetup_Tick(TAK_Platform *platform, float frame_dt);

/* Free all battle setup screen resources. Call when leaving GAMESTATE_BATTLE_SETUP. */
void BattleSetup_Shutdown(void);

/* Press Play on the next tick with the default lineup (--skirmish). */
void BattleSetup_RequestAutoStart(void);

/* ── Inspection / drive points (used by the click paths and by tests) ── */

/* Number of maps the list holds, and the row text for one of them (the
 * authored name, not the archive's lower-cased file name). */
int         BattleSetup_MapCount(void);
const char *BattleSetup_MapDisplayName(int index);
/* The .ota base name that identifies the map to the loader. */
const char *BattleSetup_MapKey(int index);

/* Select a map by list index: updates the config and reloads the .ota
 * metadata (size, players, kingdom, description). */
void BattleSetup_SelectMap(int index);

/* Selected map's description, "" when none is selected. */
const char *BattleSetup_MapDescription(void);

/* The live config the screen hands to World_BeginLoad. */
const BattleConfig *BattleSetup_Config(void);

/* Cycle a slot's colour the way clicking its PlayerColor cell does. */
void BattleSetup_CyclePlayerColor(int slot);

#endif /* TAK_BATTLE_SETUP_H */
