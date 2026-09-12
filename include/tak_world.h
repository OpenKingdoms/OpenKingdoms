#ifndef TAK_WORLD_H
#define TAK_WORLD_H

#include "tak_battle_config.h"
#include "tak_economy.h"
#include "tak_tnt.h"
#include "tak_types.h"
#include "tak_platform.h"
#include "tak_mission.h"
#include "tak_moveinfo.h"
#include "tak_sim_rand.h"

/* Forward-decl so GameWorld can carry a pointer without pulling the
 * full tak_terrain.h include (that header uses a forward-decl of
 * GameWorld, so including it here would make the cycle un-breakable). */
typedef struct TerrainGrid TerrainGrid;

/* One player start position from the OTA's [specials] block. */
typedef struct StartPos {
    int player;     /* N from "specialwhat=StartPosN" (1-based)     */
    int x;          /* XPos in OTA map-squares                      */
    int z;          /* ZPos in OTA map-squares                      */
} StartPos;

/* ── GameWorld ──────────────────────────────────────────────────────────
 *
 * The single piece of engine state that outlives a screen transition.
 *
 * Created by Battle Setup when the player hits "Play" (via World_BeginLoad),
 * filled in piecewise by the Loading screen's asset-loader phases, consumed
 * by the InGame screen, and finally released when the player exits to menu
 * (via World_End).
 *
 * Per docs/PHASE_B_PLAN.md §3: module-global with accessor (World_Get),
 * mirroring the original engine's g_pGameState pattern. This lets any
 * subsystem read the active game state without threading a pointer through
 * every screen-module function signature.
 *
 * Phase B starting scope: battle config, map identity, and a loaded flag.
 * Phase B will grow this struct with TNTFile, palette RGBA table, start
 * positions, and camera state as later loader phases land.
 *
 * Lifecycle invariants:
 *   - World_Get() returns NULL before any World_BeginLoad, and after any
 *     World_End. Any non-NULL return is a valid, readable GameWorld.
 *   - `loaded == 0` during asset-loading; the InGame screen must not read
 *     anything beyond map_name/map_kingdom/cfg until loaded == 1.
 *   - World_BeginLoad on a world that's already live performs an implicit
 *     World_End first, so repeated Play clicks can't leak. */

/* The tallies the original keeps on each player record and prints on
 * the end screen (legacy:153968-154004). */
typedef struct PlayerBattleStats {
    int32_t units_built;   /* every creation except isfeature defs (legacy:226969) */
    int32_t kills;         /* units of another player this one destroyed (legacy:227302) */
    int32_t losses;        /* own units destroyed (legacy:227296) */
    int32_t score;         /* experiencepoints of every kill (legacy:227305) */
    int32_t eliminated;    /* the army was removed at once (legacy:227541) */
    int32_t last_alive_tick; /* last tick with units left, shown as Time (legacy:206617) */
} PlayerBattleStats;

