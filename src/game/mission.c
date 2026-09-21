#include "tak_mission.h"
#include "tak_battle_config.h"
#include "tak_memory.h"
#include "tak_tdf.h"
#include "tak_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int parse_size_text(const char *text, int *out_x, int *out_y) {
    int x = 0, y = 0;
    if (!text || !out_x || !out_y) return -1;
    if (sscanf(text, " %d x %d", &x, &y) != 2) return -1;
    if (x <= 0 || y <= 0) return -1;
    *out_x = x;
    *out_y = y;
    return 0;
}

static int append_placement(MissionData *mission,
                            int *capacity,
                            const MissionPlacement *placement) {
    MissionPlacement *bigger;
    if (!mission || !capacity || !placement) return -1;
    if (mission->placement_count >= *capacity) {
        int next = (*capacity == 0) ? 64 : (*capacity * 2);
        bigger = (MissionPlacement *)tak_realloc(
            mission->placements, (size_t)next * sizeof(MissionPlacement));
        if (!bigger) return -1;
        mission->placements = bigger;
        *capacity = next;
    }
    mission->placements[mission->placement_count++] = *placement;
    return 0;
}

static int append_objective(MissionData *mission,
                            int *capacity,
                            const MissionObjective *objective) {
    MissionObjective *bigger;
    if (!mission || !capacity || !objective) return -1;
    if (mission->objective_count >= *capacity) {
        int next = (*capacity == 0) ? 8 : (*capacity * 2);
        bigger = (MissionObjective *)tak_realloc(
            mission->objectives, (size_t)next * sizeof(MissionObjective));
        if (!bigger) return -1;
        mission->objectives = bigger;
        *capacity = next;
    }
    mission->objectives[mission->objective_count++] = *objective;
    if (objective->role == MISSION_ROLE_DEFEAT) mission->defeat_count++;
    else mission->victory_count++;
    return 0;
}

static void skip_delims(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == ',' || **p == ';') (*p)++;
}

static int is_digit_char(char c) {
    return c >= '0' && c <= '9';
}

static int is_number_token(const char *s) {
    if (!s || !*s) return 0;
    if (*s == '-' || *s == '+') s++;
    if (!is_digit_char(*s)) return 0;
    while (*s) {
        if (!is_digit_char(*s)) return 0;
        s++;
    }
    return 1;
}

static int mission_unit_matches(const MissionUnitSnapshot *u,
                                const char *unitname) {
    if (!u || !unitname || !*unitname) return 0;
    return tak_stricmp(u->unitname, unitname) == 0;
}

static int mission_unit_is_enemy(const MissionUnitSnapshot *u,
                                 int local_player) {
    return u && u->player > 0 && u->player != local_player;
}

/* An empty type name is the ANYTYPE wildcard the original stores for
 * the three conditions that accept it (legacy:239493). */
static int mission_unit_matches_any(const MissionUnitSnapshot *u,
                                    const char *unitname) {
    if (!u) return 0;
    if (!unitname || !*unitname) return 1;
    return tak_stricmp(u->unitname, unitname) == 0;
}

/* Which side a condition counts. A key that takes no unit type has to
 * say, because otherwise the victory key and the defeat key of a pair
 * would ask the same question. */
#define MISSION_OWNER_ANY   0
#define MISSION_OWNER_ENEMY 1
#define MISSION_OWNER_OWN   2

static int mission_unit_owner_ok(const MissionUnitSnapshot *u,
                                 int local_player,
                                 int owner) {
    if (!u) return 0;
    if (owner == MISSION_OWNER_ENEMY) return mission_unit_is_enemy(u, local_player);
    if (owner == MISSION_OWNER_OWN)   return u->player == local_player;
    return 1;
}

static int any_matching_dead(const MissionUnitSnapshot *units,
                             int unit_count,
                             const char *unitname,
                             int local_player,
                             int owner) {
    for (int i = 0; i < unit_count; i++) {
        if (!mission_unit_matches(&units[i], unitname)) continue;
        if (!mission_unit_owner_ok(&units[i], local_player, owner)) continue;
        if (!units[i].alive) return 1;
    }
    return 0;
}

static int no_matching_alive(const MissionUnitSnapshot *units,
                             int unit_count,
                             const char *unitname,
                             int local_player,
                             int owner,
                             int mobile_only,
                             int commander_only) {
    int seen = 0;
    for (int i = 0; i < unit_count; i++) {
        const MissionUnitSnapshot *u = &units[i];
        if (unitname && *unitname && !mission_unit_matches(u, unitname)) continue;
        if (!mission_unit_owner_ok(u, local_player, owner)) continue;
        if (mobile_only && !u->mobile) continue;
        if (commander_only && !u->commander) continue;
        seen = 1;
        if (u->alive) return 0;
    }
    return seen;
}

