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
    /* i NAME: board the transport that carries that Ident
     * (legacy:228551). */
    MISSION_CMD_BOARD,
    /* o N: the standing orders, 0 hold, 1 defend, 2 roam
     * (legacy:228580, legacy:233216). */
    MISSION_CMD_ORDERS,
    MISSION_CMD_CLOAK,
    /* d: the unit destroys itself (legacy:228524). */
    MISSION_CMD_SELF_DESTRUCT,
    /* s: the player may select the unit again (legacy:228658). */
    MISSION_CMD_SELECTABLE,
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
    MISSION_OBJ_VICTORY_TIMER_RUNS_OUT,
    MISSION_OBJ_BUILD_UNIT_TYPE,
    MISSION_OBJ_CAPTURE_UNIT_TYPE,
    MISSION_OBJ_ANY_UNIT_PASSES_X,
    MISSION_OBJ_ANY_UNIT_PASSES_Z
} MissionObjectiveType;

/* The original keeps two condition lists: eleven victory conditions
 * combined with AND and seven defeat conditions combined with OR
 * (legacy:239243-239850). */
typedef enum MissionConditionRole {
    MISSION_ROLE_VICTORY = 0,
    MISSION_ROLE_DEFEAT  = 1
} MissionConditionRole;

typedef struct MissionObjective {
    MissionObjectiveType type;
    /* MISSION_ROLE_VICTORY or MISSION_ROLE_DEFEAT. */
    int  role;
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

/* Eleven victory keys plus seven defeat keys is the whole vocabulary,
 * and no shipped mission names more than three. */
#define TAK_MISSION_MAX_CONDITIONS 18

/* GlobalHeader PlayerN lines, N = 1..10, as written ("logo 6 CREON").
 * Index 0 is unused. */
#define TAK_MISSION_PLAYER_LINES 11

typedef struct MissionData {
    char path[256];
    char mission_name[96];
    char kingdom[32];
    char player_lines[TAK_MISSION_PLAYER_LINES][96];
    int  size_x;
    int  size_y;
    /* The GlobalHeader mapping key. 1 starts the map black, anything
     * else starts it explored (legacy:168883). */
    int  mapping;
    MissionPlacement *placements;
    int placement_count;
    MissionObjective *objectives;
    int objective_count;
    /* How many of `objectives` carry each role. */
    int victory_count;
    int defeat_count;
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
/* Victory needs every victory condition at once and never fires with
 * none of them (legacy:239922). Defeat needs any one of the defeat
 * conditions (legacy:239952). Victory is read first, so it wins a tie. */
int  Mission_VictoryMet(const MissionData *mission,
                        const MissionUnitSnapshot *units,
                        int unit_count,
                        int local_player,
                        int elapsed_seconds);
int  Mission_DefeatMet(const MissionData *mission,
                       const MissionUnitSnapshot *units,
                       int unit_count,
                       int local_player,
                       int elapsed_seconds);

/* The conditions the original adds when a mission names none: a
 * victory by destroying every enemy outside campaign mode, and a
 * defeat when the player's own army dies (legacy:239825-239851).
 * Mission_LoadOTA applies this with campaign_mode set, because every
 * file it reads is a campaign mission. Applying it again is a no-op
 * unless the mode differs. */
void Mission_ApplyImplicitConditions(MissionData *mission, int campaign_mode);
void Mission_FreeCommands(MissionCommand *commands);
void Mission_Free(MissionData *mission);

struct BattleConfig;
/* Sets the options a mission file decides. Call before the fog is built. */
void Mission_ApplyVisibility(const MissionData *mission,
                             struct BattleConfig *cfg);

#endif /* TAK_MISSION_H */
