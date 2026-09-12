#ifndef TAK_SAVEGAME_H
#define TAK_SAVEGAME_H

#include <stddef.h>
#include <stdint.h>

#include "tak_battle_config.h"
#include "tak_savefile.h"
#include "tak_sha256.h"

/* The game state inside the .oksave container.
 *
 * The container carries bytes and knows nothing about a battle. This
 * module is the other half: it turns the live simulation into section
 * payloads and reads them back. Every field goes out at an explicit
 * width through tak_bytes.h, so a save written by the 32 bit Windows
 * build or the wasm32 browser build opens on a 64 bit macOS or Linux
 * build. No struct is ever handed to a write call.
 *
 * This half covers the light state: which battle was set up, which map
 * it was played on, the world scalars, the generator and the camera.
 * Units, scripts, projectiles and fog land in their own sections on top
 * of it. Nothing here touches a platform or a window. */

/* The simulation field layout. Any record change bumps it. */
#define TAK_SAVE_SCHEMA_VERSION 2u

#define TAK_SECT_DEFS TAK_SAVE_ID('D', 'E', 'F', 'S')
#define TAK_SECT_CFGB TAK_SAVE_ID('C', 'F', 'G', 'B')
#define TAK_SECT_WRLD TAK_SAVE_ID('W', 'R', 'L', 'D')
#define TAK_SECT_CAMR TAK_SAVE_ID('C', 'A', 'M', 'R')
/* The battle itself. Every one of these is required: each carries
 * simulation state the hash covers, so a reader that skipped one
 * would bring a battle up that is not the one that was saved. */
#define TAK_SECT_UNIT TAK_SAVE_ID('U', 'N', 'I', 'T')
#define TAK_SECT_UPTH TAK_SAVE_ID('U', 'P', 'T', 'H')
#define TAK_SECT_UCOB TAK_SAVE_ID('U', 'C', 'O', 'B')
#define TAK_SECT_PROJ TAK_SAVE_ID('P', 'R', 'O', 'J')
#define TAK_SECT_FEAT TAK_SAVE_ID('F', 'E', 'A', 'T')
#define TAK_SECT_FOGV TAK_SAVE_ID('F', 'O', 'G', 'V')
#define TAK_SECT_ECON TAK_SAVE_ID('E', 'C', 'O', 'N')
#define TAK_SECT_AIST TAK_SAVE_ID('A', 'I', 'S', 'T')
#define TAK_SECT_OCCU TAK_SAVE_ID('O', 'C', 'C', 'U')

/* Section widths, hand summed and asserted at compile time, so a field
 * added without bumping the version breaks the build rather than
 * corrupting saves. */
#define TAK_DEFS_RECORD_BYTES  12u
#define TAK_CFGB_BYTES        544u
#define TAK_WRLD_BYTES        456u
#define TAK_CAMR_BYTES          8u
#define TAK_UNIT_RECORD_BYTES 468u
#define TAK_PROJ_RECORD_BYTES 216u
#define TAK_FEAT_RECORD_BYTES  32u
#define TAK_ECON_BYTES        260u
#define TAK_FOGV_HEADER_BYTES  16u
#define TAK_OCCU_HEADER_BYTES   8u
#define TAK_OCCU_CELL_BYTES     4u
/* One COB piece and the fixed half of one COB thread. A thread's
 * stack follows at its live depth only: the words above the stack
 * pointer are whatever a deeper call left there. */
#define TAK_COB_PIECE_BYTES   100u
#define TAK_COB_THREAD_BYTES   24u

/* A DEFS entry names a unit definition or a feature definition. */
#define TAK_DEF_KIND_UNIT     0
#define TAK_DEF_KIND_FEATURE  1

/* What a save says before a world exists. The loader reads this to
 * know which battle to bring up and on which map. */
typedef struct TAK_SaveInfo {
    BattleConfig cfg;
    char     map_name[96];
    char     map_kingdom[32];
    uint32_t schema_version;
    uint32_t sim_tick;
    uint32_t sim_state_hash;
    uint64_t saved_at_utc;
    uint8_t  save_kind;
    char     engine_build[TAK_SAVE_BUILD_MAX];
    uint8_t  map_fingerprint[TAK_SHA256_BYTES];
    /* The camera section is optional, so a save may carry none. */
    int32_t  cam_x;
    int32_t  cam_y;
    int      has_camera;
} TAK_SaveInfo;

/* Write the live world to `path`. No platform and no window: the
 * simulation is all the writer reads. Returns 0, or -1 with a reason
 * in `err`. */
int Save_Write(const char *path, char *err, size_t err_cap);

typedef struct TAK_SaveGame TAK_SaveGame;

/* Open a save and decode the light state. The world does not have to
 * exist yet. A save whose map is missing, or whose terrain has changed
 * since it was written, is refused by the map's name. Returns NULL
 * with a reason in `err` on any refusal. */
TAK_SaveGame *Save_Read(const char *path, char *err, size_t err_cap);

const TAK_SaveInfo *Save_Info(const TAK_SaveGame *sg);

/* Put the battle back into a live world: the world scalars, the
 * generator, the camera, every unit with its orders and its script
 * threads, the shots in flight, the features, every player's fog, the
 * occupancy layer, the economy and the AI.
 *
 * ON FAILURE THE CALLER MUST CALL World_End. This never ends the
 * world itself, on any path.
 *
 * The world has to exist and has to have been through the loading
 * screen already, because this fills in units, fog and occupancy that
 * only have somewhere to go once the map is up. The sequence is
 *
 *   sg = Save_Read(path, err, cap);
 *   info = Save_Info(sg);
 *   World_BeginLoad(plat, &info->cfg, info->map_name, info->map_kingdom);
 *   World_SetRestoring(1);
 *   ... run the loading screen to the end ...
 *   Save_Apply(sg, err, cap);
 *   Save_ReadClose(sg);
 *
 * Every definition the save names is checked by name and by content
 * before anything is written, so a data set that changed under the
 * save is refused by the name of the definition that moved and the
 * world is left exactly as the loading screen made it.
 *
 * ON ANY FAILURE THE CALLER OWNS THE TEARDOWN. This function never
 * touches a platform, so it cannot release the map's GPU textures and
 * cannot call World_End itself. A refusal past the definition check
 * leaves a world holding part of a battle, and the only correct
 * answer is World_End followed by a return to the menu. Showing the
 * refusal and leaving the half restored world standing is the one
 * outcome a player must never be given.
 *
 * Returns 0, or -1 with a reason in `err`. */
int Save_Apply(TAK_SaveGame *sg, char *err, size_t err_cap);

void Save_ReadClose(TAK_SaveGame *sg);

#endif /* TAK_SAVEGAME_H */