static int any_matching_inside_radius(const MissionUnitSnapshot *units,
                                      int unit_count,
                                      const char *unitname,
                                      int x,
                                      int z,
                                      int radius) {
    int64_t rr = (int64_t)radius * (int64_t)radius;
    for (int i = 0; i < unit_count; i++) {
        const MissionUnitSnapshot *u = &units[i];
        int64_t dx, dz;
        if (!u->alive || !mission_unit_matches_any(u, unitname)) continue;
        dx = (int64_t)u->x - (int64_t)x;
        dz = (int64_t)u->z - (int64_t)z;
        if (dx * dx + dz * dz <= rr) return 1;
    }
    return 0;
}

static void read_token(const char **p, char *out, size_t out_cap) {
    size_t n = 0;
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p && **p != ' ' && **p != '\t' &&
           **p != ',' && **p != ';') {
        if (n + 1 < out_cap) out[n++] = **p;
        (*p)++;
    }
    out[n] = '\0';
}

static int read_int_arg(const char **p, int *out) {
    char token[32];
    read_token(p, token, sizeof(token));
    if (!is_number_token(token)) return -1;
    *out = (int)strtol(token, NULL, 10);
    return 0;
}

static int next_nonspace_is_number(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return is_digit_char(*p) || ((*p == '-' || *p == '+') && is_digit_char(p[1]));
}

static int next_nonspace_is_value(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return *p != '\0' && *p != ',' && *p != ';';
}

static int append_command(MissionCommand **commands,
                          int *count,
                          int *capacity,
                          const MissionCommand *cmd) {
    MissionCommand *bigger;
    if (*count >= *capacity) {
        int next = (*capacity == 0) ? 4 : (*capacity * 2);
        bigger = (MissionCommand *)tak_realloc(
            *commands, (size_t)next * sizeof(MissionCommand));
        if (!bigger) return -1;
        *commands = bigger;
        *capacity = next;
    }
    (*commands)[(*count)++] = *cmd;
    return 0;
}

int Mission_ParseInitialMission(const char *text,
                                MissionCommand **out_commands,
                                int *out_count) {
    const char *p;
    MissionCommand *commands = NULL;
    int count = 0;
    int capacity = 0;

    if (!out_commands || !out_count) return -1;
    *out_commands = NULL;
    *out_count = 0;
    if (!text || !*text) return 0;

    p = text;
    while (*p) {
        MissionCommand cmd;
        char op;
        memset(&cmd, 0, sizeof(cmd));
        skip_delims(&p);
        if (!*p) break;

        op = *p++;
        cmd.token[0] = op;
        cmd.token[1] = '\0';
        if (op >= 'A' && op <= 'Z') op = (char)(op - 'A' + 'a');

        switch (op) {
        case 'w':
            if (*p == 'a' || *p == 'A') {
                p++;
                cmd.type = MISSION_CMD_WAIT_ANIMATION;
                if (next_nonspace_is_value(p)) {
                    read_token(&p, cmd.text, sizeof(cmd.text));
                }
            } else {
                cmd.type = MISSION_CMD_WAIT;
                read_int_arg(&p, &cmd.a);
                if (next_nonspace_is_number(p)) read_int_arg(&p, &cmd.b);
            }
            break;
        case 'm':
            cmd.type = MISSION_CMD_MOVE;
            read_int_arg(&p, &cmd.a);
            read_int_arg(&p, &cmd.b);
            break;
        case 'p':
            cmd.type = MISSION_CMD_PATROL;
            read_int_arg(&p, &cmd.a);
            read_int_arg(&p, &cmd.b);
            break;
        case 'a': {
            char first[64];
            cmd.type = MISSION_CMD_ATTACK;
            read_token(&p, first, sizeof(first));
            if (is_number_token(first)) {
                cmd.a = (int)strtol(first, NULL, 10);
                read_int_arg(&p, &cmd.b);
            } else {
                copy_str(cmd.text, sizeof(cmd.text), first);
            }
            break;
        }
        case 'b':
            cmd.type = MISSION_CMD_BUILD;
            read_token(&p, cmd.text, sizeof(cmd.text));
            read_int_arg(&p, &cmd.a);
            read_int_arg(&p, &cmd.b);
            read_int_arg(&p, &cmd.c);
            break;
        case 'i':
            cmd.type = MISSION_CMD_BOARD;
            read_token(&p, cmd.text, sizeof(cmd.text));
            break;
        case 'o':
            cmd.type = MISSION_CMD_ORDERS;
            read_int_arg(&p, &cmd.a);
            break;
        case 'c':
            cmd.type = MISSION_CMD_CLOAK;
            break;
        case 'd':
            cmd.type = MISSION_CMD_SELF_DESTRUCT;
            break;
        case 's':
            cmd.type = MISSION_CMD_SELECTABLE;
            break;
        case 'v': {
            char value[32];
            cmd.type = MISSION_CMD_SPEED;
            read_token(&p, value, sizeof(value));
            cmd.value = (float)strtod(value, NULL);
            break;
        }
        case 'u':
            cmd.type = MISSION_CMD_UNLOAD;
            read_int_arg(&p, &cmd.a);
            read_int_arg(&p, &cmd.b);
            break;
        default:
            cmd.type = MISSION_CMD_UNKNOWN;
            read_token(&p, cmd.text, sizeof(cmd.text));
            break;
        }

        if (append_command(&commands, &count, &capacity, &cmd) != 0) {
            Mission_FreeCommands(commands);
            return -1;
        }
    }

    *out_commands = commands;
    *out_count = count;
    return 0;
}