typedef struct GameWorld {
    /* 1 once the asset loader has finished every phase. Until then the
     * only fields guaranteed readable are the handoff inputs below. */
    int          loaded;

    /* Copied from bs.cfg at World_BeginLoad time. Owned by this struct. */
    BattleConfig cfg;

    /* Map identity for the loader. map_name is the filename stem (no
     * extension), matching what the Battle Setup list displayed.
     * map_kingdom is the lowercased OTA kingdom= value, drives the
     * per-faction palette lookup. */
    char         map_name[96];
    char         map_kingdom[32];

    /* Populated by the Loading screen's asset-loader phases. Callers
     * must check `loaded == 1` before reading any of these. */
    TNTFile      tnt;                               /* LS_LOAD_TNT    */
    MoveInfoTable moveinfo;                         /* LS_LOAD_UNITS  */
    uint32_t     terrain_rgba[256];                 /* LS_LOAD_PALETTE */
    int          water_height;                       /* sidedata waterheight */
    /* Separate palette for feature sprites (lodestones, rocks, trees,
     * lamps). Authored against data/palettes/<kingdom>_features.pcx —
     * NOT the terrain palette. Reusing the terrain palette gives blue-
     * green blotchy garbage for stones and trees because the per-faction
     * features palette has its own dedicated entries. */
    uint32_t     features_rgba[256];                /* LS_LOAD_PALETTE */
    /* Heap-allocated in LS_LOAD_CHUNKS via TerrainGrid_Init(&world->tnt);
     * freed in World_End via TerrainGrid_Free. NULL until that phase runs. */
    TerrainGrid *grid;                              /* LS_LOAD_CHUNKS */
    StartPos     start_positions[TAK_MAX_PLAYERS];  /* LS_PARSE_OTA   */
    int          num_start_positions;               /* LS_PARSE_OTA   */
    MissionData  mission;                           /* campaign OTA units */
    int          mission_elapsed_ticks;              /* 60 Hz sim ticks */
    int          mission_elapsed_seconds;
    int          mission_objectives_satisfied;
    int          mission_victory;

    /* Skirmish end-state. Campaign maps use MissionData objectives;
     * skirmish maps use the original's unit-count rule (docs/notes/
     * 2026-09-10-end-of-battle.md). */
    int          skirmish_elapsed_ticks;
    int          skirmish_game_over;
    int          skirmish_winner_team;   /* 0 = none/draw, else normalized team */
    int          skirmish_local_result;  /* -1 defeat, 0 undecided/draw, 1 victory */
    char         skirmish_end_reason[64];
    /* Tick the verdict fired. The banner stays up for 3 s of ticks
     * before the statistics screen takes over (legacy:206566). */
    int          skirmish_end_tick;
    /* The statistics screen is up and the simulation has stopped
     * (legacy:244081). */
    int          skirmish_stats_open;
    /* One record per player slot, index 1..TAK_MAX_PLAYERS. */
    PlayerBattleStats stats[TAK_MAX_PLAYERS + 1];

    /* Diplomacy a player sets during the battle. The original told
     * only the affected player, which lockstep cannot allow, so these
     * arrive as commands everyone applies. Indexed [from][to] over
     * 1..TAK_MAX_PLAYERS. Two players stop being enemies only when
     * both have offered, so a one sided declaration cannot make
     * someone else hold fire. */
    uint8_t      allied[TAK_MAX_PLAYERS + 1][TAK_MAX_PLAYERS + 1];
    uint8_t      share_vision[TAK_MAX_PLAYERS + 1][TAK_MAX_PLAYERS + 1];
    uint8_t      share_units[TAK_MAX_PLAYERS + 1][TAK_MAX_PLAYERS + 1];
    uint8_t      share_mana[TAK_MAX_PLAYERS + 1][TAK_MAX_PLAYERS + 1];
    /* A seat that gave up. Counted out of the battle the same way an
     * army wiped off the map is. */
    uint8_t      resigned[TAK_MAX_PLAYERS + 1];

    /* World dimensions in pixels (tiles × 32) and the battle viewport. */
    int          map_pixels_w;                      /* LS_INIT_WORLD  */
    int          map_pixels_h;                      /* LS_INIT_WORLD  */
    int          viewport_w;                        /* LS_INIT_WORLD  */
    int          viewport_h;                        /* LS_INIT_WORLD  */

    /* Camera position in world pixels (top-left of viewport). Updated
     * each frame by the InGame screen's edge-scroll logic. Local
     * rendering state — never feeds back into sim in future MP. */
    int32_t      cam_x;                             /* LS_POSITION_CAMERA */
    int32_t      cam_y;                             /* LS_POSITION_CAMERA */

    /* Per-player mana pools, regen, accounting. Initialised to zero
     * in World_BeginLoad; monarch spawn (loading.c LS_FINALIZE) seeds
     * each active player's pool from the monarch's FBI fields. */
    EconomyState economy;

    /* Map features (lodestones, decorative stones, rocks, trees) read
     * from the TNT feature_layer at map load. Each entry is one
     * feature instance — its tile position, the LOCAL TNT id (which
     * indexes the per-map feature_names table at TNT offset 0x18),
     * and the resolved global registry index (built from
     * data/features/<world>/*.tdf). The legacy engine maps each
     * cell value through the per-map name table to a feature def's
     * section name, then through the global registry to spawn the
     * real feature. global_idx == -1 means we couldn't resolve. */
    struct MapFeature {
        uint16_t feat_id;     /* 0..0xFFFA = local TNT id, sentinel otherwise */
        uint16_t tile_x;
        uint16_t tile_z;
        int32_t  global_idx;  /* index into Features registry, -1 if unresolved */
        /* Where the model is drawn, in world pixels. Authored scenery
         * sits on its footprint centre, a corpse keeps the exact spot
         * and facing of the unit that fell (legacy:128220-128232). */
        int32_t  world_x;
        int32_t  world_y;
        uint16_t heading;     /* 65536 per turn, same sense as Unit.heading */
        /* The tilt the body lay with, 65536 per turn, which a raise
         * restores along with the heading (legacy:13172-13176). */
        uint16_t pitch, roll;
        /* Team colour the corpse is drawn in, or -1 for none
         * (legacy:227429). */
        int16_t  color_idx;
        /* Simulation ticks left before the instance starts to rot, or
         * -1 when its def carries no decomposetime (legacy:128400). */
        int32_t  decompose_ticks;
        /* 0 until the countdown runs out, then counts the ticks the
         * body has spent sinking into the ground. A sinking corpse can
         * no longer be swept or raised (legacy:128403-128413). */
        int16_t  sink_ticks;
    } *features;
    int        feature_count;
    int        feature_cap;   /* allocated entries; >= feature_count */

    /* Unit occupancy layer: one record per 16-px map cell, stamped
     * from each structure's yardmap. The dynamic half of passability
     * (terrain is the static half). See tak_occupancy.h; legacy keeps
     * the same per-cell occupant id (:219329) and gate tag (:218012). */
    struct TAK_OccCell *occ;
    int        occ_w;
    int        occ_h;
    uint32_t   occ_version;   /* bumps when a structure stamp changes */

    uint8_t   *fog_state;                         /* player-1 compatibility alias */
    uint8_t   *fog_layers[TAK_MAX_PLAYERS + 1];   /* 1-based player fog layers */
    int        fog_w;
    int        fog_h;
    int        fog_cell_px;
} GameWorld;

/* Create a fresh world with the given Battle Setup handoff. Copies cfg
 * by value; caller's pointer does not need to outlive this call. If a
 * world is already live it is World_End(plat)-ed first — platform is
 * required so any TerrainGrid GPU textures from the previous session
 * can be destroyed.
 *
 * Returns 0 on success, -1 on allocation failure (in which case
 * World_Get() continues to return NULL). */
int         World_BeginLoad(TAK_Platform       *plat,
                            const BattleConfig *cfg,
                            const char         *map_name,
                            const char         *kingdom);

/* Return the current world, or NULL if no BeginLoad/post-End. */
GameWorld  *World_Get(void);

/* Flip the loaded flag. Called by the asset loader after its final phase.
 * No-op if no world is live. */
void        World_MarkLoaded(void);

/* Tear down the current world and any sub-resources. plat is needed to
 * release the terrain grid's GPU textures (pass the same platform the
 * chunks were uploaded against). Safe to call when no world is live
 * (no-op). After this, World_Get() returns NULL. */
void        World_End(TAK_Platform *plat);

#endif /* TAK_WORLD_H */
