#ifndef TAK_MISSION_H
#define TAK_MISSION_H

#include <stdint.h>

typedef struct MissionPlacement {
    char section[32];
    char unitname[32];
    char ident[64];
    int  player;
    int  x;
    int  y;
    int  z;
    int  angle;
    int  health_percent;
    int  mana_percent;
    char initial_mission[256];
    struct MissionCommand *commands;
    int command_count;
} MissionPlacement;

typedef enum MissionCommandType {
    MISSION_CMD_UNKNOWN = 0,
    MISSION_CMD_WAIT,
    MISSION_CMD_WAIT_ANIMATION,
    MISSION_CMD_MOVE,
    MISSION_CMD_PATROL,
    MISSION_CMD_ATTACK,
    MISSION_CMD_BUILD,
    MISSION_CMD_IDENT,
    MISSION_CMD_OWNER,
    MISSION_CMD_CLOAK,
    MISSION_CMD_DEFEND,
    MISSION_CMD_STOP,
    MISSION_CMD_SPEED,
    MISSION_CMD_UNLOAD
} MissionCommandType;

typedef struct MissionCommand {
    MissionCommandType type;
    char token[32];
    char text[64];
    int a;
    int b;
    int c;
    float value;
} MissionCommand;

typedef enum MissionObjectiveType {
    MISSION_OBJ_UNKNOWN = 0,
    MISSION_OBJ_ALL_UNITS_KILLED,
    MISSION_OBJ_ALL_UNITS_KILLED_OF_TYPE,
    MISSION_OBJ_COMMANDER_KILLED,
    MISSION_OBJ_DEATH_TIMER_RUNS_OUT,
    MISSION_OBJ_DESTROY_ALL_UNITS,
    MISSION_OBJ_KILL_ALL_MOBILE_UNITS,
    MISSION_OBJ_KILL_ALL_OF_TYPE,
    MISSION_OBJ_KILL_ENEMY_COMMANDER,
    MISSION_OBJ_KILL_UNIT_TYPE,
    MISSION_OBJ_MOVE_UNIT_TO_RADIUS,
    MISSION_OBJ_UNIT_TYPE_KILLED,
    MISSION_OBJ_UNIT_TYPE_PASSES_X,
    MISSION_OBJ_UNIT_TYPE_PASSES_Z,
    MISSION_OBJ_VICTORY_TIMER_RUNS_OUT
} MissionObjectiveType;

typedef struct MissionObjective {
    MissionObjectiveType type;
    char key[32];
    char text[64];
    int a;
    int b;
    int c;
} MissionObjective;

typedef struct MissionUnitSnapshot {
    char unitname[32];
    int  player;
    int  x;
    int  z;
    int  alive;
    int  mobile;
    int  commander;
} MissionUnitSnapshot;

typedef struct MissionData {
    char path[256];
    char mission_name[96];
    char kingdom[32];
    int  size_x;
    int  size_y;
    /* The GlobalHeader mapping key. 1 starts the map black, anything
     * else starts it explored (legacy:168883). */
    int  mapping;
    MissionPlacement *placements;
    int placement_count;
    MissionObjective *objectives;
    int objective_count;
} MissionData;

int  Mission_LoadOTA(const char *vfs_path, MissionData *out);
int  Mission_ParseInitialMission(const char *text,
                                  MissionCommand **out_commands,
                                  int *out_count);
int  Mission_ObjectiveSatisfied(const MissionObjective *objective,
                                const MissionUnitSnapshot *units,
                                int unit_count,
                                int local_player,
                                int elapsed_seconds);
int  Mission_AllObjectivesSatisfied(const MissionData *mission,
                                    const MissionUnitSnapshot *units,
                                    int unit_count,
                                    int local_player,
                                    int elapsed_seconds);
void Mission_FreeCommands(MissionCommand *commands);
void Mission_Free(MissionData *mission);

struct BattleConfig;
/* Sets the options a mission file decides. Call before the fog is built. */
void Mission_ApplyVisibility(const MissionData *mission,
                             struct BattleConfig *cfg);

#endif /* TAK_MISSION_H */