void Mission_FreeCommands(MissionCommand *commands) {
    if (commands) tak_free(commands);
}

int Mission_ObjectiveSatisfied(const MissionObjective *objective,
                               const MissionUnitSnapshot *units,
                               int unit_count,
                               int local_player,
                               int elapsed_seconds) {
    if (!objective || unit_count < 0 || (!units && unit_count > 0)) return 0;
    if (local_player <= 0) local_player = 1;

    switch (objective->type) {
    /* Defeat: the player has nothing left. Victory: the enemy has not. */
    case MISSION_OBJ_ALL_UNITS_KILLED:
        return no_matching_alive(units, unit_count, NULL, local_player,
                                 MISSION_OWNER_OWN, 0, 0);
    case MISSION_OBJ_DESTROY_ALL_UNITS:
        return no_matching_alive(units, unit_count, NULL, local_player,
                                 MISSION_OWNER_ENEMY, 0, 0);
    /* A condition that names a unit type needs no side as well: which
     * list the key belongs to is what says whose loss it is. */
    case MISSION_OBJ_ALL_UNITS_KILLED_OF_TYPE:
    case MISSION_OBJ_KILL_ALL_OF_TYPE:
        return no_matching_alive(units, unit_count, objective->text,
                                 local_player, MISSION_OWNER_ANY, 0, 0);
    case MISSION_OBJ_KILL_ALL_MOBILE_UNITS:
        return no_matching_alive(units, unit_count, NULL, local_player,
                                 MISSION_OWNER_ENEMY, 1, 0);
    case MISSION_OBJ_COMMANDER_KILLED:
        return no_matching_alive(units, unit_count, objective->text,
                                 local_player, MISSION_OWNER_OWN, 0, 1);
    case MISSION_OBJ_KILL_ENEMY_COMMANDER:
        return no_matching_alive(units, unit_count, objective->text,
                                 local_player, MISSION_OWNER_ENEMY, 0, 1);
    case MISSION_OBJ_UNIT_TYPE_KILLED:
    case MISSION_OBJ_KILL_UNIT_TYPE:
        return any_matching_dead(units, unit_count, objective->text,
                                 local_player, MISSION_OWNER_ANY);
    /* A unit the player owns of that type, which is how a mission
     * reads both "build one" and "capture one". */
    case MISSION_OBJ_BUILD_UNIT_TYPE:
    case MISSION_OBJ_CAPTURE_UNIT_TYPE:
        for (int i = 0; i < unit_count; i++) {
            if (units[i].alive &&
                mission_unit_matches(&units[i], objective->text) &&
                units[i].player == local_player) return 1;
        }
        return 0;
    case MISSION_OBJ_MOVE_UNIT_TO_RADIUS:
        return any_matching_inside_radius(units, unit_count, objective->text,
                                          objective->a, objective->b,
                                          objective->c);
    case MISSION_OBJ_UNIT_TYPE_PASSES_X:
        for (int i = 0; i < unit_count; i++) {
            if (units[i].alive &&
                mission_unit_matches_any(&units[i], objective->text) &&
                units[i].x >= objective->a) return 1;
        }
        return 0;
    case MISSION_OBJ_UNIT_TYPE_PASSES_Z:
        for (int i = 0; i < unit_count; i++) {
            if (units[i].alive &&
                mission_unit_matches_any(&units[i], objective->text) &&
                units[i].z >= objective->a) return 1;
        }
        return 0;
    /* The defeat pair takes a bare coordinate and reads any enemy. */
    case MISSION_OBJ_ANY_UNIT_PASSES_X:
        for (int i = 0; i < unit_count; i++) {
            if (units[i].alive &&
                mission_unit_is_enemy(&units[i], local_player) &&
                units[i].x >= objective->a) return 1;
        }
        return 0;
    case MISSION_OBJ_ANY_UNIT_PASSES_Z:
        for (int i = 0; i < unit_count; i++) {
            if (units[i].alive &&
                mission_unit_is_enemy(&units[i], local_player) &&
                units[i].z >= objective->a) return 1;
        }
        return 0;
    case MISSION_OBJ_DEATH_TIMER_RUNS_OUT:
    case MISSION_OBJ_VICTORY_TIMER_RUNS_OUT:
        return elapsed_seconds >= objective->a;
    default:
        return 0;
    }
}

