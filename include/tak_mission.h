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
    /* KillUnitType and UnitTypeKilled: the deaths of the type counted
     * so far, which the save keeps. */
    int kills;
} MissionObjective;

typedef struct MissionUnitSnapshot {
    char unitname[32];
    int  player;
    int  x;
    int  z;
    int  alive;      /* on the map or aboard a transport */
    int  carried;    /* aboard a transport */
    int  mobile;
    int  commander;
} MissionUnitSnapshot;

/* Eleven victory keys plus seven defeat keys is the whole vocabulary,
 * and no shipped mission names more than three. */
#define TAK_MISSION_MAX_CONDITIONS 18

/* GlobalHeader PlayerN lines, N = 1..10, as written ("logo 6 CREON").
 * Index 0 is unused. */
#define TAK_MISSION_PLAYER_LINES 11

/* What a PlayerN line makes of its seat (legacy:169018-169095). */
typedef enum MissionSeatRelation {
    MISSION_SEAT_NEUTRAL  = 0,   /* every seat leaves it alone */
    MISSION_SEAT_ALLY     = 1,   /* allied with the player */
    MISSION_SEAT_OPPONENT = 2
} MissionSeatRelation;

typedef struct MissionSeat {
    /* The line names two of strategic, passive, opponent, ally and
     * neutral. Player1 is always present. */
    uint8_t present;
    uint8_t strategic;   /* "strategic" rather than "passive" */
    uint8_t relation;    /* MissionSeatRelation */
    int     logo;        /* "logo N", the seat's colour, -1 when unnamed */
} MissionSeat;

/* The seat a mission's victory conditions read as the enemy and its
 * defeat conditions as the player: the original's second seat and its
 * first, whatever the other lines say (legacy:240361-240377). */
#define TAK_MISSION_ENEMY_SEAT 2

typedef struct MissionData {
    char path[256];
    char mission_name[96];
    char kingdom[32];
    char player_lines[TAK_MISSION_PLAYER_LINES][96];
    MissionSeat seats[TAK_MISSION_PLAYER_LINES];
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
    /* The unit types Missions\<stem>.tdf names, the only ones the
     * mission lets anyone make (legacy:164261-164355). has_roster is 0
     * when the mission has no such file and every type is allowed. */
    int   has_roster;
    char (*roster)[32];
    int   roster_count;
} MissionData;

int  Mission_LoadOTA(const char *vfs_path, MissionData *out);
/* Reads one PlayerN line. `first` is Player1, which counts as present
 * whatever it says (legacy:169044). */
void Mission_ParseSeat(const char *line, int first, MissionSeat *out);
/* A unit's record ended: the kill keys count it (legacy:227286). */
void Mission_NoteUnitGone(MissionData *mission, const char *unitname, int owner);
/* 1 when the roster names the type, or when there is no roster. */
int  Mission_RosterAllows(const MissionData *mission, const char *unitname);
/* A placed unit's Angle as a heading in radians. The file counts
 * degrees (legacy:169614-169623) from north turning west, as the
 * original draws them. The heading turns east. */
float Mission_AngleToHeading(int angle);
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

/* Whether a condition, once met, stays met. The original latches every
 * one but the two that count what stands afresh each time
 * (legacy:239289, legacy:240940-240965). */
int  Mission_ConditionLatches(MissionObjectiveType type);
/* The verdict of one role from each condition's held flag, as
 * Mission_VictoryMet and Mission_DefeatMet decide it. */
int  Mission_RoleHeld(const MissionData *mission, const uint8_t *held, int role);

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
/* Each seat's colour from its PlayerN line's logo. */
void Mission_ApplySeatColors(const MissionData *mission,
                             struct BattleConfig *cfg);
/* Sets the options a mission file decides. Call before the fog is built. */
void Mission_ApplyVisibility(const MissionData *mission,
                             struct BattleConfig *cfg);

#endif /* TAK_MISSION_H */