static int mission_role_met(const MissionData *mission,
                            const MissionUnitSnapshot *units,
                            int unit_count,
                            int local_player,
                            int elapsed_seconds,
                            int role) {
    int seen = 0;
    if (!mission) return 0;
    for (int i = 0; i < mission->objective_count; i++) {
        const MissionObjective *obj = &mission->objectives[i];
        int met;
        if (obj->role != role) continue;
        seen = 1;
        met = Mission_ObjectiveSatisfied(obj, units, unit_count,
                                         local_player, elapsed_seconds);
        if (role == MISSION_ROLE_VICTORY) {
            if (!met) return 0;
        } else if (met) {
            return 1;
        }
    }
    return role == MISSION_ROLE_VICTORY ? seen : 0;
}

int Mission_VictoryMet(const MissionData *mission,
                       const MissionUnitSnapshot *units,
                       int unit_count,
                       int local_player,
                       int elapsed_seconds) {
    return mission_role_met(mission, units, unit_count, local_player,
                            elapsed_seconds, MISSION_ROLE_VICTORY);
}

int Mission_DefeatMet(const MissionData *mission,
                      const MissionUnitSnapshot *units,
                      int unit_count,
                      int local_player,
                      int elapsed_seconds) {
    return mission_role_met(mission, units, unit_count, local_player,
                            elapsed_seconds, MISSION_ROLE_DEFEAT);
}

void Mission_Free(MissionData *mission) {
    int i;
    if (!mission) return;
    for (i = 0; i < mission->placement_count; i++) {
        Mission_FreeCommands(mission->placements[i].commands);
    }
    if (mission->placements) tak_free(mission->placements);
    if (mission->objectives) tak_free(mission->objectives);
    memset(mission, 0, sizeof(*mission));
}

void Mission_ApplyVisibility(const MissionData *mission, BattleConfig *cfg) {
    if (!mission || !cfg) return;
    /* A mission keeps line of sight on whatever its lineofsight key says
     * and starts black only when mapping is 1 (legacy:168880-168885).
     * map_revealed is the opposite sense of the file's key. */
    cfg->line_of_sight = 1;
    cfg->map_revealed = (mission->mapping != 1);
}

typedef struct ObjectiveKeySpec {
    const char *key;
    MissionObjectiveType type;
    MissionConditionRole role;
} ObjectiveKeySpec;

/* The two lists in the order the original builds them: eleven victory
 * keys then seven defeat keys (legacy:239266-239824). */
static const ObjectiveKeySpec g_objective_keys[] = {
    { "KillEnemyCommander",   MISSION_OBJ_KILL_ENEMY_COMMANDER,   MISSION_ROLE_VICTORY },
    { "DestroyAllUnits",      MISSION_OBJ_DESTROY_ALL_UNITS,      MISSION_ROLE_VICTORY },
    { "KillAllMobileUnits",   MISSION_OBJ_KILL_ALL_MOBILE_UNITS,  MISSION_ROLE_VICTORY },
    { "BuildUnitType",        MISSION_OBJ_BUILD_UNIT_TYPE,        MISSION_ROLE_VICTORY },
    { "CaptureUnitType",      MISSION_OBJ_CAPTURE_UNIT_TYPE,      MISSION_ROLE_VICTORY },
    { "KillAllOfType",        MISSION_OBJ_KILL_ALL_OF_TYPE,       MISSION_ROLE_VICTORY },
    { "KillUnitType",         MISSION_OBJ_KILL_UNIT_TYPE,         MISSION_ROLE_VICTORY },
    { "MoveUnitToRadius",     MISSION_OBJ_MOVE_UNIT_TO_RADIUS,    MISSION_ROLE_VICTORY },
    { "UnitTypePassesX",      MISSION_OBJ_UNIT_TYPE_PASSES_X,     MISSION_ROLE_VICTORY },
    { "UnitTypePassesZ",      MISSION_OBJ_UNIT_TYPE_PASSES_Z,     MISSION_ROLE_VICTORY },
    { "VictoryTimerRunsOut",  MISSION_OBJ_VICTORY_TIMER_RUNS_OUT, MISSION_ROLE_VICTORY },
    { "CommanderKilled",      MISSION_OBJ_COMMANDER_KILLED,       MISSION_ROLE_DEFEAT },
    { "AllUnitsKilled",       MISSION_OBJ_ALL_UNITS_KILLED,       MISSION_ROLE_DEFEAT },
    { "AllUnitsKilledOfType", MISSION_OBJ_ALL_UNITS_KILLED_OF_TYPE, MISSION_ROLE_DEFEAT },
    { "UnitTypeKilled",       MISSION_OBJ_UNIT_TYPE_KILLED,       MISSION_ROLE_DEFEAT },
    { "DeathTimerRunsOut",    MISSION_OBJ_DEATH_TIMER_RUNS_OUT,   MISSION_ROLE_DEFEAT },
    { "AnyUnitPassesX",       MISSION_OBJ_ANY_UNIT_PASSES_X,      MISSION_ROLE_DEFEAT },
    { "AnyUnitPassesZ",       MISSION_OBJ_ANY_UNIT_PASSES_Z,      MISSION_ROLE_DEFEAT },
};

static void objective_from_value(MissionObjective *obj, const char *value) {
    if (!value || !*value) return;

    /* Values are all compact comma-separated scalars. Parsing locally
     * avoids building a temporary TDF just to split one value. */
    const char *p = value;
    int slot = 0;
    while (*p && slot < 4) {
        char token[64];
        skip_delims(&p);
        read_token(&p, token, sizeof(token));
        if (!token[0]) break;
        if (slot == 0 && !is_number_token(token)) {
            /* ANYTYPE is stored as no name at all (legacy:239493). */
            if (tak_stricmp(token, "ANYTYPE") != 0) {
                copy_str(obj->text, sizeof(obj->text), token);
            }
        } else {
            int v = is_number_token(token) ? (int)strtol(token, NULL, 10) : 0;
            if (slot == 0) obj->a = v;
            else if (slot == 1) obj->a = v;
            else if (slot == 2) obj->b = v;
            else if (slot == 3) obj->c = v;
        }
        slot++;
    }
}

static int parse_objectives(TDFFile *tdf, MissionData *out) {
    int capacity = 0;
    int n_specs = (int)(sizeof(g_objective_keys) / sizeof(g_objective_keys[0]));
    for (int i = 0; i < n_specs; i++) {
        const ObjectiveKeySpec *spec = &g_objective_keys[i];
        const char *value = TDF_ReadString(tdf, spec->key, NULL);
        if (value) {
            MissionObjective obj;
            memset(&obj, 0, sizeof(obj));
            obj.a = -1;
            obj.type = spec->type;
            obj.role = spec->role;
            copy_str(obj.key, sizeof(obj.key), spec->key);
            objective_from_value(&obj, value);
            /* A timer counts only above zero and a coordinate line only
             * at zero or above (legacy:239648, legacy:239791). */
            if ((spec->type == MISSION_OBJ_VICTORY_TIMER_RUNS_OUT ||
                 spec->type == MISSION_OBJ_DEATH_TIMER_RUNS_OUT) && obj.a <= 0) {
                continue;
            }
            if ((spec->type == MISSION_OBJ_ANY_UNIT_PASSES_X ||
                 spec->type == MISSION_OBJ_ANY_UNIT_PASSES_Z) && obj.a < 0) {
                continue;
            }
            if (append_objective(out, &capacity, &obj) != 0) return -1;
        }
    }
    return 0;
}

void Mission_ApplyImplicitConditions(MissionData *mission, int campaign_mode) {
    int capacity = mission ? mission->objective_count : 0;
    if (!mission) return;
    if (mission->victory_count == 0 && !campaign_mode) {
        MissionObjective obj;
        memset(&obj, 0, sizeof(obj));
        obj.type = MISSION_OBJ_DESTROY_ALL_UNITS;
        obj.role = MISSION_ROLE_VICTORY;
        copy_str(obj.key, sizeof(obj.key), "DestroyAllUnits");
        (void)append_objective(mission, &capacity, &obj);
    }
    if (mission->defeat_count == 0) {
        MissionObjective obj;
        memset(&obj, 0, sizeof(obj));
        obj.type = MISSION_OBJ_ALL_UNITS_KILLED;
        obj.role = MISSION_ROLE_DEFEAT;
        copy_str(obj.key, sizeof(obj.key), "AllUnitsKilled");
        (void)append_objective(mission, &capacity, &obj);
    }
}

int Mission_LoadOTA(const char *vfs_path, MissionData *out) {
    TDFFile *tdf;
    int capacity = 0;
    int rc = -1;

    if (!vfs_path || !out) return -1;
    memset(out, 0, sizeof(*out));
    copy_str(out->path, sizeof(out->path), vfs_path);

    tdf = TDF_Open(vfs_path);
    if (!tdf || TDF_Load(tdf) != 0) goto done;
    if (TDF_PushSection(tdf, "GlobalHeader") != 0) goto done;

    copy_str(out->mission_name, sizeof(out->mission_name),
             TDF_ReadString(tdf, "MissionName", ""));
    copy_str(out->kingdom, sizeof(out->kingdom),
             TDF_ReadString(tdf, "Kingdom", ""));
    for (int n = 1; n < TAK_MISSION_PLAYER_LINES; n++) {
        char key[16];
        snprintf(key, sizeof(key), "Player%d", n);
        copy_str(out->player_lines[n], sizeof(out->player_lines[n]),
                 TDF_ReadString(tdf, key, ""));
    }
    parse_size_text(TDF_ReadString(tdf, "Size", ""), &out->size_x, &out->size_y);
    /* A missing key reads as 0 (legacy:168883). */
    out->mapping = TDF_ReadInt(tdf, "mapping", 0);
    if (parse_objectives(tdf, out) != 0) goto done;

    if (TDF_PushSection(tdf, "Map Data") == 0 &&
        TDF_PushSection(tdf, "units") == 0) {
        const char *section = TDF_GetFirstSection(tdf);
        while (section) {
            if (TDF_PushSection(tdf, section) == 0) {
                MissionPlacement p;
                memset(&p, 0, sizeof(p));
                copy_str(p.section, sizeof(p.section), section);
                copy_str(p.unitname, sizeof(p.unitname),
                         TDF_ReadString(tdf, "Unitname", ""));
                copy_str(p.ident, sizeof(p.ident),
                         TDF_ReadString(tdf, "Ident", ""));
                p.player = TDF_ReadInt(tdf, "Player", -1);
                p.x = TDF_ReadInt(tdf, "XPos", -1);
                p.y = TDF_ReadInt(tdf, "YPos", 0);
                p.z = TDF_ReadInt(tdf, "ZPos", -1);
                p.angle = TDF_ReadInt(tdf, "Angle", 0);
                p.health_percent = TDF_ReadInt(tdf, "HealthPercentage", 100);
                p.mana_percent = TDF_ReadInt(tdf, "ManaPercentage", 100);
                copy_str(p.initial_mission, sizeof(p.initial_mission),
                         TDF_ReadString(tdf, "InitialMission", ""));
                if (Mission_ParseInitialMission(p.initial_mission,
                                                &p.commands,
                                                &p.command_count) != 0) {
                    TDF_PopSection(tdf);
                    goto done;
                }
                if (append_placement(out, &capacity, &p) != 0) {
                    Mission_FreeCommands(p.commands);
                    TDF_PopSection(tdf);
                    goto done;
                }
                TDF_PopSection(tdf);
            }
            section = TDF_GetNextSection(tdf);
        }
        TDF_PopSection(tdf);
        TDF_PopSection(tdf);
    }

    /* Every file this reads is a campaign mission, which is the mode
     * that gets no implicit victory condition (legacy:239825). */
    Mission_ApplyImplicitConditions(out, 1);
    rc = 0;

done:
    if (tdf) TDF_Close(tdf);
    if (rc != 0) Mission_Free(out);
    return rc;
}
